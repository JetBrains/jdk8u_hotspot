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

#include "gc_implementation/shenandoah/shenandoahCollectorPolicy.hpp"
#include "gc_implementation/shenandoah/shenandoahHeap.inline.hpp"

class ShenandoahHeuristics : public CHeapObj<mtGC> {

  NumberSeq _allocation_rate_bytes;
  NumberSeq _reclamation_rate_bytes;

  size_t _bytes_allocated_since_CM;
  size_t _bytes_reclaimed_this_cycle;

protected:
  size_t _bytes_allocated_start_CM;
  size_t _bytes_allocated_during_CM;

public:

  ShenandoahHeuristics();

  void record_bytes_allocated(size_t bytes);
  void record_bytes_reclaimed(size_t bytes);
  void record_bytes_start_CM(size_t bytes);
  void record_bytes_end_CM(size_t bytes);

  virtual bool should_start_concurrent_mark(size_t used, size_t capacity) const=0;
  virtual bool update_refs_early();
  virtual void choose_collection_and_free_sets(ShenandoahHeapRegionSet* region_set,
                                               ShenandoahHeapRegionSet* collection_set,
                                               ShenandoahHeapRegionSet* free_set) =0;
  void print_tracing_info();
};

ShenandoahHeuristics::ShenandoahHeuristics() :
  _bytes_allocated_since_CM(0),
  _bytes_reclaimed_this_cycle(0),
  _bytes_allocated_start_CM(0),
  _bytes_allocated_during_CM(0)
{
  if (PrintGCDetails)
    tty->print_cr("initializing heuristics");
}

void ShenandoahCollectorPolicy::record_phase_start(TimingPhase phase) {
  _timing_data[phase]._start = os::elapsedTime();

  if (PrintGCTimeStamps) {
    if (phase == init_mark)
      _tracer->report_gc_start(GCCause::_shenandoah_init_mark, _conc_timer->gc_start());
    else if (phase == full_gc)
      _tracer->report_gc_start(GCCause::_last_ditch_collection, _stw_timer->gc_start());

    gclog_or_tty->gclog_stamp(_tracer->gc_id());
    gclog_or_tty->print("[GC %s start", _phase_names[phase]);
    ShenandoahHeap* heap = (ShenandoahHeap*) Universe::heap();

    gclog_or_tty->print(" total = " SIZE_FORMAT " K, used = " SIZE_FORMAT " K free = " SIZE_FORMAT " K", heap->capacity()/ K, heap->used() /K,
                        ((heap->capacity() - heap->used())/K) );

    if (heap->calculateUsed() != heap->used()) {
      gclog_or_tty->print("calc used = " SIZE_FORMAT " K heap used = " SIZE_FORMAT " K",
                            heap->calculateUsed() / K, heap->used() / K);
    }
    //    assert(heap->calculateUsed() == heap->used(), "Just checking");
    gclog_or_tty->print_cr("]");
  }
}

void ShenandoahCollectorPolicy::record_phase_end(TimingPhase phase) {
  double end = os::elapsedTime();
  double elapsed = end - _timing_data[phase]._start;
  _timing_data[phase]._ms.add(elapsed * 1000);

  if (ShenandoahGCVerbose && PrintGCDetails) {
    tty->print_cr("PolicyPrint: %s "SIZE_FORMAT" took %lf ms", _phase_names[phase],
                  _timing_data[phase]._count++, elapsed * 1000);
  }
  if (PrintGCTimeStamps) {
    ShenandoahHeap* heap = (ShenandoahHeap*) Universe::heap();
    gclog_or_tty->gclog_stamp(_tracer->gc_id());

    gclog_or_tty->print("[GC %s end, %lf secs", _phase_names[phase], elapsed );
    gclog_or_tty->print(" total = " SIZE_FORMAT " K, used = " SIZE_FORMAT " K free = " SIZE_FORMAT " K", heap->capacity()/ K, heap->used() /K,
                        ((heap->capacity() - heap->used())/K) );

    if (heap->calculateUsed() != heap->used()) {
      gclog_or_tty->print("calc used = " SIZE_FORMAT " K heap used = " SIZE_FORMAT " K",
                            heap->calculateUsed() / K, heap->used() / K);
    }
    //    assert(heap->calculateUsed() == heap->used(), "Stashed heap used must be equal to calculated heap used");
    gclog_or_tty->print_cr("]");

    if (phase == recycle_regions) {
      _tracer->report_gc_end(_conc_timer->gc_end(), _conc_timer->time_partitions());
    } else if (phase == full_gc) {
      _tracer->report_gc_end(_stw_timer->gc_end(), _stw_timer->time_partitions());
    } else if (phase == conc_mark || phase == conc_evac || phase == conc_uprefs || phase == prepare_evac) {
      if (_conc_gc_aborted) {
        _tracer->report_gc_end(_conc_timer->gc_end(), _conc_timer->time_partitions());
        clear_conc_gc_aborted();
      }
    } else if (phase == final_evac) {
      ShenandoahHeap* heap = ShenandoahHeap::heap();
      this->record_bytes_end_CM(heap->_bytesAllocSinceCM);
    }
  }
}

void ShenandoahCollectorPolicy::report_concgc_cancelled() {
  if (PrintGCTimeStamps)  {
    gclog_or_tty->print("Concurrent GC Cancelled\n");
    set_conc_gc_aborted();
    //    _tracer->report_gc_end(_conc_timer->gc_end(), _conc_timer->time_partitions());
  }
}

bool ShenandoahHeuristics::update_refs_early() {
  return ShenandoahUpdateRefsEarly;
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

class AggressiveHeuristics : public ShenandoahHeuristics {
public:
  AggressiveHeuristics() : ShenandoahHeuristics(){
  if (PrintGCDetails)
    tty->print_cr("Initializing aggressive heuristics");
  }

  virtual bool should_start_concurrent_mark(size_t used, size_t capacity) const {
    return true;
  }
  virtual void choose_collection_and_free_sets(ShenandoahHeapRegionSet* region_set,
                                               ShenandoahHeapRegionSet* collection_set,
                                               ShenandoahHeapRegionSet* free_set) {
    region_set->set_garbage_threshold(8);
    region_set->choose_collection_and_free_sets(collection_set, free_set);
  }
};

class HalfwayHeuristics : public ShenandoahHeuristics {
public:
  HalfwayHeuristics() : ShenandoahHeuristics() {
  if (PrintGCDetails)
    tty->print_cr("Initializing halfway heuristics");
  }

  bool should_start_concurrent_mark(size_t used, size_t capacity) const {
    ShenandoahHeap* heap = ShenandoahHeap::heap();
    size_t threshold_bytes_allocated = heap->capacity() / 4;
    if (used * 2 > capacity && heap->_bytesAllocSinceCM > threshold_bytes_allocated)
      return true;
    else
      return false;
  }
  void choose_collection_and_free_sets(ShenandoahHeapRegionSet* region_set,
                                       ShenandoahHeapRegionSet* collection_set,
                                       ShenandoahHeapRegionSet* free_set) {
    region_set->set_garbage_threshold(ShenandoahHeapRegion::RegionSizeBytes / 2);
    region_set->choose_collection_and_free_sets(collection_set, free_set);
  }
};

// GC as little as possible
class LazyHeuristics : public ShenandoahHeuristics {
public:
  LazyHeuristics() : ShenandoahHeuristics() {
    if (PrintGCDetails) {
      tty->print_cr("Initializing lazy heuristics");
    }
  }

  virtual bool should_start_concurrent_mark(size_t used, size_t capacity) const {
    size_t targetStartMarking = (capacity / 5) * 4;
    if (used > targetStartMarking) {
      return true;
    } else {
      return false;
    }
  }

  virtual void choose_collection_and_free_sets(ShenandoahHeapRegionSet* region_set,
                                               ShenandoahHeapRegionSet* collection_set,
                                               ShenandoahHeapRegionSet* free_set) {
    region_set->choose_collection_and_free_sets(collection_set, free_set);
  }
};

// These are the heuristics in place when we made this class
class StatusQuoHeuristics : public ShenandoahHeuristics {
public:
  StatusQuoHeuristics() : ShenandoahHeuristics() {
    if (PrintGCDetails) {
      tty->print_cr("Initializing status quo heuristics");
    }
  }

  virtual bool should_start_concurrent_mark(size_t used, size_t capacity) const {
    size_t targetStartMarking = capacity / 16;
    ShenandoahHeap* heap = ShenandoahHeap::heap();
    size_t threshold_bytes_allocated = heap->capacity() / 4;

    if (used > targetStartMarking
        && heap->_bytesAllocSinceCM > threshold_bytes_allocated) {
      // Need to check that an appropriate number of regions have
      // been allocated since last concurrent mark too.
      return true;
    } else {
      return false;
    }
  }

  virtual void choose_collection_and_free_sets(ShenandoahHeapRegionSet* region_set,
                                               ShenandoahHeapRegionSet* collection_set,
                                               ShenandoahHeapRegionSet* free_set) {
    region_set->choose_collection_and_free_sets(collection_set, free_set);
  }
};

static uintx clamp(uintx value, uintx min, uintx max) {
  value = MAX2(value, min);
  value = MIN2(value, max);
  return value;
}

static double get_percent(uintx value) {
  double _percent = static_cast<double>(clamp(value, 0, 100));
  return _percent / 100.;
}

class DynamicHeuristics : public ShenandoahHeuristics {
private:
  double _free_threshold_factor;
  double _garbage_threshold_factor;
  double _allocation_threshold_factor;

  uintx _free_threshold;
  uintx _garbage_threshold;
  uintx _allocation_threshold;

public:
  DynamicHeuristics() : ShenandoahHeuristics() {
    if (PrintGCDetails) {
      tty->print_cr("Initializing dynamic heuristics");
    }

    _free_threshold = 0;
    _garbage_threshold = 0;
    _allocation_threshold = 0;

    _free_threshold_factor = 0.;
    _garbage_threshold_factor = 0.;
    _allocation_threshold_factor = 0.;
  }

  virtual ~DynamicHeuristics() {}

  virtual bool should_start_concurrent_mark(size_t used, size_t capacity) const {

    bool shouldStartConcurrentMark = false;

    ShenandoahHeap* heap = ShenandoahHeap::heap();
    size_t available = heap->free_regions()->capacity() - heap->free_regions()->used();
    uintx factor = heap->need_update_refs() ? ShenandoahFreeThreshold : ShenandoahInitialFreeThreshold;
    size_t targetStartMarking = (capacity * factor) / 100;

    size_t threshold_bytes_allocated = heap->capacity() * _allocation_threshold_factor;
    if (available < targetStartMarking &&
        heap->_bytesAllocSinceCM > threshold_bytes_allocated)
    {
      // Need to check that an appropriate number of regions have
      // been allocated since last concurrent mark too.
      shouldStartConcurrentMark = true;
    }

    if (shouldStartConcurrentMark && ShenandoahTracePhases) {
      tty->print_cr("Start GC at available: "SIZE_FORMAT", factor: "UINTX_FORMAT", update-refs: %s", available, factor, BOOL_TO_STR(heap->need_update_refs()));
    }
    return shouldStartConcurrentMark;
  }

  virtual void choose_collection_and_free_sets(ShenandoahHeapRegionSet* region_set,
                                               ShenandoahHeapRegionSet* collection_set,
                                               ShenandoahHeapRegionSet* free_set)
  {
    region_set->set_garbage_threshold(ShenandoahHeapRegion::RegionSizeBytes * _garbage_threshold_factor);
    region_set->choose_collection_and_free_sets(collection_set, free_set);
  }

  void set_free_threshold(uintx free_threshold) {
    this->_free_threshold_factor = get_percent(free_threshold);
    this->_free_threshold = free_threshold;
  }

  void set_garbage_threshold(uintx garbage_threshold) {
    this->_garbage_threshold_factor = get_percent(garbage_threshold);
    this->_garbage_threshold = garbage_threshold;
  }

  void set_allocation_threshold(uintx allocationThreshold) {
    this->_allocation_threshold_factor = get_percent(allocationThreshold);
    this->_allocation_threshold = allocationThreshold;
  }

  uintx get_allocation_threshold() {
    return this->_allocation_threshold;
  }

  uintx get_garbage_threshold() {
    return this->_garbage_threshold;
  }

  uintx get_free_threshold() {
    return this->_free_threshold;
  }
};


class AdaptiveHeuristics : public ShenandoahHeuristics {
private:
  size_t _max_live_data;
  double _used_threshold_factor;
  double _garbage_threshold_factor;
  double _allocation_threshold_factor;

  uintx _used_threshold;
  uintx _garbage_threshold;
  uintx _allocation_threshold;

public:
  AdaptiveHeuristics() : ShenandoahHeuristics() {
    if (PrintGCDetails) {
      tty->print_cr("Initializing dynamic heuristics");
    }

    _max_live_data = 0;

    _used_threshold = 0;
    _garbage_threshold = 0;
    _allocation_threshold = 0;

    _used_threshold_factor = 0.;
    _garbage_threshold_factor = 0.1;
    _allocation_threshold_factor = 0.;
  }

  virtual ~AdaptiveHeuristics() {}

  virtual bool should_start_concurrent_mark(size_t used, size_t capacity) const {

    ShenandoahHeap* _heap = ShenandoahHeap::heap();
    bool shouldStartConcurrentMark = false;

    size_t max_live_data = _max_live_data;
    if (max_live_data == 0) {
      max_live_data = capacity * 0.2; // Very generous initial value.
    } else {
      max_live_data *= 1.3; // Add some wiggle room.
    }
    size_t max_cycle_allocated = _heap->_max_allocated_gc;
    if (max_cycle_allocated == 0) {
      max_cycle_allocated = capacity * 0.3; // Very generous.
    } else {
      max_cycle_allocated *= 1.3; // Add 20% wiggle room. Should be enough.
    }
    size_t threshold = _heap->capacity() - max_cycle_allocated - max_live_data;
    if (used > threshold)
    {
      shouldStartConcurrentMark = true;
    }

    return shouldStartConcurrentMark;
  }

  virtual void choose_collection_and_free_sets(ShenandoahHeapRegionSet* region_set,
                                               ShenandoahHeapRegionSet* collection_set,
                                               ShenandoahHeapRegionSet* free_set)
  {
    size_t bytes_alloc = ShenandoahHeap::heap()->_bytesAllocSinceCM;
    size_t min_garbage =  bytes_alloc/* * 1.1*/;
    region_set->set_garbage_threshold(ShenandoahHeapRegion::RegionSizeBytes * _garbage_threshold_factor);
    region_set->choose_collection_and_free_sets_min_garbage(collection_set, free_set, min_garbage);
    /*
    tty->print_cr("garbage to be collected: "SIZE_FORMAT, collection_set->garbage());
    tty->print_cr("objects to be evacuated: "SIZE_FORMAT, collection_set->live_data());
    */
    _max_live_data = MAX2(_max_live_data, collection_set->live_data());
  }

  void set_used_threshold(uintx used_threshold) {
    this->_used_threshold_factor = get_percent(used_threshold);
    this->_used_threshold = used_threshold;
  }

  void set_garbage_threshold(uintx garbage_threshold) {
    this->_garbage_threshold_factor = get_percent(garbage_threshold);
    this->_garbage_threshold = garbage_threshold;
  }

  void set_allocation_threshold(uintx allocationThreshold) {
    this->_allocation_threshold_factor = get_percent(allocationThreshold);
    this->_allocation_threshold = allocationThreshold;
  }

  uintx get_allocation_threshold() {
    return this->_allocation_threshold;
  }

  uintx get_garbage_threshold() {
    return this->_garbage_threshold;
  }

  uintx get_used_threshold() {
    return this->_used_threshold;
  }
};

class NewAdaptiveHeuristics : public ShenandoahHeuristics {
private:
  size_t _max_live_data;
  double _target_heap_occupancy_factor;
  double _allocation_threshold_factor;
  size_t _last_bytesAllocSinceCM;

  uintx _target_heap_occupancy;
  uintx _allocation_threshold;

public:
  NewAdaptiveHeuristics() : ShenandoahHeuristics()
  {
    if (PrintGCDetails) {
      tty->print_cr("Initializing newadaptive heuristics");
    }
    _max_live_data = 0;
    _allocation_threshold = 0;
    _target_heap_occupancy_factor = 0.;
    _allocation_threshold_factor = 0.;
    _last_bytesAllocSinceCM = 0;
  }

  virtual ~NewAdaptiveHeuristics() {}

  virtual bool should_start_concurrent_mark(size_t used, size_t capacity) const
  {
      if (this->_bytes_allocated_during_CM > 0) {
          // Not the first concurrent mark.
          // _bytes_allocated_during_CM
          ShenandoahHeap *heap = ShenandoahHeap::heap();
          size_t threshold_bytes_allocated = heap->capacity() / 4;
          size_t targetStartMarking = (size_t) capacity * this->_target_heap_occupancy_factor;
          return (used > targetStartMarking) && (this->_bytes_allocated_during_CM > threshold_bytes_allocated);
      } else {
          // First concurrent mark.
          size_t targetStartMarking = capacity / 2;
          ShenandoahHeap *heap = ShenandoahHeap::heap();
          size_t threshold_bytes_allocated = heap->capacity() / 4;

          // Need to check that an appropriate number of regions have
          // been allocated since last concurrent mark too.
          return (used > targetStartMarking) && (heap->_bytesAllocSinceCM > threshold_bytes_allocated);
      }
  }

  virtual void choose_collection_and_free_sets(ShenandoahHeapRegionSet* region_set,
                                               ShenandoahHeapRegionSet* collection_set,
                                               ShenandoahHeapRegionSet* free_set)
  {
    ShenandoahHeap *_heap = ShenandoahHeap::heap();
    this->_last_bytesAllocSinceCM = ShenandoahHeap::heap()->_bytesAllocSinceCM;
    if (this->_last_bytesAllocSinceCM > 0) {
      size_t min_garbage = this->_last_bytesAllocSinceCM;
      region_set->choose_collection_and_free_sets_min_garbage(collection_set, free_set, min_garbage);
    } else {
      region_set->set_garbage_threshold(ShenandoahHeapRegion::RegionSizeBytes / 2);
      region_set->choose_collection_and_free_sets(collection_set, free_set);
    }
    this->_max_live_data = MAX2(this->_max_live_data, collection_set->live_data());
  }

  void set_target_heap_occupancy(uintx target_heap_occupancy) {
    this->_target_heap_occupancy_factor = get_percent(target_heap_occupancy);
    this->_target_heap_occupancy = target_heap_occupancy;
  }

  void set_allocation_threshold(uintx allocationThreshold) {
    this->_allocation_threshold_factor = get_percent(allocationThreshold);
    this->_allocation_threshold = allocationThreshold;
  }

  uintx get_allocation_threshold() {
    return this->_allocation_threshold;
  }

  uintx get_target_heap_occupancy() {
    return this->_target_heap_occupancy;
  }
};


static DynamicHeuristics *configureDynamicHeuristics() {
  DynamicHeuristics *heuristics = new DynamicHeuristics();

  heuristics->set_garbage_threshold(ShenandoahGarbageThreshold);
  heuristics->set_allocation_threshold(ShenandoahAllocationThreshold);
  heuristics->set_free_threshold(ShenandoahFreeThreshold);
  if (ShenandoahLogConfig) {
    tty->print_cr("Shenandoah dynamic heuristics thresholds: allocation "SIZE_FORMAT", used "SIZE_FORMAT", garbage "SIZE_FORMAT,
                  heuristics->get_allocation_threshold(),
                  heuristics->get_free_threshold(),
                  heuristics->get_garbage_threshold());
  }
  return heuristics;
}


static NewAdaptiveHeuristics* configureNewAdaptiveHeuristics() {
  NewAdaptiveHeuristics* heuristics = new NewAdaptiveHeuristics();

  heuristics->set_target_heap_occupancy(ShenandoahTargetHeapOccupancy);
  if (ShenandoahLogConfig) {
    tty->print_cr( "Shenandoah newadaptive heuristics target heap occupancy: "SIZE_FORMAT,
                   heuristics->get_target_heap_occupancy() );
  }
  return heuristics;
}


ShenandoahCollectorPolicy::ShenandoahCollectorPolicy() {

  ShenandoahHeapRegion::setup_heap_region_size(initial_heap_byte_size(), initial_heap_byte_size());

  initialize_all();

  _tracer = new (ResourceObj::C_HEAP, mtGC) ShenandoahTracer();
  _stw_timer = new (ResourceObj::C_HEAP, mtGC) STWGCTimer();
  _conc_timer = new (ResourceObj::C_HEAP, mtGC) ConcurrentGCTimer();
  _user_requested_gcs = 0;
  _allocation_failure_gcs = 0;
  _conc_gc_aborted = false;

  _phase_names[init_mark] = "InitMark";
  _phase_names[final_mark] = "FinalMark";
  _phase_names[accumulate_stats] = "AccumulateStats";
  _phase_names[make_parsable] = "MakeParsable";
  _phase_names[clear_liveness] = "ClearLiveness";
  _phase_names[scan_roots] = "ScanRoots";
  _phase_names[rescan_roots] = "RescanRoots";
  _phase_names[drain_satb] = "DrainSATB";
  _phase_names[drain_queues] = "DrainQueues";
  _phase_names[weakrefs] = "WeakRefs";
  _phase_names[prepare_evac] = "PrepareEvac";
  _phase_names[init_evac] = "InitEvac";
  _phase_names[final_evac] = "FinalEvacuation";
  _phase_names[final_uprefs] = "FinalUpdateRefs";

  _phase_names[update_roots] = "UpdateRoots";
  _phase_names[recycle_regions] = "RecycleRegions";
  _phase_names[reset_bitmaps] = "ResetBitmaps";
  _phase_names[resize_tlabs] = "ResizeTLABs";

  _phase_names[full_gc] = "FullGC";
  _phase_names[conc_mark] = "ConcurrentMark";
  _phase_names[conc_evac] = "ConcurrentEvacuation";
  _phase_names[conc_uprefs] = "ConcurrentUpdateReferences";

  if (ShenandoahGCHeuristics != NULL) {
    if (strcmp(ShenandoahGCHeuristics, "aggressive") == 0) {
      if (ShenandoahLogConfig) {
        tty->print_cr("Shenandoah heuristics: aggressive");
      }
      _heuristics = new AggressiveHeuristics();
    } else if (strcmp(ShenandoahGCHeuristics, "statusquo") == 0) {
      if (ShenandoahLogConfig) {
        tty->print_cr("Shenandoah heuristics: statusquo");
      }
      _heuristics = new StatusQuoHeuristics();
    } else if (strcmp(ShenandoahGCHeuristics, "halfway") == 0) {
      if (ShenandoahLogConfig) {
        tty->print_cr("Shenandoah heuristics: halfway");
      }
      _heuristics = new HalfwayHeuristics();
    } else if (strcmp(ShenandoahGCHeuristics, "lazy") == 0) {
      if (ShenandoahLogConfig) {
        tty->print_cr("Shenandoah heuristics: lazy");
      }
      _heuristics = new LazyHeuristics();
    } else if (strcmp(ShenandoahGCHeuristics, "dynamic") == 0) {
      if (ShenandoahLogConfig) {
        tty->print_cr("Shenandoah heuristics: dynamic");
      }
      _heuristics = configureDynamicHeuristics();
    } else if (strcmp(ShenandoahGCHeuristics, "adaptive") == 0) {
      if (ShenandoahLogConfig) {
        tty->print_cr("Shenandoah heuristics: adaptive");
      }
      _heuristics = new AdaptiveHeuristics();
    } else if (strcmp(ShenandoahGCHeuristics, "newadaptive") == 0) {
      if (ShenandoahLogConfig) {
        tty->print_cr("Shenandoah heuristics: newadaptive");
      }
      _heuristics = configureNewAdaptiveHeuristics();
    } else {
      fatal("Unknown -XX:ShenandoahGCHeuristics option");
    }
  } else {
      if (ShenandoahLogConfig) {
        tty->print_cr("Shenandoah heuristics: statusquo (default)");
      }
    _heuristics = new StatusQuoHeuristics();
  }

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
  ShenandoahHeap* heap = ShenandoahHeap::heap();
  return _heuristics->should_start_concurrent_mark(used, capacity);
}

bool ShenandoahCollectorPolicy::update_refs_early() {
  return _heuristics->update_refs_early();
}

void ShenandoahCollectorPolicy::choose_collection_and_free_sets(
                             ShenandoahHeapRegionSet* region_set,
                             ShenandoahHeapRegionSet* collection_set,
                             ShenandoahHeapRegionSet* free_set) {
  _heuristics->choose_collection_and_free_sets(region_set, collection_set, free_set);
}

void ShenandoahCollectorPolicy::print_tracing_info() {
  print_summary_sd("Initial Mark Pauses", 0, &(_timing_data[init_mark]._ms));
  print_summary_sd("Accumulate Stats", 2, &(_timing_data[accumulate_stats]._ms));
  print_summary_sd("Make Parsable", 2, &(_timing_data[make_parsable]._ms));
  print_summary_sd("Clear Liveness", 2, &(_timing_data[clear_liveness]._ms));
  print_summary_sd("Scan Roots", 2, &(_timing_data[scan_roots]._ms));
  print_summary_sd("Final Mark Pauses", 0, &(_timing_data[final_mark]._ms));

  print_summary_sd("Rescan Roots", 2, &(_timing_data[rescan_roots]._ms));
  print_summary_sd("Drain SATB", 2, &(_timing_data[drain_satb]._ms));
  print_summary_sd("Drain Queues", 2, &(_timing_data[drain_queues]._ms));
  if (ShenandoahProcessReferences) {
    print_summary_sd("Weak References", 2, &(_timing_data[weakrefs]._ms));
  }
  print_summary_sd("Prepare Evacuation", 2, &(_timing_data[prepare_evac]._ms));
  print_summary_sd("Initial Evacuation", 2, &(_timing_data[init_evac]._ms));

  print_summary_sd("Final Evacuation Pauses", 0, &(_timing_data[final_evac]._ms));
  print_summary_sd("Final Update Refs Pauses", 0, &(_timing_data[final_uprefs]._ms));
  print_summary_sd("Update roots", 2, &(_timing_data[update_roots]._ms));
  print_summary_sd("Recycle regions", 2, &(_timing_data[recycle_regions]._ms));
  print_summary_sd("Reset bitmaps", 2, &(_timing_data[reset_bitmaps]._ms));
  print_summary_sd("Resize TLABs", 2, &(_timing_data[resize_tlabs]._ms));
  gclog_or_tty->print_cr(" ");
  print_summary_sd("Concurrent Marking Times", 0, &(_timing_data[conc_mark]._ms));
  print_summary_sd("Concurrent Evacuation Times", 0, &(_timing_data[conc_evac]._ms));
  print_summary_sd("Concurrent Update References Times", 0, &(_timing_data[conc_uprefs]._ms));
  print_summary_sd("Full GC Times", 0, &(_timing_data[full_gc]._ms));

  gclog_or_tty->print_cr("User requested GCs: "SIZE_FORMAT, _user_requested_gcs);
  gclog_or_tty->print_cr("Allocation failure GCs: "SIZE_FORMAT, _allocation_failure_gcs);

  gclog_or_tty->print_cr(" ");
  double total_sum = _timing_data[init_mark]._ms.sum() +
    _timing_data[final_mark]._ms.sum() +
    _timing_data[final_evac]._ms.sum() +
    _timing_data[final_uprefs]._ms.sum();
  double total_avg = (_timing_data[init_mark]._ms.avg() +
                      _timing_data[final_mark]._ms.avg() +
                      _timing_data[final_evac]._ms.avg() +
                      _timing_data[final_uprefs]._ms.avg()) / 4.0;
  double total_max = MAX2(
                          MAX2(
                               MAX2(_timing_data[init_mark]._ms.maximum(),
                                    _timing_data[final_mark]._ms.maximum()),
                               _timing_data[final_evac]._ms.maximum()),
                          _timing_data[final_uprefs]._ms.maximum());

  gclog_or_tty->print_cr("%-27s = %8.2lf s, avg = %8.2lf ms, max = %8.2lf ms",
                         "Total", total_sum / 1000.0, total_avg, total_max);

}
void ShenandoahCollectorPolicy::print_summary_sd(const char* str, uint indent, const NumberSeq* seq)  {
  double sum = seq->sum();
  for (uint i = 0; i < indent; i++) gclog_or_tty->print(" ");
  gclog_or_tty->print_cr("%-27s = %8.2lf s (avg = %8.2lf ms)",
                         str, sum / 1000.0, seq->avg());
  for (uint i = 0; i < indent; i++) gclog_or_tty->print(" ");
  gclog_or_tty->print_cr("%s = "INT32_FORMAT_W(5)", std dev = %8.2lf ms, max = %8.2lf ms)",
                         "(num", seq->num(), seq->sd(), seq->maximum());
}
