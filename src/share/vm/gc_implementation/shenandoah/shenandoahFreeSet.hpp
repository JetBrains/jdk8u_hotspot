
/*
 * Copyright (c) 2016, Red Hat, Inc. and/or its affiliates.
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

#ifndef SHARE_VM_GC_SHENANDOAH_SHENANDOAHFREESET_HPP
#define SHARE_VM_GC_SHENANDOAH_SHENANDOAHFREESET_HPP

#include "gc_implementation/shenandoah/shenandoahHeapRegionSet.hpp"

class ShenandoahFreeSet : public CHeapObj<mtGC> {
private:
  ShenandoahHeapRegion** _regions;
  size_t _active_end;
  size_t _reserved_end;
  size_t _current;
  size_t _capacity;
  size_t _used;

  void assert_heaplock_owned_by_current_thread() const PRODUCT_RETURN;

public:
  ShenandoahFreeSet(size_t max_regions);
  ~ShenandoahFreeSet();

  void add_region(ShenandoahHeapRegion* r);
  void clear();

  size_t capacity() const { return _capacity; }
  size_t used()     const { return _used;     }
  size_t count()    const { return _active_end - _current; }

  void increase_used(size_t amount);

  // Regular allocation:
  ShenandoahHeapRegion* current_no_humongous() const;
  ShenandoahHeapRegion* next_no_humongous();
  size_t unsafe_peek_free() const;

  // Humongous allocation:
  ShenandoahHeapRegion* allocate_contiguous(size_t num);

  void print_on(outputStream* out) const;
};

#endif //SHARE_VM_GC_SHENANDOAH_SHENANDOAHFREESET_HPP
