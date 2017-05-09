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

#include "precompiled.hpp"
#include "gc_implementation/shenandoah/shenandoahFreeSet.hpp"
#include "gc_implementation/shenandoah/shenandoahHeap.inline.hpp"
#include "gc_implementation/shenandoah/shenandoahHeapRegion.inline.hpp"
#include "runtime/atomic.hpp"

ShenandoahFreeSet::ShenandoahFreeSet(size_t max_regions) :
  ShenandoahHeapRegionSet(max_regions),
  _capacity(0),
  _used(0)
{
}

ShenandoahFreeSet::~ShenandoahFreeSet() {
}

void ShenandoahFreeSet::increase_used(size_t num_bytes) {
  assert_heaplock_owned_by_current_thread();
  _used += num_bytes;

  assert(_used <= _capacity, err_msg("must not use more than we have: used: "SIZE_FORMAT
				     ", capacity: "SIZE_FORMAT", num_bytes: "SIZE_FORMAT,
				     _used, _capacity, num_bytes));
}

size_t ShenandoahFreeSet::used() {
  return _used;
}

size_t ShenandoahFreeSet::capacity() {
  return _capacity;
}

/**
 * Return 0 if the range starting at start is a contiguous range with
 * num regions. Returns a number > 0 otherwise. That number tells
 * the caller, how many regions to skip (because we know, there
 * can't start a contiguous range there).
 */
size_t ShenandoahFreeSet::is_contiguous(size_t start, size_t num) {
  assert_heaplock_owned_by_current_thread();

  ShenandoahHeapRegion* r1 = get(start);

  if (! r1->is_empty()) {
    return 1;
  }
  for (size_t i = 1; i < num; i++) {

    size_t index = start + i;
    if (index == _active_end) {
      // We reached the end of our free list.
      ShouldNotReachHere(); // We limit search in find_contiguous()
      return i;
    }

    ShenandoahHeapRegion* r2 = get(index);
    if (r2->region_number() != r1->region_number() + 1)
      return i;
    if (! r2->is_empty())
      return i+1;

    r1 = r2;
  }
  return 0;
}

size_t ShenandoahFreeSet::find_contiguous(size_t start, size_t num) {
  assert_heaplock_owned_by_current_thread();

  assert(start < _reserved_end, "sanity");

  size_t index = start;
  while (index + num < _active_end) {
    assert(index < _reserved_end, "sanity");
    size_t j = is_contiguous(index, num);
    if (j == 0) {
      return index;
    }
    index = index + j;
  }
  return SIZE_MAX;
}

void ShenandoahFreeSet::initialize_humongous_regions(size_t first, size_t num) {
  assert_heaplock_owned_by_current_thread();
  for (size_t i = 0; i < num; i++) {
    ShenandoahHeapRegion* current = get(first + i);
    if (i == 0)
      current->set_humongous_start(true);
    else
      current->set_humongous_continuation(true);

    assert(current->is_empty(), "must be empty");
    current->set_top(current->end());
    current->increase_live_data_words(ShenandoahHeapRegion::region_size_bytes_jint() / HeapWordSize);
  }
  increase_used(ShenandoahHeapRegion::region_size_bytes() * num);
  ShenandoahHeap::heap()->increase_used(ShenandoahHeapRegion::region_size_bytes() * num);
}

ShenandoahHeapRegion* ShenandoahFreeSet::allocate_contiguous(size_t num) {
  assert_heaplock_owned_by_current_thread();
  size_t next = _current_index;
  while (next + num < _active_end) {
    size_t first = find_contiguous(next, num);
    if (first == SIZE_MAX) return NULL;
    size_t next_current = first + num;
    assert(next_current != _active_end, "never set current==end");

    initialize_humongous_regions(first, num);

    return get(first);

  }
  return NULL;
}

void ShenandoahFreeSet::add_region(ShenandoahHeapRegion* r) {
  assert_heaplock_owned_by_current_thread();
  assert(! r->in_collection_set(), "Shouldn't be adding those to the free set");
  assert(! contains(r), "We are about to add it, it shouldn't be there already");
  assert(! r->is_humongous(), "Don't add to humongous regions");

  assert(_active_end < _reserved_end, "within bounds");

  _regions[_active_end] = r;
  _active_end++;
  _capacity += r->free();
  assert(_used <= _capacity, "must not use more than we have");
}

void ShenandoahFreeSet::clear() {
  assert_heaplock_owned_by_current_thread();
  ShenandoahHeapRegionSet::clear();
  _capacity = 0;
  _used = 0;
}

ShenandoahHeapRegion* ShenandoahFreeSet::skip_humongous(ShenandoahHeapRegion* r) {
  while (r != NULL && r->is_humongous()) {
    next();
    r = current();
  }
  return r;
}

ShenandoahHeapRegion* ShenandoahFreeSet::current_no_humongous() {
  ShenandoahHeapRegion* r = current();
  return skip_humongous(r);
}

ShenandoahHeapRegion* ShenandoahFreeSet::next_no_humongous() {
  next();
  return current_no_humongous();
}

size_t ShenandoahFreeSet::unsafe_peek_next_no_humongous() const {
  size_t index = _current_index;
  size_t end   = _active_end;
  ShenandoahHeapRegion* r;

  for (; index < end; index ++) {
    r = get(index);
    if (!r->is_humongous() && r->free() >= MinTLABSize) {
      return r->free();
    }
  }

  return ShenandoahHeapRegion::region_size_bytes();
}


void ShenandoahFreeSet::assert_heaplock_owned_by_current_thread() {
#ifdef ASSERT
  ShenandoahHeap::heap()->assert_heaplock_owned_by_current_thread();
#endif
}
