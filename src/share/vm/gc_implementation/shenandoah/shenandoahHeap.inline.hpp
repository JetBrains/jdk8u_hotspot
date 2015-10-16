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

#include "gc_implementation/g1/concurrentMark.inline.hpp"
#include "gc_implementation/shenandoah/shenandoahBarrierSet.inline.hpp"
#include "gc_implementation/shenandoah/shenandoahHeap.hpp"
#include "gc_implementation/shenandoah/shenandoahHeapRegion.inline.hpp"
#include "runtime/atomic.inline.hpp"

/*
 * Marks the object. Returns true if the object has not been marked before and has
 * been marked by this thread. Returns false if the object has already been marked,
 * or if a competing thread succeeded in marking this object.
 */
inline bool ShenandoahHeap::mark_current(oop obj) const {
#ifdef ASSERT
  if (obj != oopDesc::bs()->read_barrier(obj)) {
    tty->print_cr("heap region containing obj:");
    ShenandoahHeapRegion* obj_region = heap_region_containing(obj);
    obj_region->print();
    tty->print_cr("heap region containing forwardee:");
    ShenandoahHeapRegion* forward_region = heap_region_containing(oopDesc::bs()->read_barrier(obj));
    forward_region->print();
  }
#endif

  assert(obj == oopDesc::bs()->read_barrier(obj), "only mark forwarded copy of objects");
  return mark_current_no_checks(obj);
}

inline bool ShenandoahHeap::mark_current_no_checks(oop obj) const {
  ShenandoahHeapRegion* r = heap_region_containing(obj);
  HeapWord* addr = (HeapWord*) obj;
  return (! r->allocated_after_mark_start(addr)) && _next_mark_bit_map->parMark(addr);
}

inline bool ShenandoahHeap::is_marked_current(oop obj) const {
  ShenandoahHeapRegion* r = heap_region_containing((void*) obj);
  return is_marked_current(obj, r);
}

inline bool ShenandoahHeap::is_marked_current(oop obj, ShenandoahHeapRegion* r) const {
  HeapWord* addr = (HeapWord*) obj;
  return _next_mark_bit_map->isMarked(addr) || r->allocated_after_mark_start(addr);
}

inline bool ShenandoahHeap::need_update_refs() const {
  return _need_update_refs;
}

inline uint ShenandoahHeap::heap_region_index_containing(const void* addr) const {
  uintptr_t region_start = ((uintptr_t) addr);
  uintptr_t index = (region_start - (uintptr_t) _first_region_bottom) >> ShenandoahHeapRegion::RegionSizeShift;
#ifdef ASSERT
  if (!(index < _num_regions)) {
    tty->print_cr("heap region does not contain address, first_region_bottom: "PTR_FORMAT", real bottom of first region: "PTR_FORMAT", num_regions: "SIZE_FORMAT", region_size: "SIZE_FORMAT, p2i(_first_region_bottom), p2i(_ordered_regions[0]->bottom()), _num_regions, ShenandoahHeapRegion::RegionSizeBytes);
  }
#endif
  assert(index < _num_regions, "heap region index must be in range");
  return index;
}

inline ShenandoahHeapRegion* ShenandoahHeap::heap_region_containing(const void* addr) const {
  uint index = heap_region_index_containing(addr);
  ShenandoahHeapRegion* result = _ordered_regions[index];
#ifdef ASSERT
  if (!(addr >= result->bottom() && addr < result->end())) {
    tty->print_cr("heap region does not contain address, first_region_bottom: "PTR_FORMAT", real bottom of first region: "PTR_FORMAT", num_regions: "SIZE_FORMAT, p2i(_first_region_bottom), p2i(_ordered_regions[0]->bottom()), _num_regions);
  }
#endif
  assert(addr >= result->bottom() && addr < result->end(), "address must be in found region");
  return result;
}


oop ShenandoahHeap::maybe_update_oop_ref(oop* p) {

  assert((! is_in(p)) || (! heap_region_containing(p)->is_in_collection_set()),
         "never update refs in from-space, unless evacuation has been cancelled");

  oop heap_oop = oopDesc::load_heap_oop(p); // read p
  if (! oopDesc::is_null(heap_oop)) {

#ifdef ASSERT
    if (! is_in(heap_oop)) {
      print_heap_regions();
      tty->print_cr("object not in heap: "PTR_FORMAT", referenced by: "PTR_FORMAT, p2i((HeapWord*) heap_oop), p2i(p));
      assert(is_in(heap_oop), "object must be in heap");
    }
#endif
    assert(is_in(heap_oop), "only ever call this on objects in the heap");
    assert((! (is_in(p) && heap_region_containing(p)->is_in_collection_set())), "we don't want to update references in from-space");
    oop forwarded_oop = ShenandoahBarrierSet::resolve_oop_static_not_null(heap_oop); // read brooks ptr
    if (forwarded_oop != heap_oop) {
      // tty->print_cr("updating old ref: "PTR_FORMAT" pointing to "PTR_FORMAT" to new ref: "PTR_FORMAT, p2i(p), p2i(heap_oop), p2i(forwarded_oop));
      assert(forwarded_oop->is_oop(), "oop required");
      assert(is_in(forwarded_oop), "forwardee must be in heap");
      assert(! heap_region_containing(forwarded_oop)->is_in_collection_set(), "forwardee must not be in collection set");
      // If this fails, another thread wrote to p before us, it will be logged in SATB and the
      // reference be updated later.
      oop result = (oop) Atomic::cmpxchg_ptr(forwarded_oop, p, heap_oop);

      if (result == heap_oop) { // CAS successful.
          return forwarded_oop;
      } else {
        return result;
      }
    } else {
      return forwarded_oop;
    }
    /*
      else {
      tty->print_cr("not updating ref: "PTR_FORMAT, p2i(heap_oop));
      }
    */
  }
  return NULL;
}

#endif // SHARE_VM_GC_SHENANDOAH_SHENANDOAHHEAP_INLINE_HPP
