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

#ifndef SHARE_VM_GC_SHENANDOAH_SHENANDOAHHEAPREGIONSET_HPP
#define SHARE_VM_GC_SHENANDOAH_SHENANDOAHHEAPREGIONSET_HPP

#include "gc_implementation/shenandoah/shenandoahHeapRegion.hpp"


class ShenandoahHeapRegionSet : public CHeapObj<mtGC> {
private:
  ShenandoahHeapRegion** _regions;
  // current region to be returned from get_next()
  ShenandoahHeapRegion** _current;
  ShenandoahHeapRegion** _next;

  // last inserted region.
  ShenandoahHeapRegion** _next_free;
  ShenandoahHeapRegion** _concurrent_next_free;

  // Maximum size of the set.
  const size_t _max_regions;

  size_t _garbage_threshold;
  size_t _free_threshold;

  size_t _capacity;
  size_t _used;

  void choose_collection_set(ShenandoahHeapRegion** regions, size_t length);
  void choose_collection_set_min_garbage(ShenandoahHeapRegion** regions, size_t length, size_t min_garbage);
  void choose_free_set(ShenandoahHeapRegion** regions, size_t length);

public:
  ShenandoahHeapRegionSet(size_t max_regions);

  ShenandoahHeapRegionSet(size_t max_regions, ShenandoahHeapRegion** regions, size_t num_regions);

  ~ShenandoahHeapRegionSet();

  void set_garbage_threshold(size_t minimum_garbage) { _garbage_threshold = minimum_garbage;}
  void set_free_threshold(size_t minimum_free) { _free_threshold = minimum_free;}

  /**
   * Appends a region to the set. This is implemented to be concurrency-safe.
   */
  void append(ShenandoahHeapRegion* region);

  void clear();

  size_t length();
  size_t used_regions() {
    return _current - _regions;
  }
  size_t available_regions();
  void print();

  size_t garbage();
  size_t calculate_used() const;
  size_t live_data();
  size_t reclaimed() {return _reclaimed;}

  /**
   * Returns a pointer to the current region.
   */
   ShenandoahHeapRegion* current();

  /**
   * Gets the next region for allocation (from free-list).
   * If multiple threads are competing, one will succeed to
   * increment to the next region, the others will fail and return
   * the region that the succeeding thread got.
   */
  ShenandoahHeapRegion* get_next();

  /**
   * Claims next region for processing. This is implemented to be concurrency-safe.
   */
  ShenandoahHeapRegion* claim_next();

  void choose_collection_and_free_sets(ShenandoahHeapRegionSet* col_set, ShenandoahHeapRegionSet* free_set);
  void choose_collection_and_free_sets_min_garbage(ShenandoahHeapRegionSet* col_set, ShenandoahHeapRegionSet* free_set, size_t min_garbage);

  // Check for unreachable humongous regions and reclaim them.
  void reclaim_humongous_regions();

  void set_concurrent_iteration_safe_limits();

  void decrease_available(size_t num_bytes);

  size_t capacity() const;
  size_t used() const;

private:
  void reclaim_humongous_region_at(ShenandoahHeapRegion** r);

  ShenandoahHeapRegion** limit_region(ShenandoahHeapRegion** region);
  size_t _reclaimed;

};

#endif //SHARE_VM_GC_SHENANDOAH_SHENANDOAHHEAPREGIONSET_HPP
