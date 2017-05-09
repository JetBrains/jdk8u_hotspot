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
#include "memory/allocation.hpp"

#include "gc_implementation/shared/gcTimer.hpp"
#include "gc_implementation/shenandoah/shenandoahGCTraceTime.hpp"
#include "gc_implementation/shared/parallelCleaning.hpp"

#include "gc_implementation/shenandoah/brooksPointer.hpp"
#include "gc_implementation/shenandoah/shenandoahBarrierSet.hpp"
#include "gc_implementation/shenandoah/shenandoahCollectionSet.hpp"
#include "gc_implementation/shenandoah/shenandoahCollectorPolicy.hpp"
#include "gc_implementation/shenandoah/shenandoahConcurrentMark.hpp"
#include "gc_implementation/shenandoah/shenandoahConcurrentMark.inline.hpp"
#include "gc_implementation/shenandoah/shenandoahConcurrentThread.hpp"
#include "gc_implementation/shenandoah/shenandoahFreeSet.hpp"
#include "gc_implementation/shenandoah/shenandoahHeap.inline.hpp"
#include "gc_implementation/shenandoah/shenandoahHeapRegion.hpp"
#include "gc_implementation/shenandoah/shenandoahHeapRegionSet.hpp"
#include "gc_implementation/shenandoah/shenandoahHumongous.hpp"
#include "gc_implementation/shenandoah/shenandoahMarkCompact.hpp"
#include "gc_implementation/shenandoah/shenandoahMonitoringSupport.hpp"
#include "gc_implementation/shenandoah/shenandoahRootProcessor.hpp"
#include "gc_implementation/shenandoah/vm_operations_shenandoah.hpp"

#include "runtime/vmThread.hpp"
#include "services/mallocTracker.hpp"

SCMUpdateRefsClosure::SCMUpdateRefsClosure() : _heap(ShenandoahHeap::heap()) {}

#ifdef ASSERT
template <class T>
void AssertToSpaceClosure::do_oop_nv(T* p) {
  T o = oopDesc::load_heap_oop(p);
  if (! oopDesc::is_null(o)) {
    oop obj = oopDesc::decode_heap_oop_not_null(o);
    assert(oopDesc::unsafe_equals(obj, ShenandoahBarrierSet::resolve_oop_static_not_null(obj)),
           err_msg("need to-space object here obj: "PTR_FORMAT" , rb(obj): "PTR_FORMAT", p: "PTR_FORMAT,
		   p2i(obj), p2i(ShenandoahBarrierSet::resolve_oop_static_not_null(obj)), p2i(p)));
  }
}

void AssertToSpaceClosure::do_oop(narrowOop* p) { do_oop_nv(p); }
void AssertToSpaceClosure::do_oop(oop* p)       { do_oop_nv(p); }
#endif

const char* ShenandoahHeap::name() const {
  return "Shenandoah";
}

void ShenandoahHeap::print_heap_locations(HeapWord* start, HeapWord* end) {
  HeapWord* cur = NULL;
  for (cur = start; cur < end; cur++) {
    tty->print_cr(PTR_FORMAT" : "PTR_FORMAT, p2i(cur), p2i(*((HeapWord**) cur)));
  }
}

class ShenandoahPretouchTask : public AbstractGangTask {
private:
  ShenandoahHeapRegionSet* _regions;
  const size_t _bitmap_size;
  const size_t _page_size;
  char* _bitmap0_base;
  char* _bitmap1_base;
public:
  ShenandoahPretouchTask(ShenandoahHeapRegionSet* regions,
                         char* bitmap0_base, char* bitmap1_base, size_t bitmap_size,
                         size_t page_size) :
    AbstractGangTask("Shenandoah PreTouch"),
    _bitmap0_base(bitmap0_base),
    _bitmap1_base(bitmap1_base),
    _regions(regions),
    _bitmap_size(bitmap_size),
    _page_size(page_size) {
    _regions->clear_current_index();
  };

  virtual void work(uint worker_id) {
    ShenandoahHeapRegion* r = _regions->claim_next();
    while (r != NULL) {
      log_trace(gc, heap)("Pretouch region " SIZE_FORMAT ": " PTR_FORMAT " -> " PTR_FORMAT,
                          r->region_number(), p2i(r->bottom()), p2i(r->end()));
      os::pretouch_memory((char*) r->bottom(), (char*) r->end());

      size_t start = r->region_number()       * ShenandoahHeapRegion::region_size_bytes() / CMBitMap::mark_distance();
      size_t end   = (r->region_number() + 1) * ShenandoahHeapRegion::region_size_bytes() / CMBitMap::mark_distance();
      assert (end <= _bitmap_size, err_msg("end is sane: " SIZE_FORMAT " < " SIZE_FORMAT, end, _bitmap_size));

      log_trace(gc, heap)("Pretouch bitmap under region " SIZE_FORMAT ": " PTR_FORMAT " -> " PTR_FORMAT,
                          r->region_number(), p2i(_bitmap0_base + start), p2i(_bitmap0_base + end));
      os::pretouch_memory(_bitmap0_base + start, _bitmap0_base + end);

      log_trace(gc, heap)("Pretouch bitmap under region " SIZE_FORMAT ": " PTR_FORMAT " -> " PTR_FORMAT,
                          r->region_number(), p2i(_bitmap1_base + start), p2i(_bitmap1_base + end));
      os::pretouch_memory(_bitmap1_base + start, _bitmap1_base + end);

      r = _regions->claim_next();
    }
  }
};

jint ShenandoahHeap::initialize() {
  CollectedHeap::pre_initialize();

  BrooksPointer::initial_checks();

  size_t init_byte_size = collector_policy()->initial_heap_byte_size();
  size_t max_byte_size = collector_policy()->max_heap_byte_size();

  Universe::check_alignment(max_byte_size,
                            ShenandoahHeapRegion::region_size_bytes(),
                            "shenandoah heap");
  Universe::check_alignment(init_byte_size,
                            ShenandoahHeapRegion::region_size_bytes(),
                            "shenandoah heap");

  ReservedSpace heap_rs = Universe::reserve_heap(max_byte_size,
                                                 Arguments::conservative_max_heap_alignment());

  _reserved.set_word_size(0);
  _reserved.set_start((HeapWord*)heap_rs.base());
  _reserved.set_end((HeapWord*)(heap_rs.base() + heap_rs.size()));

  set_barrier_set(new ShenandoahBarrierSet(this));
  ReservedSpace pgc_rs = heap_rs.first_part(max_byte_size);
  _storage.initialize(pgc_rs, init_byte_size);

  _num_regions = init_byte_size / ShenandoahHeapRegion::region_size_bytes();
  _max_regions = max_byte_size / ShenandoahHeapRegion::region_size_bytes();
  _initialSize = _num_regions * ShenandoahHeapRegion::region_size_bytes();
  size_t regionSizeWords = ShenandoahHeapRegion::region_size_bytes() / HeapWordSize;
  assert(init_byte_size == _initialSize, "tautology");
  _ordered_regions = new ShenandoahHeapRegionSet(_max_regions);
  _collection_set = new ShenandoahCollectionSet(_max_regions);
  _free_regions = new ShenandoahFreeSet(_max_regions);

  // Initialize fast collection set test structure.
  _in_cset_fast_test_length = _max_regions;
  _in_cset_fast_test_base =
                   NEW_C_HEAP_ARRAY(bool, _in_cset_fast_test_length, mtGC);
  _in_cset_fast_test = _in_cset_fast_test_base -
               ((uintx) pgc_rs.base() >> ShenandoahHeapRegion::region_size_shift());

  _next_top_at_mark_starts_base =
                   NEW_C_HEAP_ARRAY(HeapWord*, _max_regions, mtGC);
  _next_top_at_mark_starts = _next_top_at_mark_starts_base -
               ((uintx) pgc_rs.base() >> ShenandoahHeapRegion::region_size_shift());

  _complete_top_at_mark_starts_base =
                   NEW_C_HEAP_ARRAY(HeapWord*, _max_regions, mtGC);
  _complete_top_at_mark_starts = _complete_top_at_mark_starts_base -
               ((uintx) pgc_rs.base() >> ShenandoahHeapRegion::region_size_shift());

  size_t i = 0;
  for (i = 0; i < _num_regions; i++) {
    _in_cset_fast_test_base[i] = false; // Not in cset
    HeapWord* bottom = (HeapWord*) pgc_rs.base() + regionSizeWords * i;
    _complete_top_at_mark_starts_base[i] = bottom;
    _next_top_at_mark_starts_base[i] = bottom;
  }

  {
    ShenandoahHeapLock lock(this);
    for (i = 0; i < _num_regions; i++) {
      ShenandoahHeapRegion* current = new ShenandoahHeapRegion(this, (HeapWord*) pgc_rs.base() +
                                                               regionSizeWords * i, regionSizeWords, i);
      _free_regions->add_region(current);
      _ordered_regions->add_region(current);
    }
  }
  assert(((size_t) _ordered_regions->active_regions()) == _num_regions, "");
  _first_region = _ordered_regions->get(0);
  assert((((size_t) base()) &
          (ShenandoahHeapRegion::region_size_bytes() - 1)) == 0,
         err_msg("misaligned heap: "PTR_FORMAT, p2i(base())));

  if (ShenandoahLogTrace) {
    ResourceMark rm;
    outputStream* out = gclog_or_tty;
    log_trace(gc, region)("All Regions");
    _ordered_regions->print(out);
    log_trace(gc, region)("Free Regions");
    _free_regions->print(out);
  }

  _recycled_regions = NEW_C_HEAP_ARRAY(size_t, _max_regions, mtGC);
  _recycled_region_count = 0;

  // The call below uses stuff (the SATB* things) that are in G1, but probably
  // belong into a shared location.
  JavaThread::satb_mark_queue_set().initialize(SATB_Q_CBL_mon,
                                               SATB_Q_FL_lock,
                                               20 /*G1SATBProcessCompletedThreshold */,
                                               Shared_SATB_Q_lock);

  // Reserve space for prev and next bitmap.
  size_t bitmap_size = CMBitMap::compute_size(heap_rs.size());
  MemRegion heap_region = MemRegion((HeapWord*) heap_rs.base(), heap_rs.size() / HeapWordSize);

  size_t page_size = UseLargePages ? (size_t)os::large_page_size() : (size_t)os::vm_page_size();

  ReservedSpace bitmap0(bitmap_size, page_size);
  os::commit_memory_or_exit(bitmap0.base(), bitmap0.size(), false, "couldn't allocate mark bitmap");
  MemTracker::record_virtual_memory_type(bitmap0.base(), mtGC);
  MemRegion bitmap_region0 = MemRegion((HeapWord*) bitmap0.base(), bitmap0.size() / HeapWordSize);

  ReservedSpace bitmap1(bitmap_size, page_size);
  os::commit_memory_or_exit(bitmap1.base(), bitmap1.size(), false, "couldn't allocate mark bitmap");
  MemTracker::record_virtual_memory_type(bitmap1.base(), mtGC);
  MemRegion bitmap_region1 = MemRegion((HeapWord*) bitmap1.base(), bitmap1.size() / HeapWordSize);

  if (ShenandoahAlwaysPreTouch) {
    assert (!AlwaysPreTouch, "Should have been overridden");

    // For NUMA, it is important to pre-touch the storage under bitmaps with worker threads,
    // before initialize() below zeroes it with initializing thread. For any given region,
    // we touch the region and the corresponding bitmaps from the same thread.

    log_info(gc, heap)("Parallel pretouch " SIZE_FORMAT " regions with " SIZE_FORMAT " byte pages",
                       _ordered_regions->count(), page_size);
    ShenandoahPretouchTask cl(_ordered_regions, bitmap0.base(), bitmap1.base(), bitmap_size, page_size);
    _workers->run_task(&cl);
  }

  _mark_bit_map0.initialize(heap_region, bitmap_region0);
  _complete_mark_bit_map = &_mark_bit_map0;

  _mark_bit_map1.initialize(heap_region, bitmap_region1);
  _next_mark_bit_map = &_mark_bit_map1;

  _monitoring_support = new ShenandoahMonitoringSupport(this);

  _concurrent_gc_thread = new ShenandoahConcurrentThread();

  ShenandoahMarkCompact::initialize();

  return JNI_OK;
}

ShenandoahHeap::ShenandoahHeap(ShenandoahCollectorPolicy* policy) :
  SharedHeap(policy),
  _shenandoah_policy(policy),
  _concurrent_mark_in_progress(0),
  _evacuation_in_progress(0),
  _full_gc_in_progress(false),
  _free_regions(NULL),
  _collection_set(NULL),
  _bytes_allocated_since_cm(0),
  _bytes_allocated_during_cm(0),
  _allocated_last_gc(0),
  _used_start_gc(0),
  _max_workers((uint)MAX2(ConcGCThreads, ParallelGCThreads)),
  _ref_processor(NULL),
  _in_cset_fast_test(NULL),
  _in_cset_fast_test_base(NULL),
  _next_top_at_mark_starts(NULL),
  _next_top_at_mark_starts_base(NULL),
  _complete_top_at_mark_starts(NULL),
  _complete_top_at_mark_starts_base(NULL),
  _mark_bit_map0(),
  _mark_bit_map1(),
  _cancelled_concgc(false),
  _need_update_refs(false),
  _need_reset_bitmaps(false),
  _heap_lock(0),
#ifdef ASSERT
  _heap_lock_owner(NULL),
#endif
  _gc_timer(new (ResourceObj::C_HEAP, mtGC) ConcurrentGCTimer())

{
  log_info(gc, init)("Parallel GC threads: "UINTX_FORMAT, ParallelGCThreads);
  log_info(gc, init)("Concurrent GC threads: "UINTX_FORMAT, ConcGCThreads);
  log_info(gc, init)("Parallel reference processing enabled: %s", BOOL_TO_STR(ParallelRefProcEnabled));

  _scm = new ShenandoahConcurrentMark();
  _used = 0;

  _max_workers = MAX2(_max_workers, 1U);
  _workers = new FlexibleWorkGang("Shenandoah GC Threads", _max_workers,
                            /* are_GC_task_threads */true,
                            /* are_ConcurrentGC_threads */false);
  if (_workers == NULL) {
    vm_exit_during_initialization("Failed necessary allocation.");
  } else {
    _workers->initialize_workers();
  }
}

class ResetNextBitmapTask : public AbstractGangTask {
private:
  ShenandoahHeapRegionSet* _regions;

public:
  ResetNextBitmapTask(ShenandoahHeapRegionSet* regions) :
    AbstractGangTask("Parallel Reset Bitmap Task"),
    _regions(regions) {
    _regions->clear_current_index();
  }

  void work(uint worker_id) {
    ShenandoahHeapRegion* region = _regions->claim_next();
    ShenandoahHeap* heap = ShenandoahHeap::heap();
    while (region != NULL) {
      HeapWord* bottom = region->bottom();
      HeapWord* top = heap->next_top_at_mark_start(region->bottom());
      if (top > bottom) {
        heap->next_mark_bit_map()->clear_range_large(MemRegion(bottom, top));
      }
      region = _regions->claim_next();
    }
  }
};

void ShenandoahHeap::reset_next_mark_bitmap(WorkGang* workers) {
  ResetNextBitmapTask task = ResetNextBitmapTask(_ordered_regions);
  workers->run_task(&task);
}

class ResetCompleteBitmapTask : public AbstractGangTask {
private:
  ShenandoahHeapRegionSet* _regions;

public:
  ResetCompleteBitmapTask(ShenandoahHeapRegionSet* regions) :
    AbstractGangTask("Parallel Reset Bitmap Task"),
    _regions(regions) {
    _regions->clear_current_index();
  }

  void work(uint worker_id) {
    ShenandoahHeapRegion* region = _regions->claim_next();
    ShenandoahHeap* heap = ShenandoahHeap::heap();
    while (region != NULL) {
      HeapWord* bottom = region->bottom();
      HeapWord* top = heap->complete_top_at_mark_start(region->bottom());
      if (top > bottom) {
        heap->complete_mark_bit_map()->clear_range_large(MemRegion(bottom, top));
      }
      region = _regions->claim_next();
    }
  }
};

void ShenandoahHeap::reset_complete_mark_bitmap(WorkGang* workers) {
  ResetCompleteBitmapTask task = ResetCompleteBitmapTask(_ordered_regions);
  workers->run_task(&task);
}

bool ShenandoahHeap::is_next_bitmap_clear() {
  HeapWord* start = _ordered_regions->bottom();
  HeapWord* end = _ordered_regions->end();
  return _next_mark_bit_map->getNextMarkedWordAddress(start, end) == end;
}

bool ShenandoahHeap::is_complete_bitmap_clear_range(HeapWord* start, HeapWord* end) {
  return _complete_mark_bit_map->getNextMarkedWordAddress(start, end) == end;
}

void ShenandoahHeap::print_on(outputStream* st) const {
  st->print("Shenandoah Heap");
  st->print(" total = " SIZE_FORMAT " K, used " SIZE_FORMAT " K ", capacity()/ K, used() /K);
  st->print(" [" PTR_FORMAT ", " PTR_FORMAT ") ",
            p2i(reserved_region().start()),
            p2i(reserved_region().end()));
  st->print("Region size = " SIZE_FORMAT "K ", ShenandoahHeapRegion::region_size_bytes() / K);
  if (_concurrent_mark_in_progress) {
    st->print("marking ");
  }
  if (_evacuation_in_progress) {
    st->print("evacuating ");
  }
  if (cancelled_concgc()) {
    st->print("cancelled ");
  }
  st->print("\n");

  // Adapted from VirtualSpace::print_on(), which is non-PRODUCT only
  st->print   ("Virtual space:");
  if (_storage.special()) st->print(" (pinned in memory)");
  st->cr();
  st->print_cr(" - committed: " SIZE_FORMAT, _storage.committed_size());
  st->print_cr(" - reserved:  " SIZE_FORMAT, _storage.reserved_size());
  st->print_cr(" - [low, high]:     [" INTPTR_FORMAT ", " INTPTR_FORMAT "]",  p2i(_storage.low()), p2i(_storage.high()));
  st->print_cr(" - [low_b, high_b]: [" INTPTR_FORMAT ", " INTPTR_FORMAT "]",  p2i(_storage.low_boundary()), p2i(_storage.high_boundary()));

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
  if (UseTLAB) {
    // This is a very tricky point in VM lifetime. We cannot easily call Threads::threads_do
    // here, because some system threads (VMThread, WatcherThread, etc) are not yet available.
    // Their initialization should be handled separately. Is we miss some threads here,
    // then any other TLAB-related activity would fail with asserts.

    InitGCLABClosure init_gclabs;
    {
      MutexLocker ml(Threads_lock);
      for (JavaThread *thread = Threads::first(); thread != NULL; thread = thread->next()) {
        init_gclabs.do_thread(thread);
      }
    }
    gc_threads_do(&init_gclabs);
  }

  _scm->initialize(_max_workers);

  ref_processing_init();
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

void ShenandoahHeap::verify_heap_size_consistency() {

  assert(calculateUsed() == used(),
         err_msg("heap used size must be consistent heap-used: "SIZE_FORMAT" regions-used: "SIZE_FORMAT, used(), calculateUsed()));
}

size_t ShenandoahHeap::used() const {
  OrderAccess::acquire();
  return _used;
}

void ShenandoahHeap::increase_used(size_t bytes) {
  Atomic::add_ptr(bytes, (intptr_t*) &_used);
}

void ShenandoahHeap::set_used(size_t bytes) {
  _used = bytes;
  OrderAccess::release();
}

void ShenandoahHeap::decrease_used(size_t bytes) {
  assert(_used >= bytes, "never decrease heap size by more than we've left");
  Atomic::add_ptr(-((intptr_t)bytes), (intptr_t*) &_used);
}

size_t ShenandoahHeap::capacity() const {
  return _num_regions * ShenandoahHeapRegion::region_size_bytes();

}

bool ShenandoahHeap::is_maximal_no_gc() const {
  Unimplemented();
  return true;
}

size_t ShenandoahHeap::max_capacity() const {
  return _max_regions * ShenandoahHeapRegion::region_size_bytes();
}

size_t ShenandoahHeap::min_capacity() const {
  return _initialSize;
}

VirtualSpace* ShenandoahHeap::storage() const {
  return (VirtualSpace*) &_storage;
}

bool ShenandoahHeap::is_in(const void* p) const {
  HeapWord* heap_base = (HeapWord*) base();
  HeapWord* last_region_end = heap_base + (ShenandoahHeapRegion::region_size_bytes() / HeapWordSize) * _num_regions;
  return p >= heap_base && p < last_region_end;
}

bool ShenandoahHeap::is_in_partial_collection(const void* p ) {
  Unimplemented();
  return false;
}

bool ShenandoahHeap::is_scavengable(const void* p) {
  return true;
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
#ifdef ASSERT
  log_debug(gc, alloc)("Allocate new tlab, requested size = " SIZE_FORMAT " bytes", word_size * HeapWordSize);
#endif
  return allocate_new_tlab(word_size, false);
}

HeapWord* ShenandoahHeap::allocate_new_gclab(size_t word_size) {
#ifdef ASSERT
  log_debug(gc, alloc)("Allocate new gclab, requested size = " SIZE_FORMAT " bytes", word_size * HeapWordSize);
#endif
  return allocate_new_tlab(word_size, true);
}

HeapWord* ShenandoahHeap::allocate_new_tlab(size_t word_size, bool evacuating) {

  HeapWord* result = allocate_memory(word_size, evacuating);

  if (result != NULL) {
    assert(! in_collection_set(result), "Never allocate in dirty region");
    _bytes_allocated_since_cm += word_size * HeapWordSize;

    log_develop_trace(gc, tlab)("allocating new tlab of size "SIZE_FORMAT" at addr "PTR_FORMAT, word_size, p2i(result));

  }
  return result;
}

ShenandoahHeap* ShenandoahHeap::heap() {
  CollectedHeap* heap = Universe::heap();
  assert(heap != NULL, "Unitialized access to ShenandoahHeap::heap()");
  assert(heap->kind() == CollectedHeap::ShenandoahHeap, "not a shenandoah heap");
  return (ShenandoahHeap*) heap;
}

ShenandoahHeap* ShenandoahHeap::heap_no_check() {
  CollectedHeap* heap = Universe::heap();
  return (ShenandoahHeap*) heap;
}

HeapWord* ShenandoahHeap::allocate_memory_work(size_t word_size) {

  ShenandoahHeapLock heap_lock(this);

  HeapWord* result = allocate_memory_under_lock(word_size);
  size_t grow_by = (word_size * HeapWordSize + ShenandoahHeapRegion::region_size_bytes() - 1) / ShenandoahHeapRegion::region_size_bytes();

  while (result == NULL && _num_regions + grow_by <= _max_regions) {
    grow_heap_by(grow_by);
    result = allocate_memory_under_lock(word_size);
  }

  return result;
}

HeapWord* ShenandoahHeap::allocate_memory(size_t word_size, bool evacuating) {
  HeapWord* result = NULL;
  result = allocate_memory_work(word_size);

  if (!evacuating) {
    // Allocation failed, try full-GC, then retry allocation.
    //
    // It might happen that one of the threads requesting allocation would unblock
    // way later after full-GC happened, only to fail the second allocation, because
    // other threads have already depleted the free storage. In this case, a better
    // strategy would be to try full-GC again.
    //
    // Lacking the way to detect progress from "collect" call, we are left with blindly
    // retrying for some bounded number of times.
    // TODO: Poll if Full GC made enough progress to warrant retry.
    int tries = 0;
    while ((result == NULL) && (tries++ < ShenandoahFullGCTries)) {
      log_debug(gc)("[" PTR_FORMAT " Failed to allocate " SIZE_FORMAT " bytes, doing full GC, try %d",
                    p2i(Thread::current()), word_size * HeapWordSize, tries);
      collect(GCCause::_allocation_failure);
      result = allocate_memory_work(word_size);
    }
  }

  // Only update monitoring counters when not calling from a write-barrier.
  // Otherwise we might attempt to grab the Service_lock, which we must
  // not do when coming from a write-barrier (because the thread might
  // already hold the Compile_lock).
  if (! evacuating) {
    monitoring_support()->update_counters();
  }

  log_develop_trace(gc, alloc)("allocate memory chunk of size "SIZE_FORMAT" at addr "PTR_FORMAT " by thread %d ",
                               word_size, p2i(result), Thread::current()->osthread()->thread_id());

  return result;
}

HeapWord* ShenandoahHeap::allocate_memory_under_lock(size_t word_size) {
  assert_heaplock_owned_by_current_thread();

  if (word_size * HeapWordSize > ShenandoahHeapRegion::region_size_bytes()) {
    return allocate_large_memory(word_size);
  }

  // Not enough memory in free region set.
  // Coming out of full GC, it is possible that there is not
  // free region available, so current_index may not be valid.
  if (word_size * HeapWordSize > _free_regions->capacity()) return NULL;

  ShenandoahHeapRegion* my_current_region = _free_regions->current_no_humongous();

  if (my_current_region == NULL) {
    return NULL; // No more room to make a new region. OOM.
  }
  assert(my_current_region != NULL, "should have a region at this point");

#ifdef ASSERT
  if (in_collection_set(my_current_region)) {
    print_heap_regions();
  }
#endif
  assert(! in_collection_set(my_current_region), "never get targetted regions in free-lists");
  assert(! my_current_region->is_humongous(), "never attempt to allocate from humongous object regions");

  HeapWord* result = my_current_region->par_allocate(word_size);

  while (result == NULL) {
    // 2nd attempt. Try next region.
#ifdef ASSERT
    if (my_current_region->free() > 0) {
      log_debug(gc, alloc)("Retire region with " SIZE_FORMAT " bytes free", my_current_region->free());
    }
#endif
    _free_regions->increase_used(my_current_region->free());
    ShenandoahHeapRegion* next_region = _free_regions->next_no_humongous();
    assert(next_region != my_current_region, "must not get current again");
    my_current_region = next_region;

    if (my_current_region == NULL) {
      return NULL; // No more room to make a new region. OOM.
    }
    assert(my_current_region != NULL, "should have a region at this point");
    assert(! in_collection_set(my_current_region), "never get targetted regions in free-lists");
    assert(! my_current_region->is_humongous(), "never attempt to allocate from humongous object regions");
    result = my_current_region->par_allocate(word_size);
  }

  my_current_region->increase_live_data_words(word_size);
  increase_used(word_size * HeapWordSize);
  _free_regions->increase_used(word_size * HeapWordSize);
  return result;
}

HeapWord* ShenandoahHeap::allocate_large_memory(size_t words) {
  assert_heaplock_owned_by_current_thread();

  size_t required_regions = ShenandoahHumongous::required_regions(words * HeapWordSize);
  if (required_regions > _max_regions) return NULL;

  ShenandoahHeapRegion* r = _free_regions->allocate_contiguous(required_regions);

  HeapWord* result = NULL;

  if (r != NULL)  {
    result = r->bottom();

    log_debug(gc, humongous)("allocating humongous object of size: "SIZE_FORMAT" KB at location "PTR_FORMAT" in start region "SIZE_FORMAT,
                             (words * HeapWordSize) / K, p2i(result), r->region_number());
  } else {
    log_debug(gc, humongous)("allocating humongous object of size: "SIZE_FORMAT" KB at location "PTR_FORMAT" failed",
                             (words * HeapWordSize) / K, p2i(result));
  }


  return result;

}

HeapWord*  ShenandoahHeap::mem_allocate(size_t size,
                                        bool*  gc_overhead_limit_was_exceeded) {

  HeapWord* filler = allocate_memory(size + BrooksPointer::word_size(), false);
  HeapWord* result = filler + BrooksPointer::word_size();
  if (filler != NULL) {
    BrooksPointer::initialize(oop(result));
    _bytes_allocated_since_cm += size * HeapWordSize;

    assert(! in_collection_set(result), "never allocate in targetted region");
    return result;
  } else {
    /*
    tty->print_cr("Out of memory. Requested number of words: "SIZE_FORMAT" used heap: "INT64_FORMAT", bytes allocated since last CM: "INT64_FORMAT,
                  size, used(), _bytes_allocated_since_cm);
    {
      print_heap_regions();
      tty->print("Printing "SIZE_FORMAT" free regions:\n", _free_regions->count());
      _free_regions->print();
    }
    */
    return NULL;
  }
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

    log_develop_trace(gc, compaction)("Calling ParallelEvacuateRegionObjectClosure on "PTR_FORMAT" of size %d\n", p2i((HeapWord*) p), p->size());

    assert(_heap->is_marked_complete(p), "expect only marked objects");
    if (oopDesc::unsafe_equals(p, ShenandoahBarrierSet::resolve_oop_static_not_null(p))) {
      _heap->evacuate_object(p, _thread);
    }
  }
};

#ifdef ASSERT
class VerifyEvacuatedObjectClosure : public ObjectClosure {

public:

  void do_object(oop p) {
    if (ShenandoahHeap::heap()->is_marked_complete(p)) {
      oop p_prime = oopDesc::bs()->read_barrier(p);
      assert(! oopDesc::unsafe_equals(p, p_prime), "Should point to evacuated copy");
      if (p->klass() != p_prime->klass()) {
        tty->print_cr("copy has different class than original:");
        p->klass()->print_on(tty);
        p_prime->klass()->print_on(tty);
      }
      assert(p->klass() == p_prime->klass(), err_msg("Should have the same class p: "PTR_FORMAT", p_prime: "PTR_FORMAT, p2i(p), p2i(p_prime)));
      //      assert(p->mark() == p_prime->mark(), "Should have the same mark");
      assert(p->size() == p_prime->size(), "Should be the same size");
      assert(oopDesc::unsafe_equals(p_prime, oopDesc::bs()->read_barrier(p_prime)), "One forward once");
    }
  }
};

void ShenandoahHeap::verify_evacuated_region(ShenandoahHeapRegion* from_region) {
  VerifyEvacuatedObjectClosure verify_evacuation;
  marked_object_iterate(from_region, &verify_evacuation);
}
#endif

void ShenandoahHeap::parallel_evacuate_region(ShenandoahHeapRegion* from_region) {

  assert(from_region->has_live(), "all-garbage regions are reclaimed earlier");

  ParallelEvacuateRegionObjectClosure evacuate_region(this);

  marked_object_iterate(from_region, &evacuate_region);

#ifdef ASSERT
  if (ShenandoahVerify && ! cancelled_concgc()) {
    verify_evacuated_region(from_region);
  }
#endif
}

class ParallelEvacuationTask : public AbstractGangTask {
private:
  ShenandoahHeap* _sh;
  ShenandoahCollectionSet* _cs;

public:
  ParallelEvacuationTask(ShenandoahHeap* sh,
                         ShenandoahCollectionSet* cs) :
    AbstractGangTask("Parallel Evacuation Task"),
    _cs(cs),
    _sh(sh) {}

  void work(uint worker_id) {

    ShenandoahHeapRegion* from_hr = _cs->claim_next();

    while (from_hr != NULL) {
      log_develop_trace(gc, region)("Thread "INT32_FORMAT" claimed Heap Region "SIZE_FORMAT,
                                    worker_id,
                                    from_hr->region_number());

      assert(from_hr->has_live(), "all-garbage regions are reclaimed early");
      _sh->parallel_evacuate_region(from_hr);

      if (_sh->cancelled_concgc()) {
        log_develop_trace(gc, region)("Cancelled concgc while evacuating region " SIZE_FORMAT "\n", from_hr->region_number());
        break;
      }
      from_hr = _cs->claim_next();
    }
  }
};

void ShenandoahHeap::recycle_dirty_regions() {
  ShenandoahHeapLock lock(this);

  size_t bytes_reclaimed = 0;

  ShenandoahHeapRegionSet* set = regions();
  set->clear_current_index();

  start_deferred_recycling();

  ShenandoahHeapRegion* r = set->claim_next();
  while (r != NULL) {
    if (in_collection_set(r)) {
      decrease_used(r->used());
      bytes_reclaimed += r->used();
      defer_recycle(r);
    }
    r = set->claim_next();
  }

  finish_deferred_recycle();

  _shenandoah_policy->record_bytes_reclaimed(bytes_reclaimed);
  if (! cancelled_concgc()) {
    clear_cset_fast_test();
  }
}

ShenandoahFreeSet* ShenandoahHeap::free_regions() {
  return _free_regions;
}

void ShenandoahHeap::print_heap_regions(outputStream* st) const {
  _ordered_regions->print(st);
}

class PrintAllRefsOopClosure: public ExtendedOopClosure {
private:
  int _index;
  const char* _prefix;

public:
  PrintAllRefsOopClosure(const char* prefix) : _index(0), _prefix(prefix) {}

private:
  template <class T>
  inline void do_oop_work(T* p) {
    oop o = oopDesc::load_decode_heap_oop(p);
    if (o != NULL) {
      if (ShenandoahHeap::heap()->is_in(o) && o->is_oop()) {
        tty->print_cr("%s "INT32_FORMAT" ("PTR_FORMAT")-> "PTR_FORMAT" (marked: %s) (%s "PTR_FORMAT")",
                      _prefix, _index,
                      p2i(p), p2i(o),
                      BOOL_TO_STR(ShenandoahHeap::heap()->is_marked_complete(o)),
                      o->klass()->internal_name(), p2i(o->klass()));
      } else {
        tty->print_cr("%s "INT32_FORMAT" ("PTR_FORMAT" dirty -> "PTR_FORMAT" (not in heap, possibly corrupted or dirty)",
                      _prefix, _index,
                      p2i(p), p2i(o));
      }
    } else {
      tty->print_cr("%s "INT32_FORMAT" ("PTR_FORMAT") -> "PTR_FORMAT, _prefix, _index, p2i(p), p2i((HeapWord*) o));
    }
    _index++;
  }

public:
  void do_oop(oop* p) {
    do_oop_work(p);
  }

  void do_oop(narrowOop* p) {
    do_oop_work(p);
  }

};

class PrintAllRefsObjectClosure : public ObjectClosure {
  const char* _prefix;

public:
  PrintAllRefsObjectClosure(const char* prefix) : _prefix(prefix) {}

  void do_object(oop p) {
    if (ShenandoahHeap::heap()->is_in(p)) {
        tty->print_cr("%s object "PTR_FORMAT" (marked: %s) (%s "PTR_FORMAT") refers to:",
                      _prefix, p2i(p),
                      BOOL_TO_STR(ShenandoahHeap::heap()->is_marked_complete(p)),
                      p->klass()->internal_name(), p2i(p->klass()));
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

private:
  template <class T>
  inline void do_oop_work(T* p) {
    oop o = oopDesc::load_decode_heap_oop(p);
    if (o != NULL) {
      if (! _heap->is_marked_complete(o)) {
        _heap->print_heap_regions();
        _heap->print_all_refs("post-mark");
        tty->print_cr("oop not marked, although referrer is marked: "PTR_FORMAT": in_heap: %s, is_marked: %s",
                      p2i((HeapWord*) o), BOOL_TO_STR(_heap->is_in(o)), BOOL_TO_STR(_heap->is_marked_complete(o)));
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
      if (! oopDesc::unsafe_equals(o, oopDesc::bs()->read_barrier(o))) {
        tty->print_cr("oops has forwardee: p: "PTR_FORMAT" (%s), o = "PTR_FORMAT" (%s), new-o: "PTR_FORMAT" (%s)",
                      p2i(p),
                      BOOL_TO_STR(_heap->in_collection_set(p)),
                      p2i(o),
                      BOOL_TO_STR(_heap->in_collection_set(o)),
                      p2i((HeapWord*) oopDesc::bs()->read_barrier(o)),
                      BOOL_TO_STR(_heap->in_collection_set(oopDesc::bs()->read_barrier(o))));
        tty->print_cr("oop class: %s", o->klass()->internal_name());
      }
      assert(oopDesc::unsafe_equals(o, oopDesc::bs()->read_barrier(o)), "oops must not be forwarded");
      assert(! _heap->in_collection_set(o), "references must not point to dirty heap regions");
      assert(_heap->is_marked_complete(o), "live oops must be marked current");
    }
  }

public:
  void do_oop(oop* p) {
    do_oop_work(p);
  }

  void do_oop(narrowOop* p) {
    do_oop_work(p);
  }

};

void ShenandoahHeap::verify_heap_after_marking() {

  verify_heap_size_consistency();

  log_trace(gc)("verifying heap after marking");

  VerifyAfterMarkingOopClosure cl;
  roots_iterate(&cl);
  ObjectToOopClosure objs(&cl);
  object_iterate(&objs);
}


void ShenandoahHeap::reclaim_humongous_region_at(ShenandoahHeapRegion* r) {
  assert(r->is_humongous_start(), "reclaim regions starting with the first one");

  oop humongous_obj = oop(r->bottom() + BrooksPointer::word_size());
  size_t size = humongous_obj->size() + BrooksPointer::word_size();
  size_t required_regions = ShenandoahHumongous::required_regions(size * HeapWordSize);
  size_t index = r->region_number();


  assert(!r->has_live(), "liveness must be zero");

  for(size_t i = 0; i < required_regions; i++) {

    ShenandoahHeapRegion* region = _ordered_regions->get(index++);

    assert((region->is_humongous_start() || region->is_humongous_continuation()),
           "expect correct humongous start or continuation");

    if (ShenandoahLogDebug) {
      log_debug(gc, humongous)("reclaiming "SIZE_FORMAT" humongous regions for object of size: "SIZE_FORMAT" words", required_regions, size);
      ResourceMark rm;
      outputStream* out = gclog_or_tty;
      region->print_on(out);
    }

    region->recycle();
    ShenandoahHeap::heap()->decrease_used(ShenandoahHeapRegion::region_size_bytes());
  }
}

class ShenandoahReclaimHumongousRegionsClosure : public ShenandoahHeapRegionClosure {

  bool doHeapRegion(ShenandoahHeapRegion* r) {
    ShenandoahHeap* heap = ShenandoahHeap::heap();

    if (r->is_humongous_start()) {
      oop humongous_obj = oop(r->bottom() + BrooksPointer::word_size());
      if (! heap->is_marked_complete(humongous_obj)) {

        heap->reclaim_humongous_region_at(r);
      }
    }
    return false;
  }
};

#ifdef ASSERT
class CheckCollectionSetClosure: public ShenandoahHeapRegionClosure {
  bool doHeapRegion(ShenandoahHeapRegion* r) {
    assert(! ShenandoahHeap::heap()->in_collection_set(r), "Should have been cleared by now");
    return false;
  }
};
#endif

void ShenandoahHeap::prepare_for_concurrent_evacuation() {
  assert(_ordered_regions->get(0)->region_number() == 0, "FIXME CHF. FIXME CHF!");

  log_develop_trace(gc)("Thread %d started prepare_for_concurrent_evacuation", Thread::current()->osthread()->thread_id());

  if (!cancelled_concgc()) {

    recycle_dirty_regions();

    ensure_parsability(true);

#ifdef ASSERT
    if (ShenandoahVerify) {
      verify_heap_after_marking();
    }
#endif

    // NOTE: This needs to be done during a stop the world pause, because
    // putting regions into the collection set concurrently with Java threads
    // will create a race. In particular, acmp could fail because when we
    // resolve the first operand, the containing region might not yet be in
    // the collection set, and thus return the original oop. When the 2nd
    // operand gets resolved, the region could be in the collection set
    // and the oop gets evacuated. If both operands have originally been
    // the same, we get false negatives.

    {
      ShenandoahHeapLock lock(this);
      _collection_set->clear();
      _free_regions->clear();

      ShenandoahReclaimHumongousRegionsClosure reclaim;
      heap_region_iterate(&reclaim);

#ifdef ASSERT
      CheckCollectionSetClosure ccsc;
      _ordered_regions->heap_region_iterate(&ccsc);
#endif

      _shenandoah_policy->choose_collection_set(_collection_set);
      _shenandoah_policy->choose_free_set(_free_regions);
    }

    if (UseShenandoahMatrix) {
      _collection_set->print();
    }

    _bytes_allocated_since_cm = 0;

    Universe::update_heap_info_at_gc();
  }
}


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
    Threads::threads_do(&cl);
  }
}

class ShenandoahEvacuateUpdateRootsClosure: public ExtendedOopClosure {
private:
  ShenandoahHeap* _heap;
  Thread* _thread;
public:
  ShenandoahEvacuateUpdateRootsClosure() :
    _heap(ShenandoahHeap::heap()), _thread(Thread::current()) {
  }

private:
  template <class T>
  void do_oop_work(T* p) {
    assert(_heap->is_evacuation_in_progress(), "Only do this when evacuation is in progress");

    T o = oopDesc::load_heap_oop(p);
    if (! oopDesc::is_null(o)) {
      oop obj = oopDesc::decode_heap_oop_not_null(o);
      if (_heap->in_collection_set(obj)) {
        assert(_heap->is_marked_complete(obj), err_msg("only evacuate marked objects %d %d",
						       _heap->is_marked_complete(obj), _heap->is_marked_complete(ShenandoahBarrierSet::resolve_oop_static_not_null(obj))));
        oop resolved = ShenandoahBarrierSet::resolve_oop_static_not_null(obj);
        if (oopDesc::unsafe_equals(resolved, obj)) {
          resolved = _heap->evacuate_object(obj, _thread);
        }
        oopDesc::encode_store_heap_oop(p, resolved);
      }
    }
#ifdef ASSERT
    else {
      // tty->print_cr("not updating root at: "PTR_FORMAT" with object: "PTR_FORMAT", is_in_heap: %s, is_in_cset: %s, is_marked: %s",
      //               p2i(p),
      //               p2i((HeapWord*) obj),
      //               BOOL_TO_STR(_heap->is_in(obj)),
      //               BOOL_TO_STR(_heap->in_cset_fast_test(obj)),
      //               BOOL_TO_STR(_heap->is_marked_complete(obj)));
    }
#endif
  }

public:
  void do_oop(oop* p) {
    do_oop_work(p);
  }
  void do_oop(narrowOop* p) {
    do_oop_work(p);
  }
};

class ShenandoahEvacuateUpdateRootsTask : public AbstractGangTask {
  ShenandoahRootEvacuator* _rp;
public:

  ShenandoahEvacuateUpdateRootsTask(ShenandoahRootEvacuator* rp) :
    AbstractGangTask("Shenandoah evacuate and update roots"),
    _rp(rp)
  {
    // Nothing else to do.
  }

  void work(uint worker_id) {
    ShenandoahEvacuateUpdateRootsClosure cl;
    MarkingCodeBlobClosure blobsCl(&cl, CodeBlobToOopClosure::FixRelocations);

    _rp->process_evacuate_roots(&cl, &blobsCl, worker_id);
  }
};

class ShenandoahFixRootsTask : public AbstractGangTask {
  ShenandoahRootEvacuator* _rp;
public:

  ShenandoahFixRootsTask(ShenandoahRootEvacuator* rp) :
    AbstractGangTask("Shenandoah update roots"),
    _rp(rp)
  {
    // Nothing else to do.
  }

  void work(uint worker_id) {
    SCMUpdateRefsClosure cl;
    MarkingCodeBlobClosure blobsCl(&cl, CodeBlobToOopClosure::FixRelocations);

    _rp->process_evacuate_roots(&cl, &blobsCl, worker_id);
  }
};
void ShenandoahHeap::evacuate_and_update_roots() {

  COMPILER2_PRESENT(DerivedPointerTable::clear());

  assert(SafepointSynchronize::is_at_safepoint(), "Only iterate roots while world is stopped");

  {
    ShenandoahRootEvacuator rp(this, workers()->active_workers(), ShenandoahCollectorPolicy::evac_thread_roots);
    ShenandoahEvacuateUpdateRootsTask roots_task(&rp);
    workers()->run_task(&roots_task);
  }

  if (cancelled_concgc()) {
    // If initial evacuation has been cancelled, we need to update all references
    // after all workers have finished. Otherwise we might run into the following problem:
    // GC thread 1 cannot allocate anymore, thus evacuation fails, leaves from-space ptr of object X.
    // GC thread 2 evacuates the same object X to to-space
    // which leaves a truly dangling from-space reference in the first root oop*. This must not happen.
    ShenandoahRootEvacuator rp(this, workers()->active_workers(), ShenandoahCollectorPolicy::evac_thread_roots);
    ShenandoahFixRootsTask update_roots_task(&rp);
    workers()->run_task(&update_roots_task);
  }

  COMPILER2_PRESENT(DerivedPointerTable::update_pointers());

#ifdef ASSERT
  {
    AssertToSpaceClosure cl;
    CodeBlobToOopClosure code_cl(&cl, !CodeBlobToOopClosure::FixRelocations);
    ShenandoahRootEvacuator rp(this, 1);
    rp.process_evacuate_roots(&cl, &code_cl, 0);
  }
#endif
}


void ShenandoahHeap::do_evacuation() {

  parallel_evacuate();

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
  log_develop_trace(gc)("starting parallel_evacuate");

  _shenandoah_policy->record_phase_start(ShenandoahCollectorPolicy::conc_evac);

  if (ShenandoahLogTrace) {
    ResourceMark rm;
    outputStream* out = gclog_or_tty;
    out->print("Printing all available regions");
    print_heap_regions(out);
  }

  if (ShenandoahLogTrace) {
    ResourceMark rm;
    outputStream* out = gclog_or_tty;
    out->print("Printing collection set which contains "SIZE_FORMAT" regions:\n", _collection_set->count());
    _collection_set->print(out);

    out->print("Printing free set which contains "SIZE_FORMAT" regions:\n", _free_regions->count());
    _free_regions->print(out);
  }

  ParallelEvacuationTask evacuationTask = ParallelEvacuationTask(this, _collection_set);

  workers()->run_task(&evacuationTask);

  if (ShenandoahLogTrace) {
    ResourceMark rm;
    outputStream* out = gclog_or_tty;
    out->print("Printing postgc collection set which contains "SIZE_FORMAT" regions:\n",
	       _collection_set->count());

    _collection_set->print(out);

    out->print("Printing postgc free regions which contain "SIZE_FORMAT" free regions:\n",
	       _free_regions->count());
    _free_regions->print(out);

  }

  if (ShenandoahLogTrace) {
    ResourceMark rm;
    outputStream* out = gclog_or_tty;
    out->print_cr("all regions after evacuation:");
    print_heap_regions(out);
  }

  _shenandoah_policy->record_phase_end(ShenandoahCollectorPolicy::conc_evac);
}

class VerifyEvacuationClosure: public ExtendedOopClosure {
private:
  ShenandoahHeap*  _heap;
  ShenandoahHeapRegion* _from_region;

public:
  VerifyEvacuationClosure(ShenandoahHeapRegion* from_region) :
    _heap(ShenandoahHeap::heap()), _from_region(from_region) { }
private:
  template <class T>
  inline void do_oop_work(T* p) {
    oop heap_oop = oopDesc::load_decode_heap_oop(p);
    if (! oopDesc::is_null(heap_oop)) {
      guarantee(! _from_region->is_in(heap_oop), err_msg("no references to from-region allowed after evacuation: "PTR_FORMAT, p2i((HeapWord*) heap_oop)));
    }
  }

public:
  void do_oop(oop* p)       {
    do_oop_work(p);
  }

  void do_oop(narrowOop* p) {
    do_oop_work(p);
  }

};

void ShenandoahHeap::roots_iterate(OopClosure* cl) {

  assert(SafepointSynchronize::is_at_safepoint(), "Only iterate roots while world is stopped");

  CodeBlobToOopClosure blobsCl(cl, false);
  CLDToOopClosure cldCl(cl);

  ShenandoahRootProcessor rp(this, 1);
  rp.process_all_roots(cl, NULL, &cldCl, &blobsCl, 0);
}

void ShenandoahHeap::verify_evacuation(ShenandoahHeapRegion* from_region) {

  VerifyEvacuationClosure rootsCl(from_region);
  roots_iterate(&rootsCl);

}

bool ShenandoahHeap::supports_tlab_allocation() const {
  return true;
}


size_t  ShenandoahHeap::unsafe_max_tlab_alloc(Thread *thread) const {
  size_t idx = _free_regions->current_index();
  ShenandoahHeapRegion* current = _free_regions->get_or_null(idx);
  if (current == NULL) {
    return 0;
  } else if (current->free() >= MinTLABSize) {
    // Current region has enough space left, can use it.
    return current->free();
  } else {
    // No more space in current region, peek next region
    return _free_regions->unsafe_peek_next_no_humongous();
  }
}

size_t ShenandoahHeap::max_tlab_size() const {
  return ShenandoahHeapRegion::region_size_bytes();
}

class ResizeGCLABClosure : public ThreadClosure {
public:
  void do_thread(Thread* thread) {
    thread->gclab().resize();
  }
};

void ShenandoahHeap::resize_all_tlabs() {
  CollectedHeap::resize_all_tlabs();

  ResizeGCLABClosure cl;
  Threads::threads_do(&cl);
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
  Threads::threads_do(&cl);
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

void ShenandoahHeap::collect(GCCause::Cause cause) {
  assert(cause != GCCause::_gc_locker, "no JNI critical callback");
  if (GCCause::is_user_requested_gc(cause)) {
    if (! DisableExplicitGC) {
      _concurrent_gc_thread->do_full_gc(cause);
    }
  } else if (cause == GCCause::_allocation_failure) {
    collector_policy()->set_should_clear_all_soft_refs(true);
    _concurrent_gc_thread->do_full_gc(cause);
  }
}

void ShenandoahHeap::do_full_collection(bool clear_all_soft_refs) {
  //assert(false, "Shouldn't need to do full collections");
}

AdaptiveSizePolicy* ShenandoahHeap::size_policy() {
  Unimplemented();
  return NULL;

}

CollectorPolicy* ShenandoahHeap::collector_policy() const {
  return _shenandoah_policy;
}


HeapWord* ShenandoahHeap::block_start(const void* addr) const {
  Space* sp = heap_region_containing(addr);
  if (sp != NULL) {
    return sp->block_start(addr);
  }
  return NULL;
}

size_t ShenandoahHeap::block_size(const HeapWord* addr) const {
  Space* sp = heap_region_containing(addr);
  assert(sp != NULL, "block_size of address outside of heap");
  return sp->block_size(addr);
}

bool ShenandoahHeap::block_is_obj(const HeapWord* addr) const {
  Space* sp = heap_region_containing(addr);
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
}

void ShenandoahHeap::gc_threads_do(ThreadClosure* tcl) const {
  workers()->threads_do(tcl);
}

void ShenandoahHeap::print_tracing_info() const {
  if (ShenandoahLogInfo || TraceGen0Time || TraceGen1Time) {
    ResourceMark rm;
    outputStream* out = gclog_or_tty;
    _shenandoah_policy->print_tracing_info(out);
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

private:
  template <class T>
  inline void do_oop_work(T* p) {
    oop obj = oopDesc::load_decode_heap_oop(p);
    if (! oopDesc::is_null(obj) && ! obj->is_oop()) {
      { // Just for debugging.
        tty->print_cr("Root location "PTR_FORMAT
                      "verified "PTR_FORMAT, p2i(p), p2i((void*) obj));
        //      obj->print_on(tty);
      }
    }
    guarantee(obj->is_oop_or_null(), "is oop or null");
  }

public:
  void do_oop(oop* p)       {
    do_oop_work(p);
  }

  void do_oop(narrowOop* p) {
    do_oop_work(p);
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

void ShenandoahHeap::verify(bool silent, VerifyOption vo) {
  if (SafepointSynchronize::is_at_safepoint() || ! UseTLAB) {

    ShenandoahVerifyRootsClosure rootsCl(vo);

    assert(Thread::current()->is_VM_thread(),
           "Expected to be executed serially by the VM thread at this point");

    roots_iterate(&rootsCl);

    bool failures = rootsCl.failures();
    log_trace(gc)("verify failures: %s", BOOL_TO_STR(failures));

    ShenandoahVerifyHeapClosure heapCl(rootsCl);

    object_iterate(&heapCl);
    // TODO: Implement rest of it.
  } else {
    tty->print("(SKIPPING roots, heapRegions, remset) ");
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
    ShenandoahHeap::heap()->marked_object_iterate(r, _cl);
    return false;
  }
};

void ShenandoahHeap::object_iterate(ObjectClosure* cl) {
  ShenandoahIterateObjectClosureRegionClosure blk(cl);
  heap_region_iterate(&blk, false, true);
}

class ShenandoahSafeObjectIterateAdjustPtrsClosure : public MetadataAwareOopClosure {
private:
  ShenandoahHeap* _heap;

public:
  ShenandoahSafeObjectIterateAdjustPtrsClosure() : _heap(ShenandoahHeap::heap()) {}

private:
  template <class T>
  inline void do_oop_work(T* p) {
    T o = oopDesc::load_heap_oop(p);
    if (!oopDesc::is_null(o)) {
      oop obj = oopDesc::decode_heap_oop_not_null(o);
      oopDesc::encode_store_heap_oop(p, BrooksPointer::forwardee(obj));
    }
  }
public:
  void do_oop(oop* p) {
    do_oop_work(p);
  }
  void do_oop(narrowOop* p) {
    do_oop_work(p);
  }
};

class ShenandoahSafeObjectIterateAndUpdate : public ObjectClosure {
private:
  ObjectClosure* _cl;
public:
  ShenandoahSafeObjectIterateAndUpdate(ObjectClosure *cl) : _cl(cl) {}

  virtual void do_object(oop obj) {
    assert (oopDesc::unsafe_equals(obj, BrooksPointer::forwardee(obj)),
            "avoid double-counting: only non-forwarded objects here");

    // Fix up the ptrs.
    ShenandoahSafeObjectIterateAdjustPtrsClosure adjust_ptrs;
    obj->oop_iterate(&adjust_ptrs);

    // Can reply the object now:
    _cl->do_object(obj);
  }
};

void ShenandoahHeap::safe_object_iterate(ObjectClosure* cl) {
  assert(SafepointSynchronize::is_at_safepoint(), "safe iteration is only available during safepoints");

  // Safe iteration does objects only with correct references.
  // This is why we skip dirty regions that have stale copies of objects,
  // and fix up the pointers in the returned objects.

  ShenandoahSafeObjectIterateAndUpdate safe_cl(cl);
  ShenandoahIterateObjectClosureRegionClosure blk(&safe_cl);
  heap_region_iterate(&blk,
                      /* skip_dirty_regions = */ true,
                      /* skip_humongous_continuations = */ true);

  _need_update_refs = false; // already updated the references
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
    ShenandoahHeapRegion* current  = _ordered_regions->get(i);
    if (skip_humongous_continuation && current->is_humongous_continuation()) {
      continue;
    }
    if (skip_dirty_regions && in_collection_set(current)) {
      continue;
    }
    if (blk->doHeapRegion(current)) {
      return;
    }
  }
}

class ClearLivenessClosure : public ShenandoahHeapRegionClosure {
  ShenandoahHeap* sh;
public:
  ClearLivenessClosure(ShenandoahHeap* heap) : sh(heap) { }

  bool doHeapRegion(ShenandoahHeapRegion* r) {
    r->clear_live_data();
    sh->set_next_top_at_mark_start(r->bottom(), r->top());
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

  _shenandoah_policy->record_bytes_allocated(_bytes_allocated_since_cm);
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

  // Make above changes visible to worker threads
  OrderAccess::fence();

  shenandoahPolicy()->record_phase_start(ShenandoahCollectorPolicy::scan_roots);
  concurrentMark()->init_mark_roots();
  shenandoahPolicy()->record_phase_end(ShenandoahCollectorPolicy::scan_roots);

  //  print_all_refs("pre-mark2");
}

class VerifyAfterEvacuationClosure : public ExtendedOopClosure {

  ShenandoahHeap* _sh;

public:
  VerifyAfterEvacuationClosure() : _sh ( ShenandoahHeap::heap() ) {}

  template<class T> void do_oop_nv(T* p) {
    T heap_oop = oopDesc::load_heap_oop(p);
    if (!oopDesc::is_null(heap_oop)) {
      oop obj = oopDesc::decode_heap_oop_not_null(heap_oop);
      guarantee(_sh->in_collection_set(obj) == (! oopDesc::unsafe_equals(obj, oopDesc::bs()->read_barrier(obj))),
                err_msg("forwarded objects can only exist in dirty (from-space) regions is_dirty: %s, is_forwarded: %s obj-klass: %s, marked: %s",
                BOOL_TO_STR(_sh->in_collection_set(obj)),
                BOOL_TO_STR(! oopDesc::unsafe_equals(obj, oopDesc::bs()->read_barrier(obj))),
                obj->klass()->external_name(),
                BOOL_TO_STR(_sh->is_marked_complete(obj)))
                );
      obj = oopDesc::bs()->read_barrier(obj);
      guarantee(! _sh->in_collection_set(obj), "forwarded oops must not point to dirty regions");
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

  ObjectToOopClosure objs(&cl);
  object_iterate(&objs);

}

void ShenandoahHeap::swap_mark_bitmaps() {
  // Swap bitmaps.
  CMBitMap* tmp1 = _complete_mark_bit_map;
  _complete_mark_bit_map = _next_mark_bit_map;
  _next_mark_bit_map = tmp1;

  // Swap top-at-mark-start pointers
  HeapWord** tmp2 = _complete_top_at_mark_starts;
  _complete_top_at_mark_starts = _next_top_at_mark_starts;
  _next_top_at_mark_starts = tmp2;

  HeapWord** tmp3 = _complete_top_at_mark_starts_base;
  _complete_top_at_mark_starts_base = _next_top_at_mark_starts_base;
  _next_top_at_mark_starts_base = tmp3;
}

void ShenandoahHeap::stop_concurrent_marking() {
  assert(concurrent_mark_in_progress(), "How else could we get here?");
  if (! cancelled_concgc()) {
    // If we needed to update refs, and concurrent marking has been cancelled,
    // we need to finish updating references.
    set_need_update_refs(false);
    swap_mark_bitmaps();
  }
  set_concurrent_mark_in_progress(false);

  if (ShenandoahLogTrace) {
    ResourceMark rm;
    outputStream* out = gclog_or_tty;
    print_heap_regions(out);
  }

}

void ShenandoahHeap::set_concurrent_mark_in_progress(bool in_progress) {
  _concurrent_mark_in_progress = in_progress ? 1 : 0;
  JavaThread::satb_mark_queue_set().set_active_all_threads(in_progress, !in_progress);
}

void ShenandoahHeap::set_evacuation_in_progress_concurrently(bool in_progress) {
  // Note: it is important to first release the _evacuation_in_progress flag here,
  // so that Java threads can get out of oom_during_evacuation() and reach a safepoint,
  // in case a VM task is pending.
  set_evacuation_in_progress(in_progress);
  MutexLocker mu(Threads_lock);
  JavaThread::set_evacuation_in_progress_all_threads(in_progress);
}

void ShenandoahHeap::set_evacuation_in_progress_at_safepoint(bool in_progress) {
  assert(SafepointSynchronize::is_at_safepoint(), "Only call this at safepoint");
  set_evacuation_in_progress(in_progress);
  JavaThread::set_evacuation_in_progress_all_threads(in_progress);
}

void ShenandoahHeap::set_evacuation_in_progress(bool in_progress) {
  _evacuation_in_progress = in_progress ? 1 : 0;
  OrderAccess::fence();
}

void ShenandoahHeap::verify_copy(oop p,oop c){
    assert(! oopDesc::unsafe_equals(p, oopDesc::bs()->read_barrier(p)), "forwarded correctly");
    assert(oopDesc::unsafe_equals(oopDesc::bs()->read_barrier(p), c), "verify pointer is correct");
    if (p->klass() != c->klass()) {
      print_heap_regions();
    }
    assert(p->klass() == c->klass(), err_msg("verify class p-size: "INT32_FORMAT" c-size: "INT32_FORMAT, p->size(), c->size()));
    assert(p->size() == c->size(), "verify size");
    // Object may have been locked between copy and verification
    //    assert(p->mark() == c->mark(), "verify mark");
    assert(oopDesc::unsafe_equals(c, oopDesc::bs()->read_barrier(c)), "verify only forwarded once");
  }

void ShenandoahHeap::oom_during_evacuation() {
  log_develop_trace(gc)("Out of memory during evacuation, cancel evacuation, schedule full GC by thread %d",
                        Thread::current()->osthread()->thread_id());

  // We ran out of memory during evacuation. Cancel evacuation, and schedule a full-GC.
  collector_policy()->set_should_clear_all_soft_refs(true);
  concurrent_thread()->try_set_full_gc();
  cancel_concgc(_oom_evacuation);

  if ((! Thread::current()->is_GC_task_thread()) && (! Thread::current()->is_ConcurrentGC_thread())) {
    assert(! Threads_lock->owned_by_self(), "must not hold Threads_lock here");
    log_warning(gc)("OOM during evacuation. Let Java thread wait until evacuation finishes.");
    while (_evacuation_in_progress) { // wait.
      Thread::current()->_ParkEvent->park(1);
    }
  }

}

HeapWord* ShenandoahHeap::tlab_post_allocation_setup(HeapWord* obj) {
  // Initialize Brooks pointer for the next object
  HeapWord* result = obj + BrooksPointer::word_size();
  BrooksPointer::initialize(oop(result));
  return result;
}

uint ShenandoahHeap::oop_extra_words() {
  return BrooksPointer::word_size();
}

void ShenandoahHeap::grow_heap_by(size_t num_regions) {
  size_t old_num_regions = _num_regions;
  ensure_new_regions(num_regions);
  for (size_t i = 0; i < num_regions; i++) {
    size_t new_region_index = i + old_num_regions;
    HeapWord* start = ((HeapWord*) base()) + (ShenandoahHeapRegion::region_size_bytes() / HeapWordSize) * new_region_index;
    ShenandoahHeapRegion* new_region = new ShenandoahHeapRegion(this, start, ShenandoahHeapRegion::region_size_bytes() / HeapWordSize, new_region_index);

    if (ShenandoahLogTrace) {
      ResourceMark rm;
      outputStream* out = gclog_or_tty;
      out->print_cr("allocating new region at index: "SIZE_FORMAT, new_region_index);
      new_region->print_on(out);
    }

    assert(_ordered_regions->active_regions() == new_region->region_number(), "must match");
    _ordered_regions->add_region(new_region);
    _in_cset_fast_test_base[new_region_index] = false; // Not in cset
    _next_top_at_mark_starts_base[new_region_index] = new_region->bottom();
    _complete_top_at_mark_starts_base[new_region_index] = new_region->bottom();

    _free_regions->add_region(new_region);
  }
}

void ShenandoahHeap::ensure_new_regions(size_t new_regions) {

  size_t num_regions = _num_regions;
  size_t new_num_regions = num_regions + new_regions;
  assert(new_num_regions <= _max_regions, "we checked this earlier");

  size_t expand_size = new_regions * ShenandoahHeapRegion::region_size_bytes();
  log_trace(gc, region)("expanding storage by "SIZE_FORMAT_HEX" bytes, for "SIZE_FORMAT" new regions", expand_size, new_regions);
  bool success = _storage.expand_by(expand_size, ShenandoahAlwaysPreTouch);
  assert(success, "should always be able to expand by requested size");

  _num_regions = new_num_regions;

}

ShenandoahForwardedIsAliveClosure::ShenandoahForwardedIsAliveClosure() :
  _heap(ShenandoahHeap::heap_no_check()) {
}

void ShenandoahForwardedIsAliveClosure::init(ShenandoahHeap* heap) {
  _heap = heap;
}

bool ShenandoahForwardedIsAliveClosure::do_object_b(oop obj) {

  assert(_heap != NULL, "sanity");
  obj = ShenandoahBarrierSet::resolve_oop_static_not_null(obj);
#ifdef ASSERT
  if (_heap->concurrent_mark_in_progress()) {
    assert(oopDesc::unsafe_equals(obj, ShenandoahBarrierSet::resolve_oop_static_not_null(obj)), "only query to-space");
  }
#endif
  assert(!oopDesc::is_null(obj), "null");
  return _heap->is_marked_next(obj);
}

void ShenandoahHeap::ref_processing_init() {
  MemRegion mr = reserved_region();

  isAlive.init(ShenandoahHeap::heap());
  assert(_max_workers > 0, "Sanity");

  _ref_processor =
    new ReferenceProcessor(mr,    // span
                           ParallelRefProcEnabled,
                           // mt processing
                           _max_workers,
                           // degree of mt processing
                           true,
                           // mt discovery
                           _max_workers,
                           // degree of mt discovery
                           false,
                           // Reference discovery is not atomic
                           &isAlive);
}

void ShenandoahHeap::acquire_pending_refs_lock() {
  _concurrent_gc_thread->slt()->manipulatePLL(SurrogateLockerThread::acquirePLL);
}

void ShenandoahHeap::release_pending_refs_lock() {
  _concurrent_gc_thread->slt()->manipulatePLL(SurrogateLockerThread::releaseAndNotifyPLL);
}
size_t ShenandoahHeap::num_regions() {
  return _num_regions;
}

size_t ShenandoahHeap::max_regions() {
  return _max_regions;
}

GCTracer* ShenandoahHeap::tracer() {
  return shenandoahPolicy()->tracer();
}

size_t ShenandoahHeap::tlab_used(Thread* thread) const {
  return _free_regions->used();
}

void ShenandoahHeap::cancel_concgc(GCCause::Cause cause) {
  if (try_cancel_concgc()) {
    log_info(gc)("Cancelling concurrent GC: %s", GCCause::to_string(cause));
    _shenandoah_policy->report_concgc_cancelled();
  }
}

void ShenandoahHeap::cancel_concgc(ShenandoahCancelCause cause) {
  if (try_cancel_concgc()) {
    log_info(gc)("Cancelling concurrent GC: %s", cancel_cause_to_string(cause));
    _shenandoah_policy->report_concgc_cancelled();
  }
}

const char* ShenandoahHeap::cancel_cause_to_string(ShenandoahCancelCause cause) {
  switch (cause) {
    case _oom_evacuation:
      return "Out of memory for evacuation";
    case _vm_stop:
      return "Stopping VM";
    default:
      return "Unknown";
  }
}

uint ShenandoahHeap::max_workers() {
  return _max_workers;
}

void ShenandoahHeap::stop() {
  // The shutdown sequence should be able to terminate when GC is running.

  // Step 1. Notify control thread that we are in shutdown.
  // Note that we cannot do that with stop(), because stop() is blocking and waits for the actual shutdown.
  // Doing stop() here would wait for the normal GC cycle to complete, never falling through to cancel below.
  _concurrent_gc_thread->prepare_for_graceful_shutdown();

  // Step 2. Notify GC workers that we are cancelling GC.
  cancel_concgc(_vm_stop);

  // Step 3. Wait until GC worker exits normally.
  _concurrent_gc_thread->stop();
}

void ShenandoahHeap::unload_classes_and_cleanup_tables() {
  ShenandoahForwardedIsAliveClosure is_alive;
  // Unload classes and purge SystemDictionary.
  bool purged_class = SystemDictionary::do_unloading(&is_alive, true);
  ParallelCleaningTask unlink_task(&is_alive, true, true, _workers->active_workers(), purged_class);
  _workers->run_task(&unlink_task);
  ClassLoaderDataGraph::purge();
}

void ShenandoahHeap::set_need_update_refs(bool need_update_refs) {
  _need_update_refs = need_update_refs;
}

//fixme this should be in heapregionset
ShenandoahHeapRegion* ShenandoahHeap::next_compaction_region(const ShenandoahHeapRegion* r) {
  size_t region_idx = r->region_number() + 1;
  ShenandoahHeapRegion* next = _ordered_regions->get(region_idx);
  guarantee(next->region_number() == region_idx, "region number must match");
  while (next->is_humongous()) {
    region_idx = next->region_number() + 1;
    next = _ordered_regions->get(region_idx);
    guarantee(next->region_number() == region_idx, "region number must match");
  }
  return next;
}

void ShenandoahHeap::set_region_in_collection_set(size_t region_index, bool b) {
  _in_cset_fast_test_base[region_index] = b;
}

ShenandoahMonitoringSupport* ShenandoahHeap::monitoring_support() {
  return _monitoring_support;
}

CMBitMap* ShenandoahHeap::complete_mark_bit_map() {
  return _complete_mark_bit_map;
}

CMBitMap* ShenandoahHeap::next_mark_bit_map() {
  return _next_mark_bit_map;
}

void ShenandoahHeap::add_free_region(ShenandoahHeapRegion* r) {
  _free_regions->add_region(r);
}

void ShenandoahHeap::clear_free_regions() {
  _free_regions->clear();
}

address ShenandoahHeap::in_cset_fast_test_addr() {
  return (address) (ShenandoahHeap::heap()->_in_cset_fast_test);
}

address ShenandoahHeap::cancelled_concgc_addr() {
  return (address) &(ShenandoahHeap::heap()->_cancelled_concgc);
}

void ShenandoahHeap::clear_cset_fast_test() {
  assert(_in_cset_fast_test_base != NULL, "sanity");
  memset(_in_cset_fast_test_base, false,
         _in_cset_fast_test_length * sizeof(bool));
}

size_t ShenandoahHeap::conservative_max_heap_alignment() {
  return ShenandoahMaxRegionSize;
}

size_t ShenandoahHeap::bytes_allocated_since_cm() {
  return _bytes_allocated_since_cm;
}

void ShenandoahHeap::set_bytes_allocated_since_cm(size_t bytes) {
  _bytes_allocated_since_cm = bytes;
}

void ShenandoahHeap::set_next_top_at_mark_start(HeapWord* region_base, HeapWord* addr) {
  uintx index = ((uintx) region_base) >> ShenandoahHeapRegion::region_size_shift();
  _next_top_at_mark_starts[index] = addr;
}

HeapWord* ShenandoahHeap::next_top_at_mark_start(HeapWord* region_base) {
  uintx index = ((uintx) region_base) >> ShenandoahHeapRegion::region_size_shift();
  return _next_top_at_mark_starts[index];
}

void ShenandoahHeap::set_complete_top_at_mark_start(HeapWord* region_base, HeapWord* addr) {
  uintx index = ((uintx) region_base) >> ShenandoahHeapRegion::region_size_shift();
  _complete_top_at_mark_starts[index] = addr;
}

HeapWord* ShenandoahHeap::complete_top_at_mark_start(HeapWord* region_base) {
  uintx index = ((uintx) region_base) >> ShenandoahHeapRegion::region_size_shift();
  return _complete_top_at_mark_starts[index];
}

void ShenandoahHeap::set_full_gc_in_progress(bool in_progress) {
  _full_gc_in_progress = in_progress;
}

bool ShenandoahHeap::is_full_gc_in_progress() const {
  return _full_gc_in_progress;
}

class NMethodOopInitializer : public OopClosure {
private:
  ShenandoahHeap* _heap;
public:
  NMethodOopInitializer() : _heap(ShenandoahHeap::heap()) {
  }

private:
  template <class T>
  inline void do_oop_work(T* p) {
    T o = oopDesc::load_heap_oop(p);
    if (! oopDesc::is_null(o)) {
      oop obj1 = oopDesc::decode_heap_oop_not_null(o);
      oop obj2 = oopDesc::bs()->write_barrier(obj1);
      if (! oopDesc::unsafe_equals(obj1, obj2)) {
        oopDesc::encode_store_heap_oop(p, obj2);
      }
    }
  }

public:
  void do_oop(oop* o) {
    do_oop_work(o);
  }
  void do_oop(narrowOop* o) {
    do_oop_work(o);
  }
};

void ShenandoahHeap::register_nmethod(nmethod* nm) {
  NMethodOopInitializer init;
  nm->oops_do(&init);
  nm->fix_oop_relocations();
}

void ShenandoahHeap::unregister_nmethod(nmethod* nm) {
}

void ShenandoahHeap::pin_object(oop o) {
  heap_region_containing(o)->pin();
}

void ShenandoahHeap::unpin_object(oop o) {
  heap_region_containing(o)->unpin();
}


GCTimer* ShenandoahHeap::gc_timer() const {
  return _gc_timer;
}

class ShenandoahCountGarbageClosure : public ShenandoahHeapRegionClosure {
private:
  size_t _garbage;
public:
  ShenandoahCountGarbageClosure() : _garbage(0) {
  }

  bool doHeapRegion(ShenandoahHeapRegion* r) {
    if (! r->is_humongous() && ! r->is_pinned() && ! r->in_collection_set()) {
      _garbage += r->garbage();
    }
    return false;
  }

  size_t garbage() {
    return _garbage;
  }
};

size_t ShenandoahHeap::garbage() {
  ShenandoahCountGarbageClosure cl;
  heap_region_iterate(&cl);
  return cl.garbage();
}

#ifdef ASSERT
void ShenandoahHeap::assert_heaplock_owned_by_current_thread() {
  assert(_heap_lock == locked, "must be locked");
  assert(_heap_lock_owner == Thread::current(), "must be owned by current thread");
}
#endif

void ShenandoahHeap::start_deferred_recycling() {
  assert_heaplock_owned_by_current_thread();
  _recycled_region_count = 0;
}

void ShenandoahHeap::defer_recycle(ShenandoahHeapRegion* r) {
  assert_heaplock_owned_by_current_thread();
  _recycled_regions[_recycled_region_count++] = r->region_number();
}

void ShenandoahHeap::finish_deferred_recycle() {
  assert_heaplock_owned_by_current_thread();
  for (size_t i = 0; i < _recycled_region_count; i++) {
    regions()->get(_recycled_regions[i])->recycle();
  }
}
