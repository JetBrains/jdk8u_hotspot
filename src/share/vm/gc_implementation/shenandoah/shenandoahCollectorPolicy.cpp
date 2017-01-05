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
  size_t _bytes_allocated_start_CM;
  size_t _bytes_allocated_during_CM;

  uint _cancelled_cm_cycles_in_a_row;
  uint _successful_cm_cycles_in_a_row;

public:

  ShenandoahHeuristics();

  void record_bytes_allocated(size_t bytes);
  void record_bytes_reclaimed(size_t bytes);
  void record_bytes_start_CM(size_t bytes);
  void record_bytes_end_CM(size_t bytes);

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

  virtual void start_choose_collection_set() {
  }
  virtual void end_choose_collection_set() {
  }
  virtual bool region_in_collection_set(ShenandoahHeapRegion* r, size_t immediate_garbage) = 0;

  void choose_collection_set(ShenandoahCollectionSet* collection_set);

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
    // Process references every Nth GC cycle.
    return cycle % ShenandoahUnloadClassesFrequency == 0;
  }

private:
  static int compare_heap_regions_by_garbage(ShenandoahHeapRegion* a, ShenandoahHeapRegion* b);

};

ShenandoahHeuristics::ShenandoahHeuristics() :
  _bytes_allocated_since_CM(0),
  _bytes_reclaimed_this_cycle(0),
  _bytes_allocated_start_CM(0),
  _bytes_allocated_during_CM(0),
  _cancelled_cm_cycles_in_a_row(0),
  _successful_cm_cycles_in_a_row(0)
{
}

int ShenandoahHeuristics::compare_heap_regions_by_garbage(ShenandoahHeapRegion* a, ShenandoahHeapRegion* b) {
  if (a == NULL) {
    if (b == NULL) {
      return 0;
    } else {
      return 1;
    }
  } else if (b == NULL) {
    return -1;
  }

  size_t garbage_a = a->garbage();
  size_t garbage_b = b->garbage();

  if (garbage_a > garbage_b)
    return -1;
  else if (garbage_a < garbage_b)
    return 1;
  else return 0;
}

void ShenandoahHeuristics::choose_collection_set(ShenandoahCollectionSet* collection_set) {
  ShenandoahHeapRegionSet* sorted_regions = ShenandoahHeap::heap()->sorted_regions();
  sorted_regions->sort(compare_heap_regions_by_garbage);

  start_choose_collection_set();

  size_t i = 0;
  size_t end = sorted_regions->active_regions();
  ShenandoahHeap* heap = ShenandoahHeap::heap();
  size_t total_garbage = heap->garbage();
  size_t immediate_garbage = 0;
  size_t immediate_regions = 0;
  for (size_t i = 0; i < end; i++) {
    ShenandoahHeapRegion* region = sorted_regions->get(i);

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
      } else if (region_in_collection_set(region, immediate_garbage)) {
        log_develop_trace(gc)("Choose region " SIZE_FORMAT " with garbage = " SIZE_FORMAT
                              " and live = " SIZE_FORMAT "\n",
                              region->region_number(), region->garbage(), region->get_live_data_bytes());
        collection_set->add_region(region);
        region->set_in_collection_set(true);
      }
    } else {
      assert(region->has_live() || region->is_empty() || region->is_pinned() || region->is_humongous(), "check rejected");
      log_develop_trace(gc)("Rejected region " SIZE_FORMAT " with garbage = " SIZE_FORMAT
                            " and live = " SIZE_FORMAT "\n",
                            region->region_number(), region->garbage(), region->get_live_data_bytes());
    }
  }

  end_choose_collection_set();

  log_debug(gc)("Total Garbage: "SIZE_FORMAT, total_garbage);
  log_debug(gc)("Immediate Garbage: "SIZE_FORMAT, immediate_garbage);
  log_debug(gc)("Immediate Garbage regions: "SIZE_FORMAT, immediate_regions);
  log_debug(gc)("Garbage to be collected: "SIZE_FORMAT, collection_set->garbage());
  log_debug(gc)("Objects to be evacuated: "SIZE_FORMAT, collection_set->live_data());
  log_debug(gc)("Live / Garbage ratio: "SIZE_FORMAT"%%", collection_set->live_data() * 100 / MAX2(collection_set->garbage(), 1UL));
  log_debug(gc)("Collected-Garbage ratio / Total-garbage: "SIZE_FORMAT"%%", collection_set->garbage() * 100 / MAX2(total_garbage, 1UL));
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
      _timing_data[phase + i]._ms.add(t * 1000.0);
    }
  }
}

void ShenandoahCollectorPolicy::record_phase_start(TimingPhase phase) {
  _timing_data[phase]._start = os::elapsedTime();

}

void ShenandoahCollectorPolicy::record_phase_end(TimingPhase phase) {
  double end = os::elapsedTime();
  double elapsed = end - _timing_data[phase]._start;
  _timing_data[phase]._ms.add(elapsed * 1000);

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
    size_t available =  free_capacity - free_used;
    uintx factor = heap->need_update_refs() ? ShenandoahFreeThreshold : ShenandoahInitialFreeThreshold;
    size_t targetStartMarking = (capacity * factor) / 100;

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
    size_t threshold = ShenandoahHeapRegion::RegionSizeBytes * ShenandoahGarbageThreshold / 100;
    return r->garbage() > threshold;
  }

};


class AdaptiveHeuristics : public ShenandoahHeuristics {
private:
  uintx _free_threshold;
public:
  AdaptiveHeuristics() :
    ShenandoahHeuristics(),
    _free_threshold(ShenandoahInitFreeThreshold) {
  }

  virtual ~AdaptiveHeuristics() {}

  virtual bool region_in_collection_set(ShenandoahHeapRegion* r, size_t immediate_garbage) {
    size_t threshold = ShenandoahHeapRegion::RegionSizeBytes * ShenandoahGarbageThreshold / 100;
    return r->garbage() > threshold;
  }

  virtual void record_cm_cancelled() {
    ShenandoahHeuristics::record_cm_cancelled();
    if (_free_threshold < ShenandoahMaxFreeThreshold) {
      _free_threshold++;
      log_debug(gc,ergo)("increasing free threshold to: "UINTX_FORMAT, _free_threshold);
    }
  }

  virtual void record_cm_success() {
    ShenandoahHeuristics::record_cm_success();
    if (_successful_cm_cycles_in_a_row > ShenandoahHappyCyclesThreshold &&
        _free_threshold > ShenandoahMinFreeThreshold) {
      _free_threshold--;
      log_debug(gc,ergo)("reducing free threshold to: "UINTX_FORMAT, _free_threshold);
      _successful_cm_cycles_in_a_row = 0;
    }
  }

  virtual bool should_start_concurrent_mark(size_t used, size_t capacity) const {
    bool shouldStartConcurrentMark = false;

    ShenandoahHeap* heap = ShenandoahHeap::heap();
    size_t free_capacity = heap->free_regions()->capacity();
    size_t free_used = heap->free_regions()->used();
    assert(free_used <= free_capacity, "must use less than capacity");
    size_t available =  free_capacity - free_used;
    uintx factor = _free_threshold;
    size_t targetStartMarking = (capacity * factor) / 100;

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

};

class GlobalHeuristics : public DynamicHeuristics {
private:
  size_t _garbage;
  size_t _min_garbage;
public:
  GlobalHeuristics() : DynamicHeuristics() {
    if (FLAG_IS_DEFAULT(ShenandoahGarbageThreshold)) {
      FLAG_SET_DEFAULT(ShenandoahGarbageThreshold, 90);
    }
  }
  virtual ~GlobalHeuristics() {}

  virtual void start_choose_collection_set() {
    _garbage = 0;
    size_t heap_garbage = ShenandoahHeap::heap()->garbage();
    _min_garbage =  heap_garbage * ShenandoahGarbageThreshold / 100;
  }

  virtual bool region_in_collection_set(ShenandoahHeapRegion* r, size_t immediate_garbage) {
    if (_garbage + immediate_garbage < _min_garbage && ! r->is_empty()) {
      _garbage += r->garbage();
      return true;
    } else {
      return false;
    }
  }

};

class RatioHeuristics : public DynamicHeuristics {
private:
  size_t _garbage;
  size_t _live;
public:
  RatioHeuristics() : DynamicHeuristics() {
    if (FLAG_IS_DEFAULT(ShenandoahGarbageThreshold)) {
      FLAG_SET_DEFAULT(ShenandoahGarbageThreshold, 95);
    }
  }
  virtual ~RatioHeuristics() {}

  virtual void start_choose_collection_set() {
    _garbage = 0;
    _live = 0;
  }

  virtual bool region_in_collection_set(ShenandoahHeapRegion* r, size_t immediate_garbage) {
    size_t min_ratio = 100 - ShenandoahGarbageThreshold;
    if (_live * 100 / MAX2(_garbage + immediate_garbage, 1UL) < min_ratio && ! r->is_empty()) {
      _garbage += r->garbage();
      _live += r->get_live_data_bytes();
      return true;
    } else {
      return false;
    }
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

  _phase_names[init_mark] = "Initial Mark Pauses (net)";
  _phase_names[init_mark_gross] = "Initial Mark Pauses (gross)";
  _phase_names[final_mark] = "Final Mark Pauses (net)";
  _phase_names[final_mark_gross] = "Final Mark Pauses (gross)";
  _phase_names[accumulate_stats] = "  Accumulate Stats";
  _phase_names[make_parsable] = "  Make Parsable";
  _phase_names[clear_liveness] = "  Clear Liveness";
  _phase_names[scan_roots] = "  Scan Roots";
  _phase_names[update_roots] = "  Update Roots";
  _phase_names[drain_satb] = "  Drain SATB";
  _phase_names[weakrefs] = "  Weak References";
  _phase_names[class_unloading] = "  Class Unloading";
  _phase_names[prepare_evac] = "  Prepare Evacuation";
  _phase_names[init_evac] = "  Initial Evacuation";

  _phase_names[scan_thread_roots] = "    Scan Thread Roots";
  _phase_names[scan_code_roots] = "    Scan Code Cache Roots";
  _phase_names[scan_string_table_roots] = "    Scan String Table  Roots";
  _phase_names[scan_universe_roots] = "    Scan Universe  Roots";
  _phase_names[scan_jni_roots] = "    Scan JNI  Roots";
  _phase_names[scan_jni_weak_roots] = "    Scan JNI Weak  Roots";
  _phase_names[scan_synchronizer_roots] = "    Scan Synchronizer  Roots";
  _phase_names[scan_flat_profiler_roots] = "    Scan Flat Profiler Roots";
  _phase_names[scan_management_roots] = "    Scan Management Roots";
  _phase_names[scan_system_dictionary_roots] = "    Scan System Dictionary Roots";
  _phase_names[scan_cldg_roots] = "    Scan CLDG  Roots";
  _phase_names[scan_jvmti_roots] = "    Scan JVMTI Roots";

  _phase_names[update_thread_roots] = "    Update Thread Roots";
  _phase_names[update_code_roots] = "    Update Code Cache Roots";
  _phase_names[update_string_table_roots] = "    Update String Table  Roots";
  _phase_names[update_universe_roots] = "    Update Universe  Roots";
  _phase_names[update_jni_roots] = "    Update JNI  Roots";
  _phase_names[update_jni_weak_roots] = "    Update JNI Weak  Roots";
  _phase_names[update_synchronizer_roots] = "    Update Synchronizer  Roots";
  _phase_names[update_flat_profiler_roots] = "    Update Flat Profiler Roots";
  _phase_names[update_management_roots] = "    Update Management Roots";
  _phase_names[update_system_dictionary_roots] = "    Update System Dictionary Roots";
  _phase_names[update_cldg_roots] = "    Update CLDG  Roots";
  _phase_names[update_jvmti_roots] = "    Update JVMTI Roots";

  _phase_names[evac_thread_roots] = "    Evacuate Thread Roots";
  _phase_names[evac_code_roots] = "    Evacuate Code Cache Roots";
  _phase_names[evac_string_table_roots] = "    Evacuate String Table  Roots";
  _phase_names[evac_universe_roots] = "    Evacuate Universe  Roots";
  _phase_names[evac_jni_roots] = "    Evacuate JNI  Roots";
  _phase_names[evac_jni_weak_roots] = "    Evacuate JNI Weak  Roots";
  _phase_names[evac_synchronizer_roots] = "    Evacuate Synchronizer  Roots";
  _phase_names[evac_flat_profiler_roots] = "    Evacuate Flat Profiler Roots";
  _phase_names[evac_management_roots] = "    Evacuate Management Roots";
  _phase_names[evac_system_dictionary_roots] = "    Evacuate System Dictionary Roots";
  _phase_names[evac_cldg_roots] = "    Evacuate CLDG  Roots";
  _phase_names[evac_jvmti_roots] = "    Evacuate JVMTI Roots";

  _phase_names[recycle_regions] = "  Recycle regions";
  _phase_names[reset_bitmaps] = "ResetBitmaps";
  _phase_names[resize_tlabs] = "Resize TLABs";

  _phase_names[full_gc] = "Full GC Times";
  _phase_names[full_gc_heapdumps] = "  Heap Dumps";
  _phase_names[full_gc_prepare] = "  Prepare";
  _phase_names[full_gc_mark] = "  Mark";
  _phase_names[full_gc_mark_drain_queues] = "    Drain Queues";
  _phase_names[full_gc_mark_weakrefs] = "    Weak References";
  _phase_names[full_gc_mark_class_unloading] = "    Class Unloading";
  _phase_names[full_gc_calculate_addresses] = "  Calculate Addresses";
  _phase_names[full_gc_adjust_pointers] = "  Adjust Pointers";
  _phase_names[full_gc_copy_objects] = "  Copy Objects";

  _phase_names[conc_mark] = "Concurrent Marking Times";
  _phase_names[conc_evac] = "Concurrent Evacuation Times";

  if (ShenandoahGCHeuristics != NULL) {
    if (strcmp(ShenandoahGCHeuristics, "aggressive") == 0) {
      log_info(gc, init)("Shenandoah heuristics: aggressive");
      _heuristics = new AggressiveHeuristics();
    } else if (strcmp(ShenandoahGCHeuristics, "dynamic") == 0) {
      log_info(gc, init)("Shenandoah heuristics: dynamic");
      _heuristics = new DynamicHeuristics();
    } else if (strcmp(ShenandoahGCHeuristics, "global") == 0) {
      log_info(gc, init)("Shenandoah heuristics: global");
      _heuristics = new GlobalHeuristics();
    } else if (strcmp(ShenandoahGCHeuristics, "ratio") == 0) {
      log_info(gc, init)("Shenandoah heuristics: ratio");
      _heuristics = new RatioHeuristics();
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
  _phase_times = new ShenandoahPhaseTimes(MAX2(ConcGCThreads, ParallelGCThreads));
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
  _space_alignment = ShenandoahHeapRegion::RegionSizeBytes;
  _heap_alignment = ShenandoahHeapRegion::RegionSizeBytes;
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

void ShenandoahCollectorPolicy::choose_collection_set(ShenandoahCollectionSet* collection_set) {
  _heuristics->choose_collection_set(collection_set);
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
  for (uint i = 0; i < _num_phases; i++) {
    if (_timing_data[i]._ms.maximum() != 0) {
      print_summary_sd(out, _phase_names[i], &(_timing_data[i]._ms));
    }
  }
  out->print_cr("User requested GCs: "SIZE_FORMAT, _user_requested_gcs);
  out->print_cr("Allocation failure GCs: "SIZE_FORMAT, _allocation_failure_gcs);
  out->print_cr("Successful concurrent markings: "SIZE_FORMAT, _successful_cm);
  out->print_cr("Degenerated concurrent markings: "SIZE_FORMAT, _degenerated_cm);

  out->print_cr(" ");
  double total_sum = _timing_data[init_mark_gross]._ms.sum() +
                     _timing_data[final_mark_gross]._ms.sum();
  double total_avg = (_timing_data[init_mark_gross]._ms.avg() +
                      _timing_data[final_mark_gross]._ms.avg()) / 2.0;
  double total_max = MAX2(_timing_data[init_mark_gross]._ms.maximum(),
                          _timing_data[final_mark_gross]._ms.maximum());

  out->print_cr("%-27s = %8.2lf s, avg = %8.2lf ms, max = %8.2lf ms",
                         "Total", total_sum / 1000.0, total_avg, total_max);

}

void ShenandoahCollectorPolicy::print_summary_sd(outputStream* out, const char* str, const NumberSeq* seq)  {
  double sum = seq->sum();
  out->print("%-34s = %8.2lf s (avg = %8.2lf ms)",
                      str, sum / 1000.0, seq->avg());
  out->print_cr("  %s = "INT32_FORMAT_W(5)", std dev = %8.2lf ms, max = %8.2lf ms)",
                         "(num", seq->num(), seq->sd(), seq->maximum());
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

GCTimer* ShenandoahCollectorPolicy::conc_timer() {return _conc_timer;}
GCTimer* ShenandoahCollectorPolicy::stw_timer() {return _stw_timer;}
