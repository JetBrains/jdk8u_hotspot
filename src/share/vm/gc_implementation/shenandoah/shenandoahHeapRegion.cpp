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

#include "memory/allocation.hpp"
#include "gc_implementation/shared/liveRange.hpp"
#include "gc_implementation/shenandoah/brooksPointer.hpp"
#include "gc_implementation/shenandoah/shenandoahHeapRegion.hpp"
#include "gc_implementation/shenandoah/shenandoahHeap.inline.hpp"
#include "gc_implementation/g1/heapRegionBounds.inline.hpp"
#include "memory/space.inline.hpp"
#include "memory/universe.hpp"
#include "runtime/mutexLocker.hpp"
#include "runtime/os.hpp"

size_t ShenandoahHeapRegion::RegionSizeShift = 0;
size_t ShenandoahHeapRegion::RegionSizeBytes = 0;

jint ShenandoahHeapRegion::initialize_heap_region(HeapWord* start,
                                                  size_t regionSizeWords, int index) {

  reserved = MemRegion((HeapWord*) start, regionSizeWords);
  ContiguousSpace::initialize(reserved, true, false);
  liveData = 0;
  _is_in_collection_set = false;
  _region_number = index;
#ifdef ASSERT
  _mem_protection_level = 1; // Off, level 1.
#endif
  return JNI_OK;
}

int ShenandoahHeapRegion::region_number() const {
  return _region_number;
}

bool ShenandoahHeapRegion::rollback_allocation(uint size) {
  set_top(top() - size);
  return true;
}

void ShenandoahHeapRegion::clearLiveData() {
  setLiveData(0);
}

void ShenandoahHeapRegion::setLiveData(size_t s) {
  Atomic::store_ptr(s, (intptr_t*) &liveData);
}

size_t ShenandoahHeapRegion::getLiveData() const {
  return liveData;
}

size_t ShenandoahHeapRegion::garbage() const {
  assert(used() >= getLiveData() || is_humongous(), err_msg("Live Data must be a subset of used() live: "SIZE_FORMAT" used: "SIZE_FORMAT, getLiveData(), used()));
  size_t result = used() - getLiveData();
  return result;
}

bool ShenandoahHeapRegion::is_in_collection_set() const {
  return _is_in_collection_set;
}

#include <sys/mman.h>

#ifdef ASSERT

void ShenandoahHeapRegion::memProtectionOn() {
  /*
  tty->print_cr("protect memory on region level: "INT32_FORMAT, _mem_protection_level);
  print(tty);
  */
  MutexLockerEx ml(ShenandoahMemProtect_lock, true);
  assert(_mem_protection_level >= 1, "invariant");

  if (--_mem_protection_level == 0) {
    if (ShenandoahVerifyWritesToFromSpace) {
      assert(! ShenandoahVerifyReadsToFromSpace, "can't verify from-space reads when verifying from-space writes");
      os::protect_memory((char*) bottom(), end() - bottom(), os::MEM_PROT_READ);
    } else {
      assert(ShenandoahVerifyReadsToFromSpace, "need to be verifying reads here");
      assert(! ShenandoahConcurrentEvacuation, "concurrent evacuation needs to be turned off for verifying from-space-reads");
      os::protect_memory((char*) bottom(), end() - bottom(), os::MEM_PROT_NONE);
    }
  }
}

void ShenandoahHeapRegion::memProtectionOff() {
  /*
  tty->print_cr("unprotect memory on region level: "INT32_FORMAT, _mem_protection_level);
  print(tty);
  */
  MutexLockerEx ml(ShenandoahMemProtect_lock, true);
  assert(_mem_protection_level >= 0, "invariant");
  if (_mem_protection_level++ == 0) {
    os::protect_memory((char*) bottom(), end() - bottom(), os::MEM_PROT_RW);
  }
}

#endif

void ShenandoahHeapRegion::set_is_in_collection_set(bool b) {
  assert(! (is_humongous() && b), "never ever enter a humongous region into the collection set");

  _is_in_collection_set = b;

  if (b) {
    // tty->print_cr("registering region in fast-cset");
    // print();
    ShenandoahHeap::heap()->register_region_with_in_cset_fast_test(this);
  }

#ifdef ASSERT
  if (ShenandoahVerifyWritesToFromSpace || ShenandoahVerifyReadsToFromSpace) {
    if (b) {
      memProtectionOn();
      assert(_mem_protection_level == 0, "need to be protected here");
    } else {
      assert(_mem_protection_level == 0, "need to be protected here");
      memProtectionOff();
    }
  }
#endif
}

ByteSize ShenandoahHeapRegion::is_in_collection_set_offset() {
  return byte_offset_of(ShenandoahHeapRegion, _is_in_collection_set);
}

void ShenandoahHeapRegion::print_on(outputStream* st) const {
  st->print_cr("ShenandoahHeapRegion: "PTR_FORMAT"/"INT32_FORMAT, p2i(this), _region_number);

  if (is_in_collection_set())
    st->print("C");
  if (is_humongous_start()) {
    st->print("H");
  }
  if (is_humongous_continuation()) {
    st->print("h");
  }
  //else
    st->print(" ");

  st->print_cr("live = "SIZE_FORMAT" garbage = "SIZE_FORMAT" bottom = "PTR_FORMAT" end = "PTR_FORMAT" top = "PTR_FORMAT,
               getLiveData(), garbage(), p2i(bottom()), p2i(end()), p2i(top()));
}


class SkipUnreachableObjectToOopClosure: public ObjectClosure {
  ExtendedOopClosure* _cl;
  bool _skip_unreachable_objects;
  ShenandoahHeap* _heap;

public:
  SkipUnreachableObjectToOopClosure(ExtendedOopClosure* cl, bool skip_unreachable_objects) :
    _cl(cl), _skip_unreachable_objects(skip_unreachable_objects), _heap(ShenandoahHeap::heap()) {}

  void do_object(oop obj) {

    if ((! _skip_unreachable_objects) || _heap->is_marked_current(obj)) {
      if (_skip_unreachable_objects) {
        assert(_heap->is_marked_current(obj), "obj must be live");
      }
      obj->oop_iterate(_cl);
    }

  }
};

void ShenandoahHeapRegion::object_iterate_interruptible(ObjectClosure* blk, bool allow_cancel) {
  HeapWord* p = bottom() + BrooksPointer::BROOKS_POINTER_OBJ_SIZE;
  ShenandoahHeap* heap = ShenandoahHeap::heap();
  while (p < top() && !(allow_cancel && heap->cancelled_concgc())) {
    blk->do_object(oop(p));
#ifdef ASSERT
    if (ShenandoahVerifyReadsToFromSpace) {
      memProtectionOff();
      p += oop(p)->size() + BrooksPointer::BROOKS_POINTER_OBJ_SIZE;
      memProtectionOn();
    } else {
      p += oop(p)->size() + BrooksPointer::BROOKS_POINTER_OBJ_SIZE;
    }
#else
      p += oop(p)->size() + BrooksPointer::BROOKS_POINTER_OBJ_SIZE;
#endif
  }
}

HeapWord* ShenandoahHeapRegion::object_iterate_careful(ObjectClosureCareful* blk) {
  HeapWord * limit = concurrent_iteration_safe_limit();
  assert(limit <= top(), "sanity check");
  for (HeapWord* p = bottom() + BrooksPointer::BROOKS_POINTER_OBJ_SIZE; p < limit;) {
    size_t size = blk->do_object_careful(oop(p));
    if (size == 0) {
      return p;  // failed at p
    } else {
      p += size + BrooksPointer::BROOKS_POINTER_OBJ_SIZE;
    }
  }
  return NULL; // all done
}

void ShenandoahHeapRegion::oop_iterate_skip_unreachable(ExtendedOopClosure* cl, bool skip_unreachable_objects) {
  SkipUnreachableObjectToOopClosure cl2(cl, skip_unreachable_objects);
  object_iterate_interruptible(&cl2, false);
}

void ShenandoahHeapRegion::fill_region() {
  ShenandoahHeap* sh = (ShenandoahHeap*) Universe::heap();

  if (free() > (BrooksPointer::BROOKS_POINTER_OBJ_SIZE + CollectedHeap::min_fill_size())) {
    HeapWord* filler = allocate(BrooksPointer::BROOKS_POINTER_OBJ_SIZE);
    HeapWord* obj = allocate(end() - top());
    sh->fill_with_object(obj, end() - obj);
    sh->initialize_brooks_ptr(filler, obj);
  }
}

void ShenandoahHeapRegion::set_humongous_start(bool start) {
  _humongous_start = start;
}

void ShenandoahHeapRegion::set_humongous_continuation(bool continuation) {
  _humongous_continuation = continuation;
}

bool ShenandoahHeapRegion::is_humongous() const {
  return _humongous_start || _humongous_continuation;
}

bool ShenandoahHeapRegion::is_humongous_start() const {
  return _humongous_start;
}

bool ShenandoahHeapRegion::is_humongous_continuation() const {
  return _humongous_continuation;
}

void ShenandoahHeapRegion::do_reset() {
  ContiguousSpace::initialize(reserved, true, false);
  clearLiveData();
  _humongous_start = false;
  _humongous_continuation = false;
}

void ShenandoahHeapRegion::recycle() {
  do_reset();
  set_is_in_collection_set(false);
}

void ShenandoahHeapRegion::reset() {
  assert(_mem_protection_level == 1, "needs to be unprotected here");
  do_reset();
  _is_in_collection_set = false;
}

HeapWord* ShenandoahHeapRegion::block_start_const(const void* p) const {
  assert(MemRegion(bottom(), end()).contains(p),
         err_msg("p ("PTR_FORMAT") not in space ["PTR_FORMAT", "PTR_FORMAT")",
                 p2i(p), p2i(bottom()), p2i(end())));
  if (p >= top()) {
    return top();
  } else {
    HeapWord* last = bottom() + BrooksPointer::BROOKS_POINTER_OBJ_SIZE;
    HeapWord* cur = last;
    while (cur <= p) {
      last = cur;
      cur += oop(cur)->size() + BrooksPointer::BROOKS_POINTER_OBJ_SIZE;
    }
    assert(oop(last)->is_oop(),
           err_msg(PTR_FORMAT" should be an object start", p2i(last)));
    return last;
  }
}

void ShenandoahHeapRegion::setup_heap_region_size(size_t initial_heap_size, size_t max_heap_size) {
  uintx region_size = ShenandoahHeapRegionSize;
  if (FLAG_IS_DEFAULT(ShenandoahHeapRegionSize)) {
    size_t average_heap_size = (initial_heap_size + max_heap_size) / 2;
    region_size = MAX2(average_heap_size / HeapRegionBounds::target_number(),
                       (uintx) HeapRegionBounds::min_size());
  }

  int region_size_log = log2_long((jlong) region_size);
  // Recalculate the region size to make sure it's a power of
  // 2. This means that region_size is the largest power of 2 that's
  // <= what we've calculated so far.
  region_size = ((uintx)1 << region_size_log);

  // Now make sure that we don't go over or under our limits.
  if (region_size < HeapRegionBounds::min_size()) {
    region_size = HeapRegionBounds::min_size();
  } else if (region_size > HeapRegionBounds::max_size()) {
    region_size = HeapRegionBounds::max_size();
  }

  // And recalculate the log.
  region_size_log = log2_long((jlong) region_size);

  // Now, set up the globals.
  guarantee(RegionSizeShift == 0, "we should only set it once");
  RegionSizeShift = region_size_log;

  guarantee(RegionSizeBytes == 0, "we should only set it once");
  RegionSizeBytes = (size_t)region_size;

  if (ShenandoahLogConfig) {
    tty->print_cr("Region size in bytes: "SIZE_FORMAT, RegionSizeBytes);
    tty->print_cr("Region size shift: "SIZE_FORMAT, RegionSizeShift);
    tty->print_cr("Initial number of regions: "SIZE_FORMAT, initial_heap_size / RegionSizeBytes);
    tty->print_cr("Maximum number of regions: "SIZE_FORMAT, max_heap_size / RegionSizeBytes);
  }
}

CompactibleSpace* ShenandoahHeapRegion::next_compaction_space() const {
  return ShenandoahHeap::heap()->next_compaction_region(this);
}

void ShenandoahHeapRegion::prepare_for_compaction(CompactPoint* cp) {
  SCAN_AND_FORWARD(cp, scan_limit, scanned_block_is_obj, scanned_block_size);
}

void ShenandoahHeapRegion::adjust_pointers() {
  // Check first is there is any work to do.
  if (used() == 0) {
    return;   // Nothing to do.
  }

  SCAN_AND_ADJUST_POINTERS(adjust_obj_size);
}

void ShenandoahHeapRegion::compact() {
  SCAN_AND_COMPACT(obj_size);
}

void ShenandoahHeapRegion::init_top_at_mark_start() {
  _top_at_mark_start = top();
}

HeapWord* ShenandoahHeapRegion::top_at_mark_start() {
  return _top_at_mark_start;
}
