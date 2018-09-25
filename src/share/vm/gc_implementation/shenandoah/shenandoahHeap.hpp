/*
 * Copyright (c) 2013, 2017, Red Hat, Inc. and/or its affiliates.
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

#include "gc_implementation/shared/markBitMap.hpp"
#include "gc_implementation/shenandoah/shenandoahAsserts.hpp"
#include "gc_implementation/shenandoah/shenandoahAllocRequest.hpp"
#include "gc_implementation/shenandoah/shenandoahHeapLock.hpp"
#include "gc_implementation/shenandoah/shenandoahEvacOOMHandler.hpp"
#include "gc_implementation/shenandoah/shenandoahSharedVariables.hpp"
#include "gc_implementation/shenandoah/shenandoahWorkGroup.hpp"

class ConcurrentGCTimer;

class ShenandoahAllocTracker;
class ShenandoahAsserts;
class ShenandoahCollectionSet;
class ShenandoahCollectorPolicy;
class ShenandoahConcurrentMark;
class ShenandoahControlThread;
class ShenandoahGCSession;
class ShenandoahFreeSet;
class ShenandoahHeapRegion;
class ShenandoahHeapRegionClosure;
class ShenandoahMarkCompact;
class ShenandoahMonitoringSupport;
class ShenandoahHeuristics;
class ShenandoahMarkingContext;
class ShenandoahPhaseTimings;
class ShenandoahPacer;
class ShenandoahVerifier;
class ShenandoahWorkGang;
class VMStructs;

class ShenandoahRegionIterator : public StackObj {
private:
  volatile jint _index;
  ShenandoahHeap* _heap;

  // No implicit copying: iterators should be passed by reference to capture the state
  ShenandoahRegionIterator(const ShenandoahRegionIterator& that);
  ShenandoahRegionIterator& operator=(const ShenandoahRegionIterator& o);

public:
  ShenandoahRegionIterator();
  ShenandoahRegionIterator(ShenandoahHeap* heap);

  // Reset iterator to default state
  void reset();

  // Returns next region, or NULL if there are no more regions.
  // This is multi-thread-safe.
  inline ShenandoahHeapRegion* next();

  // This is *not* MT safe. However, in the absence of multithreaded access, it
  // can be used to determine if there is more work to do.
  bool has_next() const;
};

class ShenandoahHeapRegionClosure : public StackObj {
public:
  // typically called on each region until it returns true;
  virtual bool heap_region_do(ShenandoahHeapRegion* r) = 0;
};

class ShenandoahUpdateRefsClosure: public OopClosure {
private:
  ShenandoahHeap* _heap;

  template <class T>
  inline void do_oop_work(T* p);

public:
  ShenandoahUpdateRefsClosure();
  inline void do_oop(oop* p);
  inline void do_oop(narrowOop* p);
};

#ifdef ASSERT
class ShenandoahAssertToSpaceClosure : public OopClosure {
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
  ShenandoahMarkingContext* const _mark_context;
public:
  ShenandoahForwardedIsAliveClosure();
  bool do_object_b(oop obj);
};

class ShenandoahIsAliveClosure: public BoolObjectClosure {
private:
  ShenandoahMarkingContext* const _mark_context;
public:
  ShenandoahIsAliveClosure();
  bool do_object_b(oop obj);
};

class ShenandoahIsAliveSelector : public StackObj {
private:
  ShenandoahIsAliveClosure _alive_cl;
  ShenandoahForwardedIsAliveClosure _fwd_alive_cl;
public:
  BoolObjectClosure* is_alive_closure();
};

// Shenandoah GC is low-pause concurrent GC that uses Brooks forwarding pointers
// to encode forwarding data. See BrooksPointer for details on forwarding data encoding.
// See ShenandoahControlThread for GC cycle structure.
//
class ShenandoahHeap : public SharedHeap {
  friend class ShenandoahAsserts;
  friend class VMStructs;
  friend class ShenandoahGCSession;

// ---------- Locks that guard important data structures in Heap
//
private:
  ShenandoahHeapLock _lock;

public:
  ShenandoahHeapLock* lock() {
    return &_lock;
  }

  void assert_heaplock_owned_by_current_thread()     PRODUCT_RETURN;
  void assert_heaplock_not_owned_by_current_thread() PRODUCT_RETURN;
  void assert_heaplock_or_safepoint()                PRODUCT_RETURN;

// ---------- Initialization, termination, identification, printing routines
//
public:
  static ShenandoahHeap* heap();
  static ShenandoahHeap* heap_no_check();
  static size_t conservative_max_heap_alignment();

  const char* name()          const { return "Shenandoah"; }
  ShenandoahHeap::Name kind() const { return CollectedHeap::ShenandoahHeap; }

  ShenandoahHeap(ShenandoahCollectorPolicy* policy);
  jint initialize();
  void post_initialize();
  void initialize_heuristics();

  void print_on(outputStream* st)               const;
  void print_extended_on(outputStream *st)      const;
  void print_tracing_info()                     const;
  void print_gc_threads_on(outputStream* st)    const;
  void print_heap_regions_on(outputStream* st)  const;

  void stop();

  void prepare_for_verify();
  void verify(bool silent, VerifyOption vo);

// ---------- Heap counters and metrics
//
private:
           size_t _initial_size;
  volatile jlong  _used;
  volatile size_t _committed;
  volatile jlong  _bytes_allocated_since_gc_start;

public:
  void increase_used(size_t bytes);
  void decrease_used(size_t bytes);
  void set_used(size_t bytes);

  void increase_committed(size_t bytes);
  void decrease_committed(size_t bytes);
  void increase_allocated(size_t bytes);

  size_t bytes_allocated_since_gc_start();
  void reset_bytes_allocated_since_gc_start();

  size_t max_capacity()     const;
  size_t initial_capacity() const;
  size_t capacity()         const;
  size_t used()             const;
  size_t committed()        const;

// ---------- Workers handling
//
private:
  uint _max_workers;
  ShenandoahWorkGang* _workers;

public:
  uint max_workers();
  void assert_gc_workers(uint nworker) PRODUCT_RETURN;

  ShenandoahWorkGang* workers() const { return _workers;}

  void gc_threads_do(ThreadClosure* tcl) const;

// ---------- Heap regions handling machinery
//
private:
  MemRegion _heap_region;
  size_t    _num_regions;
  ShenandoahHeapRegion** _regions;
  ShenandoahRegionIterator _update_refs_iterator;

public:
  inline size_t num_regions() const { return _num_regions; }

  inline ShenandoahHeapRegion* const heap_region_containing(const void* addr) const;
  inline size_t heap_region_index_containing(const void* addr) const;

  inline ShenandoahHeapRegion* const get_region(size_t region_idx) const;

  void heap_region_iterate(ShenandoahHeapRegionClosure* blk,
                           bool skip_cset_regions = false,
                           bool skip_humongous_continuation = false) const;

// ---------- GC state machinery
//
// GC state describes the important parts of collector state, that may be
// used to make barrier selection decisions in the native and generated code.
// Multiple bits can be set at once.
//
// Important invariant: when GC state is zero, the heap is stable, and no barriers
// are required.
//
public:
  enum GCStateBitPos {
    // Heap has forwarded objects: need RB, ACMP, CAS barriers.
    HAS_FORWARDED_BITPOS   = 0,

    // Heap is under marking: needs SATB barriers.
    MARKING_BITPOS    = 1,

    // Heap is under evacuation: needs WB barriers. (Set together with UNSTABLE)
    EVACUATION_BITPOS = 2,

    // Heap is under updating: needs SVRB/SVWB barriers.
    UPDATEREFS_BITPOS = 3,
  };

  enum GCState {
    STABLE        = 0,
    HAS_FORWARDED = 1 << HAS_FORWARDED_BITPOS,
    MARKING       = 1 << MARKING_BITPOS,
    EVACUATION    = 1 << EVACUATION_BITPOS,
    UPDATEREFS    = 1 << UPDATEREFS_BITPOS,
  };

private:
  ShenandoahSharedBitmap _gc_state;
  ShenandoahSharedFlag   _degenerated_gc_in_progress;
  ShenandoahSharedFlag   _full_gc_in_progress;
  ShenandoahSharedFlag   _full_gc_move_in_progress;
  ShenandoahSharedFlag   _progress_last_gc;

  void set_gc_state_mask(uint mask, bool value);

public:
  char gc_state();
  static address gc_state_addr();

  void set_concurrent_mark_in_progress(bool in_progress);
  void set_evacuation_in_progress(bool in_progress);
  void set_update_refs_in_progress(bool in_progress);
  void set_degenerated_gc_in_progress(bool in_progress);
  void set_full_gc_in_progress(bool in_progress);
  void set_full_gc_move_in_progress(bool in_progress);
  void set_has_forwarded_objects(bool cond);

  inline bool is_stable() const;
  inline bool is_idle() const;
  inline bool is_concurrent_mark_in_progress() const;
  inline bool is_update_refs_in_progress() const;
  inline bool is_evacuation_in_progress() const;
  inline bool is_degenerated_gc_in_progress() const;
  inline bool is_full_gc_in_progress() const;
  inline bool is_full_gc_move_in_progress() const;
  inline bool has_forwarded_objects() const;
  inline bool is_gc_in_progress_mask(uint mask) const;

// ---------- GC cancellation and degeneration machinery
//
// Cancelled GC flag is used to notify concurrent phases that they should terminate.
//
public:
  enum ShenandoahDegenPoint {
    _degenerated_unset,
    _degenerated_outside_cycle,
    _degenerated_mark,
    _degenerated_evac,
    _degenerated_updaterefs,
    _DEGENERATED_LIMIT,
  };

  static const char* degen_point_to_string(ShenandoahDegenPoint point) {
    switch (point) {
      case _degenerated_unset:
        return "<UNSET>";
      case _degenerated_outside_cycle:
        return "Outside of Cycle";
      case _degenerated_mark:
        return "Mark";
      case _degenerated_evac:
        return "Evacuation";
      case _degenerated_updaterefs:
        return "Update Refs";
      default:
        ShouldNotReachHere();
        return "ERROR";
    }
  };

private:
  ShenandoahSharedFlag _cancelled_gc;
  inline bool try_cancel_gc();

public:
  static address cancelled_gc_addr();

  inline bool cancelled_gc() const;

  inline void clear_cancelled_gc();

  void cancel_gc(GCCause::Cause cause);

// ---------- GC operations entry points
//
public:
  // Entry points to STW GC operations, these cause a related safepoint, that then
  // call the entry method below
  void vmop_entry_init_mark();
  void vmop_entry_final_mark();
  void vmop_entry_final_evac();
  void vmop_entry_init_updaterefs();
  void vmop_entry_final_updaterefs();
  void vmop_entry_full(GCCause::Cause cause);
  void vmop_degenerated(ShenandoahDegenPoint point);

  // Entry methods to normally STW GC operations. These set up logging, monitoring
  // and workers for net VM operation
  void entry_init_mark();
  void entry_final_mark();
  void entry_final_evac();
  void entry_init_updaterefs();
  void entry_final_updaterefs();
  void entry_full(GCCause::Cause cause);
  void entry_degenerated(int point);

  // Entry methods to normally concurrent GC operations. These set up logging, monitoring
  // for concurrent operation.
  void entry_reset();
  void entry_mark();
  void entry_preclean();
  void entry_cleanup();
  void entry_evac();
  void entry_updaterefs();
  void entry_uncommit(double shrink_before);

private:
  // Actual work for the phases
  void op_init_mark();
  void op_final_mark();
  void op_final_evac();
  void op_init_updaterefs();
  void op_final_updaterefs();
  void op_full(GCCause::Cause cause);
  void op_degenerated(ShenandoahDegenPoint point);
  void op_degenerated_fail();
  void op_degenerated_futile();

  void op_reset();
  void op_mark();
  void op_preclean();
  void op_cleanup();
  void op_evac();
  void op_updaterefs();
  void op_uncommit(double shrink_before);

  // Messages for GC trace event, they have to be immortal for
  // passing around the logging/tracing systems
  const char* init_mark_event_message() const;
  const char* final_mark_event_message() const;
  const char* conc_mark_event_message() const;
  const char* degen_event_message(ShenandoahDegenPoint point) const;

// ---------- GC subsystems
//
private:
  ShenandoahControlThread*   _control_thread;
  ShenandoahCollectorPolicy* _shenandoah_policy;
  ShenandoahHeuristics*      _heuristics;
  ShenandoahFreeSet*         _free_set;
  ShenandoahConcurrentMark*  _scm;
  ShenandoahMarkCompact*     _full_gc;
  ShenandoahPacer*           _pacer;
  ShenandoahVerifier*        _verifier;

  ShenandoahAllocTracker*    _alloc_tracker;
  ShenandoahPhaseTimings*    _phase_timings;

  ShenandoahControlThread*   control_thread()          { return _control_thread;    }
  ShenandoahMarkCompact*     full_gc()                 { return _full_gc;           }

public:
  ShenandoahCollectorPolicy* shenandoah_policy() const { return _shenandoah_policy; }
  ShenandoahHeuristics*      heuristics()        const { return _heuristics;        }
  ShenandoahFreeSet*         free_set()          const { return _free_set;          }
  ShenandoahConcurrentMark*  concurrent_mark()         { return _scm;               }
  ShenandoahPacer*           pacer()             const { return _pacer;             }

  ShenandoahPhaseTimings*    phase_timings()     const { return _phase_timings;     }
  ShenandoahAllocTracker*    alloc_tracker()     const { return _alloc_tracker;     }

  ShenandoahVerifier*        verifier();

// ---------- VM subsystem bindings
//
private:
  ShenandoahMonitoringSupport* _monitoring_support;
  ConcurrentGCTimer* _gc_timer;

public:
  ShenandoahMonitoringSupport* monitoring_support() { return _monitoring_support; }

  GCTracer* tracer();
  GCTimer* gc_timer() const;
  CollectorPolicy* collector_policy() const;

// ---------- Reference processing
//
private:
  ReferenceProcessor*  _ref_processor;
  ShenandoahSharedFlag _process_references;

  void ref_processing_init();

public:
  ReferenceProcessor* ref_processor() { return _ref_processor;}
  void set_process_references(bool pr);
  bool process_references() const;

// ---------- Class Unloading
//
private:
  ShenandoahSharedFlag _unload_classes;

public:
  void set_unload_classes(bool uc);
  bool unload_classes() const;

  // Delete entries for dead interned string and clean up unreferenced symbols
  // in symbol table, possibly in parallel.
  void unload_classes_and_cleanup_tables(bool full_gc);

// ---------- Generic interface hooks
// Minor things that super-interface expects us to implement to play nice with
// the rest of runtime. Some of the things here are not required to be implemented,
// and can be stubbed out.
//
public:
  AdaptiveSizePolicy* size_policy() shenandoah_not_implemented_return(NULL);
  bool is_maximal_no_gc() const shenandoah_not_implemented_return(false);

  bool is_in(const void* p) const;

  // All objects can potentially move
  bool is_scavengable(const void* addr) { return true; }

  void collect(GCCause::Cause cause);
  void do_full_collection(bool clear_all_soft_refs);

  // Used for parsing heap during error printing
  HeapWord* block_start(const void* addr) const;
  size_t block_size(const HeapWord* addr) const;
  bool block_is_obj(const HeapWord* addr) const;

  // Used for native heap walkers: heap dumpers, mostly
  void object_iterate(ObjectClosure* cl);
  void safe_object_iterate(ObjectClosure* cl);
  void space_iterate(SpaceClosure* scl);
  void oop_iterate(ExtendedOopClosure* cl);
  Space* space_containing(const void* oop) const;

  // Used by RMI
  jlong millis_since_last_gc();

  bool can_elide_tlab_store_barriers() const                  { return true;    }
  oop new_store_pre_barrier(JavaThread* thread, oop new_obj)  { return new_obj; }
  bool can_elide_initializing_store_barrier(oop new_obj)      { return true;    }
  bool card_mark_must_follow_store() const                    { return false;   }

  bool is_in_partial_collection(const void* p) shenandoah_not_implemented_return(false);
  bool supports_heap_inspection() const { return false; }

  void gc_prologue(bool b);
  void gc_epilogue(bool b);

  void acquire_pending_refs_lock();
  void release_pending_refs_lock();

// ---------- Code roots handling hooks
//
public:
  void register_nmethod(nmethod* nm);
  void unregister_nmethod(nmethod* nm);

// ---------- Pinning hooks
//
public:
  // Shenandoah supports per-object (per-region) pinning
  bool supports_object_pinning() const { return true; }

  oop pin_object(JavaThread* thread, oop obj);
  void unpin_object(JavaThread* thread, oop obj);

// ---------- Allocation support
//
private:
  HeapWord* allocate_memory_under_lock(ShenandoahAllocRequest& request, bool& in_new_region);
  inline HeapWord* allocate_from_gclab(Thread* thread, size_t size);
  HeapWord* allocate_from_gclab_slow(Thread* thread, size_t size);
  HeapWord* allocate_new_gclab(size_t min_size, size_t word_size, size_t* actual_size);

public:
#ifndef CC_INTERP
  void compile_prepare_oop(MacroAssembler* masm, Register obj);
#endif

  HeapWord* allocate_memory(ShenandoahAllocRequest& request);
  HeapWord* mem_allocate(size_t size, bool* what);

  uint oop_extra_words();

  void notify_mutator_alloc_words(size_t words, bool waste);

  // Shenandoah supports TLAB allocation
  bool supports_tlab_allocation() const { return true; }

  HeapWord* allocate_new_tlab(size_t word_size);
  size_t tlab_capacity(Thread *thr) const;
  size_t unsafe_max_tlab_alloc(Thread *thread) const;
  size_t max_tlab_size() const;
  size_t tlab_used(Thread* ignored) const;

  HeapWord* tlab_post_allocation_setup(HeapWord* obj);

  void resize_tlabs();
  void resize_all_tlabs();

  void accumulate_statistics_tlabs();
  void accumulate_statistics_all_gclabs();

  void make_parsable(bool retire_tlabs);
  void ensure_parsability(bool retire_tlabs);


// ---------- Marking support
//
private:
  ShenandoahMarkingContext* _marking_context;
  MemRegion _bitmap_region;
  MemRegion _aux_bitmap_region;
  MarkBitMap _verification_bit_map;
  MarkBitMap _aux_bit_map;

  size_t _bitmap_size;
  size_t _bitmap_regions_per_slice;
  size_t _bitmap_bytes_per_slice;

public:
  inline ShenandoahMarkingContext* complete_marking_context() const;
  inline ShenandoahMarkingContext* marking_context() const;
  inline void mark_complete_marking_context();
  inline void mark_incomplete_marking_context();

  template<class T>
  inline void marked_object_iterate(ShenandoahHeapRegion* region, T* cl);

  template<class T>
  inline void marked_object_iterate(ShenandoahHeapRegion* region, T* cl, HeapWord* limit);

  template<class T>
  inline void marked_object_oop_iterate(ShenandoahHeapRegion* region, T* cl, HeapWord* limit);

  void reset_mark_bitmap();

  // SATB barriers hooks
  inline bool requires_marking(const void* entry) const;
  void force_satb_flush_all_threads();

  // Support for bitmap uncommits
  bool commit_bitmap_slice(ShenandoahHeapRegion *r);
  bool uncommit_bitmap_slice(ShenandoahHeapRegion *r);
  bool is_bitmap_slice_committed(ShenandoahHeapRegion* r, bool skip_self = false);

// ---------- Evacuation support
//
private:
  ShenandoahCollectionSet* _collection_set;
  ShenandoahEvacOOMHandler _oom_evac_handler;

  void evacuate_and_update_roots();

public:
  static address in_cset_fast_test_addr();


  ShenandoahCollectionSet* collection_set() const { return _collection_set; }

  template <class T>
  inline bool in_collection_set(T obj) const;

  // Avoid accidentally calling the method above with ShenandoahHeapRegion*, which would be *wrong*.
  inline bool in_collection_set(ShenandoahHeapRegion* r) shenandoah_not_implemented_return(false);

  // Evacuates object src. Returns the evacuated object, either evacuated
  // by this thread, or by some other thread.
  inline oop  evacuate_object(oop src, Thread* thread, bool& evacuated);

  // Call before/after evacuation.
  void enter_evacuation();
  void leave_evacuation();

// ---------- Helper functions
//
public:

  template <class T>
  inline oop maybe_update_with_forwarded(T* p);

  template <class T>
  inline oop maybe_update_with_forwarded_not_null(T* p, oop obj);

  template <class T>
  inline oop update_with_forwarded_not_null(T* p, oop obj);

  inline oop atomic_compare_exchange_oop(oop n, narrowOop* addr, oop c);
  inline oop atomic_compare_exchange_oop(oop n, oop* addr, oop c);

  void trash_humongous_region_at(ShenandoahHeapRegion *r);

  void stop_concurrent_marking();

  void roots_iterate(OopClosure* cl);

private:
  void trash_cset_regions();
  void update_heap_references(bool concurrent);

// ---------- Testing helpers functions
//
private:
  ShenandoahSharedFlag _inject_alloc_failure;

  void try_inject_alloc_failure();
  bool should_inject_alloc_failure();
};

#endif // SHARE_VM_GC_SHENANDOAH_SHENANDOAHHEAP_HPP
