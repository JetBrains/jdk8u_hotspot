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

#ifndef SHARE_VM_GC_SHENANDOAH_BROOKSPOINTER_INLINE_HPP
#define SHARE_VM_GC_SHENANDOAH_BROOKSPOINTER_INLINE_HPP

#include "gc_implementation/shenandoah/brooksPointer.hpp"
#include "runtime/atomic.inline.hpp"

inline BrooksPointer BrooksPointer::get(oop obj) {
  HeapWord* hw_obj = (HeapWord*) obj;
  HeapWord* brooks_ptr = hw_obj - 1;
  // We know that the value in that memory location is a pointer to another
  // heapword/oop.
  return BrooksPointer((HeapWord**) brooks_ptr);
}

inline void BrooksPointer::set_forwardee(oop forwardee) {
  assert(ShenandoahHeap::heap()->is_in(forwardee), "forwardee must be valid oop in the heap");
  *_heap_word = (HeapWord*) forwardee;
#ifdef ASSERT
  if (ShenandoahTraceBrooksPointers) {
    tty->print_cr("setting_forwardee to "PTR_FORMAT" = "PTR_FORMAT, p2i((HeapWord*) forwardee), p2i(*_heap_word));
  }
#endif
}

inline HeapWord* BrooksPointer::cas_forwardee(HeapWord* old, HeapWord* forwardee) {
  assert(ShenandoahHeap::heap()->is_in(forwardee), "forwardee must point to a heap address");

  HeapWord* o = old;
  HeapWord* n = forwardee;
  HeapWord* result;

#ifdef ASSERT
  if (ShenandoahTraceBrooksPointers) {
    tty->print_cr("Attempting to CAS "PTR_FORMAT" value "PTR_FORMAT" from "PTR_FORMAT" to "PTR_FORMAT, p2i(_heap_word), p2i(*_heap_word), p2i(o), p2i(n));
  }
#endif

#ifdef ASSERT
  if (ShenandoahVerifyWritesToFromSpace || ShenandoahVerifyReadsToFromSpace) {
    ShenandoahHeap* sh = (ShenandoahHeap*) Universe::heap();
    ShenandoahHeapRegion* hr = sh->heap_region_containing(old);

    {
      hr->memProtectionOff();
      result =  (HeapWord*) (HeapWord*) Atomic::cmpxchg_ptr(n, _heap_word, o);
      hr->memProtectionOn();
    }
  } else {
    result =  (HeapWord*) (HeapWord*) Atomic::cmpxchg_ptr(n, _heap_word, o);
  }
#else
  result =  (HeapWord*) (HeapWord*) Atomic::cmpxchg_ptr(n, _heap_word, o);
#endif

#ifdef ASSERT
  if (ShenandoahTraceBrooksPointers) {
    tty->print_cr("Result of CAS from "PTR_FORMAT" to "PTR_FORMAT" was "PTR_FORMAT" read value was "PTR_FORMAT, p2i(o), p2i(n), p2i(result), p2i(*_heap_word));
  }
#endif

  return result;
}

#endif // SHARE_VM_GC_SHENANDOAH_BROOKSPOINTER_INLINE_HPP
