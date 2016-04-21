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

#ifndef SHARE_VM_GC_SHENANDOAH_SHENANDOAHHEAP_HPP
#define SHARE_VM_GC_SHENANDOAH_SHENANDOAHHEAP_HPP

#include "gc_implementation/shenandoah/shenandoahCollectorPolicy.hpp"
#include "gc_implementation/shenandoah/shenandoahConcurrentMark.hpp"
#include "gc_implementation/shenandoah/shenandoahConcurrentThread.hpp"
#include "gc_implementation/shenandoah/shenandoahHeapRegion.hpp"

#include "gc_implementation/g1/concurrentMark.hpp"
#include "gc_implementation/g1/heapRegionBounds.inline.hpp"

#include "memory/barrierSet.hpp"
#include "gc_interface/collectedHeap.hpp"
#include "memory/space.hpp"
#include "oops/oop.hpp"
#include "oops/markOop.hpp"


class SpaceClosure;
class GCTracer;

class ShenandoahHeapRegionClosure;
class ShenandoahHeapRegionSet;
class ShenandoahJNICritical;
class ShenandoahMonitoringSupport;

class ShenandoahAlwaysTrueClosure : public BoolObjectClosure {
public:
  bool do_object_b(oop p) { return true; }
};


class ShenandoahIsAliveClosure: public BoolObjectClosure {
private:
  ShenandoahHeap* _heap;
public:
  ShenandoahIsAliveClosure();
  void init(ShenandoahHeap* heap);
  bool do_object_b(oop obj);
};


// // A "ShenandoahHeap" is an implementation of a java heap for HotSpot.
// // It uses a new pauseless GC algorithm based on Brooks pointers.
// // Derived from G1

// //
// // CollectedHeap
// //    SharedHeap
// //      ShenandoahHeap

class ShenandoahHeap : public SharedHeap {

private:

  static ShenandoahHeap* _pgc;
  ShenandoahCollectorPolicy* _shenandoah_policy;
  VirtualSpace _storage;
  ShenandoahHeapRegion* _first_region;
  HeapWord* _first_region_bottom;

  // Sortable array of regions
  ShenandoahHeapRegionSet* _ordered_regions;
  ShenandoahHeapRegionSet* _sorted_regions;
  ShenandoahFreeSet* _free_regions;
  ShenandoahCollectionSet* _collection_set;
  ShenandoahHeapRegion* _currentAllocationRegion;
  ShenandoahConcurrentMark* _scm;



  ShenandoahConcurrentThread* _concurrent_gc_thread;

  ShenandoahMonitoringSupport* _monitoring_support;

  size_t _num_regions;
  size_t _max_regions;
  size_t _initialSize;
#ifndef NDEBUG
  uint _numAllocs;
#endif
  WorkGangBarrierSync barrierSync;
  int _max_parallel_workers;
  int _max_conc_workers;
  int _max_workers;

  FlexibleWorkGang* _conc_workers;


  volatile size_t _used;

  CMBitMap _mark_bit_map0;
  CMBitMap _mark_bit_map1;
  CMBitMap* _prev_mark_bit_map;
  CMBitMap* _next_mark_bit_map;

  bool* _in_cset_fast_test;
  bool* _in_cset_fast_test_base;
  uint _in_cset_fast_test_length;

  HeapWord** _top_at_mark_starts;
  HeapWord** _top_at_mark_starts_base;

  bool _cancelled_concgc;

  ShenandoahJNICritical* _jni_critical;

  jbyte _growing_heap;

public:
  size_t _bytesAllocSinceCM;
  size_t _bytes_allocated_during_cm;
  size_t _bytes_allocated_during_cm_start;
  size_t _max_allocated_gc;
  size_t _allocated_last_gc;
  size_t _used_start_gc;

public:
  ShenandoahHeap(ShenandoahCollectorPolicy* policy);
  inline HeapWord* allocate_from_gclab(Thread* thread, size_t size);
  HeapWord* allocate_from_gclab_slow(Thread* thread, size_t size);
  HeapWord* allocate_new_tlab(size_t word_size);
  HeapWord* allocate_new_gclab(size_t word_size);
private:
  HeapWord* allocate_new_tlab(size_t word_size, bool mark);
public:
  HeapWord* allocate_memory(size_t word_size, bool evacuating);

  // For now we are ignoring eden.
  inline bool should_alloc_in_eden(size_t size) { return false;}
  void print_on(outputStream* st) const ;

  ShenandoahHeap::Name kind() const {
    return CollectedHeap::ShenandoahHeap;
  }

  static ShenandoahHeap* heap();
  static ShenandoahHeap* heap_no_check();

  ShenandoahCollectorPolicy *shenandoahPolicy() { return _shenandoah_policy;}

  jint initialize();
  static size_t conservative_max_heap_alignment() {
    return HeapRegionBounds::max_size();
  }

  void post_initialize();
  size_t capacity() const;
  size_t used() const;
  bool is_maximal_no_gc() const;
  size_t max_capacity() const;
  size_t min_capacity() const;
  VirtualSpace* storage() const;
  virtual bool is_in(const void* p) const;
  bool is_in_partial_collection(const void* p);
  bool is_scavengable(const void* addr);
  virtual HeapWord* mem_allocate(size_t size, bool* what);
  virtual size_t unsafe_max_alloc();
  bool can_elide_tlab_store_barriers() const;
  virtual oop new_store_pre_barrier(JavaThread* thread, oop new_obj);
  bool can_elide_initializing_store_barrier(oop new_obj);
  bool card_mark_must_follow_store() const;
  bool supports_heap_inspection() const;
  void collect(GCCause::Cause);
  void do_full_collection(bool clear_all_soft_refs);
  AdaptiveSizePolicy* size_policy();
  ShenandoahCollectorPolicy* collector_policy() const;

  void ensure_parsability(bool retire_tlabs);

  void add_free_region(ShenandoahHeapRegion* r);
  void clear_free_regions();

  void oop_iterate(ExtendedOopClosure* cl, bool skip_dirty_regions,
                   bool skip_unreachable_objects);
  void oop_iterate(ExtendedOopClosure* cl) {
    oop_iterate(cl, false, false);
  }

  void roots_iterate(OopClosure* cl);
  void weak_roots_iterate(OopClosure* cl);

  void object_iterate(ObjectClosure* cl);
  void object_iterate_careful(ObjectClosureCareful* cl);
  void object_iterate_no_from_space(ObjectClosure* cl);
  void safe_object_iterate(ObjectClosure* cl);

  void marked_object_iterate(ShenandoahHeapRegion* region, ObjectClosure* cl);
  void marked_object_iterate_careful(ShenandoahHeapRegion* region, ObjectClosure* cl);
private:
  void marked_object_iterate(ShenandoahHeapRegion* region, ObjectClosure* cl, HeapWord* start, HeapWord* limit);

public:
  HeapWord* block_start(const void* addr) const;
  size_t block_size(const HeapWord* addr) const;
  bool block_is_obj(const HeapWord* addr) const;
  jlong millis_since_last_gc();
  void prepare_for_verify();
  void print_gc_threads_on(outputStream* st) const;
  void gc_threads_do(ThreadClosure* tcl) const;
  void print_tracing_info() const;
  void verify(bool silent,  VerifyOption vo);
  bool supports_tlab_allocation() const;
  virtual size_t tlab_capacity(Thread *thr) const;
  void oop_iterate(MemRegion mr, ExtendedOopClosure* ecl);
  void object_iterate_since_last_GC(ObjectClosure* cl);
  void space_iterate(SpaceClosure* scl);
  virtual size_t unsafe_max_tlab_alloc(Thread *thread) const;
  virtual size_t max_tlab_size() const;

  void resize_all_tlabs();
  void accumulate_statistics_all_gclabs();

  HeapWord* tlab_post_allocation_setup(HeapWord* obj);

  uint oop_extra_words();

#ifndef CC_INTERP
  void compile_prepare_oop(MacroAssembler* masm, Register obj);
#endif

  Space* space_containing(const void* oop) const;
  void gc_prologue(bool b);
  void gc_epilogue(bool b);

  void heap_region_iterate(ShenandoahHeapRegionClosure* blk, bool skip_dirty_regions = false, bool skip_humongous_continuation = false) const;
  inline ShenandoahHeapRegion* heap_region_containing(const void* addr) const;
  inline uint heap_region_index_containing(const void* addr) const;

  volatile unsigned int _concurrent_mark_in_progress;

  volatile unsigned int _evacuation_in_progress;
  bool _need_update_refs;
  bool _need_reset_bitmaps;

  void start_concurrent_marking();
  void stop_concurrent_marking();
  ShenandoahConcurrentMark* concurrentMark() { return _scm;}
  ShenandoahConcurrentThread* concurrent_thread() { return _concurrent_gc_thread; }

  ShenandoahMonitoringSupport* monitoring_support();

  ShenandoahJNICritical* jni_critical();

  size_t bump_object_age(HeapWord* start, HeapWord* end);

  void swap_mark_bitmaps();
  CMBitMap* prev_mark_bit_map();

  inline bool mark_current(oop obj) const;
  inline bool mark_current_no_checks(oop obj) const;
  inline bool is_marked_current(oop obj) const;
  inline bool is_marked_current(oop obj, ShenandoahHeapRegion* r) const;

  inline bool is_marked_prev(oop obj) const;
  inline bool is_marked_prev(oop obj, const ShenandoahHeapRegion* r) const;

  ReferenceProcessor* _ref_processor;

  inline bool requires_marking(const void* entry) const;
  bool is_obj_dead(const oop obj, const ShenandoahHeapRegion* r) const;

  void reset_mark_bitmap();
  void reset_mark_bitmap_range(HeapWord* from, HeapWord* to);

  bool is_bitmap_clear();

  void mark_object_live(oop obj, bool enqueue);

  void prepare_for_concurrent_evacuation();
  void do_evacuation();
  void parallel_evacuate();

  inline void initialize_brooks_ptr(oop p);

  inline oop maybe_update_oop_ref(oop* p);
  inline oop maybe_update_oop_ref_not_null(oop* p, oop obj);
  inline oop update_oop_ref_not_null(oop* p, oop obj);

  void evacuate_region(ShenandoahHeapRegion* from_region, ShenandoahHeapRegion* to_region);
  void parallel_evacuate_region(ShenandoahHeapRegion* from_region);
  void verify_evacuated_region(ShenandoahHeapRegion* from_region);

  void print_heap_regions(outputStream* st = tty) const;

  void print_all_refs(const char* prefix);

  void print_heap_objects(HeapWord* start, HeapWord* end);
  void print_heap_locations(HeapWord* start, HeapWord* end);
  void print_heap_object(oop p);

  inline oop evacuate_object(oop src, Thread* thread);
  bool is_in_collection_set(const void* p);

  inline void copy_object(oop p, HeapWord* s, size_t words);
  void verify_copy(oop p, oop c);
  void verify_heap_size_consistency();
  void verify_heap_after_marking();
  void verify_heap_after_evacuation();
  void verify_heap_after_update_refs();
  void verify_regions_after_update_refs();

  HeapWord* start_of_heap() { return _first_region_bottom + 1;}

  void cleanup_after_cancelconcgc();
  void increase_used(size_t bytes);
  void decrease_used(size_t bytes);
  void set_used(size_t bytes);

  void set_evacuation_in_progress(bool in_progress);
  inline bool is_evacuation_in_progress();

  inline bool need_update_refs() const;
  void set_need_update_refs(bool update_refs);

  ReferenceProcessor* ref_processor() { return _ref_processor;}
  virtual void ref_processing_init();
  ShenandoahIsAliveClosure isAlive;
  void evacuate_and_update_roots();

  ShenandoahFreeSet* free_regions();

  void acquire_pending_refs_lock();
  void release_pending_refs_lock();

  int max_workers();
  int max_conc_workers();
  int max_parallel_workers();
  FlexibleWorkGang* conc_workers() const{ return _conc_workers;}

  ShenandoahHeapRegionSet* regions() { return _ordered_regions;}
  ShenandoahHeapRegionSet* sorted_regions() { return _sorted_regions;}
  size_t num_regions();
  size_t max_regions();

  ShenandoahHeapRegion* next_compaction_region(const ShenandoahHeapRegion* r);

  void recycle_dirty_regions();

  void register_region_with_in_cset_fast_test(ShenandoahHeapRegion* r) {
    assert(_in_cset_fast_test_base != NULL, "sanity");
    assert(r->is_in_collection_set(), "invariant");
    uint index = r->region_number();
    assert(index < _in_cset_fast_test_length, "invariant");
    assert(!_in_cset_fast_test_base[index], "invariant");
    _in_cset_fast_test_base[index] = true;
  }
  bool in_cset_fast_test(HeapWord* obj) {
    assert(_in_cset_fast_test != NULL, "sanity");
    if (is_in(obj)) {
      // no need to subtract the bottom of the heap from obj,
      // _in_cset_fast_test is biased
      uintx index = ((uintx) obj) >> ShenandoahHeapRegion::RegionSizeShift;
      bool ret = _in_cset_fast_test[index];
      // let's make sure the result is consistent with what the slower
      // test returns
      assert( ret || !is_in_collection_set(obj), "sanity");
      assert(!ret ||  is_in_collection_set(obj), "sanity");
      return ret;
    } else {
      return false;
    }
  }

  static address in_cset_fast_test_addr() {
    return (address) (ShenandoahHeap::heap()->_in_cset_fast_test);
  }

  void clear_cset_fast_test() {
    assert(_in_cset_fast_test_base != NULL, "sanity");
    memset(_in_cset_fast_test_base, false,
           (size_t) _in_cset_fast_test_length * sizeof(bool));
  }


  inline bool allocated_after_mark_start(HeapWord* addr) const;
  void set_top_at_mark_start(HeapWord* region_base, HeapWord* addr);

  GCTracer* tracer();
  ShenandoahCollectionSet* collection_set() { return _collection_set; }
  size_t tlab_used(Thread* ignored) const;

private:

  bool call_from_write_barrier(bool evacuating);
  void check_grow_heap(bool evacuating);
  void grow_heap_by(size_t num_regions);
  void ensure_new_regions(size_t num_new_regions);

  void verify_evacuation(ShenandoahHeapRegion* from_region);
  void set_concurrent_mark_in_progress(bool in_progress);

  void oom_during_evacuation();
  void cancel_concgc();
public:
  inline bool cancelled_concgc() const;
  void clear_cancelled_concgc();

  void shutdown();

  inline bool concurrent_mark_in_progress();
  size_t calculateUsed();
  size_t calculateFree();

private:
  void verify_live();
  void verify_liveness_after_concurrent_mark();

  HeapWord* allocate_memory_work(size_t word_size);
  HeapWord* allocate_large_memory(size_t word_size);

  void set_from_region_protection(bool protect);

public:
  // Delete entries for dead interned string and clean up unreferenced symbols
  // in symbol table, possibly in parallel.
  void unlink_string_and_symbol_table(BoolObjectClosure* is_alive, bool unlink_strings = true, bool unlink_symbols = true);

  void reclaim_humongous_region_at(ShenandoahHeapRegion* r);

};

#endif // SHARE_VM_GC_SHENANDOAH_SHENANDOAHHEAP_HPP
