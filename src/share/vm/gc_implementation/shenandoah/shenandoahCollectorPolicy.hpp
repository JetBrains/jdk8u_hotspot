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

#ifndef SHARE_VM_GC_SHENANDOAH_SHENANDOAH_COLLECTOR_POLICY_HPP
#define SHARE_VM_GC_SHENANDOAH_SHENANDOAH_COLLECTOR_POLICY_HPP

#include "gc_implementation/shared/gcTrace.hpp"
#include "gc_implementation/shared/gcTimer.hpp"
#include "memory/collectorPolicy.hpp"
#include "runtime/arguments.hpp"
#include "utilities/numberSeq.hpp"

class ShenandoahCollectionSet;
class ShenandoahFreeSet;
class ShenandoahHeap;
class ShenandoahHeuristics;

class ShenandoahCollectorPolicy: public CollectorPolicy {

public:
  enum TimingPhase {
    init_mark,
    final_mark,
    init_mark_gross,
    final_mark_gross,
    accumulate_stats,
    make_parsable,
    clear_liveness,
    scan_roots,
    rescan_roots,
    drain_satb,
    drain_queues,
    weakrefs,
    class_unloading,
    prepare_evac,
    init_evac,

    recycle_regions,
    reset_bitmaps,
    resize_tlabs,
    full_gc,
    conc_mark,
    conc_evac,

    _num_phases
  };

private:
  struct TimingData {
    NumberSeq _ms;
    double _start;
    size_t _count;
  };

private:
  TimingData _timing_data[_num_phases];
  const char* _phase_names[_num_phases];

  size_t _user_requested_gcs;
  size_t _allocation_failure_gcs;

  ShenandoahHeap* _pgc;
  ShenandoahHeuristics* _heuristics;
  ShenandoahTracer* _tracer;
  STWGCTimer* _stw_timer;
  ConcurrentGCTimer* _conc_timer;

  bool _conc_gc_aborted;

public:
  ShenandoahCollectorPolicy();

  virtual ShenandoahCollectorPolicy* as_pgc_policy();

  BarrierSet::Name barrier_set_name();

  HeapWord* mem_allocate_work(size_t size,
                              bool is_tlab,
                              bool* gc_overhead_limit_was_exceeded);

  HeapWord* satisfy_failed_allocation(size_t size, bool is_tlab);

  void initialize_alignments();

  void post_heap_initialize();

  void record_phase_start(TimingPhase phase);
  void record_phase_end(TimingPhase phase);
  void report_concgc_cancelled();

  void record_user_requested_gc();
  void record_allocation_failure_gc();

  void record_bytes_allocated(size_t bytes);
  void record_bytes_reclaimed(size_t bytes);
  void record_bytes_start_CM(size_t bytes);
  void record_bytes_end_CM(size_t bytes);
  bool should_start_concurrent_mark(size_t used, size_t capacity);

  void choose_collection_set(ShenandoahCollectionSet* collection_set);
  void choose_free_set(ShenandoahFreeSet* free_set);

  void print_tracing_info();

  GCTimer* conc_timer(){return _conc_timer;}
  GCTimer* stw_timer() {return _stw_timer;}
  ShenandoahTracer* tracer() {return _tracer;}

  void set_conc_gc_aborted() { _conc_gc_aborted = true;}
  void clear_conc_gc_aborted() {_conc_gc_aborted = false;}

private:
  void print_summary_sd(const char* str, uint indent, const NumberSeq* seq);
};


#endif // SHARE_VM_GC_SHENANDOAH_SHENANDOAH_COLLECTOR_POLICY_HPP
