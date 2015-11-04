/*
 * Copyright (c) 2013, 2015, Red Hat, Inc. and/or its affiliates.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#include "precompiled.hpp"
#include "asm/macroAssembler.hpp"

#include "classfile/symbolTable.hpp"

#include "gc_interface/collectedHeap.inline.hpp"
#include "gc_implementation/g1/concurrentMark.inline.hpp"
#include "gc_implementation/shared/gcHeapSummary.hpp"
#include "gc_implementation/shared/gcTimer.hpp"
#include "gc_implementation/shared/gcTrace.hpp"
#include "gc_implementation/shared/gcTraceTime.hpp"
#include "gc_implementation/shared/isGCActiveMark.hpp"

#include "gc_implementation/shenandoah/brooksPointer.hpp"
#include "gc_implementation/shenandoah/shenandoahHumongous.hpp"
#include "gc_implementation/shenandoah/shenandoahRootProcessor.hpp"
#include "gc_implementation/shenandoah/shenandoahHeap.inline.hpp"
#include "gc_implementation/shenandoah/shenandoahJNICritical.hpp"
#include "gc_implementation/shenandoah/shenandoahBarrierSet.inline.hpp"
#include "gc_implementation/shenandoah/vm_operations_shenandoah.hpp"
#include "oops/oop.inline.hpp"
#include "runtime/vmThread.hpp"
#include "memory/iterator.hpp"
#include "memory/oopFactory.hpp"
#include "memory/referenceProcessor.hpp"
#include "memory/space.inline.hpp"
#include "memory/threadLocalAllocBuffer.inline.hpp"
#include "memory/universe.hpp"
#include "utilities/copy.hpp"
#include "gc_implementation/shared/vmGCOperations.hpp"
#include "runtime/atomic.inline.hpp"

#define __ masm->

ShenandoahHeap* ShenandoahHeap::_pgc = NULL;

void ShenandoahHeap::print_heap_locations(HeapWord* start, HeapWord* end) {
  HeapWord* cur = NULL;
  for (cur = start; cur < end; cur++) {
    tty->print_cr(PTR_FORMAT" : "PTR_FORMAT, p2i(cur), p2i(*((HeapWord**) cur)));
  }
}

void ShenandoahHeap::print_heap_objects(HeapWord* start, HeapWord* end) {
  HeapWord* cur = NULL;
  for (cur = start; cur < end; cur = cur + oop(cur)->size()) {
    oop(cur)->print();
    print_heap_locations(cur, cur + oop(cur)->size());
  }
}

void ShenandoahHeap::print_heap_object(oop p) {
  HeapWord* hw = (HeapWord*) p;
  print_heap_locations(hw-1, hw+1+p->size());
}


class PrintHeapRegionsClosure : public
   ShenandoahHeapRegionClosure {
private:
  outputStream* _st;
public:
  PrintHeapRegionsClosure() : _st(tty) {}
  PrintHeapRegionsClosure(outputStream* st) : _st(st) {}

  bool doHeapRegion(ShenandoahHeapRegion* r) {
    r->print_on(_st);
    return false;
  }
};

class PrintHeapObjectsClosure : public ShenandoahHeapRegionClosure {
public:
  bool doHeapRegion(ShenandoahHeapRegion* r) {
    tty->print_cr("Region "INT32_FORMAT" top = "PTR_FORMAT" used = "SIZE_FORMAT_HEX" free = "SIZE_FORMAT_HEX,
               r->region_number(), p2i(r->top()), r->used(), r->free());

    ShenandoahHeap::heap()->print_heap_objects(r->bottom(), r->top());
    return false;
  }
};

jint ShenandoahHeap::initialize() {
  CollectedHeap::pre_initialize();

  size_t init_byte_size = collector_policy()->initial_heap_byte_size();
  size_t max_byte_size = collector_policy()->max_heap_byte_size();
  if (ShenandoahGCVerbose)
    tty->print_cr("init_byte_size = "SIZE_FORMAT","SIZE_FORMAT_HEX"  max_byte_size = "INT64_FORMAT","SIZE_FORMAT_HEX,
             init_byte_size, init_byte_size, max_byte_size, max_byte_size);

  Universe::check_alignment(max_byte_size,
                            ShenandoahHeapRegion::RegionSizeBytes,
                            "shenandoah heap");
  Universe::check_alignment(init_byte_size,
                            ShenandoahHeapRegion::RegionSizeBytes,
                            "shenandoah heap");

  ReservedSpace heap_rs = Universe::reserve_heap(max_byte_size,
                                                 Arguments::conservative_max_heap_alignment());
  _reserved.set_word_size(0);
  _reserved.set_start((HeapWord*)heap_rs.base());
  _reserved.set_end((HeapWord*) (heap_rs.base() + heap_rs.size()));

  set_barrier_set(new ShenandoahBarrierSet());
  ReservedSpace pgc_rs = heap_rs.first_part(max_byte_size);
  _storage.initialize(pgc_rs, init_byte_size);
  if (ShenandoahGCVerbose) {
    tty->print_cr("Calling initialize on reserved space base = "PTR_FORMAT" end = "PTR_FORMAT,
               p2i(pgc_rs.base()), p2i(pgc_rs.base() + pgc_rs.size()));
  }

  _num_regions = init_byte_size / ShenandoahHeapRegion::RegionSizeBytes;
  _max_regions = max_byte_size / ShenandoahHeapRegion::RegionSizeBytes;
  _ordered_regions = NEW_C_HEAP_ARRAY(ShenandoahHeapRegion*, _max_regions, mtGC);
  for (size_t i = 0; i < _max_regions; i++) {
    _ordered_regions[i] = NULL;
  }

  _initialSize = _num_regions * ShenandoahHeapRegion::RegionSizeBytes;
  size_t regionSizeWords = ShenandoahHeapRegion::RegionSizeBytes / HeapWordSize;
  assert(init_byte_size == _initialSize, "tautology");
  _free_regions = new ShenandoahHeapRegionSet(_max_regions);
  _collection_set = new ShenandoahHeapRegionSet(_max_regions);

  for (size_t i = 0; i < _num_regions; i++) {
    ShenandoahHeapRegion* current = new ShenandoahHeapRegion();
    current->initialize_heap_region((HeapWord*) pgc_rs.base() +
                                    regionSizeWords * i, regionSizeWords, i);
    _free_regions->append(current);
    _ordered_regions[i] = current;
  }
  _first_region = _ordered_regions[0];
  _first_region_bottom = _first_region->bottom();
  assert((((size_t) _first_region_bottom) & (ShenandoahHeapRegion::RegionSizeBytes - 1)) == 0, err_msg("misaligned heap: "PTR_FORMAT, p2i(_first_region_bottom)));

  _numAllocs = 0;

  if (ShenandoahGCVerbose) {
    tty->print("All Regions\n");
    print_heap_regions();
    tty->print("Free Regions\n");
    _free_regions->print();
  }

  // The call below uses stuff (the SATB* things) that are in G1, but probably
  // belong into a shared location.
  JavaThread::satb_mark_queue_set().initialize(SATB_Q_CBL_mon,
                                               SATB_Q_FL_lock,
                                               20 /*G1SATBProcessCompletedThreshold */,
                                               Shared_SATB_Q_lock);

  // Reserve space for prev and next bitmap.
  size_t bitmap_size = CMBitMap::compute_size(heap_rs.size());
  MemRegion heap_region = MemRegion((HeapWord*) heap_rs.base(), heap_rs.size() / HeapWordSize);

  ReservedSpace bitmap(ReservedSpace::allocation_align_size_up(bitmap_size));
  os::commit_memory_or_exit(bitmap.base(), bitmap.size(), false, err_msg("couldn't allocate mark bitmap"));
  MemRegion bitmap_region = MemRegion((HeapWord*) bitmap.base(), bitmap.size() / HeapWordSize);
  _mark_bit_map.initialize(heap_region, bitmap_region);

  _next_mark_bit_map = &_mark_bit_map;
  reset_mark_bitmap();

  // Initialize fast collection set test structure.
  _in_cset_fast_test_length = _max_regions;
  _in_cset_fast_test_base =
                   NEW_C_HEAP_ARRAY(bool, (size_t) _in_cset_fast_test_length, mtGC);
  _in_cset_fast_test = _in_cset_fast_test_base -
               ((uintx) pgc_rs.base() >> ShenandoahHeapRegion::RegionSizeShift);
  clear_cset_fast_test();

  _concurrent_gc_thread = new ShenandoahConcurrentThread();
  return JNI_OK;
}

ShenandoahHeap::ShenandoahHeap(ShenandoahCollectorPolicy* policy) :
  SharedHeap(policy),
  _shenandoah_policy(policy),
  _concurrent_mark_in_progress(false),
  _evacuation_in_progress(false),
  _update_references_in_progress(false),
  _free_regions(NULL),
  _collection_set(NULL),
  _bytesAllocSinceCM(0),
  _bytes_allocated_during_cm(0),
  _max_allocated_gc(0),
  _allocated_last_gc(0),
  _used_start_gc(0),
  _max_conc_workers((int) MAX2((uint) ConcGCThreads, 1U)),
  _max_parallel_workers((int) MAX2((uint) ParallelGCThreads, 1U)),
  _ref_processor(NULL),
  _in_cset_fast_test(NULL),
  _in_cset_fast_test_base(NULL),
  _mark_bit_map(),
  _cancelled_concgc(false),
  _need_update_refs(false),
  _need_reset_bitmaps(false),
  _jni_critical(new ShenandoahJNICritical())

{
  if (ShenandoahLogConfig) {
    tty->print_cr("Parallel GC threads: "UINT32_FORMAT, ParallelGCThreads);
    tty->print_cr("Concurrent GC threads: "UINT32_FORMAT, ConcGCThreads);
    tty->print_cr("Parallel reference processing enabled: %s", BOOL_TO_STR(ParallelRefProcEnabled));
  }
  _pgc = this;
  _scm = new ShenandoahConcurrentMark();
  _used = 0;
  // This is odd.  They are concurrent gc threads, but they are also task threads.
  // Framework doesn't allow both.
  _conc_workers = new FlexibleWorkGang("Concurrent GC Threads", ConcGCThreads,
                            /* are_GC_task_threads */true,
                            /* are_ConcurrentGC_threads */false);
  if (_conc_workers == NULL) {
    vm_exit_during_initialization("Failed necessary allocation.");
  } else {
    _conc_workers->initialize_workers();
  }
}

class ResetBitmapTask : public AbstractGangTask {
private:
  ShenandoahHeapRegionSet* _regions;

public:
  ResetBitmapTask(ShenandoahHeapRegionSet* regions) :
    AbstractGangTask("Parallel Reset Bitmap Task"),
    _regions(regions) {
  }

  void work(uint worker_id) {
    ShenandoahHeapRegion* region = _regions->claim_next();
    ShenandoahHeap* heap = ShenandoahHeap::heap();
    while (region != NULL) {
      heap->reset_mark_bitmap_range(region->bottom(), region->end());
      region = _regions->claim_next();
    }
  }
};

void ShenandoahHeap::reset_mark_bitmap() {
  if (ShenandoahTracePhases) {
    tty->print_cr("Shenandoah starting concurrent reset bitmaps");
  }
  ShenandoahHeapRegionSet regions = ShenandoahHeapRegionSet(_num_regions, _ordered_regions, _num_regions);
  ResetBitmapTask task = ResetBitmapTask(&regions);
  conc_workers()->set_active_workers(_max_conc_workers);
  conc_workers()->run_task(&task);
  if (ShenandoahTracePhases) {
    tty->print_cr("Shenandoah finishing concurrent reset bitmaps");
  }
}

void ShenandoahHeap::reset_mark_bitmap_range(HeapWord* from, HeapWord* to) {
  _next_mark_bit_map->clearRange(MemRegion(from, to));
}

bool ShenandoahHeap::is_bitmap_clear() {
  HeapWord* start = _ordered_regions[0]->bottom();
  HeapWord* end = _ordered_regions[_num_regions-1]->end();
  return _next_mark_bit_map->getNextMarkedWordAddress(start, end) == end;
}

void ShenandoahHeap::print_on(outputStream* st) const {
  st->print("Shenandoah Heap");
  st->print(" total = " SIZE_FORMAT " K, used " SIZE_FORMAT " K ", capacity()/ K, used() /K);
  st->print("Region size = " SIZE_FORMAT "K ", ShenandoahHeapRegion::RegionSizeBytes / K);
  if (_concurrent_mark_in_progress) {
    st->print("marking ");
  }
  if (_evacuation_in_progress) {
    st->print("evacuating ");
  }
  if (_update_references_in_progress) {
    st->print("updating-refs ");
  }
  if (_cancelled_concgc) {
    st->print("cancelled ");
  }
  st->print("\n");

  if (Verbose) {
    print_heap_regions(st);
  }
}

class InitGCLABClosure : public ThreadClosure {
public:
  void do_thread(Thread* thread) {
    thread->gclab().initialize(true);
  }
};

void ShenandoahHeap::post_initialize() {

  {
    MutexLockerEx ml(Threads_lock);
    InitGCLABClosure init_gclabs;
    for (JavaThread *thread = Threads::first(); thread != NULL; thread = thread->next()) {
      init_gclabs.do_thread(thread);
    }
    gc_threads_do(&init_gclabs);
  }
  _scm->initialize();

  ref_processing_init();

  _max_workers = MAX(_max_parallel_workers, _max_conc_workers);
}

class CalculateUsedRegionClosure : public ShenandoahHeapRegionClosure {
  size_t sum;
public:

  CalculateUsedRegionClosure() {
    sum = 0;
  }

  bool doHeapRegion(ShenandoahHeapRegion* r) {
    sum = sum + r->used();
    return false;
  }

  size_t getResult() { return sum;}
};

size_t ShenandoahHeap::calculateUsed() {
  CalculateUsedRegionClosure cl;
  heap_region_iterate(&cl);
  return cl.getResult();
}

size_t ShenandoahHeap::calculateFree() {
  return capacity() - calculateUsed();
}

void ShenandoahHeap::verify_heap_size_consistency() {

  assert(calculateUsed() == used(),
         err_msg("heap used size must be consistent heap-used: "SIZE_FORMAT" regions-used: "SIZE_FORMAT, used(), calculateUsed()));
}

size_t ShenandoahHeap::used() const {
  return _used;
}

void ShenandoahHeap::increase_used(size_t bytes) {
  _used += bytes;
}

void ShenandoahHeap::set_used(size_t bytes) {
  _used = bytes;
}

void ShenandoahHeap::decrease_used(size_t bytes) {
  assert(_used >= bytes, "never decrease heap size by more than we've left");
  _used -= bytes;

  // Atomic::add_ptr(-bytes, &_used);
}

size_t ShenandoahHeap::capacity() const {
  return _num_regions * ShenandoahHeapRegion::RegionSizeBytes;

}

bool ShenandoahHeap::is_maximal_no_gc() const {
  Unimplemented();
  return true;
}

size_t ShenandoahHeap::max_capacity() const {
  return _max_regions * ShenandoahHeapRegion::RegionSizeBytes;
}

class IsInRegionClosure : public ShenandoahHeapRegionClosure {
  const void* _p;
  bool _result;
public:

  IsInRegionClosure(const void* p) {
    _p = p;
    _result = false;
  }

  bool doHeapRegion(ShenandoahHeapRegion* r) {
    if (r->is_in(_p)) {
      _result = true;
      return true;
    }
    return false;
  }

  bool result() { return _result;}
};

bool ShenandoahHeap::is_in(const void* p) const {
  //  IsInRegionClosure isIn(p);
  //  heap_region_iterate(&isIn);
  //  bool result = isIn.result();

  //  return isIn.result();
  HeapWord* first_region_bottom = _first_region->bottom();
  HeapWord* last_region_end = first_region_bottom + (ShenandoahHeapRegion::RegionSizeBytes / HeapWordSize) * _num_regions;
  return p > _first_region_bottom && p < last_region_end;
}

bool ShenandoahHeap::is_in_partial_collection(const void* p ) {
  Unimplemented();
  return false;
}

bool  ShenandoahHeap::is_scavengable(const void* p) {
  //  nyi();
  //  return false;
  return true;
}

HeapWord* ShenandoahHeap::allocate_from_gclab(Thread* thread, size_t size) {
  if (UseTLAB) {
  HeapWord* obj = thread->gclab().allocate(size);
  if (obj != NULL) {
    return obj;
  }
  // Otherwise...
  return allocate_from_gclab_slow(thread, size);
  } else {
    return NULL;
  }
}

HeapWord* ShenandoahHeap::allocate_from_gclab_slow(Thread* thread, size_t size) {
  // Retain tlab and allocate object in shared space if
  // the amount free in the tlab is too large to discard.
  if (thread->gclab().free() > thread->gclab().refill_waste_limit()) {
    thread->gclab().record_slow_allocation(size);
    return NULL;
  }

  // Discard gclab and allocate a new one.
  // To minimize fragmentation, the last GCLAB may be smaller than the rest.
  size_t new_gclab_size = thread->gclab().compute_size(size);

  thread->gclab().clear_before_allocation();

  if (new_gclab_size == 0) {
    return NULL;
  }

  // Allocate a new GCLAB...
  HeapWord* obj = allocate_new_gclab(new_gclab_size);
  if (obj == NULL) {
    return NULL;
  }

  if (ZeroTLAB) {
    // ..and clear it.
    Copy::zero_to_words(obj, new_gclab_size);
  } else {
    // ...and zap just allocated object.
#ifdef ASSERT
    // Skip mangling the space corresponding to the object header to
    // ensure that the returned space is not considered parsable by
    // any concurrent GC thread.
    size_t hdr_size = oopDesc::header_size();
    Copy::fill_to_words(obj + hdr_size, new_gclab_size - hdr_size, badHeapWordVal);
#endif // ASSERT
  }
  thread->gclab().fill(obj, obj + size, new_gclab_size);
  return obj;
}

HeapWord* ShenandoahHeap::allocate_new_tlab(size_t word_size) {
  return allocate_new_tlab(word_size, true);
}

HeapWord* ShenandoahHeap::allocate_new_gclab(size_t word_size) {
  return allocate_new_tlab(word_size, false);
}

HeapWord* ShenandoahHeap::allocate_new_tlab(size_t word_size, bool mark) {
  HeapWord* result = allocate_memory(word_size);

  if (result != NULL) {
    assert(! heap_region_containing(result)->is_in_collection_set(), "Never allocate in dirty region");
    _bytesAllocSinceCM += word_size * HeapWordSize;

#ifdef ASSERT
    if (ShenandoahTraceTLabs)
      tty->print_cr("allocating new tlab of size "SIZE_FORMAT" at addr "PTR_FORMAT, word_size, p2i(result));
#endif

  }
  return result;
}

ShenandoahHeap* ShenandoahHeap::heap() {
  assert(_pgc != NULL, "Unitialized access to ShenandoahHeap::heap()");
  assert(_pgc->kind() == CollectedHeap::ShenandoahHeap, "not a shenandoah heap");
  return _pgc;
}

ShenandoahHeap* ShenandoahHeap::heap_no_check() {
  return _pgc;
}

class VM_ShenandoahVerifyHeap: public VM_GC_Operation {
public:
  VM_ShenandoahVerifyHeap(unsigned int gc_count_before,
                   unsigned int full_gc_count_before,
                   GCCause::Cause cause)
    : VM_GC_Operation(gc_count_before, cause, full_gc_count_before) { }
  virtual VMOp_Type type() const { return VMOp_G1CollectFull; }
  virtual void doit() {
    if (ShenandoahGCVerbose)
      tty->print_cr("verifying heap");
     Universe::heap()->ensure_parsability(false);
     Universe::verify();
  }
  virtual const char* name() const {
    return "Shenandoah verify trigger";
  }
};

HeapWord* ShenandoahHeap::allocate_memory(size_t word_size) {
  HeapWord* result = NULL;
  result = allocate_memory_with_lock(word_size);

  if (result == NULL && ! Thread::current()->is_evacuating()) { // Allocation failed, try full-GC, then retry allocation.
    // tty->print_cr("failed to allocate "SIZE_FORMAT " bytes, free regions:", word_size * HeapWordSize);
    // _free_regions->print();
    collect(GCCause::_allocation_failure);
    result = allocate_memory_with_lock(word_size);
  }

  return result;
}

HeapWord* ShenandoahHeap::allocate_memory_with_lock(size_t word_size) {
  return allocate_memory_shenandoah_lock(word_size);
}

HeapWord* ShenandoahHeap::allocate_memory_heap_lock(size_t word_size) {
  ShouldNotReachHere();
  MutexLocker ml(Heap_lock);
  return allocate_memory_work(word_size);
}

HeapWord* ShenandoahHeap::allocate_memory_shenandoah_lock(size_t word_size) {
  MutexLockerEx ml(ShenandoahHeap_lock, true);
  return allocate_memory_work(word_size);
}

ShenandoahHeapRegion* ShenandoahHeap::check_skip_humongous(ShenandoahHeapRegion* region) const {
  while (region != NULL && region->is_humongous()) {
    region = _free_regions->get_next();
  }
  return region;
}

ShenandoahHeapRegion* ShenandoahHeap::get_next_region_skip_humongous() const {
  ShenandoahHeapRegion* next = _free_regions->get_next();
  return check_skip_humongous(next);
}

ShenandoahHeapRegion* ShenandoahHeap::get_current_region_skip_humongous() const {
  ShenandoahHeapRegion* current = _free_regions->current();
  return check_skip_humongous(current);
}


ShenandoahHeapRegion* ShenandoahHeap::check_grow_heap(ShenandoahHeapRegion* current) {
  if (current == NULL) {
    if (grow_heap_by()) {
      current = _free_regions->get_next();
      assert(current != NULL, "After successfully growing the heap we should have a region");
      assert(! current->is_humongous(), "new region must not be humongous");
    } else {
      current = NULL; // No more room to make a new region. OOM.
    }
  }
  return current;
}

ShenandoahHeapRegion* ShenandoahHeap::get_current_region() {
  ShenandoahHeapRegion* current = get_current_region_skip_humongous();
  return check_grow_heap(current);
}

ShenandoahHeapRegion* ShenandoahHeap::get_next_region() {
  ShenandoahHeapRegion* current = get_next_region_skip_humongous();
  return check_grow_heap(current);
}


HeapWord* ShenandoahHeap::allocate_memory_work(size_t word_size) {

  if (word_size * HeapWordSize > ShenandoahHeapRegion::RegionSizeBytes) {
    assert(! Thread::current()->is_evacuating(), "no humongous allocation for evacuating thread");
    return allocate_large_memory(word_size);
  }

  ShenandoahHeapRegion* my_current_region = get_current_region();
  if (my_current_region == NULL) {
    return NULL; // No more room to make a new region. OOM.
  }
  assert(my_current_region != NULL, "should have a region at this point");

#ifdef ASSERT
  if (my_current_region->is_in_collection_set()) {
    print_heap_regions();
  }
#endif
  assert(! my_current_region->is_in_collection_set(), "never get targetted regions in free-lists");
  assert(! my_current_region->is_humongous(), "never attempt to allocate from humongous object regions");

  HeapWord* result;

  result = my_current_region->par_allocate(word_size);
  while (result == NULL && my_current_region != NULL) {
    // 2nd attempt. Try next region.
    size_t remaining = my_current_region->free();
    my_current_region = get_next_region();
    if (my_current_region == NULL) {
      return NULL; // No more room to make a new region. OOM.
    }
    // _free_regions->decrease_available(remaining);
    assert(my_current_region != NULL, "should have a region at this point");
    assert(! my_current_region->is_in_collection_set(), "never get targetted regions in free-lists");
    assert(! my_current_region->is_humongous(), "never attempt to allocate from humongous object regions");
    result = my_current_region->par_allocate(word_size);
  }

  if (result != NULL) {
    my_current_region->increase_live_data(word_size * HeapWordSize);
    increase_used(word_size * HeapWordSize);
    _free_regions->decrease_available(word_size * HeapWordSize);
  }
  return result;
}

HeapWord* ShenandoahHeap::allocate_large_memory(size_t words) {
  if (ShenandoahTraceHumongous) {
    gclog_or_tty->print_cr("allocating humongous object of size: "SIZE_FORMAT" KB", (words * HeapWordSize) / K);
  }

  uint required_regions = ShenandoahHumongous::required_regions(words * HeapWordSize);

  assert(required_regions <= _max_regions, "sanity check");

  HeapWord* result;
  ShenandoahHeapRegion* free_regions[required_regions];

  bool success = find_contiguous_free_regions(required_regions, free_regions);
  if (! success) {
    success = allocate_contiguous_free_regions(required_regions, free_regions);
  }
  if (! success) {
    result = NULL; // Throw OOM, we cannot allocate the huge object.
  } else {
    // Initialize huge object flags in the regions.
    size_t live = words * HeapWordSize;
    free_regions[0]->set_humongous_start(true);
    free_regions[0]->increase_live_data(live);

    for (uint i = 0; i < required_regions; i++) {
      if (i == 0) {
        free_regions[0]->set_humongous_start(true);
      } else {
        free_regions[i]->set_humongous_continuation(true);
      }
      free_regions[i]->set_top(free_regions[i]->end());
      increase_used(ShenandoahHeapRegion::RegionSizeBytes);
    }
    _free_regions->decrease_available(ShenandoahHeapRegion::RegionSizeBytes * required_regions);
    result = free_regions[0]->bottom();
  }
  return result;
}

bool ShenandoahHeap::find_contiguous_free_regions(uint num_free_regions, ShenandoahHeapRegion** free_regions) {
  if (ShenandoahTraceHumongous) {
    gclog_or_tty->print_cr("trying to find "UINT32_FORMAT" contiguous free regions", num_free_regions);
  }
  uint free_regions_index = 0;
  for (uint regions_index = 0; regions_index < _num_regions; regions_index++) {
    // Claim a free region.
    ShenandoahHeapRegion* region = _ordered_regions[regions_index];
    bool free = false;
    if (region != NULL) {
      if (region->free() == ShenandoahHeapRegion::RegionSizeBytes) {
        assert(! region->is_humongous(), "don't reuse occupied humongous regions");
        free = true;
      }
    }
    if (! free) {
      // Not contiguous, reset search
      free_regions_index = 0;
      continue;
    }
    assert(free_regions_index < num_free_regions, "array bounds");
    free_regions[free_regions_index] = region;
    free_regions_index++;

    if (free_regions_index == num_free_regions) {
      if (ShenandoahTraceHumongous) {
        gclog_or_tty->print_cr("found "UINT32_FORMAT" contiguous free regions:", num_free_regions);
        for (uint i = 0; i < num_free_regions; i++) {
          gclog_or_tty->print(UINT32_FORMAT": " , i);
          free_regions[i]->print_on(gclog_or_tty);
        }
      }
      return true;
    }

  }
  if (ShenandoahTraceHumongous) {
    gclog_or_tty->print_cr("failed to find "UINT32_FORMAT" free regions", num_free_regions);
  }
  return false;
}

bool ShenandoahHeap::allocate_contiguous_free_regions(uint num_free_regions, ShenandoahHeapRegion** free_regions) {
  // We need to be smart here to avoid interleaved allocation of regions when concurrently
  // allocating for large objects. We get the new index into regions array using CAS, where can
  // subsequently safely allocate new regions.
  int new_regions_index = ensure_new_regions(num_free_regions);
  if (new_regions_index == -1) {
    return false;
  }

  int last_new_region = new_regions_index + num_free_regions;

  // Now we can allocate new regions at the found index without being scared that
  // other threads allocate in the same contiguous region.
  if (ShenandoahGCVerbose) {
    tty->print_cr("allocate contiguous regions:");
  }
  for (int i = new_regions_index; i < last_new_region; i++) {
    ShenandoahHeapRegion* region = new ShenandoahHeapRegion();
    HeapWord* start = _first_region_bottom + (ShenandoahHeapRegion::RegionSizeBytes / HeapWordSize) * i;
    region->initialize_heap_region(start, ShenandoahHeapRegion::RegionSizeBytes / HeapWordSize, i);
    _ordered_regions[i] = region;
    uint index = i - new_regions_index;
    assert(index < num_free_regions, "array bounds");
    free_regions[index] = region;

    if (ShenandoahGCVerbose) {
      region->print();
    }
  }
  return true;
}

HeapWord* ShenandoahHeap::mem_allocate_locked(size_t size,
                                              bool* gc_overhead_limit_was_exceeded) {

  // This was used for allocation while holding the Heap_lock.
  // HeapWord* filler = allocate_memory(BrooksPointer::BROOKS_POINTER_OBJ_SIZE + size);

  HeapWord* filler = allocate_memory(BrooksPointer::BROOKS_POINTER_OBJ_SIZE + size);
  HeapWord* result = filler + BrooksPointer::BROOKS_POINTER_OBJ_SIZE;
  if (filler != NULL) {
    initialize_brooks_ptr(filler, result);
    _bytesAllocSinceCM += size * HeapWordSize;
#ifdef ASSERT
    if (ShenandoahTraceAllocations) {
      if (*gc_overhead_limit_was_exceeded)
        tty->print("gc_overhead_limit_was_exceeded");
      tty->print_cr("mem_allocate_locked object of size "SIZE_FORMAT" uat addr "PTR_FORMAT, size, p2i(result));
    }
#endif

    assert(! heap_region_containing(result)->is_in_collection_set(), "never allocate in targetted region");
    return result;
  } else {
    tty->print_cr("Out of memory. Requested number of words: "SIZE_FORMAT" used heap: "INT64_FORMAT", bytes allocated since last CM: "INT64_FORMAT, size, used(), _bytesAllocSinceCM);
    {
      MutexLockerEx ml(ShenandoahHeap_lock, true);
      print_heap_regions();
      tty->print("Printing "SIZE_FORMAT" free regions:\n", _free_regions->length());
      _free_regions->print();
    }
    assert(false, "Out of memory");
    return NULL;
  }
}

class PrintOopContents: public OopClosure {
public:
  void do_oop(oop* o) {
    oop obj = *o;
    tty->print_cr("References oop "PTR_FORMAT, p2i((HeapWord*) obj));
    obj->print();
  }

  void do_oop(narrowOop* o) {
    assert(false, "narrowOops aren't implemented");
  }
};

HeapWord*  ShenandoahHeap::mem_allocate(size_t size,
                                        bool*  gc_overhead_limit_was_exceeded) {

#ifdef ASSERT
  if (ShenandoahVerify && _numAllocs > 1000000) {
    _numAllocs = 0;
  //   VM_ShenandoahVerifyHeap op(0, 0, GCCause::_allocation_failure);
  //   if (Thread::current()->is_VM_thread()) {
  //     op.doit();
  //   } else {
  //     // ...and get the VM thread to execute it.
  //     VMThread::execute(&op);
  //   }
  }
  _numAllocs++;
#endif

  // MutexLockerEx ml(ShenandoahHeap_lock, true);
  HeapWord* result = mem_allocate_locked(size, gc_overhead_limit_was_exceeded);
  return result;
}

class ParallelEvacuateRegionObjectClosure : public ObjectClosure {
private:
  ShenandoahHeap* _heap;
  Thread* _thread;
  public:
  ParallelEvacuateRegionObjectClosure(ShenandoahHeap* heap) :
    _heap(heap), _thread(Thread::current()) {
  }

  void do_object(oop p) {

#ifdef ASSERT
    if (ShenandoahTraceEvacuations) {
      tty->print_cr("Calling ParallelEvacuateRegionObjectClosure on "PTR_FORMAT, p2i((HeapWord*) p));
    }
#endif

    assert(_heap->is_marked_current(p), "expect only marked objects");
    if (p == ShenandoahBarrierSet::resolve_oop_static_not_null(p)) {
      _heap->evacuate_object(p, _thread);
    }
  }
};

//fixme
void ShenandoahHeap::initialize_brooks_ptr(HeapWord* filler, HeapWord* obj, bool new_obj) {
  BrooksPointer brooks_ptr = BrooksPointer::get(oop(obj));
  brooks_ptr.set_forwardee(oop(obj));
}

void ShenandoahHeap::initialize_brooks_ptr(oop p) {
  BrooksPointer brooks_ptr = BrooksPointer::get(p);
  brooks_ptr.set_forwardee(p);
}

class VerifyEvacuatedObjectClosure : public ObjectClosure {

public:

  void do_object(oop p) {
    if (ShenandoahHeap::heap()->is_marked_current(p)) {
      oop p_prime = oopDesc::bs()->read_barrier(p);
      assert(p != p_prime, "Should point to evacuated copy");
#ifdef ASSERT
      if (p->klass() != p_prime->klass()) {
        tty->print_cr("copy has different class than original:");
        p->klass()->print_on(tty);
        p_prime->klass()->print_on(tty);
      }
#endif
      assert(p->klass() == p_prime->klass(), err_msg("Should have the same class p: "PTR_FORMAT", p_prime: "PTR_FORMAT, p2i((HeapWord*) p), p2i((HeapWord*) p_prime)));
      assert(p->size() == p_prime->size(), "Should be the same size");
      assert(p_prime == oopDesc::bs()->read_barrier(p_prime), "One forward once");
    }
  }
};

void ShenandoahHeap::verify_evacuated_region(ShenandoahHeapRegion* from_region) {
  if (ShenandoahGCVerbose) {
    tty->print("Verifying From Region\n");
    from_region->print();
  }

  VerifyEvacuatedObjectClosure verify_evacuation;
  from_region->object_iterate_interruptible(&verify_evacuation, false);
}

void ShenandoahHeap::parallel_evacuate_region(ShenandoahHeapRegion* from_region) {

  assert(from_region->getLiveData() > 0, "all-garbage regions are reclaimed earlier");

  ParallelEvacuateRegionObjectClosure evacuate_region(this);

#ifdef ASSERT
  if (ShenandoahGCVerbose) {
    tty->print_cr("parallel_evacuate_region starting from_region "INT32_FORMAT": free_regions = "SIZE_FORMAT,  from_region->region_number(), _free_regions->available_regions());
  }
#endif

  marked_object_iterate(from_region, &evacuate_region);

#ifdef ASSERT
  if (ShenandoahVerify && ! cancelled_concgc()) {
    verify_evacuated_region(from_region);
  }
  if (ShenandoahGCVerbose) {
    tty->print_cr("parallel_evacuate_region after from_region = "INT32_FORMAT": free_regions = "SIZE_FORMAT, from_region->region_number(), _free_regions->available_regions());
  }
#endif
}

class ParallelEvacuationTask : public AbstractGangTask {
private:
  ShenandoahHeap* _sh;
  ShenandoahHeapRegionSet* _cs;

public:
  ParallelEvacuationTask(ShenandoahHeap* sh,
                         ShenandoahHeapRegionSet* cs) :
    AbstractGangTask("Parallel Evacuation Task"),
    _cs(cs),
    _sh(sh) {}

  void work(uint worker_id) {

    ShenandoahHeapRegion* from_hr = _cs->claim_next();

    while (from_hr != NULL) {
      if (ShenandoahGCVerbose) {
        tty->print_cr("Thread "INT32_FORMAT" claimed Heap Region "INT32_FORMAT,
                   worker_id,
                   from_hr->region_number());
        from_hr->print();
      }

      assert(from_hr->getLiveData() > 0, "all-garbage regions are reclaimed early");
      _sh->parallel_evacuate_region(from_hr);

      if (_sh->cancelled_concgc()) {
        if (ShenandoahTracePhases) {
          tty->print_cr("Cancelled concurrent evacuation");
        }
        break;
      }
      from_hr = _cs->claim_next();
    }

    Thread::current()->gclab().make_parsable(true);
  }
};

class RecycleDirtyRegionsClosure: public ShenandoahHeapRegionClosure {
private:
  ShenandoahHeap* _heap;
  size_t _bytes_reclaimed;
public:
  RecycleDirtyRegionsClosure() : _heap(ShenandoahHeap::heap()) {}

  bool doHeapRegion(ShenandoahHeapRegion* r) {

    // If evacuation has been cancelled, we can't recycle regions, we only
    // clear their collection-set status.
    if (_heap->cancelled_concgc()) {
      r->set_is_in_collection_set(false);
      return false;
    }

    if (r->is_in_collection_set()) {
      //      tty->print_cr("recycling region "INT32_FORMAT":", r->region_number());
      //      r->print_on(tty);
      //      tty->print_cr(" ");
      _heap->decrease_used(r->used());
      _bytes_reclaimed += r->used();
      r->recycle();
      _heap->free_regions()->append(r);
    }

    return false;
  }
  size_t bytes_reclaimed() { return _bytes_reclaimed;}
  void clear_bytes_reclaimed() {_bytes_reclaimed = 0;}
};

void ShenandoahHeap::recycle_dirty_regions() {
  RecycleDirtyRegionsClosure cl;
  cl.clear_bytes_reclaimed();

  heap_region_iterate(&cl);

  _shenandoah_policy->record_bytes_reclaimed(cl.bytes_reclaimed());
  clear_cset_fast_test();
}

ShenandoahHeapRegionSet* ShenandoahHeap::free_regions() {
  return _free_regions;
}

void ShenandoahHeap::print_heap_regions(outputStream* st) const {
  PrintHeapRegionsClosure pc1(st);
  heap_region_iterate(&pc1);
}

class PrintAllRefsOopClosure: public ExtendedOopClosure {
private:
  int _index;
  const char* _prefix;

public:
  PrintAllRefsOopClosure(const char* prefix) : _index(0), _prefix(prefix) {}

  void do_oop(oop* p)       {
    oop o = *p;
    if (o != NULL) {
      if (ShenandoahHeap::heap()->is_in(o) && o->is_oop()) {
        tty->print_cr("%s "INT32_FORMAT" ("PTR_FORMAT")-> "PTR_FORMAT" (marked: %s) (%s "PTR_FORMAT")", _prefix, _index, p2i(p), p2i((HeapWord*) o), BOOL_TO_STR(ShenandoahHeap::heap()->is_marked_current(o)), o->klass()->internal_name(), p2i(o->klass()));
      } else {
        //        tty->print_cr("%s "INT32_FORMAT" ("PTR_FORMAT" dirty: %s) -> "PTR_FORMAT" (not in heap, possibly corrupted or dirty (%s))", _prefix, _index, p2i(p), BOOL_TO_STR(ShenandoahHeap::heap()->heap_region_containing(p)->is_in_collection_set()), p2i((HeapWord*) o), BOOL_TO_STR(ShenandoahHeap::heap()->heap_region_containing(o)->is_in_collection_set()));
        tty->print_cr("%s "INT32_FORMAT" ("PTR_FORMAT" dirty -> "PTR_FORMAT" (not in heap, possibly corrupted or dirty)", _prefix, _index, p2i(p), p2i((HeapWord*) o));
      }
    } else {
      tty->print_cr("%s "INT32_FORMAT" ("PTR_FORMAT") -> "PTR_FORMAT, _prefix, _index, p2i(p), p2i((HeapWord*) o));
    }
    _index++;
  }

  void do_oop(narrowOop* p) {
    Unimplemented();
  }

};

class PrintAllRefsObjectClosure : public ObjectClosure {
  const char* _prefix;

public:
  PrintAllRefsObjectClosure(const char* prefix) : _prefix(prefix) {}

  void do_object(oop p) {
    if (ShenandoahHeap::heap()->is_in(p)) {
        tty->print_cr("%s object "PTR_FORMAT" (marked: %s) (%s "PTR_FORMAT") refers to:", _prefix, p2i((HeapWord*) p), BOOL_TO_STR(ShenandoahHeap::heap()->is_marked_current(p)), p->klass()->internal_name(), p2i(p->klass()));
        PrintAllRefsOopClosure cl(_prefix);
        p->oop_iterate(&cl);
      }
  }
};

void ShenandoahHeap::print_all_refs(const char* prefix) {
  tty->print_cr("printing all references in the heap");
  tty->print_cr("root references:");

  ensure_parsability(false);

  PrintAllRefsOopClosure cl(prefix);
  roots_iterate(&cl);

  tty->print_cr("heap references:");
  PrintAllRefsObjectClosure cl2(prefix);
  object_iterate(&cl2);
}

class VerifyAfterMarkingOopClosure: public ExtendedOopClosure {
private:
  ShenandoahHeap*  _heap;

public:
  VerifyAfterMarkingOopClosure() :
    _heap(ShenandoahHeap::heap()) { }

  void do_oop(oop* p)       {
    oop o = *p;
    if (o != NULL) {
      if (! _heap->is_marked_current(o)) {
        _heap->print_heap_regions();
        _heap->print_all_refs("post-mark");
        tty->print_cr("oop not marked, although referrer is marked: "PTR_FORMAT": in_heap: %s, is_marked: %s",
                      p2i((HeapWord*) o), BOOL_TO_STR(_heap->is_in(o)), BOOL_TO_STR(_heap->is_marked_current(o)));
        _heap->print_heap_locations((HeapWord*) o, (HeapWord*) o + o->size());

        tty->print_cr("oop class: %s", o->klass()->internal_name());
        if (_heap->is_in(p)) {
          oop referrer = oop(_heap->heap_region_containing(p)->block_start_const(p));
          tty->print_cr("Referrer starts at addr "PTR_FORMAT, p2i((HeapWord*) referrer));
          referrer->print();
          _heap->print_heap_locations((HeapWord*) referrer, (HeapWord*) referrer + referrer->size());
        }
        tty->print_cr("heap region containing object:");
        _heap->heap_region_containing(o)->print();
        tty->print_cr("heap region containing referrer:");
        _heap->heap_region_containing(p)->print();
        tty->print_cr("heap region containing forwardee:");
        _heap->heap_region_containing(oopDesc::bs()->read_barrier(o))->print();
      }
      assert(o->is_oop(), "oop must be an oop");
      assert(Metaspace::contains(o->klass()), "klass pointer must go to metaspace");
      if (! (o == oopDesc::bs()->read_barrier(o))) {
        tty->print_cr("oops has forwardee: p: "PTR_FORMAT" (%s), o = "PTR_FORMAT" (%s), new-o: "PTR_FORMAT" (%s)", p2i(p), BOOL_TO_STR(_heap->heap_region_containing(p)->is_in_collection_set()), p2i((HeapWord*) o),  BOOL_TO_STR(_heap->heap_region_containing(o)->is_in_collection_set()), p2i((HeapWord*) oopDesc::bs()->read_barrier(o)), BOOL_TO_STR(_heap->heap_region_containing(oopDesc::bs()->read_barrier(o))->is_in_collection_set()));
        tty->print_cr("oop class: %s", o->klass()->internal_name());
      }
      assert(o == oopDesc::bs()->read_barrier(o), "oops must not be forwarded");
      assert(! _heap->heap_region_containing(o)->is_in_collection_set(), "references must not point to dirty heap regions");
      assert(_heap->is_marked_current(o), "live oops must be marked current");
    }
  }

  void do_oop(narrowOop* p) {
    Unimplemented();
  }

};

class IterateMarkedCurrentObjectsClosure: public ObjectClosure {
private:
  ShenandoahHeap* _heap;
  ExtendedOopClosure* _cl;
public:
  IterateMarkedCurrentObjectsClosure(ExtendedOopClosure* cl) :
    _heap(ShenandoahHeap::heap()), _cl(cl) {};

  void do_object(oop p) {
    if (_heap->is_marked_current(p)) {
      p->oop_iterate(_cl);
    }
  }

};

void ShenandoahHeap::verify_heap_after_marking() {

  verify_heap_size_consistency();

  if (ShenandoahGCVerbose) {
    tty->print("verifying heap after marking\n");
  }
  ensure_parsability(false);
  VerifyAfterMarkingOopClosure cl;
  roots_iterate(&cl);

  IterateMarkedCurrentObjectsClosure marked_oops(&cl);
  object_iterate(&marked_oops);
}

void ShenandoahHeap::prepare_for_concurrent_evacuation() {
  if (!cancelled_concgc()) {

    recycle_dirty_regions();

      ensure_parsability(true);

      // NOTE: This needs to be done during a stop the world pause, because
      // putting regions into the collection set concurrently with Java threads
      // will create a race. In particular, acmp could fail because when we
      // resolve the first operand, the containing region might not yet be in
      // the collection set, and thus return the original oop. When the 2nd
      // operand gets resolved, the region could be in the collection set
      // and the oop gets evacuated. If both operands have originally been
      // the same, we get false negatives.
      ShenandoahHeapRegionSet regions = ShenandoahHeapRegionSet(_num_regions, _ordered_regions, _num_regions);
      regions.reclaim_humongous_regions();
      _collection_set->clear();
      _free_regions->clear();
      _shenandoah_policy->choose_collection_and_free_sets(&regions, _collection_set, _free_regions);

      if (PrintGCTimeStamps) {
        gclog_or_tty->print("Collection set used = " SIZE_FORMAT " K live = " SIZE_FORMAT " K reclaimable = " SIZE_FORMAT " K\n",
                            _collection_set->used() / K, _collection_set->live_data() / K, _collection_set->garbage() / K);
      }

      if (_collection_set->length() == 0)
        cancel_concgc();

      _bytesAllocSinceCM = 0;

      Universe::update_heap_info_at_gc();
    }
}


class ShenandoahUpdateRootsClosure: public ExtendedOopClosure {

  void do_oop(oop* p)       {
    ShenandoahHeap::heap()->maybe_update_oop_ref(p);
  }

  void do_oop(narrowOop* p) {
    Unimplemented();
  }
};

void ShenandoahHeap::update_roots() {

  COMPILER2_PRESENT(DerivedPointerTable::clear());

  assert(SafepointSynchronize::is_at_safepoint(), "Only iterate roots while world is stopped");

  ShenandoahUpdateRootsClosure cl;
  CodeBlobToOopClosure blobsCl(&cl, false);
  CLDToOopClosure cldCl(&cl);

  ClassLoaderDataGraph::clear_claimed_marks();

  {
    ShenandoahRootProcessor rp(this, 1);
    rp.process_all_roots(&cl, &cldCl, &blobsCl);
    ShenandoahIsAliveClosure is_alive;
    JNIHandles::weak_oops_do(&is_alive, &cl);
  }

  COMPILER2_PRESENT(DerivedPointerTable::update_pointers());
}

class ShenandoahUpdateObjectsClosure : public ObjectClosure {
  ShenandoahHeap* _heap;

public:
  ShenandoahUpdateObjectsClosure() :
    _heap(ShenandoahHeap::heap()) {
  }

  void do_object(oop p) {
    ShenandoahUpdateRootsClosure refs_cl;
    assert(ShenandoahHeap::heap()->is_in(p), "only update objects in heap (where else?)");

    if (_heap->is_marked_current(p)) {
      p->oop_iterate(&refs_cl);
    }
  }

};

class ParallelUpdateRefsTask : public AbstractGangTask {
private:
  ShenandoahHeapRegionSet* _regions;

public:
  ParallelUpdateRefsTask(ShenandoahHeapRegionSet* regions) :
    AbstractGangTask("Parallel Update References Task"),
  _regions(regions) {
  }

  void work(uint worker_id) {
    ShenandoahUpdateObjectsClosure update_refs_cl;
    ShenandoahHeapRegion* region = _regions->claim_next();
    ShenandoahHeap* heap = ShenandoahHeap::heap();
    while (region != NULL && ! heap->cancelled_concgc()) {
      if ((! region->is_in_collection_set()) && ! region->is_humongous_continuation()) {
        heap->marked_object_iterate_careful(region, &update_refs_cl);
      }
      heap->reset_mark_bitmap_range(region->bottom(), region->end());
      region = _regions->claim_next();
    }
    if (ShenandoahTracePhases && heap->cancelled_concgc()) {
      tty->print_cr("Cancelled concurrent update references");
    }
  }
};

class RetireTLABClosure : public ThreadClosure {
private:
  bool _retire;

public:
  RetireTLABClosure(bool retire) : _retire(retire) {
  }

  void do_thread(Thread* thread) {
    thread->gclab().make_parsable(_retire);
  }
};

void ShenandoahHeap::ensure_parsability(bool retire_tlabs) {
  if (UseTLAB) {
  CollectedHeap::ensure_parsability(retire_tlabs);

  RetireTLABClosure cl(retire_tlabs);
  for (JavaThread *thread = Threads::first(); thread != NULL; thread = thread->next()) {
    cl.do_thread(thread);
  }
  gc_threads_do(&cl);
  }
}

void ShenandoahHeap::prepare_for_update_references() {
  ensure_parsability(true);

  ShenandoahHeapRegionSet regions = ShenandoahHeapRegionSet(_num_regions, _ordered_regions, _num_regions);
  regions.set_concurrent_iteration_safe_limits();

  if (ShenandoahVerifyReadsToFromSpace) {
    set_from_region_protection(false);

    // We need to update the roots so that they are ok for C2 when returning from the safepoint.
    update_roots();

    set_from_region_protection(true);

  } else {
    // We need to update the roots so that they are ok for C2 when returning from the safepoint.
    update_roots();
  }

  set_update_references_in_progress(true);
}

void ShenandoahHeap::update_references() {

  ShenandoahHeapRegionSet regions = ShenandoahHeapRegionSet(_num_regions, _ordered_regions, _num_regions);
  ParallelUpdateRefsTask task = ParallelUpdateRefsTask(&regions);
  set_par_threads(_max_conc_workers);
  conc_workers()->set_active_workers(_max_conc_workers);
  _shenandoah_policy->record_phase_start(ShenandoahCollectorPolicy::conc_uprefs);
  conc_workers()->run_task(&task);
  _shenandoah_policy->record_phase_end(ShenandoahCollectorPolicy::conc_uprefs);
  conc_workers()->set_active_workers(_max_conc_workers);
  set_par_threads(0);

  if (! cancelled_concgc()) {
    VM_ShenandoahUpdateRootRefs update_roots;
    if (ShenandoahConcurrentUpdateRefs) {
      VMThread::execute(&update_roots);
    } else {
      update_roots.doit();
    }

    _allocated_last_gc = used() - _used_start_gc;
    size_t max_allocated_gc = MAX2(_max_allocated_gc, _allocated_last_gc);
    /*
      tty->print_cr("prev max_allocated_gc: "SIZE_FORMAT", new max_allocated_gc: "SIZE_FORMAT", allocated_last_gc: "SIZE_FORMAT" diff %f", _max_allocated_gc, max_allocated_gc, _allocated_last_gc, ((double) max_allocated_gc/ (double) _allocated_last_gc));
    */
    _max_allocated_gc = max_allocated_gc;

    // Update-references completed, no need to update-refs during marking.
    set_need_update_refs(false);
  }

  Universe::update_heap_info_at_gc();

  set_update_references_in_progress(false);
}


class ShenandoahEvacuateUpdateRootsClosure: public ExtendedOopClosure {
private:
  ShenandoahHeap* _heap;
  Thread* _thread;
public:
  ShenandoahEvacuateUpdateRootsClosure() :
    _heap(ShenandoahHeap::heap()), _thread(Thread::current()) {
  }

  void do_oop(oop* p) {
    assert(_heap->is_evacuation_in_progress(), "Only do this when evacuation is in progress");

    oop obj = oopDesc::load_heap_oop(p);
    if (obj != NULL && _heap->in_cset_fast_test((HeapWord*) obj)) {
      assert(_heap->is_marked_current(obj), err_msg("only evacuate marked objects %d %d", _heap->is_marked_current(obj), _heap->is_marked_current(ShenandoahBarrierSet::resolve_oop_static_not_null(obj))));
      oop resolved = ShenandoahBarrierSet::resolve_oop_static_not_null(obj);
      if (resolved == obj) {
        resolved = _heap->evacuate_object(obj, _thread);
      }
      oopDesc::store_heap_oop(p, resolved);
    }
#ifdef ASSERT
    else if (! oopDesc::is_null(obj)) {
      // tty->print_cr("not updating root at: "PTR_FORMAT" with object: "PTR_FORMAT", is_in_heap: %s, is_in_cset: %s, is_marked: %s", p2i(p), p2i((HeapWord*) obj), BOOL_TO_STR(_heap->is_in(obj)), BOOL_TO_STR(_heap->in_cset_fast_test(obj)), BOOL_TO_STR(_heap->is_marked_current(obj)));
    }
#endif
  }

  void do_oop(narrowOop* p) {
    Unimplemented();
  }
};

class ShenandoahEvacuateUpdateStrongRootsTask : public AbstractGangTask {
  ShenandoahRootProcessor* _rp;
public:

  ShenandoahEvacuateUpdateStrongRootsTask(ShenandoahRootProcessor* rp) :
    AbstractGangTask("Shenandoah evacuate and update strong roots"),
    _rp(rp)
  {
    // Nothing else to do.
  }

  void work(uint worker_id) {
    ShenandoahEvacuateUpdateRootsClosure cl;
    CodeBlobToOopClosure blobsCl(&cl, false);
    CLDToOopClosure cldCl(&cl);

    _rp->process_all_roots(&cl, &cldCl, &blobsCl);
  }
};

class ShenandoahEvacuateUpdateWeakRootsTask : public AbstractGangTask {
public:

  ShenandoahEvacuateUpdateWeakRootsTask() : AbstractGangTask("Shenandoah evacuate and update weak roots") {
    // Nothing else to do.
  }

  void work(uint worker_id) {
    ShenandoahEvacuateUpdateRootsClosure cl;
    ShenandoahIsAliveClosure is_alive;
    JNIHandles::weak_oops_do(&is_alive, &cl);

    ShenandoahHeap* heap = ShenandoahHeap::heap();
    if (ShenandoahProcessReferences) {
      heap->ref_processor()->weak_oops_do(&cl);
    }
  }
};

void ShenandoahHeap::evacuate_and_update_roots() {

  COMPILER2_PRESENT(DerivedPointerTable::clear());

  if (ShenandoahVerifyReadsToFromSpace) {
    set_from_region_protection(false);
  }

  assert(SafepointSynchronize::is_at_safepoint(), "Only iterate roots while world is stopped");
  ClassLoaderDataGraph::clear_claimed_marks();

  {
    set_par_threads(_max_parallel_workers);
    ShenandoahRootProcessor rp(this, _max_parallel_workers);
    ShenandoahEvacuateUpdateStrongRootsTask strong_roots_task(&rp);
    workers()->set_active_workers(_max_parallel_workers);
    workers()->run_task(&strong_roots_task);
    set_par_threads(0);
  }

  // We process weak roots using only 1 worker thread, multi-threaded weak roots
  // processing is not implemented yet. We can't use the VMThread itself, because
  // we need to grab the Heap_lock.
  {
    ShenandoahEvacuateUpdateWeakRootsTask weak_roots_task;
    set_par_threads(1);
    workers()->set_active_workers(1);
    workers()->run_task(&weak_roots_task);
    workers()->set_active_workers(_max_parallel_workers);
    set_par_threads(0);
  }

  if (ShenandoahVerifyReadsToFromSpace) {
    set_from_region_protection(true);
  }

  COMPILER2_PRESENT(DerivedPointerTable::update_pointers());

}


void ShenandoahHeap::do_evacuation() {
  assert(Thread::current()->is_VM_thread() || ShenandoahConcurrentEvacuation, "Only evacuate from VMThread unless we do concurrent evacuation");

  parallel_evacuate();

  if (! ShenandoahConcurrentEvacuation) {
    // We need to make sure that after leaving the safepoint, all
    // GC roots are up-to-date. This is an assumption built into
    // the hotspot compilers, especially C2, that allows it to
    // do optimizations like lifting barriers outside of a loop.

    if (ShenandoahVerifyReadsToFromSpace) {
      set_from_region_protection(false);

      update_roots();

      set_from_region_protection(true);

    } else {
      update_roots();
    }
  }

  if (ShenandoahVerify && ! cancelled_concgc()) {
    VM_ShenandoahVerifyHeapAfterEvacuation verify_after_evacuation;
    if (Thread::current()->is_VM_thread()) {
      verify_after_evacuation.doit();
    } else {
      VMThread::execute(&verify_after_evacuation);
    }
  }

}

void ShenandoahHeap::parallel_evacuate() {

  if (! cancelled_concgc()) {
    assert(Thread::current()->is_VM_thread() || ShenandoahConcurrentEvacuation, "Only evacuate from VMThread unless we do concurrent evacuation");

    if (ShenandoahGCVerbose) {
      tty->print_cr("starting parallel_evacuate");
      //    PrintHeapRegionsClosure pc1;
      //    heap_region_iterate(&pc1);
    }

    _shenandoah_policy->record_phase_start(ShenandoahCollectorPolicy::conc_evac);

    if (ShenandoahGCVerbose) {
      tty->print("Printing all available regions");
      print_heap_regions();
    }

    if (ShenandoahPrintCollectionSet) {
      tty->print("Printing collection set which contains "SIZE_FORMAT" regions:\n", _collection_set->length());
      _collection_set->print();

      tty->print("Printing free set which contains "SIZE_FORMAT" regions:\n", _free_regions->length());
      _free_regions->print();

      //    if (_collection_set->length() == 0)
      //      print_heap_regions();
    }

    ParallelEvacuationTask evacuationTask = ParallelEvacuationTask(this, _collection_set);

    conc_workers()->set_active_workers(_max_conc_workers);
    conc_workers()->run_task(&evacuationTask);
    //workers()->set_active_workers(_max_parallel_workers);

    if (ShenandoahGCVerbose) {

      tty->print("Printing postgc collection set which contains "SIZE_FORMAT" regions:\n", _collection_set->available_regions());
      _collection_set->print();

      tty->print("Printing postgc free regions which contain "SIZE_FORMAT" free regions:\n", _free_regions->available_regions());
      _free_regions->print();

      tty->print_cr("finished parallel_evacuate");
      print_heap_regions();

      tty->print_cr("all regions after evacuation:");
      print_heap_regions();
    }

    _shenandoah_policy->record_phase_end(ShenandoahCollectorPolicy::conc_evac);
  }
}

class VerifyEvacuationClosure: public ExtendedOopClosure {
private:
  ShenandoahHeap*  _heap;
  ShenandoahHeapRegion* _from_region;

public:
  VerifyEvacuationClosure(ShenandoahHeapRegion* from_region) :
    _heap(ShenandoahHeap::heap()), _from_region(from_region) { }

  void do_oop(oop* p)       {
    oop heap_oop = oopDesc::load_heap_oop(p);
    if (! oopDesc::is_null(heap_oop)) {
      guarantee(! _from_region->is_in(heap_oop), err_msg("no references to from-region allowed after evacuation: "PTR_FORMAT, p2i((HeapWord*) heap_oop)));
    }
  }

  void do_oop(narrowOop* p) {
    Unimplemented();
  }

};

void ShenandoahHeap::roots_iterate(OopClosure* cl) {

  assert(SafepointSynchronize::is_at_safepoint(), "Only iterate roots while world is stopped");

  CodeBlobToOopClosure blobsCl(cl, false);
  CLDToOopClosure cldCl(cl);

  ClassLoaderDataGraph::clear_claimed_marks();

  ShenandoahRootProcessor rp(this, 1);
  rp.process_all_roots(cl, &cldCl, &blobsCl);
}

void ShenandoahHeap::weak_roots_iterate(OopClosure* cl) {
  if (ShenandoahProcessReferences) {
    ref_processor()->weak_oops_do(cl);
  }
  ShenandoahAlwaysTrueClosure always_true;
  JNIHandles::weak_oops_do(&always_true, cl);
}

void ShenandoahHeap::verify_evacuation(ShenandoahHeapRegion* from_region) {

  VerifyEvacuationClosure rootsCl(from_region);
  roots_iterate(&rootsCl);

}

bool ShenandoahHeap::supports_tlab_allocation() const {
  return true;
}


size_t  ShenandoahHeap::unsafe_max_tlab_alloc(Thread *thread) const {
  ShenandoahHeapRegion* current = get_current_region_skip_humongous();
  if (current == NULL)
    return 0;
  else if (current->free() > MinTLABSize) {
    return current->free();
  } else {
    return MinTLABSize;
  }
}

size_t ShenandoahHeap::max_tlab_size() const {
  return ShenandoahHeapRegion::RegionSizeBytes;
}

class ResizeGCLABClosure : public ThreadClosure {
public:
  void do_thread(Thread* thread) {
    thread->gclab().resize();
  }
};

void ShenandoahHeap::resize_all_tlabs() {
  CollectedHeap::resize_all_tlabs();

  if (PrintTLAB && Verbose) {
    tty->print_cr("Resizing Shenandoah GCLABs...");
  }

  ResizeGCLABClosure cl;
  for (JavaThread *thread = Threads::first(); thread != NULL; thread = thread->next()) {
    cl.do_thread(thread);
  }
  gc_threads_do(&cl);

  if (PrintTLAB && Verbose) {
    tty->print_cr("Done resizing Shenandoah GCLABs...");
  }
}

class AccumulateStatisticsGCLABClosure : public ThreadClosure {
public:
  void do_thread(Thread* thread) {
    thread->gclab().accumulate_statistics();
    thread->gclab().initialize_statistics();
  }
};

void ShenandoahHeap::accumulate_statistics_all_gclabs() {

  AccumulateStatisticsGCLABClosure cl;
  for (JavaThread *thread = Threads::first(); thread != NULL; thread = thread->next()) {
    cl.do_thread(thread);
  }
  gc_threads_do(&cl);
}

bool  ShenandoahHeap::can_elide_tlab_store_barriers() const {
  return true;
}

oop ShenandoahHeap::new_store_pre_barrier(JavaThread* thread, oop new_obj) {
  // Overridden to do nothing.
  return new_obj;
}

bool  ShenandoahHeap::can_elide_initializing_store_barrier(oop new_obj) {
  return true;
}

bool ShenandoahHeap::card_mark_must_follow_store() const {
  return false;
}

bool ShenandoahHeap::supports_heap_inspection() const {
  return false;
}

size_t ShenandoahHeap::unsafe_max_alloc() {
  return ShenandoahHeapRegion::RegionSizeBytes / HeapWordSize;
}

void ShenandoahHeap::collect(GCCause::Cause cause) {
  if (GCCause::is_user_requested_gc(cause)) {
    if (! DisableExplicitGC) {
      if (ShenandoahTraceFullGC) {
        gclog_or_tty->print_cr("Shenandoah-full-gc: requested full GC");
      }
      cancel_concgc();
      _concurrent_gc_thread->do_full_gc(cause);
    }
  } else if (cause == GCCause::_allocation_failure) {

    if (ShenandoahTraceFullGC) {
      size_t f_used = free_regions()->used();
      size_t f_capacity = free_regions()->capacity();
      assert(f_used <= f_capacity, "must use less than we have");
      gclog_or_tty->print_cr("Shenandoah-full-gc: full GC for allocation failure heap free: "SIZE_FORMAT", available: "SIZE_FORMAT, capacity() - used(),  f_capacity - f_used);
    }
    cancel_concgc();
    collector_policy()->set_should_clear_all_soft_refs(true);
      _concurrent_gc_thread->do_full_gc(cause);

  } else if (cause == GCCause::_gc_locker) {

    if (ShenandoahTraceJNICritical) {
      gclog_or_tty->print_cr("Resuming deferred evacuation after JNI critical regions");
    }

    jni_critical()->notify_jni_critical();
  }
}

void ShenandoahHeap::do_full_collection(bool clear_all_soft_refs) {
  //assert(false, "Shouldn't need to do full collections");
}

AdaptiveSizePolicy* ShenandoahHeap::size_policy() {
  Unimplemented();
  return NULL;

}

ShenandoahCollectorPolicy* ShenandoahHeap::collector_policy() const {
  return _shenandoah_policy;
}


HeapWord* ShenandoahHeap::block_start(const void* addr) const {
  Space* sp = space_containing(addr);
  if (sp != NULL) {
    return sp->block_start(addr);
  }
  return NULL;
}

size_t ShenandoahHeap::block_size(const HeapWord* addr) const {
  Space* sp = space_containing(addr);
  assert(sp != NULL, "block_size of address outside of heap");
  return sp->block_size(addr);
}

bool ShenandoahHeap::block_is_obj(const HeapWord* addr) const {
  Space* sp = space_containing(addr);
  return sp->block_is_obj(addr);
}

jlong ShenandoahHeap::millis_since_last_gc() {
  return 0;
}

void ShenandoahHeap::prepare_for_verify() {
  if (SafepointSynchronize::is_at_safepoint() || ! UseTLAB) {
    ensure_parsability(false);
  }
}

void ShenandoahHeap::print_gc_threads_on(outputStream* st) const {
  workers()->print_worker_threads_on(st);
  conc_workers()->print_worker_threads_on(st);
}

void ShenandoahHeap::gc_threads_do(ThreadClosure* tcl) const {
  workers()->threads_do(tcl);
  conc_workers()->threads_do(tcl);
}

void ShenandoahHeap::print_tracing_info() const {
  if (PrintGCDetails) {
    _shenandoah_policy->print_tracing_info();
  }
}

class ShenandoahVerifyRootsClosure: public ExtendedOopClosure {
private:
  ShenandoahHeap*  _heap;
  VerifyOption     _vo;
  bool             _failures;
public:
  // _vo == UsePrevMarking -> use "prev" marking information,
  // _vo == UseNextMarking -> use "next" marking information,
  // _vo == UseMarkWord    -> use mark word from object header.
  ShenandoahVerifyRootsClosure(VerifyOption vo) :
    _heap(ShenandoahHeap::heap()),
    _vo(vo),
    _failures(false) { }

  bool failures() { return _failures; }

  void do_oop(oop* p)       {
    if (*p != NULL) {
      oop heap_oop = oopDesc::load_heap_oop(p);
      oop obj = oopDesc::decode_heap_oop_not_null(heap_oop);
      if (!obj->is_oop()) {
        { // Just for debugging.
          gclog_or_tty->print_cr("Root location "PTR_FORMAT
                                 "verified "PTR_FORMAT, p2i(p), p2i((void*) obj));
          //      obj->print_on(gclog_or_tty);
        }
      }
      guarantee(obj->is_oop(), "is_oop");
    }
  }

  void do_oop(narrowOop* p) {
    Unimplemented();
  }

};

class ShenandoahVerifyHeapClosure: public ObjectClosure {
private:
  ShenandoahVerifyRootsClosure _rootsCl;
public:
  ShenandoahVerifyHeapClosure(ShenandoahVerifyRootsClosure rc) :
    _rootsCl(rc) {};

  void do_object(oop p) {
    _rootsCl.do_oop(&p);
  }
};

class ShenandoahVerifyKlassClosure: public KlassClosure {
  OopClosure *_oop_closure;
 public:
  ShenandoahVerifyKlassClosure(OopClosure* cl) : _oop_closure(cl) {}
  void do_klass(Klass* k) {
    k->oops_do(_oop_closure);
  }
};

void ShenandoahHeap::verify(bool silent , VerifyOption vo) {
  if (SafepointSynchronize::is_at_safepoint() || ! UseTLAB) {

    ShenandoahVerifyRootsClosure rootsCl(vo);

    assert(Thread::current()->is_VM_thread(),
           "Expected to be executed serially by the VM thread at this point");

    roots_iterate(&rootsCl);

    bool failures = rootsCl.failures();
    if (ShenandoahGCVerbose)
      gclog_or_tty->print("verify failures: %s", BOOL_TO_STR(failures));

    ShenandoahVerifyHeapClosure heapCl(rootsCl);

    object_iterate(&heapCl);
    // TODO: Implement rest of it.
#ifdef ASSERT_DISABLED
    verify_live();
#endif
  } else {
    if (!silent) gclog_or_tty->print("(SKIPPING roots, heapRegions, remset) ");
  }
}
size_t ShenandoahHeap::tlab_capacity(Thread *thr) const {
  return _free_regions->capacity();
}

class ShenandoahIterateObjectClosureRegionClosure: public ShenandoahHeapRegionClosure {
  ObjectClosure* _cl;
public:
  ShenandoahIterateObjectClosureRegionClosure(ObjectClosure* cl) : _cl(cl) {}
  bool doHeapRegion(ShenandoahHeapRegion* r) {
    r->object_iterate_interruptible(_cl, false);
    return false;
  }
};

class ShenandoahIterateObjectClosureCarefulRegionClosure: public ShenandoahHeapRegionClosure {
  ObjectClosureCareful* _cl;
public:
  ShenandoahIterateObjectClosureCarefulRegionClosure(ObjectClosureCareful* cl) : _cl(cl) {}
  bool doHeapRegion(ShenandoahHeapRegion* r) {
    r->object_iterate_careful(_cl);
    return false;
  }
};

void ShenandoahHeap::object_iterate(ObjectClosure* cl) {
  ShenandoahIterateObjectClosureRegionClosure blk(cl);
  heap_region_iterate(&blk, false, true);
}

void ShenandoahHeap::object_iterate_careful(ObjectClosureCareful* cl) {
  ShenandoahIterateObjectClosureCarefulRegionClosure blk(cl);
  heap_region_iterate(&blk, false, true);
}

void ShenandoahHeap::safe_object_iterate(ObjectClosure* cl) {
  Unimplemented();
}

void ShenandoahHeap::marked_object_iterate(ShenandoahHeapRegion* region, ObjectClosure* cl) {
  marked_object_iterate(region, cl, region->bottom(), region->top());
}

void ShenandoahHeap::marked_object_iterate_careful(ShenandoahHeapRegion* region, ObjectClosure* cl) {
  marked_object_iterate(region, cl, region->bottom(), region->concurrent_iteration_safe_limit());
}

void ShenandoahHeap::marked_object_iterate(ShenandoahHeapRegion* region, ObjectClosure* cl,
                                           HeapWord* addr, HeapWord* limit) {
  addr += BrooksPointer::BROOKS_POINTER_OBJ_SIZE;
  HeapWord* last_addr = NULL;
  size_t last_size = 0;
  HeapWord* top_at_mark_start = region->top_at_mark_start();
  while (addr < limit) {
    if (addr < top_at_mark_start) {
      addr = _next_mark_bit_map->getNextMarkedWordAddress(addr, top_at_mark_start + BrooksPointer::BROOKS_POINTER_OBJ_SIZE);
    }
    if (addr < limit) {
      oop obj = oop(addr);
      assert(is_marked_current(obj), "object expected to be marked");
      cl->do_object(obj);
      last_addr = addr;
      last_size = obj->size();
      addr += obj->size() + BrooksPointer::BROOKS_POINTER_OBJ_SIZE;
    } else {
      break;
    }
  }
}

class ShenandoahIterateOopClosureRegionClosure : public ShenandoahHeapRegionClosure {
  MemRegion _mr;
  ExtendedOopClosure* _cl;
  bool _skip_unreachable_objects;
public:
  ShenandoahIterateOopClosureRegionClosure(ExtendedOopClosure* cl, bool skip_unreachable_objects) :
    _cl(cl), _skip_unreachable_objects(skip_unreachable_objects) {}
  ShenandoahIterateOopClosureRegionClosure(MemRegion mr, ExtendedOopClosure* cl)
    :_mr(mr), _cl(cl) {}
  bool doHeapRegion(ShenandoahHeapRegion* r) {
    r->oop_iterate_skip_unreachable(_cl, _skip_unreachable_objects);
    return false;
  }
};

void ShenandoahHeap::oop_iterate(ExtendedOopClosure* cl, bool skip_dirty_regions, bool skip_unreachable_objects) {
  ShenandoahIterateOopClosureRegionClosure blk(cl, skip_unreachable_objects);
  heap_region_iterate(&blk, skip_dirty_regions, true);
}

void ShenandoahHeap::oop_iterate(MemRegion mr,
                                 ExtendedOopClosure* cl) {
  ShenandoahIterateOopClosureRegionClosure blk(mr, cl);
  heap_region_iterate(&blk, false, true);
}

void  ShenandoahHeap::object_iterate_since_last_GC(ObjectClosure* cl) {
  Unimplemented();
}

class SpaceClosureRegionClosure: public ShenandoahHeapRegionClosure {
  SpaceClosure* _cl;
public:
  SpaceClosureRegionClosure(SpaceClosure* cl) : _cl(cl) {}
  bool doHeapRegion(ShenandoahHeapRegion* r) {
    _cl->do_space(r);
    return false;
  }
};

void  ShenandoahHeap::space_iterate(SpaceClosure* cl) {
  SpaceClosureRegionClosure blk(cl);
  heap_region_iterate(&blk);
}

Space*  ShenandoahHeap::space_containing(const void* oop) const {
  Space* res = heap_region_containing(oop);
  return res;
}

void  ShenandoahHeap::gc_prologue(bool b) {
  Unimplemented();
}

void  ShenandoahHeap::gc_epilogue(bool b) {
  Unimplemented();
}

// Apply blk->doHeapRegion() on all committed regions in address order,
// terminating the iteration early if doHeapRegion() returns true.
void ShenandoahHeap::heap_region_iterate(ShenandoahHeapRegionClosure* blk, bool skip_dirty_regions, bool skip_humongous_continuation) const {
  for (size_t i = 0; i < _num_regions; i++) {
    ShenandoahHeapRegion* current  = _ordered_regions[i];
    if (skip_humongous_continuation && current->is_humongous_continuation()) {
      continue;
    }
    if (skip_dirty_regions && current->is_in_collection_set()) {
      continue;
    }
    if (blk->doHeapRegion(current)) {
      return;
    }
  }
}

/**
 * Maybe we need that at some point...
oop* ShenandoahHeap::resolve_oop_ptr(oop* p) {
  if (is_in(p) && heap_region_containing(p)->is_dirty()) {
    // If the reference is in an object in from-space, we need to first
    // find its to-space counterpart.
    // TODO: This here is slow (linear search inside region). Make it faster.
    oop from_space_oop = oop_containing_oop_ptr(p);
    HeapWord* to_space_obj = (HeapWord*) oopDesc::bs()->read_barrier(from_space_oop);
    return (oop*) (to_space_obj + ((HeapWord*) p - ((HeapWord*) from_space_oop)));
  } else {
    return p;
  }
}

oop ShenandoahHeap::oop_containing_oop_ptr(oop* p) {
  HeapWord* from_space_ref = (HeapWord*) p;
  ShenandoahHeapRegion* region = heap_region_containing(from_space_ref);
  HeapWord* from_space_obj = NULL;
  for (HeapWord* curr = region->bottom(); curr < from_space_ref; ) {
    oop curr_obj = (oop) curr;
    if (curr < from_space_ref && from_space_ref < (curr + curr_obj->size())) {
      from_space_obj = curr;
      break;
    } else {
      curr += curr_obj->size();
    }
  }
  assert (from_space_obj != NULL, "must not happen");
  oop from_space_oop = (oop) from_space_obj;
  assert (from_space_oop->is_oop(), "must be oop");
  assert(ShenandoahBarrierSet::is_brooks_ptr(oop(((HeapWord*) from_space_oop) - BrooksPointer::BROOKS_POINTER_OBJ_SIZE)), "oop must have a brooks ptr");
  return from_space_oop;
}
 */

class ClearLivenessClosure : public ShenandoahHeapRegionClosure {
  ShenandoahHeap* sh;
public:
  ClearLivenessClosure(ShenandoahHeap* heap) : sh(heap) { }

  bool doHeapRegion(ShenandoahHeapRegion* r) {
    r->clearLiveData();
    r->init_top_at_mark_start();
    return false;
  }
};


void ShenandoahHeap::start_concurrent_marking() {

  shenandoahPolicy()->record_phase_start(ShenandoahCollectorPolicy::accumulate_stats);
  accumulate_statistics_all_tlabs();
  shenandoahPolicy()->record_phase_end(ShenandoahCollectorPolicy::accumulate_stats);

  set_concurrent_mark_in_progress(true);
  // We need to reset all TLABs because we'd lose marks on all objects allocated in them.
  if (UseTLAB) {
    shenandoahPolicy()->record_phase_start(ShenandoahCollectorPolicy::make_parsable);
    ensure_parsability(true);
    shenandoahPolicy()->record_phase_end(ShenandoahCollectorPolicy::make_parsable);
  }

  _shenandoah_policy->record_bytes_allocated(_bytesAllocSinceCM);
  _used_start_gc = used();

#ifdef ASSERT
  if (ShenandoahDumpHeapBeforeConcurrentMark) {
    ensure_parsability(false);
    print_all_refs("pre-mark");
  }
#endif

  shenandoahPolicy()->record_phase_start(ShenandoahCollectorPolicy::clear_liveness);
  ClearLivenessClosure clc(this);
  heap_region_iterate(&clc);
  shenandoahPolicy()->record_phase_end(ShenandoahCollectorPolicy::clear_liveness);

  // print_all_refs("pre -mark");

  // oopDesc::_debug = true;

  shenandoahPolicy()->record_phase_start(ShenandoahCollectorPolicy::scan_roots);
  ClassLoaderDataGraph::clear_claimed_marks();
  concurrentMark()->prepare_unmarked_root_objs();
  shenandoahPolicy()->record_phase_end(ShenandoahCollectorPolicy::scan_roots);

  //  print_all_refs("pre-mark2");
}


class VerifyLivenessClosure : public ExtendedOopClosure {

  ShenandoahHeap* _sh;

public:
  VerifyLivenessClosure() : _sh ( ShenandoahHeap::heap() ) {}

  template<class T> void do_oop_nv(T* p) {
    T heap_oop = oopDesc::load_heap_oop(p);
    if (!oopDesc::is_null(heap_oop)) {
      oop obj = oopDesc::decode_heap_oop_not_null(heap_oop);
      guarantee(_sh->heap_region_containing(obj)->is_in_collection_set() == (obj != oopDesc::bs()->read_barrier(obj)),
                err_msg("forwarded objects can only exist in dirty (from-space) regions is_dirty: %s, is_forwarded: %s",
                        BOOL_TO_STR(_sh->heap_region_containing(obj)->is_in_collection_set()),
                        BOOL_TO_STR(obj != oopDesc::bs()->read_barrier(obj)))
                );
      obj = oopDesc::bs()->read_barrier(obj);
      guarantee(! _sh->heap_region_containing(obj)->is_in_collection_set(), "forwarded oops must not point to dirty regions");
      guarantee(obj->is_oop(), "is_oop");
      ShenandoahHeap* sh = (ShenandoahHeap*) Universe::heap();
      if (! sh->is_marked_current(obj)) {
        sh->print_on(tty);
      }
      assert(sh->is_marked_current(obj), err_msg("Referenced Objects should be marked obj: "PTR_FORMAT", marked: %s, is_in_heap: %s",
                                               p2i((HeapWord*) obj), BOOL_TO_STR(sh->is_marked_current(obj)), BOOL_TO_STR(sh->is_in(obj))));
    }
  }

  void do_oop(oop* p)       { do_oop_nv(p); }
  void do_oop(narrowOop* p) { do_oop_nv(p); }

};

void ShenandoahHeap::verify_live() {

  VerifyLivenessClosure cl;
  roots_iterate(&cl);

  IterateMarkedCurrentObjectsClosure marked_oops(&cl);
  object_iterate(&marked_oops);

}

class VerifyAfterEvacuationClosure : public ExtendedOopClosure {

  ShenandoahHeap* _sh;

public:
  VerifyAfterEvacuationClosure() : _sh ( ShenandoahHeap::heap() ) {}

  template<class T> void do_oop_nv(T* p) {
    T heap_oop = oopDesc::load_heap_oop(p);
    if (!oopDesc::is_null(heap_oop)) {
      oop obj = oopDesc::decode_heap_oop_not_null(heap_oop);
      guarantee(_sh->heap_region_containing(obj)->is_in_collection_set() == (obj != oopDesc::bs()->read_barrier(obj)),
                err_msg("forwarded objects can only exist in dirty (from-space) regions is_dirty: %s, is_forwarded: %s obj-klass: %s, marked: %s",
                        BOOL_TO_STR(_sh->heap_region_containing(obj)->is_in_collection_set()),
                        BOOL_TO_STR(obj != oopDesc::bs()->read_barrier(obj)), obj->klass()->external_name(), BOOL_TO_STR(_sh->is_marked_current(obj)))
                );
      obj = oopDesc::bs()->read_barrier(obj);
      guarantee(! _sh->heap_region_containing(obj)->is_in_collection_set(), "forwarded oops must not point to dirty regions");
      guarantee(obj->is_oop(), "is_oop");
      guarantee(Metaspace::contains(obj->klass()), "klass pointer must go to metaspace");
    }
  }

  void do_oop(oop* p)       { do_oop_nv(p); }
  void do_oop(narrowOop* p) { do_oop_nv(p); }

};

class VerifyAfterUpdateRefsClosure : public ExtendedOopClosure {

  ShenandoahHeap* _sh;

public:
  VerifyAfterUpdateRefsClosure() : _sh ( ShenandoahHeap::heap() ) {}

  template<class T> void do_oop_nv(T* p) {
    T heap_oop = oopDesc::load_heap_oop(p);
    if (!oopDesc::is_null(heap_oop)) {
      oop obj = oopDesc::decode_heap_oop_not_null(heap_oop);
      guarantee((! _sh->heap_region_containing(obj)->is_in_collection_set()),
                err_msg("no live reference must point to from-space, is_marked: %s",
                        BOOL_TO_STR(_sh->is_marked_current(obj))));
      if (obj != oopDesc::bs()->read_barrier(obj) && _sh->is_in(p)) {
        tty->print_cr("top-limit: "PTR_FORMAT", p: "PTR_FORMAT, p2i(_sh->heap_region_containing(p)->concurrent_iteration_safe_limit()), p2i(p));
      }
      guarantee(obj == oopDesc::bs()->read_barrier(obj), "no live reference must point to forwarded object");
      guarantee(obj->is_oop(), "is_oop");
      guarantee(Metaspace::contains(obj->klass()), "klass pointer must go to metaspace");
    }
  }

  void do_oop(oop* p)       { do_oop_nv(p); }
  void do_oop(narrowOop* p) { do_oop_nv(p); }

};

void ShenandoahHeap::verify_heap_after_evacuation() {

  verify_heap_size_consistency();

  ensure_parsability(false);

  VerifyAfterEvacuationClosure cl;
  roots_iterate(&cl);

  IterateMarkedCurrentObjectsClosure marked_oops(&cl);
  object_iterate(&marked_oops);

}

class VerifyRegionsAfterUpdateRefsClosure : public ShenandoahHeapRegionClosure {
public:
  bool doHeapRegion(ShenandoahHeapRegion* r) {
    assert(! r->is_in_collection_set(), "no region must be in collection set");
    assert(! ShenandoahHeap::heap()->in_cset_fast_test(r->bottom()), "no region must be in collection set");
    return false;
  }
};

void ShenandoahHeap::verify_regions_after_update_refs() {
  VerifyRegionsAfterUpdateRefsClosure verify_regions;
  heap_region_iterate(&verify_regions);
}

void ShenandoahHeap::verify_heap_after_update_refs() {

  verify_heap_size_consistency();

  ensure_parsability(false);

  VerifyAfterUpdateRefsClosure cl;

  roots_iterate(&cl);
  weak_roots_iterate(&cl);
  oop_iterate(&cl, true, true);

}

void ShenandoahHeap::stop_concurrent_marking() {
  assert(concurrent_mark_in_progress(), "How else could we get here?");
  if (! cancelled_concgc()) {
    // If we needed to update refs, and concurrent marking has been cancelled,
    // we need to finish updating references.
    set_need_update_refs(false);
  }
  set_concurrent_mark_in_progress(false);

  if (ShenandoahGCVerbose) {
    print_heap_regions();
  }

#ifdef ASSERT
  if (ShenandoahVerify && ! _cancelled_concgc) {
    verify_heap_after_marking();
  }

#endif
}

bool ShenandoahHeap::concurrent_mark_in_progress() {
  return _concurrent_mark_in_progress;
}

void ShenandoahHeap::set_concurrent_mark_in_progress(bool in_progress) {
  if (ShenandoahTracePhases) {
    if (in_progress) {
      gclog_or_tty->print_cr("Shenandoah starting concurrent marking, heap used: "SIZE_FORMAT" MB", used() / M);
    } else {
      gclog_or_tty->print_cr("Shenandoah finishing concurrent marking, heap used: "SIZE_FORMAT" MB", used() / M);
    }
  }

  _concurrent_mark_in_progress = in_progress;
  JavaThread::satb_mark_queue_set().set_active_all_threads(in_progress, ! in_progress);
}

void ShenandoahHeap::set_evacuation_in_progress(bool in_progress) {
  if (ShenandoahTracePhases) {
    if (ShenandoahConcurrentEvacuation) {
      if (in_progress) {
        gclog_or_tty->print_cr("Shenandoah starting concurrent evacuation, heap used: "SIZE_FORMAT" MB", used() / M);
      } else {
        gclog_or_tty->print_cr("Shenandoah finishing concurrent evacuation, heap used: "SIZE_FORMAT" MB", used() / M);
      }
    } else {
      if (in_progress) {
        gclog_or_tty->print_cr("Shenandoah starting non-concurrent evacuation");
      } else {
        gclog_or_tty->print_cr("Shenandoah finishing non-concurrent evacuation");
      }
    }
  }
  JavaThread::set_evacuation_in_progress_all_threads(in_progress);
  _evacuation_in_progress = in_progress;
  OrderAccess::fence();
}

bool ShenandoahHeap::is_evacuation_in_progress() {
  return _evacuation_in_progress;
}

bool ShenandoahHeap::is_update_references_in_progress() {
  return _update_references_in_progress;
}

void ShenandoahHeap::set_update_references_in_progress(bool update_refs_in_progress) {
  if (ShenandoahTracePhases) {
    if (ShenandoahConcurrentUpdateRefs) {
      if (update_refs_in_progress) {
        gclog_or_tty->print_cr("Shenandoah starting concurrent reference-updating");
      } else {
        gclog_or_tty->print_cr("Shenandoah finishing concurrent reference-updating");
      }
    } else {
      if (update_refs_in_progress) {
        gclog_or_tty->print_cr("Shenandoah starting non-concurrent reference-updating");
      } else {
        gclog_or_tty->print_cr("Shenandoah finishing non-concurrent reference-updating");
      }
    }
  }
  _update_references_in_progress = update_refs_in_progress;
}

void ShenandoahHeap::verify_copy(oop p,oop c){
    assert(p != oopDesc::bs()->read_barrier(p), "forwarded correctly");
    assert(oopDesc::bs()->read_barrier(p) == c, "verify pointer is correct");
    if (p->klass() != c->klass()) {
      print_heap_regions();
    }
    assert(p->klass() == c->klass(), err_msg("verify class p-size: "INT32_FORMAT" c-size: "INT32_FORMAT, p->size(), c->size()));
    assert(p->size() == c->size(), "verify size");
    // Object may have been locked between copy and verification
    //    assert(p->mark() == c->mark(), "verify mark");
    assert(c == oopDesc::bs()->read_barrier(c), "verify only forwarded once");
  }

void ShenandoahHeap::oom_during_evacuation() {
  // tty->print_cr("Out of memory during evacuation, cancel evacuation, schedule full GC");
  // We ran out of memory during evacuation. Cancel evacuation, and schedule a full-GC.
  collector_policy()->set_should_clear_all_soft_refs(true);
  concurrent_thread()->schedule_full_gc();
  cancel_concgc();

  if ((! Thread::current()->is_GC_task_thread()) && (! Thread::current()->is_ConcurrentGC_thread())) {
    tty->print_cr("OOM during evacuation. Let Java thread wait until evacuation settlded..");
    while (_evacuation_in_progress) { // wait.
      Thread::current()->_ParkEvent->park(1) ;
    }
  }

}

void ShenandoahHeap::copy_object(oop p, HeapWord* s) {
  HeapWord* filler = s;
  assert(s != NULL, "allocation of brooks pointer must not fail");
  HeapWord* copy = s + BrooksPointer::BROOKS_POINTER_OBJ_SIZE;

  guarantee(copy != NULL, "allocation of copy object must not fail");
  Copy::aligned_disjoint_words((HeapWord*) p, copy, p->size());
  initialize_brooks_ptr(filler, copy);

#ifdef ASSERT
  if (ShenandoahTraceEvacuations) {
    tty->print_cr("copy object from "PTR_FORMAT" to: "PTR_FORMAT, p2i((HeapWord*) p), p2i(copy));
  }
#endif
}

oop ShenandoahHeap::evacuate_object(oop p, Thread* thread) {
  ShenandoahHeapRegion* hr;
  size_t required;

#ifdef ASSERT
  if (ShenandoahVerifyReadsToFromSpace) {
    hr = heap_region_containing(p);
    {
      hr->memProtectionOff();
      required  = BrooksPointer::BROOKS_POINTER_OBJ_SIZE + p->size();
      hr->memProtectionOn();
    }
  } else {
    required  = BrooksPointer::BROOKS_POINTER_OBJ_SIZE + p->size();
  }
#else
    required  = BrooksPointer::BROOKS_POINTER_OBJ_SIZE + p->size();
#endif

  assert(! heap_region_containing(p)->is_humongous(), "never evacuate humongous objects");

  // Don't even attempt to evacuate anything if evacuation has been cancelled.
  if (_cancelled_concgc) {
    return ShenandoahBarrierSet::resolve_oop_static(p);
  }

  bool alloc_from_gclab = true;
  thread->set_evacuating(true);
  HeapWord* filler = allocate_from_gclab(thread, required);
  if (filler == NULL) {
    filler = allocate_memory(required);
    alloc_from_gclab = false;
  }
  thread->set_evacuating(false);

  if (filler == NULL) {
    oom_during_evacuation();
    // If this is a Java thread, it should have waited
    // until all GC threads are done, and then we
    // return the forwardee.
    oop resolved = ShenandoahBarrierSet::resolve_oop_static(p);
    return resolved;
  }

  HeapWord* copy = filler + BrooksPointer::BROOKS_POINTER_OBJ_SIZE;

#ifdef ASSERT
  if (ShenandoahVerifyReadsToFromSpace) {
    hr->memProtectionOff();
    copy_object(p, filler);
    hr->memProtectionOn();
  } else {
    copy_object(p, filler);
  }
#else
    copy_object(p, filler);
#endif

  HeapWord* result = BrooksPointer::get(p).cas_forwardee((HeapWord*) p, copy);

  oop return_val;
  if (result == (HeapWord*) p) {
    return_val = oop(copy);

#ifdef ASSERT
    if (ShenandoahTraceEvacuations) {
      tty->print("Copy of "PTR_FORMAT" to "PTR_FORMAT" succeeded \n", p2i((HeapWord*) p), p2i(copy));
    }
    assert(return_val->is_oop(), "expect oop");
    assert(p->klass() == return_val->klass(), err_msg("Should have the same class p: "PTR_FORMAT", copy: "PTR_FORMAT, p2i((HeapWord*) p), p2i((HeapWord*) copy)));
#endif
  }  else {
    if (alloc_from_gclab) {
      thread->gclab().rollback(required);
    }
#ifdef ASSERT
    if (ShenandoahTraceEvacuations) {
      tty->print_cr("Copy of "PTR_FORMAT" to "PTR_FORMAT" failed, use other: "PTR_FORMAT, p2i((HeapWord*) p), p2i(copy), p2i((HeapWord*) result));
    }
#endif
    return_val = (oopDesc*) result;
  }

  return return_val;
}

HeapWord* ShenandoahHeap::tlab_post_allocation_setup(HeapWord* obj) {
  HeapWord* result = obj + BrooksPointer::BROOKS_POINTER_OBJ_SIZE;
  initialize_brooks_ptr(obj, result);
  return result;
}

uint ShenandoahHeap::oop_extra_words() {
  return BrooksPointer::BROOKS_POINTER_OBJ_SIZE;
}

bool ShenandoahHeap::grow_heap_by() {
  int new_region_index = ensure_new_regions(1);
  if (new_region_index != -1) {
    ShenandoahHeapRegion* new_region = new ShenandoahHeapRegion();
    HeapWord* start = _first_region_bottom + (ShenandoahHeapRegion::RegionSizeBytes / HeapWordSize) * new_region_index;
    new_region->initialize_heap_region(start, ShenandoahHeapRegion::RegionSizeBytes / HeapWordSize, new_region_index);
    if (ShenandoahGCVerbose) {
      tty->print_cr("allocating new region at index: "INT32_FORMAT, new_region_index);
      new_region->print();
    }
    _ordered_regions[new_region_index] = new_region;
    _free_regions->append(new_region);
    return true;
  } else {
    return false;
  }
}

int ShenandoahHeap::ensure_new_regions(int new_regions) {

  size_t num_regions = _num_regions;
  size_t new_num_regions = num_regions + new_regions;
  if (new_num_regions >= _max_regions) {
    // Not enough regions left.
    return -1;
  }

  size_t expand_size = new_regions * ShenandoahHeapRegion::RegionSizeBytes;
  if (ShenandoahGCVerbose) {
    tty->print_cr("expanding storage by "SIZE_FORMAT_HEX" bytes, for "INT32_FORMAT" new regions", expand_size, new_regions);
  }
  bool success = _storage.expand_by(expand_size);
  assert(success, "should always be able to expand by requested size");

  _num_regions = new_num_regions;

  return num_regions;

}

ShenandoahIsAliveClosure::ShenandoahIsAliveClosure() :
  _heap(ShenandoahHeap::heap_no_check()) {
}

void ShenandoahIsAliveClosure::init(ShenandoahHeap* heap) {
  _heap = heap;
}

bool ShenandoahIsAliveClosure::do_object_b(oop obj) {

  assert(_heap != NULL, "sanity");
#ifdef ASSERT
  if (_heap->concurrent_mark_in_progress()) {
    assert(obj == ShenandoahBarrierSet::resolve_oop_static_not_null(obj), "only query from-space");
  }
#endif
  assert(!oopDesc::is_null(obj), "null");
  return _heap->is_marked_current(obj);
}

void ShenandoahHeap::ref_processing_init() {
  MemRegion mr = reserved_region();

  // Concurrent Mark ref processor
//   _ref_processor =
//     new ReferenceProcessor(mr,    // span
//                            ParallelRefProcEnabled && (ParallelGCThreads > 1),
//                                 // mt processing
//                            (int) ParallelGCThreads,
//                                 // degree of mt processing
//                            (ParallelGCThreads > 1) || (ConcGCThreads > 1),
//                                 // mt discovery
//                            (int) MAX2(ParallelGCThreads, ConcGCThreads),
//                                 // degree of mt discovery
//                            false,
//                                 // Reference discovery is not atomic
//                         &isAlive);
//                                 // is alive closure
//                                 // (for efficiency/performance)

  isAlive.init(ShenandoahHeap::heap());
  _ref_processor =
    new ReferenceProcessor(mr,    // span
                           ParallelRefProcEnabled && (ConcGCThreads > 1),
                           // mt processing
                           (int) ConcGCThreads,
                           // degree of mt processing
                           (ConcGCThreads > 1),
                           // mt discovery
                           (int) ConcGCThreads,
                           // degree of mt discovery
                           false,
                           // Reference discovery is not atomic
                           &isAlive);
  // is alive closure
  // (for efficiency/performance)



}

#ifdef ASSERT
void ShenandoahHeap::set_from_region_protection(bool protect) {
  for (uint i = 0; i < _num_regions; i++) {
    ShenandoahHeapRegion* region = _ordered_regions[i];
    if (region != NULL && region->is_in_collection_set()) {
      if (protect) {
        region->memProtectionOn();
      } else {
        region->memProtectionOff();
      }
    }
  }
}
#endif

void ShenandoahHeap::acquire_pending_refs_lock() {
  _concurrent_gc_thread->slt()->manipulatePLL(SurrogateLockerThread::acquirePLL);
}

void ShenandoahHeap::release_pending_refs_lock() {
  _concurrent_gc_thread->slt()->manipulatePLL(SurrogateLockerThread::releaseAndNotifyPLL);
}

ShenandoahHeapRegion** ShenandoahHeap::heap_regions() {
  return _ordered_regions;
}

size_t ShenandoahHeap::num_regions() {
  return _num_regions;
}

size_t ShenandoahHeap::max_regions() {
  return _max_regions;
}

GCTracer* ShenandoahHeap::tracer() {
  return collector_policy()->tracer();
}

size_t ShenandoahHeap::tlab_used(Thread* thread) const {
  return _free_regions->used();
}

void ShenandoahHeap::cancel_concgc() {
  // only report it once
  if (!_cancelled_concgc) {
    if (ShenandoahTracePhases) {
      tty->print_cr("Cancelling GC");
    }
    _cancelled_concgc = true;
    OrderAccess::fence();
    _shenandoah_policy->report_concgc_cancelled();
  }

  if ((! Thread::current()->is_GC_task_thread()) && (! Thread::current()->is_ConcurrentGC_thread())) {
    while (_evacuation_in_progress) { // wait.
      Thread::current()->_ParkEvent->park(1) ;
    }
  }
}

void ShenandoahHeap::clear_cancelled_concgc() {
  _cancelled_concgc = false;
}

int ShenandoahHeap::max_workers() {
  return _max_workers;
}

int ShenandoahHeap::max_parallel_workers() {
  return _max_parallel_workers;
}
int ShenandoahHeap::max_conc_workers() {
  return _max_conc_workers;
}

void ShenandoahHeap::shutdown() {
  // We set this early here, to let GC threads terminate before we ask the concurrent thread
  // to terminate, which would otherwise block until all GC threads come to finish normally.
  _cancelled_concgc = true;
  _concurrent_gc_thread->shutdown();
  cancel_concgc();
}

class ShenandoahStringSymbolTableUnlinkTask : public AbstractGangTask {
private:
  BoolObjectClosure* _is_alive;
  int _initial_string_table_size;
  int _initial_symbol_table_size;

  bool  _process_strings;
  int _strings_processed;
  int _strings_removed;

  bool  _process_symbols;
  int _symbols_processed;
  int _symbols_removed;

public:
  ShenandoahStringSymbolTableUnlinkTask(BoolObjectClosure* is_alive, bool process_strings, bool process_symbols) :
    AbstractGangTask("String/Symbol Unlinking"),
    _is_alive(is_alive),
    _process_strings(process_strings), _strings_processed(0), _strings_removed(0),
    _process_symbols(process_symbols), _symbols_processed(0), _symbols_removed(0) {

    _initial_string_table_size = StringTable::the_table()->table_size();
    _initial_symbol_table_size = SymbolTable::the_table()->table_size();
    if (process_strings) {
      StringTable::clear_parallel_claimed_index();
    }
    if (process_symbols) {
      SymbolTable::clear_parallel_claimed_index();
    }
  }

  ~ShenandoahStringSymbolTableUnlinkTask() {
    guarantee(!_process_strings || StringTable::parallel_claimed_index() >= _initial_string_table_size,
              err_msg("claim value %d after unlink less than initial string table size %d",
                      StringTable::parallel_claimed_index(), _initial_string_table_size));
    guarantee(!_process_symbols || SymbolTable::parallel_claimed_index() >= _initial_symbol_table_size,
              err_msg("claim value %d after unlink less than initial symbol table size %d",
                      SymbolTable::parallel_claimed_index(), _initial_symbol_table_size));

    if (ShenandoahTraceStringSymbolTableScrubbing) {
      gclog_or_tty->print_cr("Cleaned string and symbol table, "
                             "strings: "SIZE_FORMAT" processed, "SIZE_FORMAT" removed, "
                             "symbols: "SIZE_FORMAT" processed, "SIZE_FORMAT" removed",
                             strings_processed(), strings_removed(),
                             symbols_processed(), symbols_removed());
    }
  }

  void work(uint worker_id) {
    int strings_processed = 0;
    int strings_removed = 0;
    int symbols_processed = 0;
    int symbols_removed = 0;
    if (_process_strings) {
      StringTable::possibly_parallel_unlink(_is_alive, &strings_processed, &strings_removed);
      Atomic::add(strings_processed, &_strings_processed);
      Atomic::add(strings_removed, &_strings_removed);
    }
    if (_process_symbols) {
      SymbolTable::possibly_parallel_unlink(&symbols_processed, &symbols_removed);
      Atomic::add(symbols_processed, &_symbols_processed);
      Atomic::add(symbols_removed, &_symbols_removed);
    }
  }

  size_t strings_processed() const { return (size_t)_strings_processed; }
  size_t strings_removed()   const { return (size_t)_strings_removed; }

  size_t symbols_processed() const { return (size_t)_symbols_processed; }
  size_t symbols_removed()   const { return (size_t)_symbols_removed; }
};

void ShenandoahHeap::unlink_string_and_symbol_table(BoolObjectClosure* is_alive, bool process_strings, bool process_symbols) {

  workers()->set_active_workers(_max_parallel_workers);
  ShenandoahStringSymbolTableUnlinkTask shenandoah_unlink_task(is_alive, process_strings, process_symbols);
  workers()->run_task(&shenandoah_unlink_task);

  //  if (G1StringDedup::is_enabled()) {
  //    G1StringDedup::unlink(is_alive);
  //  }
}

bool ShenandoahHeap::is_obj_ill(const oop obj) const {
  return ! is_marked_current(obj);
}

void ShenandoahHeap::set_need_update_refs(bool need_update_refs) {
  _need_update_refs = need_update_refs;
}

ShenandoahJNICritical* ShenandoahHeap::jni_critical() {
  return _jni_critical;
}

ShenandoahHeapRegion* ShenandoahHeap::next_compaction_region(const ShenandoahHeapRegion* r) {
  int region_idx = r->region_number() + 1;
  ShenandoahHeapRegion* next = _ordered_regions[region_idx];
  guarantee(next->region_number() == region_idx, "region number must match");
  while (next->is_humongous()) {
    region_idx = next->region_number() + 1;
    next = _ordered_regions[region_idx];
    guarantee(next->region_number() == region_idx, "region number must match");
  }
  return next;
}

bool ShenandoahHeap::is_in_collection_set(const void* p) {
  return heap_region_containing(p)->is_in_collection_set();
}
