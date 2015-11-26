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

#include "gc_implementation/shenandoah/brooksPointer.hpp"
#include "gc_implementation/shenandoah/shenandoahHeap.inline.hpp"
#include "gc_implementation/shenandoah/shenandoahHeapRegion.hpp"
#include "gc_implementation/shenandoah/shenandoahHeapRegionSet.hpp"
#include "gc_implementation/shenandoah/shenandoahHumongous.hpp"
#include "memory/resourceArea.hpp"
#include "utilities/quickSort.hpp"

ShenandoahHeapRegionSet::ShenandoahHeapRegionSet(size_t max_regions) :
  _max_regions(max_regions),
  _regions(NEW_C_HEAP_ARRAY(ShenandoahHeapRegion*, max_regions, mtGC)),
  _garbage_threshold(ShenandoahHeapRegion::RegionSizeBytes / 2),
  _free_threshold(ShenandoahHeapRegion::RegionSizeBytes / 2),
  _capacity(0), _used(0)
{

  _next = &_regions[0];
  _current = NULL;
  _next_free = &_regions[0];
}

ShenandoahHeapRegionSet::ShenandoahHeapRegionSet(size_t max_regions, ShenandoahHeapRegion** regions, size_t num_regions) :
  _max_regions(num_regions),
  _regions(NEW_C_HEAP_ARRAY(ShenandoahHeapRegion*, max_regions, mtGC)),
  _garbage_threshold(ShenandoahHeapRegion::RegionSizeBytes / 2),
  _free_threshold(ShenandoahHeapRegion::RegionSizeBytes / 2),
  _capacity(0), _used(0)
{

  // Make copy of the regions array so that we can sort without destroying the original.
  memcpy(_regions, regions, sizeof(ShenandoahHeapRegion*) * num_regions);

  _next = &_regions[0];
  _current = NULL;
  _next_free = &_regions[num_regions];
}

ShenandoahHeapRegionSet::~ShenandoahHeapRegionSet() {
  FREE_C_HEAP_ARRAY(ShenandoahHeapRegion*, _regions, mtGC);
}

int compareHeapRegionsByGarbage(ShenandoahHeapRegion* a, ShenandoahHeapRegion* b) {
  if (a == NULL) {
    if (b == NULL) {
      return 0;
    } else {
      return 1;
    }
  } else if (b == NULL) {
    return -1;
  }

  size_t garbage_a = a->garbage();
  size_t garbage_b = b->garbage();

  if (garbage_a > garbage_b)
    return -1;
  else if (garbage_a < garbage_b)
    return 1;
  else return 0;
}

ShenandoahHeapRegion* ShenandoahHeapRegionSet::current() {
  ShenandoahHeapRegion** current = _current;
  if (current == NULL) {
    return get_next();
  } else {
    return *(limit_region(current));
  }
}

size_t ShenandoahHeapRegionSet::length() {
  return _next_free - _regions;
}

size_t ShenandoahHeapRegionSet::available_regions() {
  return (_regions + _max_regions) - _next_free;
}

void ShenandoahHeapRegionSet::append(ShenandoahHeapRegion* region) {
  assert(_next_free < _regions + _max_regions, "need space for additional regions");
  assert(SafepointSynchronize::is_at_safepoint() || ShenandoahHeap_lock->owned_by_self() || ! Universe::is_fully_initialized(), "only append regions to list while world is stopped");

  // Grab next slot.
  ShenandoahHeapRegion** next_free = _next_free;
  _next_free++;

  // Insert new region into slot.
  *next_free = region;

  _capacity += region->free();
  assert(_used <= _capacity, err_msg("must not use more than we have: used: "SIZE_FORMAT", capacity: "SIZE_FORMAT, _used, _capacity));
}

void ShenandoahHeapRegionSet::clear() {
  _current = NULL;
  _next = _regions;
  _next_free = _regions;
  _capacity = 0;
  _used = 0;
}

ShenandoahHeapRegion* ShenandoahHeapRegionSet::claim_next() {
  ShenandoahHeapRegion** next = (ShenandoahHeapRegion**) Atomic::add_ptr(sizeof(ShenandoahHeapRegion**), &_next);
  next--;
  if (next < _next_free) {
    return *next;
  } else {
    return NULL;
  }
}

ShenandoahHeapRegion* ShenandoahHeapRegionSet::get_next() {

  ShenandoahHeapRegion** next = _next;
  if (next < _next_free) {
    _current = next;
    _next++;
    return *next;
  } else {
    return NULL;
  }
}

ShenandoahHeapRegion** ShenandoahHeapRegionSet::limit_region(ShenandoahHeapRegion** region) {
  if (region >= _next_free) {
    return NULL;
  } else {
    return region;
  }
}

void ShenandoahHeapRegionSet::print() {
  for (ShenandoahHeapRegion** i = _regions; i < _next_free; i++) {
    if (i == _current) {
      tty->print_cr("C->");
    }
    if (i == _next) {
      tty->print_cr("N->");
    }
    (*i)->print();
  }
}

void ShenandoahHeapRegionSet::choose_collection_and_free_sets(ShenandoahHeapRegionSet* col_set, ShenandoahHeapRegionSet* free_set) {
  col_set->choose_collection_set(_regions, length());
  free_set->choose_free_set(_regions, length());
  //  assert(col_set->length() > 0 && free_set->length() > 0, "Better have some regions in the collection and free sets");

}

void ShenandoahHeapRegionSet::choose_collection_and_free_sets_min_garbage(ShenandoahHeapRegionSet* col_set, ShenandoahHeapRegionSet* free_set, size_t min_garbage) {
  col_set->choose_collection_set_min_garbage(_regions, length(), min_garbage);
  free_set->choose_free_set(_regions, length());
  //  assert(col_set->length() > 0 && free_set->length() > 0, "Better have some regions in the collection and free sets");
}

void ShenandoahHeapRegionSet::choose_collection_set(ShenandoahHeapRegion** regions, size_t length) {

  clear();

  assert(length <= _max_regions, "must not blow up array");

  ShenandoahHeapRegion** tmp = NEW_C_HEAP_ARRAY(ShenandoahHeapRegion*, length, mtGC);

  memcpy(tmp, regions, sizeof(ShenandoahHeapRegion*) * length);

  QuickSort::sort<ShenandoahHeapRegion*>(tmp, length, compareHeapRegionsByGarbage, false);

  ShenandoahHeapRegion** r = tmp;
  ShenandoahHeapRegion** end = tmp + length;

  // We don't want the current allocation region in the collection set because a) it is still being allocated into and b) This is where the write barriers will allocate their copies.

  while (r < end) {
    ShenandoahHeapRegion* region = *r;
    if (region->garbage() > _garbage_threshold && ! region->is_humongous()) {
      //      tty->print("choose region %d with garbage = " SIZE_FORMAT " and live = " SIZE_FORMAT " and _garbage_threshold = " SIZE_FORMAT "\n",
      //                 region->region_number(), region->garbage(), region->getLiveData(), _garbage_threshold);

      assert(! region->is_humongous(), "no humongous regions in collection set");

      if (region->getLiveData() == 0) {
        // We can recycle it right away and put it in the free set.
        ShenandoahHeap::heap()->decrease_used(region->used());
        region->recycle();
      } else {
        append(region);
        region->set_is_in_collection_set(true);
      }
      //    } else {
      //      tty->print("rejected region %d with garbage = " SIZE_FORMAT " and live = " SIZE_FORMAT " and _garbage_threshold = " SIZE_FORMAT "\n",
      //                 region->region_number(), region->garbage(), region->getLiveData(), _garbage_threshold);
    }
    r++;
  }

  FREE_C_HEAP_ARRAY(ShenandoahHeapRegion*, tmp, mtGC);

}

void ShenandoahHeapRegionSet::choose_collection_set_min_garbage(ShenandoahHeapRegion** regions, size_t length, size_t min_garbage) {

  clear();

  assert(length <= _max_regions, "must not blow up array");

  ShenandoahHeapRegion** tmp = NEW_C_HEAP_ARRAY(ShenandoahHeapRegion*, length, mtGC);

  memcpy(tmp, regions, sizeof(ShenandoahHeapRegion*) * length);

  QuickSort::sort<ShenandoahHeapRegion*>(tmp, length, compareHeapRegionsByGarbage, false);

  ShenandoahHeapRegion** r = tmp;
  ShenandoahHeapRegion** end = tmp + length;

  // We don't want the current allocation region in the collection set because a) it is still being allocated into and b) This is where the write barriers will allocate their copies.

  size_t garbage = 0;
  while (r < end && garbage < min_garbage) {
    ShenandoahHeapRegion* region = *r;
    if (region->garbage() > _garbage_threshold && ! region->is_humongous()) {
      append(region);
      garbage += region->garbage();
      region->set_is_in_collection_set(true);
    }
    r++;
  }

  FREE_C_HEAP_ARRAY(ShenandoahHeapRegion*, tmp, mtGC);

  /*
  tty->print_cr("choosen region with "SIZE_FORMAT" garbage given "SIZE_FORMAT" min_garbage", garbage, min_garbage);
  */
}


void ShenandoahHeapRegionSet::choose_free_set(ShenandoahHeapRegion** regions, size_t length) {

  clear();
  ShenandoahHeapRegion** end = regions + length;

  for (ShenandoahHeapRegion** r = regions; r < end; r++) {
    ShenandoahHeapRegion* region = *r;
    if ((! region->is_in_collection_set())
        && (! region->is_humongous())) {
      append(region);
    }
  }
}

void ShenandoahHeapRegionSet::reclaim_humongous_regions() {

  ShenandoahHeap* heap = ShenandoahHeap::heap();
  for (ShenandoahHeapRegion** r = _regions; r < _next_free; r++) {
    // We can immediately reclaim humongous objects/regions that are no longer reachable.
    ShenandoahHeapRegion* region = *r;
    if (region->is_humongous_start()) {
      oop humongous_obj = oop(region->bottom() + BrooksPointer::BROOKS_POINTER_OBJ_SIZE);
      if (! heap->is_marked_prev(humongous_obj)) {
        reclaim_humongous_region_at(r);
      }
    }
  }

}

void ShenandoahHeapRegionSet::reclaim_humongous_region_at(ShenandoahHeapRegion** r) {
  assert((*r)->is_humongous_start(), "reclaim regions starting with the first one");

  oop humongous_obj = oop((*r)->bottom() + BrooksPointer::BROOKS_POINTER_OBJ_SIZE);
  size_t size = humongous_obj->size() + BrooksPointer::BROOKS_POINTER_OBJ_SIZE;
  uint required_regions = ShenandoahHumongous::required_regions(size * HeapWordSize);

  if (ShenandoahTraceHumongous) {
    tty->print_cr("reclaiming "UINT32_FORMAT" humongous regions for object of size: "SIZE_FORMAT" words", required_regions, size);
  }

  assert((*r)->getLiveData() == 0, "liveness must be zero");

  for (ShenandoahHeapRegion** i = r; i < r + required_regions; i++) {
    ShenandoahHeapRegion* region = *i;

    assert(i == r ? region->is_humongous_start() : region->is_humongous_continuation(),
           "expect correct humongous start or continuation");

    if (ShenandoahTraceHumongous) {
      region->print();
    }

    region->reset();
    ShenandoahHeap::heap()->decrease_used(ShenandoahHeapRegion::RegionSizeBytes);
  }
}

void ShenandoahHeapRegionSet::set_concurrent_iteration_safe_limits() {
  for (ShenandoahHeapRegion** i = _regions; i < _next_free; i++) {
    ShenandoahHeapRegion* region = *i;
    region->set_concurrent_iteration_safe_limit(region->top());
  }
}

size_t ShenandoahHeapRegionSet::garbage() {
  size_t garbage = 0;
  for (ShenandoahHeapRegion** i = _regions; i < _next_free; i++) {
    ShenandoahHeapRegion* region = *i;
    garbage += region->garbage();
  }
  return garbage;
}

size_t ShenandoahHeapRegionSet::calculate_used() const {
  size_t used = 0;
  for (ShenandoahHeapRegion** i = _regions; i < _next_free; i++) {
    ShenandoahHeapRegion* region = *i;
    used += region->used();
  }
  return used;
}

size_t ShenandoahHeapRegionSet::live_data() {
  size_t live = 0;
  for (ShenandoahHeapRegion** i = _regions; i < _next_free; i++) {
    ShenandoahHeapRegion* region = *i;
    live += region->getLiveData();
  }
  return live;
}

void ShenandoahHeapRegionSet::decrease_available(size_t num_bytes) {
  assert(_used <= _capacity, "must not use more than we have");
  _used += num_bytes;
}

size_t ShenandoahHeapRegionSet::capacity() const {
  return _capacity;
}

size_t ShenandoahHeapRegionSet::used() const {
#ifdef ASSERT
  {
    MutexLockerEx ml(ShenandoahHeap_lock, true);
    assert(_capacity - _used <= ShenandoahHeap::heap()->capacity() - ShenandoahHeap::heap()->used(),
           err_msg("free-set-available must be smaller/equal heap-available, freeset-used: "SIZE_FORMAT", freeset-capacity: "SIZE_FORMAT", heap-used: "SIZE_FORMAT", heap-capacity: "SIZE_FORMAT,
                   _used, _capacity, ShenandoahHeap::heap()->used(), ShenandoahHeap::heap()->capacity()));
    assert(ShenandoahHeap::heap()->used() >= _used, err_msg("must not be > heap used: freeset-used: "SIZE_FORMAT", heap-used: "SIZE_FORMAT, _used, ShenandoahHeap::heap()->used()));
  }
#endif
  return _used;
}
