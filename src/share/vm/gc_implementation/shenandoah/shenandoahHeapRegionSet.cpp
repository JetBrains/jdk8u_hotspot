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

#include "gc_implementation/shenandoah/shenandoahHeapRegionSet.hpp"
#include "gc_implementation/shenandoah/shenandoahHeapRegion.inline.hpp"
#include "gc_implementation/shenandoah/brooksPointer.hpp"
#include "gc_implementation/shenandoah/shenandoahHumongous.hpp"
#include "memory/resourceArea.hpp"
#include "utilities/quickSort.hpp"

ShenandoahHeapRegionSet::ShenandoahHeapRegionSet(size_t max_regions, size_t current_regions):
  _reserved_end(max_regions),
  _active_end(0),
  _regions(NEW_C_HEAP_ARRAY(ShenandoahHeapRegion*, max_regions, mtGC)),
  _write_index(0),
  _current_index(0)
{
}

ShenandoahHeapRegionSet::ShenandoahHeapRegionSet(size_t max_regions, size_t current_regions,
                                                 ShenandoahHeapRegionSet* shrs) :
 _reserved_end(max_regions),
 _active_end(shrs->active_end()),
 _regions(NEW_C_HEAP_ARRAY(ShenandoahHeapRegion*, max_regions, mtGC)),
 _write_index(shrs->active_end()),
  _current_index(0)
{
  memcpy(_regions, shrs->_regions, sizeof(ShenandoahHeapRegion*) * current_regions);
}

ShenandoahHeapRegionSet::~ShenandoahHeapRegionSet() {
  FREE_C_HEAP_ARRAY(ShenandoahHeapRegion*, _regions, mtGC);
}

class PrintHeapRegionClosure : public ShenandoahHeapRegionClosure {
 public:
  bool doHeapRegion(ShenandoahHeapRegion* r) {
    r->print();
    return false;
  }
};

class PrintHeapRegionSummaryClosure : public ShenandoahHeapRegionClosure {
 public:
  bool doHeapRegion(ShenandoahHeapRegion* r) {
    tty->print(""SIZE_FORMAT, r->region_number());
    return false;
  }
};

void ShenandoahHeapRegionSet::add_region(ShenandoahHeapRegion* r) {
  if (_active_end < _reserved_end) {
    _regions[_active_end] = r;
    _active_end++;
    _write_index++;
  }
}

void ShenandoahHeapRegionSet::write_region(ShenandoahHeapRegion* r, size_t index) {
  _regions[index] = r;
}

void ShenandoahFreeSet::increase_used(size_t num_bytes) {
  assert(_used <= _capacity, "must not use more than we have");
  Atomic::add((jlong) num_bytes, (jlong*) &_used);
}

// Apply blk->doHeapRegion() on all committed regions in address order,
// terminating the iteration early if doHeapRegion() returns true.
void ShenandoahHeapRegionSet::active_heap_region_iterate(ShenandoahHeapRegionClosure* blk,
                                                  bool skip_dirty_regions,
                                                  bool skip_humongous_continuation) const {
  size_t i;
  for (i = 0; i < _active_end; i++) {
    ShenandoahHeapRegion* current = _regions[i];
    assert(current->region_number() <= _reserved_end, "Tautology");

    if (skip_humongous_continuation && current->is_humongous_continuation()) {
      continue;
    }
    if (skip_dirty_regions && current->is_in_collection_set()) {
      continue;
    }
    if (blk->doHeapRegion(current)) {
      return;
    }
  }
}

void ShenandoahHeapRegionSet::unclaimed_heap_region_iterate(ShenandoahHeapRegionClosure* blk,
                                                  bool skip_dirty_regions,
                                                  bool skip_humongous_continuation) const {
  size_t i;
  for (i = _current_index + 1; i < _active_end; i++) {
    ShenandoahHeapRegion* current = _regions[i];
    assert(current->region_number() <= _reserved_end, "Tautology");

    if (skip_humongous_continuation && current->is_humongous_continuation()) {
      continue;
    }
    if (skip_dirty_regions && current->is_in_collection_set()) {
      continue;
    }
    if (blk->doHeapRegion(current)) {
      return;
    }
  }
}

// Iterates over all of the regions.
void ShenandoahHeapRegionSet::heap_region_iterate(ShenandoahHeapRegionClosure* blk,
                                                  bool skip_dirty_regions,
                                                  bool skip_humongous_continuation) const {
  active_heap_region_iterate(blk, skip_dirty_regions, skip_humongous_continuation);
}

void ShenandoahHeapRegionSet::print() {
  Unimplemented();
}

size_t ShenandoahFreeSet::used() {
  return _used;
}

size_t ShenandoahFreeSet::capacity() {
  return _capacity;
}

ShenandoahCollectionSet::ShenandoahCollectionSet(size_t max_regions) :
  ShenandoahHeapRegionSet(max_regions, max_regions),
  _garbage(0), _live_data(0) {
}

void ShenandoahCollectionSet::add_region(ShenandoahHeapRegion* r) {
  ShenandoahHeapRegionSet::add_region(r);
  _garbage += r->garbage();
  _live_data += r->getLiveData();
}

size_t ShenandoahCollectionSet::garbage() {
  return _garbage;
}

size_t ShenandoahCollectionSet::live_data() {
  return _live_data;
}

ShenandoahHeapRegion* ShenandoahHeapRegionSet::claim_next() {
  size_t next = Atomic::add(1, (jlong*) &_current_index) - 1;
  if (next < active_end()) {
    return get(next);
  } else {
    return NULL;
  }
  return NULL;
}

void ShenandoahCollectionSet::clear() {
  /*
  tty->print("Thread %d called collectionset clear\n",
             Thread::current()->osthread()->thread_id());
  */
  size_t end = active_end();
  for (size_t i = 0; i < end; i++) {
    get(i)->set_is_in_collection_set(false);
  }
  ShenandoahHeapRegionSet::clear();
  ShenandoahHeap::heap()->clear_cset_fast_test();
  _garbage = 0;
  _live_data = 0;
}

bool ShenandoahFreeSet::is_contiguous(size_t start, size_t num) {
  assert(active_end() <= reserved_end(), "sanity");
  if (start + num >= active_end()) return false;

  ShenandoahHeapRegion* r1 = get(start);

  if (! r1->is_empty()) {
    return false;
  }

  for (size_t i = start + 1; i < start + num; i++) {
    ShenandoahHeapRegion* r2 = get(i);
    if (r2->region_number() != r1->region_number() + 1)
      return false;
    if (! r2->is_empty())
      return false;

    r1 = r2;
  }
  return true;
}

size_t ShenandoahFreeSet::find_contiguous(size_t num, size_t start) {

  size_t end = active_end() - num;
  for (size_t index = start; index < end; index++) {
    if (is_contiguous(index, num)) return index;
  }
  return active_end();
}

ShenandoahHeapRegion* ShenandoahFreeSet::claim_contiguous(size_t num) {
  /*
  tty->print("entering claim_contiguous %d regions with current region = %d and count = %ld\n",
             num, safe_get(current_index()), count());
  */

  size_t current_idx = _current_index;
  size_t end = active_end();
  size_t first = find_contiguous(num, current_idx + 1);
  size_t next_current = first + num;
  while (next_current <= end) {
    while (current_idx < first) {
      size_t result = (size_t) Atomic::cmpxchg((jlong) next_current, (jlong*) &_current_index, (jlong) current_idx);
      if (result == current_idx) {
        /*
        tty->print("Just claimed %ld regions to get %d regions\n",
        last - first + 1, num);
        tty->print("About to return");
        for (int i = first; i <= last; i++) {
          tty->print("region %d, ", get(i)->region_number());
        }
        tty->print("\n");
        */
        for (size_t i = current_idx + 1; i < first; i++) {

          /*
          tty->print("About to push region %d back on the list\n",
                     get(i)->region_number());
          */
          push_bottom(get(i));
        }

        for (size_t i = 0; i < num; i++) {
          ShenandoahHeapRegion* current = get(first + i);
          if (i == 0)
            current->set_humongous_start(true);
          else
            current->set_humongous_continuation(true);

          current->set_top(current->end());
          current->increase_live_data(ShenandoahHeapRegion::RegionSizeBytes);
        }
        increase_used(ShenandoahHeapRegion::RegionSizeBytes * num);
        ShenandoahHeap::heap()->increase_used(ShenandoahHeapRegion::RegionSizeBytes * num);
        /*
        tty->print("About to return from claim_contiguous first = %ld get(first) = %d\n",
                   first, get(first)->region_number());
        */

        assert(current_index() != first, "current overlaps with contiguous regions");
        return get(first);
      }
      assert(result > current_idx, "strong monotonic");
      current_idx = result;
    }
    size_t contiguous = find_contiguous(num, current_idx + 1);
    assert(contiguous > first, "strong monotonic");
    first = contiguous;
    next_current = first + num;
  }
  return NULL;
}


ShenandoahFreeSet::ShenandoahFreeSet(size_t max_regions) :
  ShenandoahHeapRegionSet(max_regions, max_regions),
  _capacity(0),
  _used(0)
{
}

void ShenandoahFreeSet::clear() {
  /*
  tty->print("Thread %d called ShenandoahFreeSet::clear()\n",
             Thread::current()->osthread()->thread_id());
  */
  ShenandoahHeapRegionSet::clear();
  _capacity = 0;
  _used = 0;
}

void ShenandoahHeapRegionSet::push_bottom(ShenandoahHeapRegion* r) {

  size_t bottom = Atomic::add(1, (jlong*) &_write_index) - 1;

  if (bottom >= reserved_end()) {
    // TODO: Out of bounds. Drop region. Consider larger free-list or ring-buffer.
    if (ShenandoahWarnings) {
      tty->print_cr("Dropping free region in par_add_region(), most likely because of humongous allocation");
    }
    return;
  }

  write_region(r, bottom);

  // loop until we succeed in bringing the active_end up to our
  // write index
  // active_end gets set to 0 when we start a full gc
  while (active_end() <= bottom) {
    size_t test = (size_t) Atomic::cmpxchg((jlong) bottom+1, (jlong*) &_active_end, (jlong) bottom);
    if (test == bottom) {
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

  /*
  tty->print("Thread %d just added region %d to the free list\n",
             Thread::current()->osthread()->thread_id(),
             r->region_number());
  */
  ShenandoahHeapRegionSet::add_region(r);

  _capacity += r->free();
  assert(_used <= _capacity, "must not use more than we have");
}

void ShenandoahHeapRegionSet::par_add_region(ShenandoahHeapRegion* r) {
  push_bottom(r);
}

void ShenandoahFreeSet::par_add_region(ShenandoahHeapRegion* r) {
  ShenandoahHeapRegionSet::par_add_region(r);
  Atomic::add((jlong) r->free(), (jlong*) &_capacity);
}

void ShenandoahFreeSet::write_region(ShenandoahHeapRegion* r, size_t index) {
  assert(!r->is_in_collection_set(), "Shouldn't be adding those to the free set");
  /*
  tty->print("Thread %d just added region %d to the free list at pos %ld\n",
             Thread::current()->osthread()->thread_id(),
             r->region_number(),
             index);
  */
  assert(index < reserved_end(), "within bounds");
  ShenandoahHeapRegionSet::write_region(r, index);
}

size_t ShenandoahFreeSet::claim_next(size_t idx) {
  size_t next = idx + 1;
  size_t result = (size_t) Atomic::cmpxchg((jlong) next, (jlong*) &_current_index, (jlong) idx);
  /*
  tty->print("claim_next idx = %ld next = %ld current_index = %ld result = %ld\n",
             idx, next, _current_index, result);
  */
  if (result == idx) {
    result = next;
  }
  if (result < active_end()) {
    return result;
  } else {
    return -1;
  }
}

class FindRegionClosure : public ShenandoahHeapRegionClosure {
  ShenandoahHeapRegion* _query;
  bool _result;
public:

  FindRegionClosure(ShenandoahHeapRegion* query) : _query(query), _result(false) {}

  bool doHeapRegion(ShenandoahHeapRegion* r) {
    if (r == _query) {
      _result = true;
    } else {
        _result = false;
    }
    return _result;
  }

  bool result() { return _result;}
};

bool ShenandoahHeapRegionSet::contains(ShenandoahHeapRegion* r) {
  FindRegionClosure cl(r);
  unclaimed_heap_region_iterate(&cl);
  return cl.result();
}
