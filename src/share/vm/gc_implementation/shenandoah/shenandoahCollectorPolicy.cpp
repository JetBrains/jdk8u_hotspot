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
#include "gc_implementation/shared/gcTimer.hpp"
#include "gc_implementation/shenandoah/shenandoahCollectionSet.hpp"
#include "gc_implementation/shenandoah/shenandoahFreeSet.hpp"
#include "gc_implementation/shenandoah/shenandoahCollectorPolicy.hpp"
#include "gc_implementation/shenandoah/shenandoahHeap.inline.hpp"
#include "gc_implementation/shenandoah/shenandoahLogging.hpp"
#include "gc_implementation/shenandoah/shenandoahPhaseTimes.hpp"

class ShenandoahHeuristics : public CHeapObj<mtGC> {

  NumberSeq _allocation_rate_bytes;
  NumberSeq _reclamation_rate_bytes;

  size_t _bytes_allocated_since_CM;
  size_t _bytes_reclaimed_this_cycle;

protected:
  typedef struct {
    size_t region_number;
    size_t garbage;
  } RegionGarbage;

  static int compare_by_garbage(RegionGarbage a, RegionGarbage b) {
    if (a.garbage > b.garbage)
      return -1;
    else if (a.garbage < b.garbage)
      return 1;
    else return 0;
  }

  RegionGarbage* get_region_garbage_cache(size_t num) {
    RegionGarbage* res = _region_garbage;
    if (res == NULL) {
      res = NEW_C_HEAP_ARRAY(RegionGarbage, num, mtGC);
      _region_garbage_size = num;
    } else if (_region_garbage_size < num) {
      REALLOC_C_HEAP_ARRAY(RegionGarbage, _region_garbage, num, mtGC);
      _region_garbage_size = num;
    }
    return res;
  }

  RegionGarbage* _region_garbage;
  size_t _region_garbage_size;

  size_t _bytes_allocated_start_CM;
  size_t _bytes_allocated_during_CM;

  uint _cancelled_cm_cycles_in_a_row;
  uint _successful_cm_cycles_in_a_row;

  size_t _bytes_in_cset;

public:

  ShenandoahHeuristics();
  ~ShenandoahHeuristics();

  void record_bytes_allocated(size_t bytes);
  void record_bytes_reclaimed(size_t bytes);
  void record_bytes_start_CM(size_t bytes);
  void record_bytes_end_CM(size_t bytes);

  size_t bytes_in_cset() const { return _bytes_in_cset; }

  virtual void print_thresholds() {
  }

  virtual bool should_start_concurrent_mark(size_t used, size_t capacity) const=0;

  virtual bool handover_cancelled_marking() {
    return _cancelled_cm_cycles_in_a_row <= ShenandoahFullGCThreshold;
  }

  virtual void record_cm_cancelled() {
    _cancelled_cm_cycles_in_a_row++;
    _successful_cm_cycles_in_a_row = 0;
  }

  virtual void record_cm_success() {
    _cancelled_cm_cycles_in_a_row = 0;
    _successful_cm_cycles_in_a_row++;
  }

  virtual void record_full_gc() {
    _bytes_in_cset = 0;
  }

  virtual void start_choose_collection_set() {
  }
  virtual void end_choose_collection_set() {
  }
  virtual bool region_in_collection_set(ShenandoahHeapRegion* r, size_t immediate_garbage) = 0;

  virtual void choose_collection_set(ShenandoahCollectionSet* collection_set, int* connections=NULL);
  virtual void choose_free_set(ShenandoahFreeSet* free_set);

  virtual bool process_references() {
    if (ShenandoahRefProcFrequency == 0) return false;
    size_t cycle = ShenandoahHeap::heap()->shenandoahPolicy()->cycle_counter();
    // Process references every Nth GC cycle.
    return cycle % ShenandoahRefProcFrequency == 0;
  }

  virtual bool unload_classes() {
    if (ShenandoahUnloadClassesFrequency == 0) return false;
    size_t cycle = ShenandoahHeap::heap()->shenandoahPolicy()->cycle_counter();
    // Unload classes every Nth GC cycle.
    // This should not happen in the same cycle as process_references to amortize costs.
    // Offsetting by one is enough to break the rendezvous when periods are equal.
    // When periods are not equal, offsetting by one is just as good as any other guess.
    return (cycle + 1) % ShenandoahUnloadClassesFrequency == 0;
  }

  virtual bool needs_regions_sorted_by_garbage() {
    // Most of them do not.
    return false;
  }
};

ShenandoahHeuristics::ShenandoahHeuristics() :
  _bytes_allocated_since_CM(0),
  _bytes_reclaimed_this_cycle(0),
  _bytes_allocated_start_CM(0),
  _bytes_allocated_during_CM(0),
  _bytes_in_cset(0),
  _cancelled_cm_cycles_in_a_row(0),
  _successful_cm_cycles_in_a_row(0),
  _region_garbage(NULL),
  _region_garbage_size(0)
{
}

ShenandoahHeuristics::~ShenandoahHeuristics() {
  if (_region_garbage != NULL) {
    FREE_C_HEAP_ARRAY(RegionGarbage, _region_garbage, mtGC);
  }
}

void ShenandoahHeuristics::choose_collection_set(ShenandoahCollectionSet* collection_set, int* connections) {
  start_choose_collection_set();

  ShenandoahHeap* heap = ShenandoahHeap::heap();

  // Step 1. Build up the region candidates we care about, rejecting losers and accepting winners right away.

  ShenandoahHeapRegionSet* regions = heap->regions();
  size_t active = regions->active_regions();

  RegionGarbage* candidates = get_region_garbage_cache(active);

  size_t cand_idx = 0;
  _bytes_in_cset = 0;

  size_t immediate_garbage = 0;
  size_t immediate_regions = 0;
  for (size_t i = 0; i < active; i++) {
    ShenandoahHeapRegion* region = regions->get(i);

    if (! region->is_humongous() && ! region->is_pinned()) {
      if ((! region->is_empty()) && ! region->has_live()) {
        // We can recycle it right away and put it in the free set.
        immediate_regions++;
        immediate_garbage += region->garbage();
        heap->decrease_used(region->used());
        region->recycle();
        log_develop_trace(gc)("Choose region " SIZE_FORMAT " for immediate reclaim with garbage = " SIZE_FORMAT
                              " and live = " SIZE_FORMAT "\n",
                              region->region_number(), region->garbage(), region->get_live_data_bytes());
      } else {
        // This is our candidate for later consideration.
        candidates[cand_idx].region_number = region->region_number();
        candidates[cand_idx].garbage = region->garbage();
        cand_idx++;
      }
    } else {
      assert(region->has_live() || region->is_empty() || region->is_pinned() || region->is_humongous(), "check rejected");
      log_develop_trace(gc)("Rejected region " SIZE_FORMAT " with garbage = " SIZE_FORMAT
                            " and live = " SIZE_FORMAT "\n",
                            region->region_number(), region->garbage(), region->get_live_data_bytes());
    }
  }

  // Step 2. Process the remanining candidates, if any.

  if (cand_idx > 0) {
    if (needs_regions_sorted_by_garbage()) {
      QuickSort::sort<RegionGarbage>(candidates, (int)cand_idx, compare_by_garbage, false);
    }

    for (size_t i = 0; i < cand_idx; i++) {
      ShenandoahHeapRegion *region = regions->get_fast(candidates[i].region_number);
      if (region_in_collection_set(region, immediate_garbage)) {
        log_develop_trace(gc)("Choose region " SIZE_FORMAT " with garbage = " SIZE_FORMAT
                                      " and live = " SIZE_FORMAT "\n",
                              region->region_number(), region->garbage(), region->get_live_data_bytes());
        collection_set->add_region(region);
        region->set_in_collection_set(true);
        _bytes_in_cset += region->used();
      }
    }
  }

  end_choose_collection_set();

  size_t total_garbage = heap->garbage();
  log_debug(gc)("Total Garbage: "SIZE_FORMAT, total_garbage);
  log_debug(gc)("Immediate Garbage: "SIZE_FORMAT, immediate_garbage);
  log_debug(gc)("Immediate Garbage regions: "SIZE_FORMAT, immediate_regions);
  log_debug(gc)("Garbage to be collected: "SIZE_FORMAT, collection_set->garbage());
  log_debug(gc)("Objects to be evacuated: "SIZE_FORMAT, collection_set->live_data());
  log_debug(gc)("Live / Garbage ratio: "SIZE_FORMAT"%%", collection_set->live_data() * 100 / MAX2(collection_set->garbage(), (size_t)1));
  log_debug(gc)("Collected-Garbage ratio / Total-garbage: "SIZE_FORMAT"%%", collection_set->garbage() * 100 / MAX2(total_garbage, (size_t)1));
}

void ShenandoahHeuristics::choose_free_set(ShenandoahFreeSet* free_set) {

  ShenandoahHeapRegionSet* ordered_regions = ShenandoahHeap::heap()->regions();
  size_t i = 0;
  size_t end = ordered_regions->active_regions();

  ShenandoahHeap* heap = ShenandoahHeap::heap();
  while (i < end) {
    ShenandoahHeapRegion* region = ordered_regions->get(i++);
    if ((! heap->in_collection_set(region))
        && (! region->is_humongous())
        && (! region->is_pinned())) {
      free_set->add_region(region);
    }
  }
}

void ShenandoahCollectorPolicy::record_workers_start(TimingPhase phase) {
  for (uint i = 0; i < ShenandoahPhaseTimes::GCParPhasesSentinel; i++) {
    _phase_times->reset(i);
  }
}

void ShenandoahCollectorPolicy::record_workers_end(TimingPhase phase) {
  if (phase != _num_phases) {
    for (uint i = 0; i < ShenandoahPhaseTimes::GCParPhasesSentinel; i++) {
      double t = _phase_times->average(i);
      _timing_data[phase + i]._secs.add(t);
    }
  }
}

void ShenandoahCollectorPolicy::record_phase_start(TimingPhase phase) {
  _timing_data[phase]._start = os::elapsedTime();

}

void ShenandoahCollectorPolicy::record_phase_end(TimingPhase phase) {
  double end = os::elapsedTime();
  double elapsed = end - _timing_data[phase]._start;
  _timing_data[phase]._secs.add(elapsed);
}

void ShenandoahCollectorPolicy::report_concgc_cancelled() {
}

void ShenandoahHeuristics::record_bytes_allocated(size_t bytes) {
  _bytes_allocated_since_CM = bytes;
  _bytes_allocated_start_CM = bytes;
  _allocation_rate_bytes.add(bytes);
}

void ShenandoahHeuristics::record_bytes_reclaimed(size_t bytes) {
  _bytes_reclaimed_this_cycle = bytes;
  _reclamation_rate_bytes.add(bytes);
}

void ShenandoahHeuristics::record_bytes_start_CM(size_t bytes) {
  _bytes_allocated_start_CM = bytes;
}

void ShenandoahHeuristics::record_bytes_end_CM(size_t bytes) {
  _bytes_allocated_during_CM = (bytes > _bytes_allocated_start_CM) ? (bytes - _bytes_allocated_start_CM)
                                                                   : bytes;
}

class PassiveHeuristics : public ShenandoahHeuristics {
public:
  PassiveHeuristics() : ShenandoahHeuristics() {
  }

  virtual bool region_in_collection_set(ShenandoahHeapRegion* r, size_t immediate_garbage) {
    return r->garbage() > 0;
  }

  virtual bool should_start_concurrent_mark(size_t used, size_t capacity) const {
    // Never do concurrent GCs.
    return false;
  }

  virtual bool process_references() {
    // Randomly process refs with 50% chance.
    return (os::random() & 1) == 1;
  }

  virtual bool unload_classes() {
    // Randomly unload classes with 50% chance.
    return (os::random() & 1) == 1;
  }
};

class AggressiveHeuristics : public ShenandoahHeuristics {
public:
  AggressiveHeuristics() : ShenandoahHeuristics() {
  }

  virtual bool region_in_collection_set(ShenandoahHeapRegion* r, size_t immediate_garbage) {
    return r->garbage() > 0;
  }

  virtual bool should_start_concurrent_mark(size_t used, size_t capacity) const {
    return true;
  }

  virtual bool process_references() {
    // Randomly process refs with 50% chance.
    return (os::random() & 1) == 1;
  }

  virtual bool unload_classes() {
    // Randomly unload classes with 50% chance.
    return (os::random() & 1) == 1;
  }
};

class DynamicHeuristics : public ShenandoahHeuristics {
public:
  DynamicHeuristics() : ShenandoahHeuristics() {
  }

  void print_thresholds() {
    log_info(gc, init)("Shenandoah heuristics thresholds: allocation "SIZE_FORMAT", free "SIZE_FORMAT", garbage "SIZE_FORMAT,
                       ShenandoahAllocationThreshold,
                       ShenandoahFreeThreshold,
                       ShenandoahGarbageThreshold);
  }

  virtual ~DynamicHeuristics() {}

  virtual bool should_start_concurrent_mark(size_t used, size_t capacity) const {

    bool shouldStartConcurrentMark = false;

    ShenandoahHeap* heap = ShenandoahHeap::heap();
    size_t free_capacity = heap->free_regions()->capacity();
    size_t free_used = heap->free_regions()->used();
    assert(free_used <= free_capacity, "must use less than capacity");
    size_t cset = MIN2(_bytes_in_cset, (ShenandoahCSetThreshold * capacity) / 100);
    size_t available =  free_capacity - free_used + cset;
    uintx threshold = ShenandoahFreeThreshold + ShenandoahCSetThreshold;
    size_t targetStartMarking = (capacity * threshold) / 100;

    size_t threshold_bytes_allocated = heap->capacity() * ShenandoahAllocationThreshold / 100;
    if (available < targetStartMarking &&
        heap->bytes_allocated_since_cm() > threshold_bytes_allocated)
    {
      // Need to check that an appropriate number of regions have
      // been allocated since last concurrent mark too.
      shouldStartConcurrentMark = true;
    }

    return shouldStartConcurrentMark;
  }

  virtual bool region_in_collection_set(ShenandoahHeapRegion* r, size_t immediate_garbage) {
    size_t threshold = ShenandoahHeapRegion::region_size_bytes() * ShenandoahGarbageThreshold / 100;
    return r->garbage() > threshold;
  }

};


class AdaptiveHeuristics : public ShenandoahHeuristics {
private:
  uintx _free_threshold;
  TruncatedSeq* _cset_history;

public:
  AdaptiveHeuristics() :
    ShenandoahHeuristics(),
    _free_threshold(ShenandoahInitFreeThreshold),
    _cset_history(new TruncatedSeq((uint)ShenandoahHappyCyclesThreshold)) {

    _cset_history->add((double) ShenandoahCSetThreshold);
    _cset_history->add((double) ShenandoahCSetThreshold);
  }

  virtual ~AdaptiveHeuristics() {
    delete _cset_history;
  }

  virtual bool region_in_collection_set(ShenandoahHeapRegion* r, size_t immediate_garbage) {
    size_t threshold = ShenandoahHeapRegion::region_size_bytes() * ShenandoahGarbageThreshold / 100;
    return r->garbage() > threshold;
  }

  virtual void record_cm_cancelled() {
    ShenandoahHeuristics::record_cm_cancelled();
    if (_free_threshold < ShenandoahMaxFreeThreshold) {
      _free_threshold++;
      log_info(gc,ergo)("increasing free threshold to: "UINTX_FORMAT, _free_threshold);
    }
  }

  virtual void record_cm_success() {
    ShenandoahHeuristics::record_cm_success();
    if (_successful_cm_cycles_in_a_row > ShenandoahHappyCyclesThreshold &&
        _free_threshold > ShenandoahMinFreeThreshold) {
      _free_threshold--;
      log_info(gc,ergo)("reducing free threshold to: "UINTX_FORMAT, _free_threshold);
      _successful_cm_cycles_in_a_row = 0;
    }
  }

  virtual bool should_start_concurrent_mark(size_t used, size_t capacity) const {
    bool shouldStartConcurrentMark = false;

    ShenandoahHeap* heap = ShenandoahHeap::heap();
    size_t free_capacity = heap->free_regions()->capacity();
    size_t free_used = heap->free_regions()->used();
    assert(free_used <= free_capacity, "must use less than capacity");
    // size_t cset_threshold = (size_t) _cset_history->maximum();
    size_t cset_threshold = (size_t) _cset_history->davg();
    size_t cset = MIN2(_bytes_in_cset, (cset_threshold * capacity) / 100);
    size_t available =  free_capacity - free_used + cset;
    uintx factor = _free_threshold + cset_threshold;
    size_t targetStartMarking = (capacity * factor) / 100;

    size_t threshold_bytes_allocated = heap->capacity() * ShenandoahAllocationThreshold / 100;
    if (available < targetStartMarking &&
        heap->bytes_allocated_since_cm() > threshold_bytes_allocated)
    {
      // Need to check that an appropriate number of regions have
      // been allocated since last concurrent mark too.
      shouldStartConcurrentMark = true;
    }

    if (shouldStartConcurrentMark) {
      log_info(gc,ergo)("predicted cset threshold: "SIZE_FORMAT, cset_threshold);
      log_info(gc,ergo)("Starting concurrent mark at "SIZE_FORMAT"K CSet ("SIZE_FORMAT"%%)", _bytes_in_cset / K, _bytes_in_cset * 100 / capacity);
      _cset_history->add((double) (_bytes_in_cset * 100 / capacity));
    }
    return shouldStartConcurrentMark;
  }

};

ShenandoahCollectorPolicy::ShenandoahCollectorPolicy() :
  _cycle_counter(0),
  _successful_cm(0),
  _degenerated_cm(0)
{

  ShenandoahHeapRegion::setup_heap_region_size(initial_heap_byte_size(), max_heap_byte_size());

  initialize_all();

  _tracer = new (ResourceObj::C_HEAP, mtGC) ShenandoahTracer();
  _stw_timer = new (ResourceObj::C_HEAP, mtGC) STWGCTimer();
  _conc_timer = new (ResourceObj::C_HEAP, mtGC) ConcurrentGCTimer();
  _user_requested_gcs = 0;
  _allocation_failure_gcs = 0;
  _conc_gc_aborted = false;

  _phase_names[total_pause]                     = "Total Pauses (net)";
  _phase_names[total_pause_gross]               = "Total Pauses (gross)";
  _phase_names[init_mark]                       = "Initial Mark Pauses (net)";
  _phase_names[init_mark_gross]                 = "Initial Mark Pauses (gross)";
  _phase_names[final_mark]                      = "Final Mark Pauses (net)";
  _phase_names[final_mark_gross]                = "Final Mark Pauses (gross)";
  _phase_names[accumulate_stats]                = "  Accumulate Stats";
  _phase_names[make_parsable]                   = "  Make Parsable";
  _phase_names[clear_liveness]                  = "  Clear Liveness";
  _phase_names[scan_roots]                      = "  Scan Roots";
  _phase_names[update_roots]                    = "  Update Roots";
  _phase_names[drain_satb]                      = "  Drain SATB";
  _phase_names[weakrefs]                        = "  Weak References";
  _phase_names[class_unloading]                 = "  Class Unloading";
  _phase_names[prepare_evac]                    = "  Prepare Evacuation";
  _phase_names[init_evac]                       = "  Initial Evacuation";

  _phase_names[scan_thread_roots]               = "    S: Thread Roots";
  _phase_names[scan_code_roots]                 = "    S: Code Cache Roots";
  _phase_names[scan_string_table_roots]         = "    S: String Table Roots";
  _phase_names[scan_universe_roots]             = "    S: Universe Roots";
  _phase_names[scan_jni_roots]                  = "    S: JNI Roots";
  _phase_names[scan_jni_weak_roots]             = "    S: JNI Weak Roots";
  _phase_names[scan_synchronizer_roots]         = "    S: Synchronizer Roots";
  _phase_names[scan_flat_profiler_roots]        = "    S: Flat Profiler Roots";
  _phase_names[scan_management_roots]           = "    S: Management Roots";
  _phase_names[scan_system_dictionary_roots]    = "    S: System Dict Roots";
  _phase_names[scan_cldg_roots]                 = "    S: CLDG Roots";
  _phase_names[scan_jvmti_roots]                = "    S: JVMTI Roots";

  _phase_names[update_thread_roots]             = "    U: Thread Roots";
  _phase_names[update_code_roots]               = "    U: Code Cache Roots";
  _phase_names[update_string_table_roots]       = "    U: String Table Roots";
  _phase_names[update_universe_roots]           = "    U: Universe Roots";
  _phase_names[update_jni_roots]                = "    U: JNI Roots";
  _phase_names[update_jni_weak_roots]           = "    U: JNI Weak Roots";
  _phase_names[update_synchronizer_roots]       = "    U: Synchronizer Roots";
  _phase_names[update_flat_profiler_roots]      = "    U: Flat Profiler Roots";
  _phase_names[update_management_roots]         = "    U: Management Roots";
  _phase_names[update_system_dictionary_roots]  = "    U: System Dict Roots";
  _phase_names[update_cldg_roots]               = "    U: CLDG Roots";
  _phase_names[update_jvmti_roots]              = "    U: JVMTI Roots";

  _phase_names[evac_thread_roots]               = "    E: Thread Roots";
  _phase_names[evac_code_roots]                 = "    E: Code Cache Roots";
  _phase_names[evac_string_table_roots]         = "    E: String Table Roots";
  _phase_names[evac_universe_roots]             = "    E: Universe Roots";
  _phase_names[evac_jni_roots]                  = "    E: JNI Roots";
  _phase_names[evac_jni_weak_roots]             = "    E: JNI Weak Roots";
  _phase_names[evac_synchronizer_roots]         = "    E: Synchronizer Roots";
  _phase_names[evac_flat_profiler_roots]        = "    E: Flat Profiler Roots";
  _phase_names[evac_management_roots]           = "    E: Management Roots";
  _phase_names[evac_system_dictionary_roots]    = "    E: System Dict Roots";
  _phase_names[evac_cldg_roots]                 = "    E: CLDG Roots";
  _phase_names[evac_jvmti_roots]                = "    E: JVMTI Roots";

  _phase_names[recycle_regions]                 = "  Recycle regions";
  _phase_names[reset_bitmaps]                   = "Reset Bitmaps";
  _phase_names[resize_tlabs]                    = "Resize TLABs";

  _phase_names[full_gc]                         = "Full GC";
  _phase_names[full_gc_heapdumps]               = "  Heap Dumps";
  _phase_names[full_gc_prepare]                 = "  Prepare";
  _phase_names[full_gc_mark]                    = "  Mark";
  _phase_names[full_gc_mark_drain_queues]       = "    Drain Queues";
  _phase_names[full_gc_mark_weakrefs]           = "    Weak References";
  _phase_names[full_gc_mark_class_unloading]    = "    Class Unloading";
  _phase_names[full_gc_calculate_addresses]     = "  Calculate Addresses";
  _phase_names[full_gc_adjust_pointers]         = "  Adjust Pointers";
  _phase_names[full_gc_copy_objects]            = "  Copy Objects";

  _phase_names[conc_mark]                       = "Concurrent Marking";
  _phase_names[conc_evac]                       = "Concurrent Evacuation";

  if (ShenandoahGCHeuristics != NULL) {
    if (strcmp(ShenandoahGCHeuristics, "aggressive") == 0) {
      log_info(gc, init)("Shenandoah heuristics: aggressive");
      _heuristics = new AggressiveHeuristics();
    } else if (strcmp(ShenandoahGCHeuristics, "dynamic") == 0) {
      log_info(gc, init)("Shenandoah heuristics: dynamic");
      _heuristics = new DynamicHeuristics();
    } else if (strcmp(ShenandoahGCHeuristics, "adaptive") == 0) {
      log_info(gc, init)("Shenandoah heuristics: adaptive");
      _heuristics = new AdaptiveHeuristics();
    } else if (strcmp(ShenandoahGCHeuristics, "passive") == 0) {
      log_info(gc, init)("Shenandoah heuristics: passive");
      _heuristics = new PassiveHeuristics();
    } else {
      vm_exit_during_initialization("Unknown -XX:ShenandoahGCHeuristics option");
    }
    _heuristics->print_thresholds();
  } else {
      ShouldNotReachHere();
  }
  _phase_times = new ShenandoahPhaseTimes((uint)MAX2(ConcGCThreads, ParallelGCThreads));
}

ShenandoahCollectorPolicy* ShenandoahCollectorPolicy::as_pgc_policy() {
  return this;
}

BarrierSet::Name ShenandoahCollectorPolicy::barrier_set_name() {
  return BarrierSet::ShenandoahBarrierSet;
}

HeapWord* ShenandoahCollectorPolicy::mem_allocate_work(size_t size,
                                                       bool is_tlab,
                                                       bool* gc_overhead_limit_was_exceeded) {
  guarantee(false, "Not using this policy feature yet.");
  return NULL;
}

HeapWord* ShenandoahCollectorPolicy::satisfy_failed_allocation(size_t size, bool is_tlab) {
  guarantee(false, "Not using this policy feature yet.");
  return NULL;
}

void ShenandoahCollectorPolicy::initialize_alignments() {

  // This is expected by our algorithm for ShenandoahHeap::heap_region_containing().
  _space_alignment = ShenandoahHeapRegion::region_size_bytes();
  _heap_alignment = ShenandoahHeapRegion::region_size_bytes();
}

void ShenandoahCollectorPolicy::post_heap_initialize() {
  // Nothing to do here (yet).
}

void ShenandoahCollectorPolicy::record_bytes_allocated(size_t bytes) {
  _heuristics->record_bytes_allocated(bytes);
}

void ShenandoahCollectorPolicy::record_bytes_start_CM(size_t bytes) {
  _heuristics->record_bytes_start_CM(bytes);
}

void ShenandoahCollectorPolicy::record_bytes_end_CM(size_t bytes) {
  _heuristics->record_bytes_end_CM(bytes);
}

void ShenandoahCollectorPolicy::record_bytes_reclaimed(size_t bytes) {
  _heuristics->record_bytes_reclaimed(bytes);
}

void ShenandoahCollectorPolicy::record_user_requested_gc() {
  _user_requested_gcs++;
}

void ShenandoahCollectorPolicy::record_allocation_failure_gc() {
  _allocation_failure_gcs++;
}

bool ShenandoahCollectorPolicy::should_start_concurrent_mark(size_t used,
                                                             size_t capacity) {
  return _heuristics->should_start_concurrent_mark(used, capacity);
}

bool ShenandoahCollectorPolicy::handover_cancelled_marking() {
  return _heuristics->handover_cancelled_marking();
}

void ShenandoahCollectorPolicy::record_cm_success() {
  _heuristics->record_cm_success();
  _successful_cm++;
}

void ShenandoahCollectorPolicy::record_cm_degenerated() {
  _degenerated_cm++;
}

void ShenandoahCollectorPolicy::record_cm_cancelled() {
  _heuristics->record_cm_cancelled();
}

void ShenandoahCollectorPolicy::record_full_gc() {
  _heuristics->record_full_gc();
}

void ShenandoahCollectorPolicy::choose_collection_set(ShenandoahCollectionSet* collection_set, int* connections) {
  _heuristics->choose_collection_set(collection_set, connections);
}

void ShenandoahCollectorPolicy::choose_free_set(ShenandoahFreeSet* free_set) {
   _heuristics->choose_free_set(free_set);
}


bool ShenandoahCollectorPolicy::process_references() {
  return _heuristics->process_references();
}

bool ShenandoahCollectorPolicy::unload_classes() {
  return _heuristics->unload_classes();
}

void ShenandoahCollectorPolicy::print_tracing_info(outputStream* out) {
  out->cr();
  out->print_cr("GC STATISTICS:");
  out->print_cr("  \"gross\" pauses include time to safepoint. \"net\" pauses are times spent in GC.");
  out->print_cr("  \"a\" is average time for each phase, look at levels to see if average makes sense.");
  out->print_cr("  \"lvls\" are quantiles: 0%% (minimum), 25%%, 50%% (median), 75%%, 100%% (maximum).");
  out->cr();

  for (uint i = 0; i < _num_phases; i++) {
    if (_timing_data[i]._secs.maximum() != 0) {
      print_summary_sd(out, _phase_names[i], &(_timing_data[i]._secs));
    }
  }

  out->cr();
  out->print_cr("" SIZE_FORMAT " allocation failure and " SIZE_FORMAT " user requested GCs", _allocation_failure_gcs, _user_requested_gcs);
  out->print_cr("" SIZE_FORMAT " successful and " SIZE_FORMAT " degenerated concurrent markings", _successful_cm, _degenerated_cm);
  out->cr();
}

void ShenandoahCollectorPolicy::print_summary_sd(outputStream* out, const char* str, const HdrSeq* seq)  {
  out->print_cr("%-27s = %8.2lf s (a = %8.0lf us) (n = "INT32_FORMAT_W(5)") (lvls, us = %8.0lf, %8.0lf, %8.0lf, %8.0lf, %8.0lf)",
          str,
          seq->sum(),
          seq->avg() * 1000000.0,
          seq->num(),
          seq->percentile(0)  * 1000000.0,
          seq->percentile(25) * 1000000.0,
          seq->percentile(50) * 1000000.0,
          seq->percentile(75) * 1000000.0,
          seq->maximum() * 1000000.0
  );
}

void ShenandoahCollectorPolicy::increase_cycle_counter() {
  _cycle_counter++;
}

size_t ShenandoahCollectorPolicy::cycle_counter() const {
  return _cycle_counter;
}

ShenandoahPhaseTimes* ShenandoahCollectorPolicy::phase_times() {
  return _phase_times;
}


uint ShenandoahCollectorPolicy::calc_workers_for_java_threads(uint application_workers) {
  return (uint)(ShenandoahGCWorkerPerJavaThread * application_workers);
}

uint ShenandoahCollectorPolicy::calc_workers_for_live_set(size_t live_data) {
  return (uint)(live_data / HeapSizePerGCThread);
}


uint ShenandoahCollectorPolicy::calc_default_active_workers(
                                                     uint total_workers,
                                                     uint min_workers,
                                                     uint active_workers,
                                                     uint application_workers,
                                                     uint  workers_by_java_threads,
                                                     uint  workers_by_liveset) {
  // If the user has turned off using a dynamic number of GC threads
  // or the users has requested a specific number, set the active
  // number of workers to all the workers.
  uint new_active_workers = total_workers;
  uint prev_active_workers = active_workers;
  uint active_workers_by_JT = 0;
  uint active_workers_by_liveset = 0;

  active_workers_by_JT = MAX2(workers_by_java_threads, min_workers);

  // Choose a number of GC threads based on the live set.
  active_workers_by_liveset =
      MAX2((uint) 2U, workers_by_liveset);

  uint max_active_workers =
    MAX2(active_workers_by_JT, active_workers_by_liveset);

  new_active_workers = MIN2(max_active_workers, total_workers);

  // Increase GC workers instantly but decrease them more
  // slowly.
  if (new_active_workers < prev_active_workers) {
    new_active_workers =
      MAX2(min_workers, (prev_active_workers + new_active_workers) / 2);
  }

  if (UseNUMA) {
    uint numa_groups = (uint)os::numa_get_groups_num();
    assert(numa_groups <= total_workers, "Not enough workers to cover all numa groups");
    new_active_workers = MAX2(new_active_workers, numa_groups);
  }

  // Check once more that the number of workers is within the limits.
  assert(min_workers <= total_workers, "Minimum workers not consistent with total workers");
  assert(new_active_workers >= min_workers, "Minimum workers not observed");
  assert(new_active_workers <= total_workers, "Total workers not observed");

  log_trace(gc, task)("ShenandoahCollectorPolicy::calc_default_active_workers() : "
     "active_workers(): " UINTX_FORMAT "  new_active_workers: " UINTX_FORMAT "  "
     "prev_active_workers: " UINTX_FORMAT "\n"
     " active_workers_by_JT: " UINTX_FORMAT "  active_workers_by_liveset: " UINTX_FORMAT,
     (uintx)active_workers, (uintx)new_active_workers, (uintx)prev_active_workers,
     (uintx)active_workers_by_JT, (uintx)active_workers_by_liveset);
  assert(new_active_workers > 0, "Always need at least 1");
  return new_active_workers;
}

/**
 * Initial marking phase also update references of live objects from previous concurrent GC cycle,
 * so we take Java threads and live set into account.
 */
uint ShenandoahCollectorPolicy::calc_workers_for_init_marking(uint active_workers,
                                            uint application_workers) {

  if (!UseDynamicNumberOfGCThreads ||
     (!FLAG_IS_DEFAULT(ParallelGCThreads) && !ForceDynamicNumberOfGCThreads)) {
    assert(ParallelGCThreads > 0, "Always need at least 1");
    return ParallelGCThreads;
  } else {
    ShenandoahCollectorPolicy* policy = (ShenandoahCollectorPolicy*)ShenandoahHeap::heap()->collector_policy();
    size_t live_data = policy->_heuristics->bytes_in_cset();

    return calc_default_active_workers(ParallelGCThreads, (ParallelGCThreads > 1) ? 2 : 1,
      active_workers, application_workers,
      calc_workers_for_java_threads(application_workers),
      calc_workers_for_live_set(live_data));
  }
}

uint ShenandoahCollectorPolicy::calc_workers_for_conc_marking(uint active_workers,
                                            uint application_workers) {

  if (!UseDynamicNumberOfGCThreads ||
     (!FLAG_IS_DEFAULT(ConcGCThreads) && !ForceDynamicNumberOfGCThreads)) {
    assert(ConcGCThreads > 0, "Always need at least 1");
    return ConcGCThreads;
  } else {
    return calc_default_active_workers(ConcGCThreads,
      (ConcGCThreads > 1 ? 2 : 1), active_workers,
      application_workers, calc_workers_for_java_threads(application_workers), 0);
  }
}

uint ShenandoahCollectorPolicy::calc_workers_for_final_marking(uint active_workers,
                                            uint application_workers) {

  if (!UseDynamicNumberOfGCThreads ||
     (!FLAG_IS_DEFAULT(ParallelGCThreads) && !ForceDynamicNumberOfGCThreads)) {
    assert(ParallelGCThreads > 0, "Always need at least 1");
    return ParallelGCThreads;
  } else {
    return calc_default_active_workers(ParallelGCThreads,
      (ParallelGCThreads > 1 ? 2 : 1), active_workers,
      application_workers, calc_workers_for_java_threads(application_workers), 0);
  }
}

  // Calculate workers for concurrent evacuation (concurrent GC)
uint ShenandoahCollectorPolicy::calc_workers_for_conc_evacuation(uint active_workers,
                                            uint application_workers) {
  if (!UseDynamicNumberOfGCThreads ||
     (!FLAG_IS_DEFAULT(ConcGCThreads) && !ForceDynamicNumberOfGCThreads)) {
    assert(ConcGCThreads > 0, "Always need at least 1");
    return ConcGCThreads;
  } else {
    return calc_workers_for_evacuation(false, // not a full GC
      ConcGCThreads, active_workers, application_workers);
  }
}

  // Calculate workers for parallel evaculation (full GC)
uint ShenandoahCollectorPolicy::calc_workers_for_parallel_evacuation(uint active_workers,
                                            uint application_workers) {
  if (!UseDynamicNumberOfGCThreads ||
     (!FLAG_IS_DEFAULT(ParallelGCThreads) && !ForceDynamicNumberOfGCThreads)) {
    assert(ParallelGCThreads > 0, "Always need at least 1");
    return ParallelGCThreads;
  } else {
    return calc_workers_for_evacuation(true, // a full GC
      ParallelGCThreads, active_workers, application_workers);
  }
}


uint ShenandoahCollectorPolicy::calc_workers_for_evacuation(bool full_gc,
                                            uint total_workers,
                                            uint active_workers,
                                            uint application_workers) {

  // Calculation based on live set
  size_t live_data = 0;
  ShenandoahHeap* heap = ShenandoahHeap::heap();
  if (full_gc) {
    ShenandoahHeapRegionSet* regions = heap->regions();
    for (size_t index = 0; index < regions->active_regions(); index ++) {
      live_data += regions->get_fast(index)->get_live_data_bytes();
    }
  } else {
    ShenandoahCollectorPolicy* policy = (ShenandoahCollectorPolicy*)heap->collector_policy();
    live_data = policy->_heuristics->bytes_in_cset();
  }

  uint active_workers_by_liveset = calc_workers_for_live_set(live_data);
  return calc_default_active_workers(total_workers,
      (total_workers > 1 ? 2 : 1), active_workers,
      application_workers, 0, active_workers_by_liveset);
}


GCTimer* ShenandoahCollectorPolicy::conc_timer() {return _conc_timer;}
GCTimer* ShenandoahCollectorPolicy::stw_timer() {return _stw_timer;}
