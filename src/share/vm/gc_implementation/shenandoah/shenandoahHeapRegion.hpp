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

#ifndef SHARE_VM_GC_SHENANDOAH_SHENANDOAHHEAPREGION_HPP
#define SHARE_VM_GC_SHENANDOAH_SHENANDOAHHEAPREGION_HPP

#include "memory/space.hpp"
#include "memory/universe.hpp"
#include "utilities/sizes.hpp"

class ShenandoahHeapRegion : public ContiguousSpace {

private:
  // Auxiliary functions for scan_and_forward support.
  // See comments for CompactibleSpace for more information.
  inline HeapWord* scan_limit() const {
    return top();
  }

  inline bool scanned_block_is_obj(const HeapWord* addr) const {
    return true; // Always true, since scan_limit is top
  }

  bool block_is_obj(const HeapWord* addr) const;
  size_t block_size(const HeapWord* addr) const;

  inline size_t scanned_block_size(const HeapWord* addr) const {
    return ShenandoahHeapRegion::block_size(addr);
  }

    // Auxiliary functions for scan_and_{forward,adjust_pointers,compact} support.
  inline size_t adjust_obj_size(size_t size) const {
    return size + 1;
  }

  inline size_t obj_size(const HeapWord* addr) const {
    return ShenandoahHeapRegion::block_size(addr);
  }

public:
  virtual oop make_oop(HeapWord* addr) const {
    return oop(addr+1);
  }

  virtual oop compact_oop(HeapWord* addr) const {
    return oop(addr + 1);
  }

public:
  static size_t RegionSizeBytes;
  static size_t RegionSizeShift;

private:
  size_t _region_number;
  volatile size_t liveData;
  MemRegion reserved;
  bool _is_in_collection_set;

  bool _humongous_start;
  bool _humongous_continuation;

  HeapWord* _top_at_mark_start;
  HeapWord* _top_at_prev_mark_start;
  HeapWord* _top_prev_mark_bitmap;

  HeapWord* _new_top;

#ifdef ASSERT
  int _mem_protection_level;
#endif

public:
  static void setup_heap_region_size(size_t initial_heap_size, size_t max_heap_size);

  jint initialize_heap_region(HeapWord* start, size_t regionSize, int index);


  size_t region_number() const;

  // Roll back the previous allocation of an object with specified size.
  // Returns TRUE when successful, FALSE if not successful or not supported.
  bool rollback_allocation(uint size);

  void clearLiveData();
  void setLiveData(size_t s);
  inline void increase_live_data(size_t s);

  size_t getLiveData() const;

  void print_on(outputStream* st) const;

  size_t garbage() const;

  void recycle();
  void reset();

  void oop_iterate_skip_unreachable(ExtendedOopClosure* cl, bool skip_unreachable_objects);

  void marked_object_iterate(ObjectClosure* blk);

  void object_iterate_interruptible(ObjectClosure* blk, bool allow_cancel);

  HeapWord* object_iterate_careful(ObjectClosureCareful* cl);

  HeapWord* block_start_const(const void* p) const;

  // Just before GC we need to fill the current region.
  void fill_region();

  bool is_in_collection_set() const;

  void set_is_in_collection_set(bool b);

  void set_humongous_start(bool start);
  void set_humongous_continuation(bool continuation);

  bool is_humongous() const;
  bool is_humongous_start() const;
  bool is_humongous_continuation() const;

#ifdef ASSERT
  void memProtectionOn();
  void memProtectionOff();
#endif

  static ByteSize is_in_collection_set_offset();
  // The following are for humongous regions.  We need to save the
  markOop saved_mark_word;
  void save_mark_word(oop obj) {saved_mark_word = obj->mark();}
  markOop mark_word() {return saved_mark_word;}

  virtual CompactibleSpace* next_compaction_space() const;

  // Override for scan_and_forward support.
  void prepare_for_compaction(CompactPoint* cp);
  void adjust_pointers();
  void compact();

  void init_top_at_mark_start();
  void set_top_at_mark_start(HeapWord* top);
  HeapWord* top_at_mark_start();
  void reset_top_at_prev_mark_start();
  HeapWord* top_at_prev_mark_start();
  HeapWord* top_prev_mark_bitmap();

  void set_top_prev_mark_bitmap(HeapWord* top);
  void swap_top_at_mark_start();

  inline bool allocated_after_mark_start(HeapWord* addr);
  bool allocated_after_prev_mark_start(HeapWord* addr) const;

  void set_new_top(HeapWord* new_top) { _new_top = new_top; }
  HeapWord* new_top() const { return _new_top; }

private:
  void do_reset();

};

#endif // SHARE_VM_GC_SHENANDOAH_SHENANDOAHHEAPREGION_HPP
