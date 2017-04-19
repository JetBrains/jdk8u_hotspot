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

#ifndef SHARE_VM_GC_SHENANDOAH_SHENANDOAHHEAPREGION_INLINE_HPP
#define SHARE_VM_GC_SHENANDOAH_SHENANDOAHHEAPREGION_INLINE_HPP

#include "gc_implementation/shenandoah/shenandoahHeapRegion.hpp"
#include "runtime/atomic.hpp"

inline void ShenandoahHeapRegion::increase_live_data_words(size_t s) {
  assert (s <= (size_t)max_jint, "sanity");
  increase_live_data_words((jint)s);
}

inline void ShenandoahHeapRegion::increase_live_data_words(jint s) {
  jint new_live_data = Atomic::add(s, &_live_data);
#ifdef ASSERT
  size_t live_bytes = (size_t)(new_live_data * HeapWordSize);
  size_t used_bytes = used();
  assert(live_bytes <= used_bytes || is_humongous(),
         err_msg("can't have more live data than used: " SIZE_FORMAT ", " SIZE_FORMAT, live_bytes, used_bytes));
#endif
}

#endif // SHARE_VM_GC_SHENANDOAH_SHENANDOAHHEAPREGION_INLINE_HPP
