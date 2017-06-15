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

#include "gc_implementation/g1/concurrentMark.hpp"
#include "gc_implementation/shenandoah/shenandoahWorkGroup.hpp"

class ConcurrentGCTimer;

class ShenandoahCollectorPolicy;
class ShenandoahHeapRegion;
class ShenandoahHeapRegionClosure;
class ShenandoahHeapRegionSet;
class ShenandoahCollectionSet;
class ShenandoahFreeSet;
class ShenandoahConcurrentMark;
class ShenandoahVerifier;
class ShenandoahConcurrentThread;
class ShenandoahMonitoringSupport;

class SCMUpdateRefsClosure: public OopClosure {
private:
  ShenandoahHeap* _heap;

  template <class T>
  inline void do_oop_work(T* p);

public:
  SCMUpdateRefsClosure();

public:
  inline void do_oop(oop* p);
  inline void do_oop(narrowOop* p);
};

#ifdef ASSERT
class AssertToSpaceClosure : public OopClosure {
private:
  template <class T>
  void do_oop_nv(T* p);
public:
  void do_oop(narrowOop* p);
  void do_oop(oop* p);
};
#endif

class ShenandoahAlwaysTrueClosure : public BoolObjectClosure {
public:
  bool do_object_b(oop p) { return true; }
};


class ShenandoahForwardedIsAliveClosure: public BoolObjectClosure {

private:
  ShenandoahHeap* _heap;
public:
  ShenandoahForwardedIsAliveClosure();
  void init(ShenandoahHeap* heap);
  bool do_object_b(oop obj);
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
  friend class ShenandoahCollectionSet;

  enum LockState { unlocked = 0, locked = 1 };

public:
  class ShenandoahHeapLock : public StackObj {
  private:
    ShenandoahHeap* _heap;

  public:
    ShenandoahHeapLock(ShenandoahHeap* heap) : _heap(heap) {
      while (OrderAccess::load_acquire(& _heap->_heap_lock) == locked || Atomic::cmpxchg(locked, &_heap->_heap_lock, unlocked) == locked) {
        SpinPause();
      }
      assert(_heap->_heap_lock == locked, "sanity");

#ifdef ASSERT
      assert(_heap->_heap_lock_owner == NULL, "must not be owned");
      _heap->_heap_lock_owner = Thread::current();
#endif
    }

    ~ShenandoahHeapLock() {
#ifdef ASSERT
      _heap->assert_heaplock_owned_by_current_thread();
      _heap->_heap_lock_owner = NULL;
#endif
      OrderAccess::release_store_fence(&_heap->_heap_lock, unlocked);
    }

  };

public:
  enum ShenandoahCancelCause {
    _oom_evacuation,
    _vm_stop,
  };
private:

  ShenandoahCollectorPolicy* _shenandoah_policy;
  VirtualSpace _storage;
  size_t _bitmap_size;
  MemRegion _heap_region;

  // Sortable array of regions
  ShenandoahHeapRegionSet* _ordered_regions;
  ShenandoahFreeSet* _free_regions;
  ShenandoahCollectionSet* _collection_set;
  ShenandoahConcurrentMark* _scm;
  ShenandoahVerifier*  _verifier;



  ShenandoahConcurrentThread* _concurrent_gc_thread;

  ShenandoahMonitoringSupport* _monitoring_support;

  size_t _num_regions;
  size_t _max_regions;
  size_t _initialSize;
  uint _max_workers;

  FlexibleWorkGang* _workers;

  volatile size_t _used;

  CMBitMap _verification_bit_map;
  CMBitMap _mark_bit_map0;
  CMBitMap _mark_bit_map1;
  CMBitMap* _complete_mark_bit_map;
  CMBitMap* _next_mark_bit_map;

  char* _in_cset_fast_test;
  char* _in_cset_fast_test_base;
  size_t _in_cset_fast_test_length;

  HeapWord** _complete_top_at_mark_starts;
  HeapWord** _complete_top_at_mark_starts_base;

  HeapWord** _next_top_at_mark_starts;
  HeapWord** _next_top_at_mark_starts_base;

  volatile jbyte _cancelled_concgc;

  size_t _bytes_allocated_since_cm;
  size_t _bytes_allocated_during_cm;
  size_t _allocated_last_gc;
  size_t _used_start_gc;

  unsigned int _concurrent_mark_in_progress;

  bool _full_gc_in_progress;
  bool _update_refs_in_progress;

  unsigned int _evacuation_in_progress;
  bool _need_update_refs;
  bool _need_reset_bitmaps;

  ReferenceProcessor* _ref_processor;

  ShenandoahForwardedIsAliveClosure _forwarded_is_alive;
  ShenandoahIsAliveClosure _is_alive;

  ConcurrentGCTimer* _gc_timer;

  // See allocate_memory()
  volatile jbyte _heap_lock;

#ifdef ASSERT
  Thread* volatile _heap_lock_owner;
#endif

public:
  ShenandoahHeap(ShenandoahCollectorPolicy* policy);

  const char* name() const /* override */;
  HeapWord* allocate_new_tlab(size_t word_size) /* override */;
  void print_on(outputStream* st) const /* override */;
  void print_extended_on(outputStream *st) const /* override */;

  ShenandoahHeap::Name kind() const  /* override */{
    return CollectedHeap::ShenandoahHeap;
  }

  jint initialize() /* override */;
  void post_initialize() /* override */;
  size_t capacity() const /* override */;
  size_t used() const /* override */;
  bool is_maximal_no_gc() const /* override */;
  size_t max_capacity() const /* override */;
  size_t min_capacity() const /* override */;
  bool is_in(const void* p) const /* override */;
  bool is_scavengable(const void* addr) /* override */;
  HeapWord* mem_allocate(size_t size, bool* what) /* override */;
  bool can_elide_tlab_store_barriers() const /* override */;
  oop new_store_pre_barrier(JavaThread* thread, oop new_obj) /* override */;
  bool can_elide_initializing_store_barrier(oop new_obj) /* override */;
  bool card_mark_must_follow_store() const /* override */;
  void collect(GCCause::Cause) /* override */;
  void do_full_collection(bool clear_all_soft_refs) /* override */;
  AdaptiveSizePolicy* size_policy() /* override */;
  CollectorPolicy* collector_policy() const /* override */;
  void ensure_parsability(bool retire_tlabs) /* override */;
  HeapWord* block_start(const void* addr) const /* override */;
  size_t block_size(const HeapWord* addr) const /* override */;
  bool block_is_obj(const HeapWord* addr) const /* override */;
  jlong millis_since_last_gc() /* override */;
  void prepare_for_verify() /* override */;
  void print_gc_threads_on(outputStream* st) const /* override */;
  void gc_threads_do(ThreadClosure* tcl) const /* override */;
  void print_tracing_info() const /* override */;
  void verify(bool silent, VerifyOption vo) /* override */;
  bool supports_tlab_allocation() const /* override */;
  size_t tlab_capacity(Thread *thr) const /* override */;
  void object_iterate(ObjectClosure* cl) /* override */;
  void safe_object_iterate(ObjectClosure* cl) /* override */;
  size_t unsafe_max_tlab_alloc(Thread *thread) const /* override */;
  size_t max_tlab_size() const /* override */;
  void resize_all_tlabs() /* override */;
  void accumulate_statistics_all_gclabs() /* override */;
  HeapWord* tlab_post_allocation_setup(HeapWord* obj) /* override */;
  uint oop_extra_words() /* override */;
  size_t tlab_used(Thread* ignored) const /* override */;
  void stop() /* override */;
  bool is_in_partial_collection(const void* p) /* override */;
  bool supports_heap_inspection() const /* override */;

  void space_iterate(SpaceClosure* scl) /* override */;
  void oop_iterate(ExtendedOopClosure* cl, bool skip_dirty_regions,
                   bool skip_unreachable_objects);
  void oop_iterate(ExtendedOopClosure* cl) {
    oop_iterate(cl, false, false);
  }

  Space* space_containing(const void* oop) const;
  void gc_prologue(bool b);
  void gc_epilogue(bool b);

#ifndef CC_INTERP
  void compile_prepare_oop(MacroAssembler* masm, Register obj) /* override */;
#endif

  void register_nmethod(nmethod* nm);
  void unregister_nmethod(nmethod* nm);

  void pin_object(oop o) /* override */;
  void unpin_object(oop o) /* override */;

  static ShenandoahHeap* heap();
  static ShenandoahHeap* heap_no_check();
  static size_t conservative_max_heap_alignment();
  static address in_cset_fast_test_addr();
  static address cancelled_concgc_addr();

  ShenandoahCollectorPolicy *shenandoahPolicy() { return _shenandoah_policy;}

  inline ShenandoahHeapRegion* heap_region_containing(const void* addr) const;
  inline size_t heap_region_index_containing(const void* addr) const;
  inline bool requires_marking(const void* entry) const;
  template <class T>
  inline oop maybe_update_oop_ref(T* p);

  void recycle_dirty_regions();

  void start_concurrent_marking();
  void stop_concurrent_marking();
  inline bool concurrent_mark_in_progress();
  static address concurrent_mark_in_progress_addr();

  void prepare_for_concurrent_evacuation();
  void evacuate_and_update_roots();

  void update_heap_references(ShenandoahHeapRegionSet* regions);
  void concurrent_update_heap_references();
  void prepare_update_refs();
  void finish_update_refs();

  void roots_iterate(OopClosure* cl);

private:
  void set_evacuation_in_progress(bool in_progress);

  // Only set/unset via ShenandoahHeapRegion
  void set_region_in_collection_set(size_t region_index, bool b);

  // Only via ShenandoahCollectionSet
  void clear_cset_fast_test();
public:
  inline bool is_evacuation_in_progress();
  void set_evacuation_in_progress_concurrently(bool in_progress);
  void set_evacuation_in_progress_at_safepoint(bool in_progress);

  void set_full_gc_in_progress(bool in_progress);
  bool is_full_gc_in_progress() const;

  void set_update_refs_in_progress(bool in_progress);
  bool is_update_refs_in_progress() const;
  static address update_refs_in_progress_addr();

  inline bool need_update_refs() const;
  void set_need_update_refs(bool update_refs);

  inline bool region_in_collection_set(size_t region_index) const;

  void acquire_pending_refs_lock();
  void release_pending_refs_lock();

  // Mainly there to avoid accidentally calling the templated
  // method below with ShenandoahHeapRegion* which would be *wrong*.
  inline bool in_collection_set(ShenandoahHeapRegion* r) const;

  template <class T>
  inline bool in_collection_set(T obj) const;

  inline bool allocated_after_next_mark_start(HeapWord* addr) const;
  void set_next_top_at_mark_start(HeapWord* region_base, HeapWord* addr);
  HeapWord* next_top_at_mark_start(HeapWord* region_base);

  inline bool allocated_after_complete_mark_start(HeapWord* addr) const;
  void set_complete_top_at_mark_start(HeapWord* region_base, HeapWord* addr);
  HeapWord* complete_top_at_mark_start(HeapWord* region_base);

  // Evacuates object src. Returns the evacuated object if this thread
  // succeeded, otherwise rolls back the evacuation and returns the
  // evacuated object by the competing thread. 'succeeded' is an out
  // param and set to true if this thread succeeded, otherwise to false.
  inline oop  evacuate_object(oop src, Thread* thread, bool& evacuated);
  inline bool cancelled_concgc() const;
  inline bool try_cancel_concgc();
  inline void clear_cancelled_concgc();

  ShenandoahHeapRegionSet* regions() { return _ordered_regions;}
  ShenandoahFreeSet* free_regions();
  ShenandoahCollectionSet* collection_set() { return _collection_set; }
  void clear_free_regions();
  void add_free_region(ShenandoahHeapRegion* r);

  void increase_used(size_t bytes);
  void decrease_used(size_t bytes);

  void set_used(size_t bytes);

  size_t garbage();

  void reset_next_mark_bitmap(WorkGang* gang);
  void reset_complete_mark_bitmap(WorkGang* gang);

  CMBitMap* complete_mark_bit_map();
  CMBitMap* next_mark_bit_map();
  inline bool is_marked_complete(oop obj) const;
  inline bool mark_next(oop obj) const;
  inline bool is_marked_next(oop obj) const;
  bool is_next_bitmap_clear();
  bool is_complete_bitmap_clear_range(HeapWord* start, HeapWord* end);

  void parallel_evacuate_region(ShenandoahHeapRegion* from_region);

  template <class T>
  inline oop update_oop_ref_not_null(T* p, oop obj);

  template <class T>
  inline oop maybe_update_oop_ref_not_null(T* p, oop obj);

  void print_heap_regions(outputStream* st = tty) const;

  size_t bytes_allocated_since_cm();
  void set_bytes_allocated_since_cm(size_t bytes);

  void reclaim_humongous_region_at(ShenandoahHeapRegion* r);

  VirtualSpace* storage() const;

  ShenandoahMonitoringSupport* monitoring_support();
  ShenandoahConcurrentMark* concurrentMark() { return _scm;}
  ShenandoahVerifier* verifier();

  ReferenceProcessor* ref_processor() { return _ref_processor;}

  FlexibleWorkGang* workers() const { return _workers;}

  uint max_workers();

  void do_evacuation();
  ShenandoahHeapRegion* next_compaction_region(const ShenandoahHeapRegion* r);

  void heap_region_iterate(ShenandoahHeapRegionClosure* blk, bool skip_dirty_regions = false, bool skip_humongous_continuation = false) const;

  // Delete entries for dead interned string and clean up unreferenced symbols
  // in symbol table, possibly in parallel.
  void unload_classes_and_cleanup_tables(bool full_gc);

  size_t num_regions() const { return _num_regions; }
  size_t max_regions() const { return _max_regions; }

  // TODO: consider moving this into ShenandoahHeapRegion.

  BoolObjectClosure* is_alive_closure();

private:
  template<class T>
  inline void marked_object_iterate(ShenandoahHeapRegion* region, T* cl, HeapWord* limit);

  template<class T>
  inline void marked_object_oop_iterate(ShenandoahHeapRegion* region, T* cl, HeapWord* limit);

public:
  template<class T>
  inline void marked_object_iterate(ShenandoahHeapRegion* region, T* cl);

  template<class T>
  inline void marked_object_safe_iterate(ShenandoahHeapRegion* region, T* cl);

  template<class T>
  inline void marked_object_oop_iterate(ShenandoahHeapRegion* region, T* cl);

  template<class T>
  inline void marked_object_oop_safe_iterate(ShenandoahHeapRegion* region, T* cl);

  GCTimer* gc_timer() const;
  GCTracer* tracer();

  void swap_mark_bitmaps();

  void cancel_concgc(GCCause::Cause cause);
  void cancel_concgc(ShenandoahCancelCause cause);

  void assert_heaplock_owned_by_current_thread() PRODUCT_RETURN;
  void assert_heaplock_or_safepoint() PRODUCT_RETURN;

public:
  typedef enum {
    _alloc_shared,      // Allocate common, outside of TLAB
    _alloc_shared_gc,   // Allocate common, outside of GCLAB
    _alloc_tlab,        // Allocate TLAB
    _alloc_gclab,       // Allocate GCLAB
  } AllocType;
private:
  HeapWord* allocate_new_lab(size_t word_size, AllocType type);
  HeapWord* allocate_memory_under_lock(size_t word_size, AllocType type);
  HeapWord* allocate_memory(size_t word_size, AllocType type);
  // Shenandoah functionality.
  inline HeapWord* allocate_from_gclab(Thread* thread, size_t size);
  HeapWord* allocate_from_gclab_slow(Thread* thread, size_t size);
  HeapWord* allocate_new_gclab(size_t word_size);

  template<class T>
  inline void do_marked_object(CMBitMap* bitmap, T* cl, oop obj);

  ShenandoahConcurrentThread* concurrent_thread() { return _concurrent_gc_thread; }

  inline bool mark_next_no_checks(oop obj) const;

  void parallel_evacuate();

  inline oop atomic_compare_exchange_oop(oop n, narrowOop* addr, oop c);
  inline oop atomic_compare_exchange_oop(oop n, oop* addr, oop c);

  void ref_processing_init();

  void grow_heap_by(size_t num_regions);
  void ensure_new_regions(size_t num_new_regions);

  void set_concurrent_mark_in_progress(bool in_progress);

  void oom_during_evacuation();

  HeapWord* allocate_memory_work(size_t word_size, AllocType type);
  HeapWord* allocate_large_memory(size_t word_size);

  const char* cancel_cause_to_string(ShenandoahCancelCause cause);

private:
  size_t* _recycled_regions;
  size_t _recycled_region_count;

public:
  void start_deferred_recycling();
  void defer_recycle(ShenandoahHeapRegion* r);
  void finish_deferred_recycle();

};

#endif // SHARE_VM_GC_SHENANDOAH_SHENANDOAHHEAP_HPP
