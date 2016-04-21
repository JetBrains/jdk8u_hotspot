/*
 * Copyright (c) 2015, Red Hat, Inc. and/or its affiliates.
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

#ifndef SHARE_VM_GC_SHENANDOAH_SHENANDOAHHEAP_INLINE_HPP
#define SHARE_VM_GC_SHENANDOAH_SHENANDOAHHEAP_INLINE_HPP

#include "memory/threadLocalAllocBuffer.inline.hpp"
#include "gc_implementation/shenandoah/brooksPointer.inline.hpp"
#include "gc_implementation/g1/concurrentMark.inline.hpp"
#include "gc_implementation/shenandoah/shenandoahBarrierSet.inline.hpp"
#include "gc_implementation/shenandoah/shenandoahHeap.hpp"
#include "gc_implementation/shenandoah/shenandoahHeapRegionSet.hpp"
#include "gc_implementation/shenandoah/shenandoahHeapRegion.inline.hpp"
#include "oops/oop.inline.hpp"
#include "runtime/atomic.inline.hpp"
#include "utilities/copy.hpp"

/*
 * Marks the object. Returns true if the object has not been marked before and has
 * been marked by this thread. Returns false if the object has already been marked,
 * or if a competing thread succeeded in marking this object.
 */
inline bool ShenandoahHeap::mark_current(oop obj) const {
#ifdef ASSERT
  if (! oopDesc::unsafe_equals(obj, oopDesc::bs()->read_barrier(obj))) {
    tty->print_cr("heap region containing obj:");
    ShenandoahHeapRegion* obj_region = heap_region_containing(obj);
    obj_region->print();
    tty->print_cr("heap region containing forwardee:");
    ShenandoahHeapRegion* forward_region = heap_region_containing(oopDesc::bs()->read_barrier(obj));
    forward_region->print();
  }
#endif

  assert(oopDesc::unsafe_equals(obj, oopDesc::bs()->read_barrier(obj)), "only mark forwarded copy of objects");
  return mark_current_no_checks(obj);
}

inline bool ShenandoahHeap::mark_current_no_checks(oop obj) const {
  HeapWord* addr = (HeapWord*) obj;
  return (! allocated_after_mark_start(addr)) && _next_mark_bit_map->parMark(addr);
}

inline bool ShenandoahHeap::is_marked_current(oop obj) const {
  HeapWord* addr = (HeapWord*) obj;
  return allocated_after_mark_start(addr) || _next_mark_bit_map->isMarked(addr);
}

inline bool ShenandoahHeap::is_marked_current(oop obj, ShenandoahHeapRegion* r) const {
  HeapWord* addr = (HeapWord*) obj;
  return _next_mark_bit_map->isMarked(addr) || r->allocated_after_mark_start(addr);
}

inline bool ShenandoahHeap::is_marked_prev(oop obj) const {
  ShenandoahHeapRegion* r = heap_region_containing((void*) obj);
  return is_marked_prev(obj, r);
}

inline bool ShenandoahHeap::is_marked_prev(oop obj, const ShenandoahHeapRegion* r) const {
  HeapWord* addr = (HeapWord*) obj;
  return _prev_mark_bit_map->isMarked(addr) || r->allocated_after_prev_mark_start(addr);
}

inline bool ShenandoahHeap::need_update_refs() const {
  return _need_update_refs;
}

inline uint ShenandoahHeap::heap_region_index_containing(const void* addr) const {
  uintptr_t region_start = ((uintptr_t) addr);
  uintptr_t index = (region_start - (uintptr_t) _first_region_bottom) >> ShenandoahHeapRegion::RegionSizeShift;
#ifdef ASSERT
  if (!(index < _num_regions)) {
    tty->print_cr("heap region does not contain address, first_region_bottom: "PTR_FORMAT", real bottom of first region: "PTR_FORMAT", num_regions: "SIZE_FORMAT", region_size: "SIZE_FORMAT, p2i(_first_region_bottom), p2i(_ordered_regions->get(0)->bottom()), _num_regions, ShenandoahHeapRegion::RegionSizeBytes);
  }
#endif
  assert(index < _num_regions, "heap region index must be in range");
  return index;
}

inline ShenandoahHeapRegion* ShenandoahHeap::heap_region_containing(const void* addr) const {
  uint index = heap_region_index_containing(addr);
  ShenandoahHeapRegion* result = _ordered_regions->get(index);
#ifdef ASSERT
  if (!(addr >= result->bottom() && addr < result->end())) {
    tty->print_cr("heap region does not contain address, first_region_bottom: "PTR_FORMAT", real bottom of first region: "PTR_FORMAT", num_regions: "SIZE_FORMAT, p2i(_first_region_bottom), p2i(_ordered_regions->get(0)->bottom()), _num_regions);
  }
#endif
  assert(addr >= result->bottom() && addr < result->end(), "address must be in found region");
  return result;
}

inline oop ShenandoahHeap::update_oop_ref_not_null(oop* p, oop obj) {
  if (in_cset_fast_test((HeapWord*) obj)) {
    oop forw = ShenandoahBarrierSet::resolve_oop_static_not_null(obj);
    assert(! oopDesc::unsafe_equals(forw, obj) || is_full_gc_in_progress(), "expect forwarded object");
    obj = forw;
    assert(obj->is_oop(), "sanity");
    oopDesc::store_heap_oop(p, obj);
  }
#ifdef ASSERT
  else {
    assert(oopDesc::unsafe_equals(obj, ShenandoahBarrierSet::resolve_oop_static_not_null(obj)), "expect not forwarded");
  }
#endif
  return obj;
}

inline oop ShenandoahHeap::maybe_update_oop_ref(oop* p) {
  oop obj = oopDesc::load_heap_oop(p);
  if (! oopDesc::is_null(obj)) {
    return maybe_update_oop_ref_not_null(p, obj);
  } else {
    return obj;
  }
}

inline oop ShenandoahHeap::maybe_update_oop_ref_not_null(oop* p, oop heap_oop) {

  assert((! is_in(p)) || (! heap_region_containing(p)->is_in_collection_set())
         || is_full_gc_in_progress(),
         "never update refs in from-space, unless evacuation has been cancelled");

#ifdef ASSERT
  if (! is_in(heap_oop)) {
    print_heap_regions();
    tty->print_cr("object not in heap: "PTR_FORMAT", referenced by: "PTR_FORMAT, p2i((HeapWord*) heap_oop), p2i(p));
    assert(is_in(heap_oop), "object must be in heap");
  }
#endif
  assert(is_in(heap_oop), "only ever call this on objects in the heap");
  if (in_cset_fast_test((HeapWord*) heap_oop)) {
    oop forwarded_oop = ShenandoahBarrierSet::resolve_oop_static_not_null(heap_oop); // read brooks ptr
    assert(! oopDesc::unsafe_equals(forwarded_oop, heap_oop) || is_full_gc_in_progress(), "expect forwarded object");
    // tty->print_cr("updating old ref: "PTR_FORMAT" pointing to "PTR_FORMAT" to new ref: "PTR_FORMAT, p2i(p), p2i(heap_oop), p2i(forwarded_oop));
    assert(forwarded_oop->is_oop(), "oop required");
    assert(is_in(forwarded_oop), "forwardee must be in heap");
    assert(oopDesc::bs()->is_safe(forwarded_oop), "forwardee must not be in collection set");
    // If this fails, another thread wrote to p before us, it will be logged in SATB and the
    // reference be updated later.
    oop result = (oop) Atomic::cmpxchg_ptr(forwarded_oop, p, heap_oop);

    if (oopDesc::unsafe_equals(result, heap_oop)) { // CAS successful.
      return forwarded_oop;
    } else {
      return result;
    }
  } else {
    assert(oopDesc::unsafe_equals(heap_oop, ShenandoahBarrierSet::resolve_oop_static_not_null(heap_oop)), "expect not forwarded");
    return heap_oop;
  }
}

inline bool ShenandoahHeap::cancelled_concgc() const {
  bool cancelled = _cancelled_concgc;
  return cancelled;
}

inline HeapWord* ShenandoahHeap::allocate_from_gclab(Thread* thread, size_t size) {
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

inline void ShenandoahHeap::initialize_brooks_ptr(oop p) {
  BrooksPointer brooks_ptr = BrooksPointer::get(p);
  brooks_ptr.set_forwardee(p);
}

inline void ShenandoahHeap::copy_object(oop p, HeapWord* s, size_t words) {
  HeapWord* filler = s;
  assert(s != NULL, "allocation of brooks pointer must not fail");
  HeapWord* copy = s + BrooksPointer::BROOKS_POINTER_OBJ_SIZE;

  guarantee(copy != NULL, "allocation of copy object must not fail");
  Copy::aligned_disjoint_words((HeapWord*) p, copy, words);
  initialize_brooks_ptr(oop(copy));

#ifdef ASSERT
  if (ShenandoahTraceEvacuations) {
    tty->print_cr("copy object from "PTR_FORMAT" to: "PTR_FORMAT, p2i((HeapWord*) p), p2i(copy));
  }
#endif
}

inline oop ShenandoahHeap::evacuate_object(oop p, Thread* thread) {
  size_t required;

#ifdef ASSERT
  ShenandoahHeapRegion* hr;
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
  HeapWord* filler = allocate_from_gclab(thread, required);
  if (filler == NULL) {
    filler = allocate_memory(required, true);
    alloc_from_gclab = false;
  }

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
    copy_object(p, filler, required - BrooksPointer::BROOKS_POINTER_OBJ_SIZE);
    hr->memProtectionOn();
  } else {
    copy_object(p, filler, required - BrooksPointer::BROOKS_POINTER_OBJ_SIZE);
  }
#else
    copy_object(p, filler, required - BrooksPointer::BROOKS_POINTER_OBJ_SIZE);
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

inline bool ShenandoahHeap::requires_marking(const void* entry) const {
  return ! is_marked_current(oop(entry));
}

inline bool ShenandoahHeap::concurrent_mark_in_progress() {
  return _concurrent_mark_in_progress;
}


inline bool ShenandoahHeap::is_evacuation_in_progress() {
  return _evacuation_in_progress;
}

inline bool ShenandoahHeap::allocated_after_mark_start(HeapWord* addr) const {
  uintx index = ((uintx) addr) >> ShenandoahHeapRegion::RegionSizeShift;
  HeapWord* top_at_mark_start = _top_at_mark_starts[index];
  bool alloc_after_mark_start = addr >= top_at_mark_start;
#ifdef ASSERT
  ShenandoahHeapRegion* r = heap_region_containing(addr);
  assert(alloc_after_mark_start == r->allocated_after_mark_start(addr), "sanity");
#endif
  return alloc_after_mark_start;
}
#endif // SHARE_VM_GC_SHENANDOAH_SHENANDOAHHEAP_INLINE_HPP
