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

#include "gc_implementation/shenandoah/shenandoahFreeSet.hpp"
#include "gc_implementation/shenandoah/shenandoahHeap.hpp"
#include "gc_implementation/shenandoah/shenandoahHeapRegion.inline.hpp"
#include "runtime/atomic.inline.hpp"

ShenandoahFreeSet::ShenandoahFreeSet(size_t max_regions) :
  ShenandoahHeapRegionSet(max_regions),
  _write_index(0),
  _capacity(0),
  _used(0)
{
}

ShenandoahFreeSet::~ShenandoahFreeSet() {
}

void ShenandoahFreeSet::increase_used(size_t num_bytes) {
  assert(_used <= _capacity, "must not use more than we have");
  Atomic::add((jlong) num_bytes, (jlong*) &_used);
}

size_t ShenandoahFreeSet::used() {
  return _used;
}

size_t ShenandoahFreeSet::capacity() {
  return _capacity;
}

bool ShenandoahFreeSet::is_contiguous(size_t start, size_t num) {
  assert(_active_end <= _reserved_end, "sanity");

  ShenandoahHeapRegion* r1 = get(start);

  if (! r1->is_empty()) {
    return false;
  }

  for (size_t i = start + 1; i < start + num; i++) {
    // The modulo will take care of wrapping around.
    ShenandoahHeapRegion* r2 = get(i % _reserved_end);
    if (r2->region_number() != r1->region_number() + 1)
      return false;
    if (! r2->is_empty())
      return false;

    r1 = r2;
  }
  return true;
}

size_t ShenandoahFreeSet::find_contiguous(size_t start, size_t num) {

  assert(start < _reserved_end, "sanity");

  // The modulo will take care of wrapping around.
  for (size_t index = start; index != _active_end; index = (index + 1) % _reserved_end) {
    assert(index < _reserved_end, "sanity");
    if (is_contiguous(index, num)) {
      return index;
    }
  }
  return SIZE_MAX;
}

void ShenandoahFreeSet::push_back_regions(size_t start, size_t end) {
  for (size_t i = start; i != end; i = (i + 1) % _reserved_end) {
    ShenandoahHeapRegion* r = get(i);
    // We subtract the capacity here, and add it back in par_add_region.
    Atomic::add(- ((jlong)r->free()), (jlong*) &_capacity);
    par_add_region(get(i));
  }
}

void ShenandoahFreeSet::initialize_humongous_regions(size_t first, size_t num) {
  for (size_t i = 0; i < num; i++) {
    ShenandoahHeapRegion* current = get((first + i) % _reserved_end);
    if (i == 0)
      current->set_humongous_start(true);
    else
      current->set_humongous_continuation(true);

    current->set_top(current->end());
    current->increase_live_data(ShenandoahHeapRegion::RegionSizeBytes);
  }
  increase_used(ShenandoahHeapRegion::RegionSizeBytes * num);
  ShenandoahHeap::heap()->increase_used(ShenandoahHeapRegion::RegionSizeBytes * num);
}

ShenandoahHeapRegion* ShenandoahFreeSet::claim_contiguous(size_t num) {
  size_t current_idx = _current_index;
  size_t next = (current_idx + 1) % _reserved_end;
  while (next != _active_end) {
    size_t first = find_contiguous(next, num);
    if (first == SIZE_MAX) return NULL;
    size_t next_current = (first + num) % _reserved_end;

    size_t result = (size_t) Atomic::cmpxchg((jlong) next_current, (jlong*) &_current_index, (jlong) current_idx);
    if (result == current_idx) {

      push_back_regions((current_idx + 1) % _reserved_end, first);

      initialize_humongous_regions(first, num);
      assert(current_index() != first, "current overlaps with contiguous regions");
      return get(first);
    }

    current_idx = result;
    next = (current_idx + 1) % _reserved_end;
  }
  return NULL;
}

void ShenandoahFreeSet::clear() {
  _active_end = _current_index;
  _write_index = _current_index;
  _capacity = 0;
  _used = 0;
}

void ShenandoahFreeSet::par_add_region(ShenandoahHeapRegion* r) {

  size_t next = Atomic::add(1, (jlong*) &_write_index) % _reserved_end;
  size_t bottom = (next == 0 ? _reserved_end : next) - 1;

  _regions[bottom] = r;

  // loop until we succeed in bringing the active_end up to our
  // write index
  // active_end gets set to 0 when we start a full gc
  while (true) {
    size_t test = (size_t) Atomic::cmpxchg((jlong) next, (jlong*) &_active_end, (jlong) bottom);
    if (test == bottom) {
      Atomic::add((jlong) r->free(), (jlong*) &_capacity);
      return;
    } else {
      // Don't starve competing threads.
      os::yield();
    }
  }

}

void ShenandoahFreeSet::add_region(ShenandoahHeapRegion* r) {
  assert(!r->is_in_collection_set(), "Shouldn't be adding those to the free set");
  assert(!contains(r), "We are about to add it, it shouldn't be there already");
  assert(!r->is_humongous(), "Don't add to humongous regions");

  assert(_active_end < _reserved_end, "within bounds and no wrapping here");

  _regions[_active_end] = r;
  _active_end = (_active_end + 1) % _reserved_end;
  _write_index++;
  _capacity += r->free();
  assert(_used <= _capacity, "must not use more than we have");
}

size_t ShenandoahFreeSet::claim_next(size_t idx) {
  size_t next = (idx + 1) % _reserved_end;
  if (next == _active_end) {
    // Don't increase _current_index up to _active_end.
    return SIZE_MAX;
  }
  size_t result = (size_t) Atomic::cmpxchg((jlong) next, (jlong*) &_current_index, (jlong) idx);

  if (result == idx) {
    result = next;
  }
  assert (result != _active_end, "don't increase current into active_end");
  return result;
}
