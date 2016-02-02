
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

class ShenandoahFreeSet : public ShenandoahHeapRegionSet {

private:
  size_t _capacity;
  size_t _used;

  size_t _write_index;

  bool is_contiguous(size_t start, size_t num);
  size_t find_contiguous(size_t num, size_t start);
  void push_back_regions(size_t start, size_t end);
  void initialize_humongous_regions(size_t first, size_t num);

public:
  ShenandoahFreeSet(size_t max_regions);
  ~ShenandoahFreeSet();
  void add_region(ShenandoahHeapRegion* r);
  void par_add_region(ShenandoahHeapRegion* r);

  size_t claim_next(size_t current);

  size_t capacity();
  ShenandoahHeapRegion* claim_contiguous(size_t num);
  void clear();

  size_t used();

  void increase_used(size_t amount);
};

#endif //SHARE_VM_GC_SHENANDOAH_SHENANDOAHFREESET_HPP
