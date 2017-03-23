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

#include "precompiled.hpp"
#include "gc_implementation/shenandoah/shenandoahHeap.hpp"
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

void ShenandoahHeapRegionSet::add_region_check_for_duplicates(ShenandoahHeapRegion* r) {
  // FIXME There's a bug where the zeroth region is not checked, so check it here
  if (_active_end < _reserved_end && !contains(r) && _regions[0] != r) {
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
    if (skip_dirty_regions && current->in_collection_set()) {
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

  // There's a bug here where the zeroth region is missed  --chf
  for (i = _current_index + 1; i < _active_end; i++) {
    ShenandoahHeapRegion* current = _regions[i];
    assert(current->region_number() <= _reserved_end, "Tautology");

    if (skip_humongous_continuation && current->is_humongous_continuation()) {
      continue;
    }
    if (skip_dirty_regions && current->in_collection_set()) {
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

class PrintHeapRegionsClosure : public
   ShenandoahHeapRegionClosure {
private:
  outputStream* _st;
public:
  PrintHeapRegionsClosure() : _st(tty) {}
  PrintHeapRegionsClosure(outputStream* st) : _st(st) {}

  bool doHeapRegion(ShenandoahHeapRegion* r) {
    r->print_on(_st);
    return false;
  }
};

void ShenandoahHeapRegionSet::print(outputStream* out) {
  out->print_cr("_current_index: "SIZE_FORMAT" current region: %p, _active_end: "SIZE_FORMAT, _current_index, _regions[_current_index], _active_end);

  PrintHeapRegionsClosure pc1(out);
  heap_region_iterate(&pc1, false, false);
}

void ShenandoahHeapRegionSet::next() {
  if (_current_index < _active_end) {
    _current_index++;
  }
}

ShenandoahHeapRegion* ShenandoahHeapRegionSet::claim_next() {
  size_t next = (size_t) Atomic::add_ptr(1, (intptr_t*) &_current_index) - 1;
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
    _result = (r == _query);
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
  if (i < _active_end) {
    return _regions[i];
  } else {
    return NULL;
  }
}

ShenandoahHeapRegion* ShenandoahHeapRegionSet::current() const {
  if (_current_index < _active_end) {
    return get(_current_index);
  } else {
    return NULL;
  }
}
