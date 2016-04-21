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
#include "utilities/quickSort.hpp"

ShenandoahHeapRegionSet::ShenandoahHeapRegionSet(size_t max_regions):
  _reserved_end(max_regions),
  _active_end(0),
  _regions(NEW_C_HEAP_ARRAY(ShenandoahHeapRegion*, max_regions, mtGC)),
  _current_index(0)
{
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


size_t ShenandoahHeapRegionSet::count() const {
  return _active_end - _current_index;
}

void ShenandoahHeapRegionSet::clear() {
  _active_end = 0;
  _current_index = 0;
}

void ShenandoahHeapRegionSet::add_region(ShenandoahHeapRegion* r) {
  if (_active_end < _reserved_end) {
    _regions[_active_end] = r;
    _active_end++;
  }
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
  tty->print_cr("_current_index: "SIZE_FORMAT" current region: %p, _active_end: "SIZE_FORMAT, _current_index, _regions[_current_index], _active_end);
  //  Unimplemented();
}

ShenandoahHeapRegion* ShenandoahHeapRegionSet::next() {
  size_t next = _current_index;
  if (next < _active_end) {
    _current_index++;
    return get(next);
  } else {
    return NULL;
  }
}

ShenandoahHeapRegion* ShenandoahHeapRegionSet::claim_next() {
  size_t next = Atomic::add(1, (jlong*) &_current_index) - 1;
  if (next < _active_end) {
    return get(next);
  } else {
    return NULL;
  }
  return NULL;
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

HeapWord* ShenandoahHeapRegionSet::bottom() const {
  return get(0)->bottom();
}

HeapWord* ShenandoahHeapRegionSet::end() const {
  return get(_active_end - 1)->end();
}

ShenandoahHeapRegion* ShenandoahHeapRegionSet::get(size_t i) const {
  if (i < _reserved_end) {
    return _regions[i];
  } else {
    return NULL;
  }
}
