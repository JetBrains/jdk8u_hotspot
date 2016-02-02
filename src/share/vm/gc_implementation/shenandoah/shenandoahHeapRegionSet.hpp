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

class ShenandoahHeapRegionClosure : public StackObj {
  bool _complete;
  void incomplete() {_complete = false;}

public:

  ShenandoahHeapRegionClosure(): _complete(true) {}

  // typically called on each region until it returns true;
  virtual bool doHeapRegion(ShenandoahHeapRegion* r) = 0;

  bool complete() { return _complete;}
};

// The basic set.
// Implements iteration.
class ShenandoahHeapRegionSet: public CHeapObj<mtGC> {
private:
  size_t _reserved_end;
  size_t _active_end;

  ShenandoahHeapRegion** _regions;

  size_t _write_index;

protected:
  size_t _current_index;

public:

  ShenandoahHeapRegionSet(size_t max_regions, size_t current_region);
  ShenandoahHeapRegionSet(size_t max_regions, size_t current_region, ShenandoahHeapRegionSet* shrs);

  ~ShenandoahHeapRegionSet();

  size_t   max_regions()     { return _reserved_end;}
  size_t   active_regions()  { return _active_end;}

  HeapWord* bottom()   { return get(0)->bottom();}
  HeapWord* end()      { return get(last_index())->end(); }

  void clear() {
    _active_end = 0;
    _write_index = 0;
    _current_index = 0;
  }

  size_t count() { return _active_end - _current_index;}

  ShenandoahHeapRegion* get(size_t i) {
    if (i < active_end()) {
      return _regions[i];
    } else {
      return NULL;
    }
  }

  void add_region(ShenandoahHeapRegion* r);
  void par_add_region(ShenandoahHeapRegion* r);
  void push_bottom(ShenandoahHeapRegion* r);

  void write_region(ShenandoahHeapRegion* r, size_t index);

  ShenandoahHeapRegion** getArray() {return _regions;}
  ShenandoahHeapRegion*  claim_next();

  void print();
public:

  void heap_region_iterate(ShenandoahHeapRegionClosure* blk,
                           bool skip_dirty_regions = false,
                           bool skip_humongous_continuation = false) const;

  size_t current_index()   { return _current_index;}
  void clear_current_index() {_current_index = 0; }

protected:
  size_t reserved_end()      { return _reserved_end;}
  size_t active_end()        { return _active_end;}

  size_t  last_index()      { return active_end() - 1;}

  bool contains(ShenandoahHeapRegion* r);

  void active_heap_region_iterate(ShenandoahHeapRegionClosure* blk,
                           bool skip_dirty_regions = false,
                           bool skip_humongous_continuation = false) const;

  void unclaimed_heap_region_iterate(ShenandoahHeapRegionClosure* blk,
                           bool skip_dirty_regions = false,
                           bool skip_humongous_continuation = false) const;

};

class ShenandoahCollectionSet: public ShenandoahHeapRegionSet {
private:
  size_t _garbage;
  size_t _live_data;
public:
  ShenandoahCollectionSet(size_t max_regions);
  ~ShenandoahCollectionSet();
  void add_region(ShenandoahHeapRegion* r);
  size_t live_data();
  size_t garbage();
  void clear();
};

class ShenandoahFreeSet : public ShenandoahHeapRegionSet {

private:
  size_t _capacity;
  size_t _used;

  bool is_contiguous(size_t start, size_t num);
  size_t find_contiguous(size_t num, size_t start);
public:
  ShenandoahFreeSet(size_t max_regions);
  void add_region(ShenandoahHeapRegion* r);
  void par_add_region(ShenandoahHeapRegion* r);
  void write_region(ShenandoahHeapRegion* r, size_t index);

  size_t claim_next(size_t current);

  // push n bottom used for growing the heap.
  size_t capacity();
  ShenandoahHeapRegion* claim_contiguous(size_t num);
  void clear();

  size_t used();

  void increase_used(size_t amount);
};

#endif //SHARE_VM_GC_SHENANDOAH_SHENANDOAHHEAPREGIONSET_HPP
