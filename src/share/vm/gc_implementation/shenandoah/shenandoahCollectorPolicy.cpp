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
  bool _update_refs_early;
  bool _update_refs_adaptive;

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
      _region_garbage = res;
      _region_garbage_size = num;
    } else if (_region_garbage_size < num) {
      res = REALLOC_C_HEAP_ARRAY(RegionGarbage, _region_garbage, num, mtGC);
      _region_garbage = res;
      _region_garbage_size = num;
    }
    return res;
  }

  RegionGarbage* _region_garbage;
  size_t _region_garbage_size;

  size_t _bytes_allocated_start_CM;
  size_t _bytes_allocated_during_CM;

  size_t _bytes_allocated_after_last_gc;

  uint _cancelled_cm_cycles_in_a_row;
  uint _successful_cm_cycles_in_a_row;

  uint _cancelled_uprefs_cycles_in_a_row;
  uint _successful_uprefs_cycles_in_a_row;

  size_t _bytes_in_cset;

  double _last_cycle_end;

public:

  ShenandoahHeuristics();
  virtual ~ShenandoahHeuristics();

  void record_bytes_allocated(size_t bytes);
  void record_bytes_reclaimed(size_t bytes);
  void record_bytes_start_CM(size_t bytes);
  void record_bytes_end_CM(size_t bytes);

  void record_gc_start() {
    // Do nothing.
  }

  void record_gc_end() {
    _bytes_allocated_after_last_gc = ShenandoahHeap::heap()->used();
  }

  virtual void record_cycle_start() {
    // Do nothing
  }

  virtual void record_cycle_end() {
    _last_cycle_end = os::elapsedTime();
  }

  virtual void record_phase_start(ShenandoahCollectorPolicy::TimingPhase phase) {
    // Do nothing
  }

  virtual void record_phase_end(ShenandoahCollectorPolicy::TimingPhase phase) {
    // Do nothing
  }

  size_t bytes_in_cset() const { return _bytes_in_cset; }

  virtual void print_thresholds() {
  }

  virtual bool should_start_concurrent_mark(size_t used, size_t capacity) const=0;

  virtual bool should_start_update_refs() {
    return _update_refs_early;
  }

  virtual bool update_refs() const {
    return _update_refs_early;
  }

  virtual bool handover_cancelled_marking() {
    return _cancelled_cm_cycles_in_a_row <= ShenandoahFullGCThreshold;
  }

  virtual bool handover_cancelled_uprefs() {
    return _cancelled_uprefs_cycles_in_a_row <= ShenandoahFullGCThreshold;
  }

  virtual void record_cm_cancelled() {
    _cancelled_cm_cycles_in_a_row++;
    _successful_cm_cycles_in_a_row = 0;
  }

  virtual void record_cm_success() {
    _cancelled_cm_cycles_in_a_row = 0;
    _successful_cm_cycles_in_a_row++;
  }

  virtual void record_uprefs_cancelled() {
    _cancelled_uprefs_cycles_in_a_row++;
    _successful_uprefs_cycles_in_a_row = 0;
  }

  virtual void record_uprefs_success() {
    _cancelled_uprefs_cycles_in_a_row = 0;
    _successful_uprefs_cycles_in_a_row++;
  }

  virtual void record_allocation_failure_gc() {
    _bytes_in_cset = 0;
  }

  virtual void record_user_requested_gc() {
    _bytes_in_cset = 0;
  }

  virtual void record_peak_occupancy() {
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

  virtual const char* name() = 0;
  virtual bool is_diagnostic() = 0;
  virtual bool is_experimental() = 0;
};

ShenandoahHeuristics::ShenandoahHeuristics() :
  _bytes_allocated_since_CM(0),
  _bytes_reclaimed_this_cycle(0),
  _bytes_allocated_start_CM(0),
  _bytes_allocated_during_CM(0),
  _bytes_allocated_after_last_gc(0),
  _bytes_in_cset(0),
  _cancelled_cm_cycles_in_a_row(0),
  _successful_cm_cycles_in_a_row(0),
  _cancelled_uprefs_cycles_in_a_row(0),
  _successful_uprefs_cycles_in_a_row(0),
  _region_garbage(NULL),
  _region_garbage_size(0),
  _update_refs_early(false),
  _update_refs_adaptive(false),
  _last_cycle_end(0)
{
  if (strcmp(ShenandoahUpdateRefsEarly, "on") == 0 ||
      strcmp(ShenandoahUpdateRefsEarly, "true") == 0 ) {
    _update_refs_early = true;
  } else if (strcmp(ShenandoahUpdateRefsEarly, "off") == 0 ||
             strcmp(ShenandoahUpdateRefsEarly, "false") == 0 ) {
    _update_refs_early = false;
  } else if (strcmp(ShenandoahUpdateRefsEarly, "adaptive") == 0) {
    _update_refs_adaptive = true;
    _update_refs_early = true;
  } else {
    vm_exit_during_initialization("Unknown -XX:ShenandoahUpdateRefsEarly option: %s", ShenandoahUpdateRefsEarly);
  }
}

ShenandoahHeuristics::~ShenandoahHeuristics() {
  if (_region_garbage != NULL) {
    FREE_C_HEAP_ARRAY(RegionGarbage, _region_garbage, mtGC);
  }
}

void ShenandoahHeuristics::choose_collection_set(ShenandoahCollectionSet* collection_set, int* connections) {
  assert(collection_set->count() == 0, "Must be empty");
  start_choose_collection_set();

  ShenandoahHeap* heap = ShenandoahHeap::heap();

  // Poll this before populating collection set.
  size_t total_garbage = heap->garbage();

  // Step 1. Build up the region candidates we care about, rejecting losers and accepting winners right away.

  ShenandoahHeapRegionSet* regions = heap->regions();
  size_t active = regions->active_regions();

  RegionGarbage* candidates = get_region_garbage_cache(active);

  size_t cand_idx = 0;
  _bytes_in_cset = 0;

  heap->start_deferred_recycling();

  size_t immediate_garbage = 0;
  size_t immediate_regions = 0;
  for (size_t i = 0; i < active; i++) {
    ShenandoahHeapRegion* region = regions->get(i);

    // Reclaim humongous regions here, and count them as the immediate garbage
    if (region->is_humongous_start()) {
      assert(region->has_live() == heap->is_marked_complete(oop(region->bottom() + BrooksPointer::word_size())),
             "Humongous liveness and marks should agree");
      if (!region->has_live()) {
        size_t reclaimed = heap->reclaim_humongous_region_at(region);
        immediate_regions += reclaimed;
        immediate_garbage += reclaimed * ShenandoahHeapRegion::region_size_bytes();
      }
    }

    if (region->is_regular()) {
      if (!region->has_live()) {
        // We can recycle it right away and put it in the free set.
        immediate_regions++;
        immediate_garbage += region->garbage();
        heap->decrease_used(region->used());
        heap->defer_recycle(region);
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
      log_develop_trace(gc)("Rejected region " SIZE_FORMAT " with garbage = " SIZE_FORMAT
                            " and live = " SIZE_FORMAT "\n",
                            region->region_number(), region->garbage(), region->get_live_data_bytes());
    }
  }
  heap->finish_deferred_recycle();

  // Step 2. Process the remanining candidates, if any.

  if (cand_idx > 0) {
    if (needs_regions_sorted_by_garbage()) {
      QuickSort::sort<RegionGarbage>(candidates, (int)cand_idx, compare_by_garbage, false);
    }

    for (size_t i = 0; i < cand_idx; i++) {
      ShenandoahHeapRegion *region = regions->get(candidates[i].region_number);
      if (region_in_collection_set(region, immediate_garbage)) {
        log_develop_trace(gc)("Choose region " SIZE_FORMAT " with garbage = " SIZE_FORMAT
                                      " and live = " SIZE_FORMAT "\n",
                              region->region_number(), region->garbage(), region->get_live_data_bytes());
        collection_set->add_region(region);
        _bytes_in_cset += region->used();
      }
    }
  }

  end_choose_collection_set();

  // Step 3. Look back at collection set, and see if it's worth it to collect,
  // given the amount of immediately reclaimable garbage.

  log_info(gc, ergo)("Total Garbage: "SIZE_FORMAT"M",
                     total_garbage / M);

  size_t total_garbage_regions = immediate_regions + collection_set->count();
  size_t immediate_percent = total_garbage_regions == 0 ? 0 : (immediate_regions * 100 / total_garbage_regions);

  log_info(gc, ergo)("Immediate Garbage: "SIZE_FORMAT"M, "SIZE_FORMAT" regions ("SIZE_FORMAT"%% of total)",
                     immediate_garbage / M, immediate_regions, immediate_percent);

  if (immediate_percent > ShenandoahImmediateThreshold) {
    collection_set->clear();
  } else {
    log_info(gc, ergo)("Garbage to be collected: "SIZE_FORMAT"M ("SIZE_FORMAT"%% of total), "SIZE_FORMAT" regions",
                       collection_set->garbage() / M, collection_set->garbage() * 100 / MAX2(total_garbage, (size_t)1), collection_set->count());
    log_info(gc, ergo)("Live objects to be evacuated: "SIZE_FORMAT"M",
                       collection_set->live_data() / M);
    log_info(gc, ergo)("Live/garbage ratio in collected regions: "SIZE_FORMAT"%%",
                       collection_set->live_data() * 100 / MAX2(collection_set->garbage(), (size_t)1));
  }

  collection_set->update_region_status();
}

void ShenandoahHeuristics::choose_free_set(ShenandoahFreeSet* free_set) {
  ShenandoahHeapRegionSet* ordered_regions = ShenandoahHeap::heap()->regions();
  for (size_t i = 0; i < ordered_regions->active_regions(); i++) {
    ShenandoahHeapRegion* region = ordered_regions->get(i);
    if (region->is_alloc_allowed()) {
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
  if (is_at_shutdown()) {
    // Do not record the past-shutdown events
    return;
  }

  guarantee(phase == init_evac ||
            phase == scan_roots ||
            phase == update_roots ||
            phase == final_update_refs_roots ||
            phase == full_gc_roots ||
            phase == _num_phases,
            "only in these phases we can add per-thread phase times");
  if (phase != _num_phases) {
    // Merge _phase_time to counters below the given phase.
    for (uint i = 0; i < ShenandoahPhaseTimes::GCParPhasesSentinel; i++) {
      double t = _phase_times->average(i);
      _timing_data[phase + i + 1]._secs.add(t);
    }
  }
}

void ShenandoahCollectorPolicy::record_phase_start(TimingPhase phase) {
  _timing_data[phase]._start = os::elapsedTime();
  _heuristics->record_phase_start(phase);
}

void ShenandoahCollectorPolicy::record_phase_end(TimingPhase phase) {
  if (!is_at_shutdown()) {
    double end = os::elapsedTime();
    double elapsed = end - _timing_data[phase]._start;
    _timing_data[phase]._secs.add(elapsed);
  }
  _heuristics->record_phase_end(phase);
}

void ShenandoahCollectorPolicy::record_phase_time(TimingPhase phase, jint time_us) {
  if (!is_at_shutdown()) {
    _timing_data[phase]._secs.add((double)time_us / 1000 / 1000);
  }
}

void ShenandoahCollectorPolicy::record_alloc_latency(size_t words_size,
                                                     ShenandoahHeap::AllocType _alloc_type, double latency_us) {
  _alloc_size[_alloc_type].add(words_size);
  _alloc_latency[_alloc_type].add((size_t)latency_us);
}

void ShenandoahCollectorPolicy::record_gc_start() {
  _heuristics->record_gc_start();
}

void ShenandoahCollectorPolicy::record_gc_end() {
  _heuristics->record_gc_end();
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

class ShenandoahPassiveHeuristics : public ShenandoahHeuristics {
public:
  ShenandoahPassiveHeuristics() : ShenandoahHeuristics() {
    // Do not allow concurrent cycles.
    FLAG_SET_DEFAULT(ExplicitGCInvokesConcurrent, false);
  }

  virtual bool region_in_collection_set(ShenandoahHeapRegion* r, size_t immediate_garbage) {
    return r->garbage() > 0;
  }

  virtual bool should_start_concurrent_mark(size_t used, size_t capacity) const {
    // Never do concurrent GCs.
    return false;
  }

  virtual bool process_references() {
    if (ShenandoahRefProcFrequency == 0) return false;
    // Always process references.
    return true;
  }

  virtual bool unload_classes() {
    if (ShenandoahUnloadClassesFrequency == 0) return false;
    // Always unload classes.
    return true;
  }

  virtual const char* name() {
    return "passive";
  }

  virtual bool is_diagnostic() {
    return true;
  }

  virtual bool is_experimental() {
    return false;
  }
};

class ShenandoahAggressiveHeuristics : public ShenandoahHeuristics {
public:
  ShenandoahAggressiveHeuristics() : ShenandoahHeuristics() {
    // Do not shortcut evacuation
    if (FLAG_IS_DEFAULT(ShenandoahImmediateThreshold)) {
      FLAG_SET_DEFAULT(ShenandoahImmediateThreshold, 100);
    }
  }

  virtual bool region_in_collection_set(ShenandoahHeapRegion* r, size_t immediate_garbage) {
    return r->garbage() > 0;
  }

  virtual bool should_start_concurrent_mark(size_t used, size_t capacity) const {
    return true;
  }

  virtual bool process_references() {
    if (ShenandoahRefProcFrequency == 0) return false;
    // Randomly process refs with 50% chance.
    return (os::random() & 1) == 1;
  }

  virtual bool unload_classes() {
    if (ShenandoahUnloadClassesFrequency == 0) return false;
    // Randomly unload classes with 50% chance.
    return (os::random() & 1) == 1;
  }

  virtual const char* name() {
    return "aggressive";
  }

  virtual bool is_diagnostic() {
    return true;
  }

  virtual bool is_experimental() {
    return false;
  }
};

class ShenandoahDynamicHeuristics : public ShenandoahHeuristics {
public:
  ShenandoahDynamicHeuristics() : ShenandoahHeuristics() {
  }

  void print_thresholds() {
    log_info(gc, init)("Shenandoah heuristics thresholds: allocation "SIZE_FORMAT", free "SIZE_FORMAT", garbage "SIZE_FORMAT,
                       ShenandoahAllocationThreshold,
                       ShenandoahFreeThreshold,
                       ShenandoahGarbageThreshold);
  }

  virtual ~ShenandoahDynamicHeuristics() {}

  virtual bool should_start_concurrent_mark(size_t used, size_t capacity) const {

    bool shouldStartConcurrentMark = false;

    ShenandoahHeap* heap = ShenandoahHeap::heap();
    size_t free_capacity = heap->free_regions()->capacity();
    size_t free_used = heap->free_regions()->used();
    assert(free_used <= free_capacity, "must use less than capacity");
    size_t available =  free_capacity - free_used;

    if (! update_refs()) {
      // Count in the memory available after cset reclamation.
      size_t cset = MIN2(_bytes_in_cset, (ShenandoahCSetThreshold * capacity) / 100);
      available += cset;
    }

    uintx threshold = ShenandoahFreeThreshold + ShenandoahCSetThreshold;
    size_t targetStartMarking = (capacity * threshold) / 100;

    double last_time_ms = (os::elapsedTime() - _last_cycle_end) * 1000;
    bool periodic_gc = (last_time_ms > ShenandoahGuaranteedGCInterval);

    size_t threshold_bytes_allocated = heap->capacity() * ShenandoahAllocationThreshold / 100;
    if (available < targetStartMarking &&
        heap->bytes_allocated_since_cm() > threshold_bytes_allocated) {
      // Need to check that an appropriate number of regions have
      // been allocated since last concurrent mark too.
      shouldStartConcurrentMark = true;
    } else if (periodic_gc) {
      log_info(gc,ergo)("Periodic GC triggered. Time since last GC: %.0f ms, Guaranteed Interval: " UINTX_FORMAT " ms",
                        last_time_ms, ShenandoahGuaranteedGCInterval);
      shouldStartConcurrentMark = true;
    }

    return shouldStartConcurrentMark;
  }

  virtual bool region_in_collection_set(ShenandoahHeapRegion* r, size_t immediate_garbage) {
    size_t threshold = ShenandoahHeapRegion::region_size_bytes() * ShenandoahGarbageThreshold / 100;
    return r->garbage() > threshold;
  }

  virtual const char* name() {
    return "dynamic";
  }

  virtual bool is_diagnostic() {
    return false;
  }

  virtual bool is_experimental() {
    return false;
  }
};

class ShenandoahContinuousHeuristics : public ShenandoahHeuristics {
public:
  virtual bool should_start_concurrent_mark(size_t used, size_t capacity) const {
    // Start the cycle, unless completely idle.
    return ShenandoahHeap::heap()->bytes_allocated_since_cm() > 0;
  }

  virtual bool region_in_collection_set(ShenandoahHeapRegion* r, size_t immediate_garbage) {
    size_t threshold = ShenandoahHeapRegion::region_size_bytes() * ShenandoahGarbageThreshold / 100;
    return r->garbage() > threshold;
  }

  virtual const char* name() {
    return "continuous";
  }

  virtual bool is_diagnostic() {
    return false;
  }

  virtual bool is_experimental() {
    return false;
  }
};

class ShenandoahAdaptiveHeuristics : public ShenandoahHeuristics {
private:
  uintx _free_threshold;
  TruncatedSeq* _cset_history;
  size_t _peak_occupancy;
  TruncatedSeq* _cycle_gap_history;
  double _conc_mark_start;
  TruncatedSeq* _conc_mark_duration_history;
  double _conc_uprefs_start;
  TruncatedSeq* _conc_uprefs_duration_history;
public:
  ShenandoahAdaptiveHeuristics() :
    ShenandoahHeuristics(),
    _free_threshold(ShenandoahInitFreeThreshold),
    _peak_occupancy(0),
    _conc_mark_start(0),
    _conc_mark_duration_history(new TruncatedSeq(5)),
    _conc_uprefs_start(0),
    _conc_uprefs_duration_history(new TruncatedSeq(5)),
    _cycle_gap_history(new TruncatedSeq(5)),
    _cset_history(new TruncatedSeq((uint)ShenandoahHappyCyclesThreshold)) {

    _cset_history->add((double) ShenandoahCSetThreshold);
    _cset_history->add((double) ShenandoahCSetThreshold);
  }

  virtual ~ShenandoahAdaptiveHeuristics() {
    delete _cset_history;
  }

  virtual bool region_in_collection_set(ShenandoahHeapRegion* r, size_t immediate_garbage) {
    size_t threshold = ShenandoahHeapRegion::region_size_bytes() * ShenandoahGarbageThreshold / 100;
    return r->garbage() > threshold;
  }

  static const intx MaxNormalStep = 5;      // max step towards goal under normal conditions
  static const intx CancelledGC_Hit = 10;   // how much to step on cancelled GC
  static const intx AllocFailure_Hit = 20;  // how much to step on allocation failure full GC
  static const intx UserRequested_Hit = 0;  // how much to step on user requested full GC

  void handle_cycle_success() {
    ShenandoahHeap* heap = ShenandoahHeap::heap();
    size_t capacity = heap->capacity();

    size_t current_threshold = (capacity - _peak_occupancy) * 100 / capacity;
    size_t min_threshold = ShenandoahMinFreeThreshold;
    intx step = min_threshold - current_threshold;
    step = MAX2(step, (intx) -MaxNormalStep);
    step = MIN2(step, (intx) MaxNormalStep);

    log_info(gc, ergo)("Capacity: " SIZE_FORMAT "M, Peak Occupancy: " SIZE_FORMAT
                              "M, Lowest Free: " SIZE_FORMAT "M, Free Threshold: " UINTX_FORMAT "M",
                       capacity / M, _peak_occupancy / M,
                       (capacity - _peak_occupancy) / M, ShenandoahMinFreeThreshold * capacity / 100 / M);

    if (step > 0) {
      // Pessimize
      adjust_free_threshold(step);
    } else if (step < 0) {
      // Optimize, if enough happy cycles happened
      if (_successful_cm_cycles_in_a_row > ShenandoahHappyCyclesThreshold &&
          (! update_refs() || (_successful_uprefs_cycles_in_a_row > ShenandoahHappyCyclesThreshold)) &&
          _free_threshold > 0) {
        adjust_free_threshold(step);
        _successful_cm_cycles_in_a_row = 0;
        _successful_uprefs_cycles_in_a_row = 0;
      }
    } else {
      // do nothing
    }
    _peak_occupancy = 0;
  }

  void record_cycle_end() {
    ShenandoahHeuristics::record_cycle_end();
    handle_cycle_success();
  }

  void record_cycle_start() {
    ShenandoahHeuristics::record_cycle_start();
    double last_cycle_gap = (os::elapsedTime() - _last_cycle_end);
    _cycle_gap_history->add(last_cycle_gap);
  }

  void record_phase_start(ShenandoahCollectorPolicy::TimingPhase phase) {
    if (phase == ShenandoahCollectorPolicy::conc_mark) {
      _conc_mark_start = os::elapsedTime();
    } else if (phase == ShenandoahCollectorPolicy::conc_update_refs) {
      _conc_uprefs_start = os::elapsedTime();
    } // Else ignore

  }

  virtual void record_phase_end(ShenandoahCollectorPolicy::TimingPhase phase) {
    if (phase == ShenandoahCollectorPolicy::conc_mark) {
      _conc_mark_duration_history->add(os::elapsedTime() - _conc_mark_start);
    } else if (phase == ShenandoahCollectorPolicy::conc_update_refs) {
      _conc_uprefs_duration_history->add(os::elapsedTime() - _conc_uprefs_start);
    } // Else ignore
  }

  void adjust_free_threshold(intx adj) {
    intx new_value = adj + _free_threshold;
    uintx new_threshold = (uintx)MAX2<intx>(new_value, 0);
    new_threshold = MAX2(new_threshold, ShenandoahMinFreeThreshold);
    new_threshold = MIN2(new_threshold, ShenandoahMaxFreeThreshold);
    if (new_threshold != _free_threshold) {
      _free_threshold = new_threshold;
      log_info(gc,ergo)("Adjusting free threshold to: " UINTX_FORMAT "%% (" SIZE_FORMAT "M)",
                        _free_threshold, _free_threshold * ShenandoahHeap::heap()->capacity() / 100 / M);
    }
  }

  virtual void record_cm_cancelled() {
    ShenandoahHeuristics::record_cm_cancelled();
    adjust_free_threshold(CancelledGC_Hit);
  }

  virtual void record_cm_success() {
    ShenandoahHeuristics::record_cm_success();
  }

  virtual void record_uprefs_cancelled() {
    ShenandoahHeuristics::record_uprefs_cancelled();
    adjust_free_threshold(CancelledGC_Hit);
  }

  virtual void record_uprefs_success() {
    ShenandoahHeuristics::record_uprefs_success();
  }

  virtual void record_user_requested_gc() {
    ShenandoahHeuristics::record_user_requested_gc();
    adjust_free_threshold(UserRequested_Hit);
  }

  virtual void record_allocation_failure_gc() {
    ShenandoahHeuristics::record_allocation_failure_gc();
    adjust_free_threshold(AllocFailure_Hit);
  }

  virtual void record_peak_occupancy() {
    _peak_occupancy = MAX2(_peak_occupancy, ShenandoahHeap::heap()->used());
  }

  virtual bool should_start_concurrent_mark(size_t used, size_t capacity) const {
    bool shouldStartConcurrentMark = false;

    ShenandoahHeap* heap = ShenandoahHeap::heap();
    size_t free_capacity = heap->free_regions()->capacity();
    size_t free_used = heap->free_regions()->used();
    assert(free_used <= free_capacity, "must use less than capacity");
    size_t available =  free_capacity - free_used;
    uintx factor = _free_threshold;
    size_t cset_threshold = 0;
    if (! update_refs()) {
      // Count in the memory available after cset reclamation.
      cset_threshold = (size_t) _cset_history->davg();
      size_t cset = MIN2(_bytes_in_cset, (cset_threshold * capacity) / 100);
      available += cset;
      factor += cset_threshold;
    }

    double last_time_ms = (os::elapsedTime() - _last_cycle_end) * 1000;
    bool periodic_gc = (last_time_ms > ShenandoahGuaranteedGCInterval);
    size_t threshold_available = (capacity * factor) / 100;
    size_t bytes_allocated = heap->bytes_allocated_since_cm();
    size_t threshold_bytes_allocated = heap->capacity() * ShenandoahAllocationThreshold / 100;

    if (available < threshold_available &&
            bytes_allocated > threshold_bytes_allocated) {
      log_info(gc,ergo)("Concurrent marking triggered. Free: " SIZE_FORMAT "M, Free Threshold: " SIZE_FORMAT
                                "M; Allocated: " SIZE_FORMAT "M, Alloc Threshold: " SIZE_FORMAT "M",
                        available / M, threshold_available / M, available / M, threshold_bytes_allocated / M);
      // Need to check that an appropriate number of regions have
      // been allocated since last concurrent mark too.
      shouldStartConcurrentMark = true;
    } else if (periodic_gc) {
      log_info(gc,ergo)("Periodic GC triggered. Time since last GC: %.0f ms, Guaranteed Interval: " UINTX_FORMAT " ms",
          last_time_ms, ShenandoahGuaranteedGCInterval);
      shouldStartConcurrentMark = true;
    }

    if (shouldStartConcurrentMark) {
      if (! update_refs()) {
        log_info(gc,ergo)("Predicted cset threshold: " SIZE_FORMAT ", " SIZE_FORMAT "K CSet ("SIZE_FORMAT"%%)",
                          cset_threshold, _bytes_in_cset / K, _bytes_in_cset * 100 / capacity);
        _cset_history->add((double) (_bytes_in_cset * 100 / capacity));
      }
    }
    return shouldStartConcurrentMark;
  }

  virtual bool should_start_update_refs() {
    if (! _update_refs_adaptive) {
      return _update_refs_early;
    }

    double cycle_gap_avg = _cycle_gap_history->avg();
    double conc_mark_avg = _conc_mark_duration_history->avg();
    double conc_uprefs_avg = _conc_uprefs_duration_history->avg();

    if (_update_refs_early) {
      double threshold = ShenandoahMergeUpdateRefsMinGap / 100.0;
      if (conc_mark_avg + conc_uprefs_avg > cycle_gap_avg * threshold) {
        _update_refs_early = false;
      }
    } else {
      double threshold = ShenandoahMergeUpdateRefsMaxGap / 100.0;
      if (conc_mark_avg + conc_uprefs_avg < cycle_gap_avg * threshold) {
        _update_refs_early = true;
      }
    }
    return _update_refs_early;
  }

  virtual const char* name() {
    return "adaptive";
  }

  virtual bool is_diagnostic() {
    return false;
  }

  virtual bool is_experimental() {
    return false;
  }
};

ShenandoahCollectorPolicy::ShenandoahCollectorPolicy() :
  _cycle_counter(0),
  _successful_cm(0),
  _degenerated_cm(0),
  _successful_uprefs(0),
  _degenerated_uprefs(0),
  _in_shutdown(0)
{

  ShenandoahHeapRegion::setup_heap_region_size(initial_heap_byte_size(), max_heap_byte_size());

  initialize_all();

  _tracer = new (ResourceObj::C_HEAP, mtGC) ShenandoahTracer();
  _user_requested_gcs = 0;
  _allocation_failure_gcs = 0;

  _phase_names[total_pause]                     = "Total Pauses (N)";
  _phase_names[total_pause_gross]               = "Total Pauses (G)";
  _phase_names[init_mark]                       = "Pause Init Mark (N)";
  _phase_names[init_mark_gross]                 = "Pause Init Mark (G)";
  _phase_names[final_mark]                      = "Pause Final Mark (N)";
  _phase_names[final_mark_gross]                = "Pause Final Mark (G)";
  _phase_names[accumulate_stats]                = "  Accumulate Stats";
  _phase_names[make_parsable]                   = "  Make Parsable";
  _phase_names[clear_liveness]                  = "  Clear Liveness";
  _phase_names[resize_tlabs]                    = "  Resize TLABs";
  _phase_names[finish_queues]                   = "  Finish Queues";
  _phase_names[weakrefs]                        = "  Weak References";
  _phase_names[weakrefs_process]                = "    Process";
  _phase_names[weakrefs_enqueue]                = "    Enqueue";
  _phase_names[purge]                           = "  System Purge";
  _phase_names[purge_class_unload]              = "    Unload Classes";
  _phase_names[purge_par]                       = "    Parallel Cleanup";
  _phase_names[purge_par_codecache]             = "      Code Cache";
  _phase_names[purge_par_symbstring]            = "      String/Symbol Tables";
  _phase_names[purge_par_rmt]                   = "      Resolved Methods";
  _phase_names[purge_par_classes]               = "      Clean Classes";
  _phase_names[purge_par_sync]                  = "      Synchronization";
  _phase_names[purge_cldg]                      = "    CLDG";
  _phase_names[prepare_evac]                    = "  Prepare Evacuation";

  _phase_names[scan_roots]                      = "  Scan Roots";
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

  _phase_names[update_roots]                    = "  Update Roots";
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

  _phase_names[init_evac]                       = "  Initial Evacuation";
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

  _phase_names[full_gc]                         = "Pause Full GC";
  _phase_names[full_gc_heapdumps]               = "  Heap Dumps";
  _phase_names[full_gc_prepare]                 = "  Prepare";
  _phase_names[full_gc_roots]                   = "  Roots";
  _phase_names[full_gc_thread_roots]            = "    F: Thread Roots";
  _phase_names[full_gc_code_roots]              = "    F: Code Cache Roots";
  _phase_names[full_gc_string_table_roots]      = "    F: String Table Roots";
  _phase_names[full_gc_universe_roots]          = "    F: Universe Roots";
  _phase_names[full_gc_jni_roots]               = "    F: JNI Roots";
  _phase_names[full_gc_jni_weak_roots]          = "    F: JNI Weak Roots";
  _phase_names[full_gc_synchronizer_roots]      = "    F: Synchronizer Roots";
  _phase_names[full_gc_flat_profiler_roots]     = "    F: Flat Profiler Roots";
  _phase_names[full_gc_management_roots]        = "    F: Management Roots";
  _phase_names[full_gc_system_dictionary_roots] = "    F: System Dict Roots";
  _phase_names[full_gc_cldg_roots]              = "    F: CLDG Roots";
  _phase_names[full_gc_jvmti_roots]             = "    F: JVMTI Roots";
  _phase_names[full_gc_mark]                    = "  Mark";
  _phase_names[full_gc_mark_finish_queues]      = "    Finish Queues";
  _phase_names[full_gc_weakrefs]                = "    Weak References";
  _phase_names[full_gc_weakrefs_process]        = "      Process";
  _phase_names[full_gc_weakrefs_enqueue]        = "      Enqueue";
  _phase_names[full_gc_purge]                   = "    System Purge";
  _phase_names[full_gc_purge_class_unload]      = "      Unload Classes";
  _phase_names[full_gc_purge_par]               = "    Parallel Cleanup";
  _phase_names[full_gc_purge_par_codecache]     = "      Code Cache";
  _phase_names[full_gc_purge_par_symbstring]    = "      String/Symbol Tables";
  _phase_names[full_gc_purge_par_rmt]           = "      Resolved Methods";
  _phase_names[full_gc_purge_par_classes]       = "      Clean Classes";
  _phase_names[full_gc_purge_par_sync]          = "      Synchronization";
  _phase_names[full_gc_purge_cldg]              = "      CLDG";
  _phase_names[full_gc_calculate_addresses]     = "  Calculate Addresses";
  _phase_names[full_gc_adjust_pointers]         = "  Adjust Pointers";
  _phase_names[full_gc_copy_objects]            = "  Copy Objects";
  _phase_names[full_gc_resize_tlabs]            = "  Resize TLABs";

  _phase_names[conc_mark]                       = "Concurrent Marking";
  _phase_names[conc_preclean]                   = "Concurrent Precleaning";
  _phase_names[conc_evac]                       = "Concurrent Evacuation";
  _phase_names[conc_reset_bitmaps]              = "Concurrent Reset Bitmaps";

  _phase_names[init_update_refs_gross]          = "Pause Init  Update Refs (G)";
  _phase_names[init_update_refs]                = "Pause Init  Update Refs (N)";
  _phase_names[conc_update_refs]                = "Concurrent Update Refs";
  _phase_names[final_update_refs_gross]         = "Pause Final Update Refs (G)";
  _phase_names[final_update_refs]               = "Pause Final Update Refs (N)";

  _phase_names[final_update_refs_finish_work]          = "  Finish Work";
  _phase_names[final_update_refs_roots]                = "  Update Roots";
  _phase_names[final_update_refs_thread_roots]         = "    UR: Thread Roots";
  _phase_names[final_update_refs_code_roots]           = "    UR: Code Cache Roots";
  _phase_names[final_update_refs_string_table_roots]   = "    UR: String Table Roots";
  _phase_names[final_update_refs_universe_roots]       = "    UR: Universe Roots";
  _phase_names[final_update_refs_jni_roots]            = "    UR: JNI Roots";
  _phase_names[final_update_refs_jni_weak_roots]       = "    UR: JNI Weak Roots";
  _phase_names[final_update_refs_synchronizer_roots]   = "    UR: Synchronizer Roots";
  _phase_names[final_update_refs_flat_profiler_roots]  = "    UR: Flat Profiler Roots";
  _phase_names[final_update_refs_management_roots]     = "    UR: Management Roots";
  _phase_names[final_update_refs_system_dict_roots]    = "    UR: System Dict Roots";
  _phase_names[final_update_refs_cldg_roots]           = "    UR: CLDG Roots";
  _phase_names[final_update_refs_jvmti_roots]          = "    UR: JVMTI Roots";
  _phase_names[final_update_refs_recycle]              = "  Recycle";


  if (ShenandoahGCHeuristics != NULL) {
    if (strcmp(ShenandoahGCHeuristics, "aggressive") == 0) {
      _heuristics = new ShenandoahAggressiveHeuristics();
    } else if (strcmp(ShenandoahGCHeuristics, "dynamic") == 0) {
      _heuristics = new ShenandoahDynamicHeuristics();
    } else if (strcmp(ShenandoahGCHeuristics, "adaptive") == 0) {
      _heuristics = new ShenandoahAdaptiveHeuristics();
    } else if (strcmp(ShenandoahGCHeuristics, "passive") == 0) {
      _heuristics = new ShenandoahPassiveHeuristics();
    } else if (strcmp(ShenandoahGCHeuristics, "continuous") == 0) {
      _heuristics = new ShenandoahContinuousHeuristics();
    } else {
      vm_exit_during_initialization("Unknown -XX:ShenandoahGCHeuristics option");
    }

    if (_heuristics->is_diagnostic() && !UnlockDiagnosticVMOptions) {
      vm_exit_during_initialization(
              err_msg("Heuristics \"%s\" is diagnostic, and must be enabled via -XX:+UnlockDiagnosticVMOptions.",
                      _heuristics->name()));
    }
    if (_heuristics->is_experimental() && !UnlockExperimentalVMOptions) {
      vm_exit_during_initialization(
              err_msg("Heuristics \"%s\" is experimental, and must be enabled via -XX:+UnlockExperimentalVMOptions.",
                      _heuristics->name()));
    }
    log_info(gc, init)("Shenandoah heuristics: %s",
                       _heuristics->name());
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
  _heuristics->record_user_requested_gc();
  _user_requested_gcs++;
}

void ShenandoahCollectorPolicy::record_allocation_failure_gc() {
  _heuristics->record_allocation_failure_gc();
  _allocation_failure_gcs++;
}

bool ShenandoahCollectorPolicy::should_start_concurrent_mark(size_t used,
                                                             size_t capacity) {
  return _heuristics->should_start_concurrent_mark(used, capacity);
}

bool ShenandoahCollectorPolicy::handover_cancelled_marking() {
  return _heuristics->handover_cancelled_marking();
}

bool ShenandoahCollectorPolicy::handover_cancelled_uprefs() {
  return _heuristics->handover_cancelled_uprefs();
}

bool ShenandoahCollectorPolicy::update_refs() {
  return _heuristics->update_refs();
}

bool ShenandoahCollectorPolicy::should_start_update_refs() {
  return _heuristics->should_start_update_refs();
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

void ShenandoahCollectorPolicy::record_uprefs_success() {
  _heuristics->record_uprefs_success();
  _successful_uprefs++;
}

void ShenandoahCollectorPolicy::record_uprefs_degenerated() {
  _degenerated_uprefs++;
}

void ShenandoahCollectorPolicy::record_uprefs_cancelled() {
  _heuristics->record_uprefs_cancelled();
}

void ShenandoahCollectorPolicy::record_peak_occupancy() {
  _heuristics->record_peak_occupancy();
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
  out->print_cr("  \"(G)\" (gross) pauses include VM time: time to notify and block threads, do the pre-");
  out->print_cr("        and post-safepoint housekeeping. Use -XX:+PrintSafepointStatistics to dissect.");
  out->print_cr("  \"(N)\" (net) pauses are the times spent in the actual GC code.");
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
  out->print_cr("" SIZE_FORMAT " successful and " SIZE_FORMAT " degenerated update references  ", _successful_uprefs, _degenerated_uprefs);
  out->cr();

  out->print_cr("ALLOCATION TRACING");
  out->print_cr("  These are the slow-path allocations, including TLAB/GCLAB refills, and out-of-TLAB allocations.");
  out->print_cr("  In-TLAB/GCLAB allocations happen orders of magnitude more frequently, and without delays.");
  out->cr();

  if (ShenandoahAllocationTrace) {
    out->print("%18s", "");
    for (size_t t = 0; t < ShenandoahHeap::_ALLOC_LIMIT; t++) {
      out->print("%12s", ShenandoahHeap::alloc_type_to_string(ShenandoahHeap::AllocType(t)));
    }
    out->cr();

    out->print_cr("Counts:");
    out->print("%18s", "#");
    for (size_t t = 0; t < ShenandoahHeap::_ALLOC_LIMIT; t++) {
      out->print(SIZE_FORMAT_W(12), _alloc_size[t].num());
    }
    out->cr();
    out->cr();

    // Figure out max and min levels
    int lat_min_level = +1000;
    int lat_max_level = -1000;
    int size_min_level = +1000;
    int size_max_level = -1000;
    for (size_t t = 0; t < ShenandoahHeap::_ALLOC_LIMIT; t++) {
      lat_min_level = MIN2(lat_min_level, _alloc_latency[t].min_level());
      lat_max_level = MAX2(lat_max_level, _alloc_latency[t].max_level());
      size_min_level = MIN2(size_min_level, _alloc_size[t].min_level());
      size_max_level = MAX2(size_max_level, _alloc_size[t].max_level());
    }

    out->print_cr("Latencies (in microseconds):");
    for (int c = lat_min_level; c <= lat_max_level; c++) {
      out->print("%7d - %7d:", (c == 0) ? 0 : 1 << (c - 1), 1 << c);
      for (size_t t = 0; t < ShenandoahHeap::_ALLOC_LIMIT; t++) {
        out->print(SIZE_FORMAT_W(12), _alloc_latency[t].level(c));
      }
      out->cr();
    }
    out->cr();

    out->print_cr("Sizes (in bytes):");
    for (int c = size_min_level; c <= size_max_level; c++) {
      out->print("%7d - %7d:", (c == 0) ? 0 : 1 << (c - 1), 1 << c);
      for (size_t t = 0; t < ShenandoahHeap::_ALLOC_LIMIT; t++) {
        out->print(SIZE_FORMAT_W(12), _alloc_size[t].level(c));
      }
      out->cr();
    }
    out->cr();
  } else {
    out->print_cr("  Allocation tracing is disabled, use -XX:+ShenandoahAllocationTrace to enable.");
  }
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

  new_active_workers = MIN2(max_active_workers, new_active_workers);

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
      live_data += regions->get(index)->get_live_data_bytes();
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

void ShenandoahCollectorPolicy::record_cycle_start() {
  _cycle_counter++;
  _heuristics->record_cycle_start();
}

void ShenandoahCollectorPolicy::record_cycle_end() {
  _heuristics->record_cycle_end();
}

void ShenandoahCollectorPolicy::record_shutdown() {
  OrderAccess::release_store_fence(&_in_shutdown, 1);
}

bool ShenandoahCollectorPolicy::is_at_shutdown() {
  return OrderAccess::load_acquire(&_in_shutdown) == 1;
}
