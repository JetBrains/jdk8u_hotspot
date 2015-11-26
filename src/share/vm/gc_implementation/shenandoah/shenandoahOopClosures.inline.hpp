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

#ifndef SHARE_VM_GC_SHENANDOAH_SHENANDOAHOOPCLOSURES_INLINE_HPP
#define SHARE_VM_GC_SHENANDOAH_SHENANDOAHOOPCLOSURES_INLINE_HPP

#include "gc_implementation/shenandoah/shenandoahHeap.inline.hpp"
#include "gc_implementation/shenandoah/shenandoahConcurrentMark.inline.hpp"

inline void ShenandoahMarkUpdateRefsClosure::do_oop_nv(oop* p) {
  // We piggy-back reference updating to the marking tasks.
  oop obj = _heap->maybe_update_oop_ref(p);
  assert(obj == ShenandoahBarrierSet::resolve_oop_static(obj), "need to-space object here");
  if (! oopDesc::is_null(obj)) {
    ShenandoahConcurrentMark::mark_and_push(obj, _heap, _queue->queue());
  }
}

inline void ShenandoahMarkRefsClosure::do_oop_nv(oop* p) {
  oop obj = oopDesc::load_heap_oop(p);
  assert(obj == ShenandoahBarrierSet::resolve_oop_static(obj), "need to-space object here");

  if (! oopDesc::is_null(obj)) {
    ShenandoahConcurrentMark::mark_and_push(obj, _heap, _queue->queue());
  }
}

#endif // SHARE_VM_GC_SHENANDOAH_SHENANDOAHOOPCLOSURES_INLINE_HPP
