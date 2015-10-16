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

#ifndef SHARE_VM_GC_SHENANDOAH_SHENANDOAHBARRIERSET_INLINE_HPP
#define SHARE_VM_GC_SHENANDOAH_SHENANDOAHBARRIERSET_INLINE_HPP

#include "gc_implementation/shenandoah/shenandoahBarrierSet.hpp"
#include "gc_implementation/shenandoah/shenandoahHeap.inline.hpp"

inline oop ShenandoahBarrierSet::get_shenandoah_forwardee_helper(oop p) {
  assert(UseShenandoahGC, "must only be called when Shenandoah is used.");
  assert(Universe::heap()->is_in(p), "We shouldn't be calling this on objects not in the heap");
  oop forwardee;
#ifdef ASSERT
  if (ShenandoahVerifyReadsToFromSpace) {
    ShenandoahHeap* heap = (ShenandoahHeap *) Universe::heap();
    ShenandoahHeapRegion* region = heap->heap_region_containing(p);
    {
      region->memProtectionOff();
      forwardee = oop( *((HeapWord**) ((HeapWord*) p) - 1));
      region->memProtectionOn();
    }
  } else {
    forwardee = oop( *((HeapWord**) ((HeapWord*) p) - 1));
  }
#else
  forwardee = oop( *((HeapWord**) ((HeapWord*) p) - 1));
#endif
  return forwardee;
}

inline oop ShenandoahBarrierSet::resolve_oop_static_not_null(oop p) {
  assert(p != NULL, "Must be NULL checked");

  oop result = get_shenandoah_forwardee_helper(p);

  if (result != NULL) {
#ifdef ASSERT
    if (result != p) {
      oop second_forwarding = get_shenandoah_forwardee_helper(result);

      // We should never be forwarded more than once.
      if (result != second_forwarding) {
        ShenandoahHeap* sh = (ShenandoahHeap*) Universe::heap();
        tty->print("first reference "PTR_FORMAT" is in heap region:\n", p2i((HeapWord*) p));
        sh->heap_region_containing(p)->print();
        tty->print("first_forwarding "PTR_FORMAT" is in heap region:\n", p2i((HeapWord*) result));
        sh->heap_region_containing(result)->print();
        tty->print("final reference "PTR_FORMAT" is in heap region:\n", p2i((HeapWord*) second_forwarding));
        sh->heap_region_containing(second_forwarding)->print();
        assert(get_shenandoah_forwardee_helper(result) == result, "Only one fowarding per customer");
      }
    }
#endif
    if (! ShenandoahVerifyReadsToFromSpace) {
      // is_oop() would trigger a SEGFAULT when we're checking from-space-access.
      assert(ShenandoahHeap::heap()->is_in(result) && result->is_oop(), "resolved oop must be a valid oop in the heap");
    }
  }
  return result;
}

inline oop ShenandoahBarrierSet::resolve_oop_static(oop p) {
  if (((HeapWord*) p) != NULL) {
    return resolve_oop_static_not_null(p);
  } else {
    return p;
  }
}

inline oop ShenandoahBarrierSet::resolve_oop_static_no_check(oop p) {
  if (((HeapWord*) p) != NULL) {
    return get_shenandoah_forwardee_helper(p);
  } else {
    return p;
  }
}

template <class T>
inline oop ShenandoahBarrierSet::resolve_and_update_oop_static(T p, oop obj) {
  oop forw = ShenandoahBarrierSet::resolve_oop_static_not_null(obj);
  if (forw != obj) {
    obj = forw;
    oopDesc::encode_store_heap_oop_not_null(p, obj);
  }
  return obj;
}

#endif //SHARE_VM_GC_SHENANDOAH_SHENANDOAHBARRIERSET_INLINE_HPP
