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

ShenandoahFreeSet::ShenandoahFreeSet(size_t max_regions) :
  _regions(NEW_C_HEAP_ARRAY(ShenandoahHeapRegion*, max_regions, mtGC)),
  _active_end(0),
  _reserved_end(max_regions),
  _current(0),
  _capacity(0),
  _used(0)
{
}

ShenandoahFreeSet::~ShenandoahFreeSet() {
  FREE_C_HEAP_ARRAY(ShenandoahHeapRegion*, _regions, mtGC);
}

void ShenandoahFreeSet::increase_used(size_t num_bytes) {
  assert_heaplock_owned_by_current_thread();
  _used += num_bytes;

  assert(_used <= _capacity, err_msg("must not use more than we have: used: "SIZE_FORMAT
                                     ", capacity: "SIZE_FORMAT", num_bytes: "SIZE_FORMAT,
                                     _used, _capacity, num_bytes));
}

ShenandoahHeapRegion* ShenandoahFreeSet::allocate_contiguous(size_t words_size) {
  assert_heaplock_owned_by_current_thread();

  size_t num = ShenandoahHeapRegion::required_regions(words_size * HeapWordSize);

  // No regions left to satisfy allocation, bye.
  if (num > count()) {
    return NULL;
  }

  // Find the continuous interval of $num regions, starting from $beg and ending in $end,
  // inclusive. Current index maintains the dense prefix position: there is no reason to scan
  // before it.

  size_t beg = _current;
  size_t end = beg;

  while (true) {
    if (end >= _active_end) {
      // Hit the end, goodbye
      return NULL;
    }

    // If region is not empty, the current [beg; end] is useless, and we may fast-forward.
    if (!_regions[end]->is_empty()) {
      end++;
      beg = end;
      continue;
    }

    // If regions are not adjacent, then current [beg; end] is useless, and we may fast-forward.
    // The difference is that "end" is still usable as the beginning of new candidate interval.
    if ((end != 0) && _regions[end - 1]->region_number() + 1 != _regions[end]->region_number()) {
      beg = end;
      end++;
      continue;
    }

    if ((end - beg + 1) == num) {
      // found the match
      break;
    }

    end++;
  };

#ifdef ASSERT
  assert ((end - beg + 1) == num, "Found just enough regions");
  for (size_t i = beg; i <= end; i++) {
    assert(_regions[i]->is_empty(), "Should be empty");
    assert(i == beg || _regions[i-1]->region_number() + 1 == _regions[i]->region_number(), "Should be contiguous");
  }
#endif

  ShenandoahHeap* sh = ShenandoahHeap::heap();

  // Initialize regions:
  for (size_t i = beg; i <= end; i++) {
    ShenandoahHeapRegion* r = _regions[i];
    if (i == beg) {
      r->make_humongous_start();
    } else {
      r->make_humongous_cont();
    }

    // Trailing region may be non-full, record the remainder there
    size_t remainder = words_size & ShenandoahHeapRegion::region_size_words_mask();
    size_t used_words;
    if ((i == end) && (remainder != 0)) {
      used_words = remainder;
    } else {
      used_words = ShenandoahHeapRegion::region_size_words();
    }

    r->increase_live_data_words(used_words);
    r->set_top(r->bottom() + used_words);
    r->reset_alloc_stats_to_shared();
    sh->increase_used(used_words * HeapWordSize);
  }

  // While individual regions report their true use, all humongous regions are
  // marked used in the free set.
  increase_used(ShenandoahHeapRegion::region_size_bytes() * num);

  // Allocated at dense prefix? Move the pointer appropriately.
  // This may require fast-forwarding over existing humongous regions.
  if (beg == _current) {
    _current += num;
    while (_current < _active_end && _regions[_current]->is_humongous()) {
      _current++;
    }
  }

  return _regions[beg];
}

void ShenandoahFreeSet::add_region(ShenandoahHeapRegion* r) {
  assert_heaplock_owned_by_current_thread();
  assert(!r->in_collection_set(), "Shouldn't be adding those to the free set");
  assert(r->is_alloc_allowed(), "Should only add regions that can be allocated at");

#ifdef ASSERT
  for (size_t i = 0; i < _active_end; i++) {
    assert (r != _regions[i], "We are about to add it, it shouldn't be there already");
  }
#endif
  assert(_active_end < _reserved_end, "within bounds");

  _regions[_active_end] = r;
  _active_end++;
  _capacity += r->free();
  assert(_used <= _capacity, "must not use more than we have");
}

void ShenandoahFreeSet::clear() {
  assert_heaplock_owned_by_current_thread();
  _active_end = 0;
  _current = 0;
  _capacity = 0;
  _used = 0;
}

ShenandoahHeapRegion* ShenandoahFreeSet::current_no_humongous() const {
  assert_heaplock_owned_by_current_thread();

  if (_current < _active_end) {
    ShenandoahHeapRegion* r = _regions[_current];
    assert (!r->is_humongous(), err_msg("Cannot be humongous, region number #" SIZE_FORMAT, _regions[_current]->region_number()));
    return r;
  } else {
    return NULL;
  }
}

ShenandoahHeapRegion* ShenandoahFreeSet::next_no_humongous() {
  assert_heaplock_owned_by_current_thread();

  for (size_t index = _current + 1; index < _active_end; index++) {
    ShenandoahHeapRegion* r = _regions[index];
    if (!r->is_humongous()) {
      _current = index;
      return r;
    }
  }

  // No regions left
  _current = _active_end;
  return NULL;
}

size_t ShenandoahFreeSet::unsafe_peek_free() const {
  // Deliberately not locked, this method is unsafe when free set is modified.

  for (size_t index = _current; index < _active_end; index++) {
    ShenandoahHeapRegion* r = _regions[index];
    if (!r->is_humongous() && r->free() >= MinTLABSize) {
      return r->free();
    }
  }

  // It appears that no regions left
  return 0;
}

void ShenandoahFreeSet::print_on(outputStream* out) const {
  out->print_cr("Free Set: " SIZE_FORMAT "", count());
  for (size_t index = _current; index < _active_end; index++) {
    _regions[index]->print_on(out);
  }
}

#ifdef ASSERT
void ShenandoahFreeSet::assert_heaplock_owned_by_current_thread() const {
  ShenandoahHeap::heap()->assert_heaplock_owned_by_current_thread();
}
#endif
