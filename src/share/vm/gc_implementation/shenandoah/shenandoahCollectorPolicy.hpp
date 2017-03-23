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

#include "memory/collectorPolicy.hpp"
#include "runtime/arguments.hpp"
#include "utilities/numberSeq.hpp"

class ShenandoahCollectionSet;
class ShenandoahFreeSet;
class ShenandoahHeap;
class ShenandoahHeuristics;
class ShenandoahPhaseTimes;

class STWGCTimer;
class ConcurrentGCTimer;

class ShenandoahCollectorPolicy: public CollectorPolicy {
public:
  enum TimingPhase {
    total_pause_gross,
    total_pause,

    init_mark_gross,
    init_mark,
    accumulate_stats,
    make_parsable,
    clear_liveness,
    scan_roots,
    scan_thread_roots,
    scan_code_roots,
    scan_string_table_roots,
    scan_universe_roots,
    scan_jni_roots,
    scan_jni_weak_roots,
    scan_synchronizer_roots,
    scan_flat_profiler_roots,
    scan_management_roots,
    scan_system_dictionary_roots,
    scan_cldg_roots,
    scan_jvmti_roots,

    resize_tlabs,

    final_mark_gross,
    final_mark,
    update_roots,
    update_thread_roots,
    update_code_roots,
    update_string_table_roots,
    update_universe_roots,
    update_jni_roots,
    update_jni_weak_roots,
    update_synchronizer_roots,
    update_flat_profiler_roots,
    update_management_roots,
    update_system_dictionary_roots,
    update_cldg_roots,
    update_jvmti_roots,
    drain_satb,
    weakrefs,
    class_unloading,
    prepare_evac,
    recycle_regions,
    init_evac,
    evac_thread_roots,
    evac_code_roots,
    evac_string_table_roots,
    evac_universe_roots,
    evac_jni_roots,
    evac_jni_weak_roots,
    evac_synchronizer_roots,
    evac_flat_profiler_roots,
    evac_management_roots,
    evac_system_dictionary_roots,
    evac_cldg_roots,
    evac_jvmti_roots,

    conc_mark,
    conc_evac,
    reset_bitmaps,

    full_gc,
    full_gc_heapdumps,
    full_gc_prepare,
    full_gc_mark,
    full_gc_mark_drain_queues,
    full_gc_mark_weakrefs,
    full_gc_mark_class_unloading,
    full_gc_calculate_addresses,
    full_gc_adjust_pointers,
    full_gc_copy_objects,

    _num_phases
  };

private:
  struct TimingData {
    HdrSeq _secs;
    double _start;
    size_t _count;
  };

private:
  TimingData _timing_data[_num_phases];
  const char* _phase_names[_num_phases];

  size_t _user_requested_gcs;
  size_t _allocation_failure_gcs;
  size_t _degenerated_cm;
  size_t _successful_cm;

  ShenandoahHeap* _pgc;
  ShenandoahHeuristics* _heuristics;
  ShenandoahTracer* _tracer;
  STWGCTimer* _stw_timer;
  ConcurrentGCTimer* _conc_timer;

  bool _conc_gc_aborted;

  size_t _cycle_counter;

  ShenandoahPhaseTimes* _phase_times;

public:
  ShenandoahCollectorPolicy();

  ShenandoahPhaseTimes* phase_times();

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

  void record_workers_start(TimingPhase phase);
  void record_workers_end(TimingPhase phase);

  void report_concgc_cancelled();

  void record_user_requested_gc();
  void record_allocation_failure_gc();

  void record_bytes_allocated(size_t bytes);
  void record_bytes_reclaimed(size_t bytes);
  void record_bytes_start_CM(size_t bytes);
  void record_bytes_end_CM(size_t bytes);
  bool should_start_concurrent_mark(size_t used, size_t capacity);
  bool handover_cancelled_marking();

  void record_cm_cancelled();
  void record_cm_success();
  void record_cm_degenerated();
  void record_full_gc();

  void choose_collection_set(ShenandoahCollectionSet* collection_set, int* connections=NULL);
  void choose_free_set(ShenandoahFreeSet* free_set);

  bool process_references();
  bool unload_classes();

  void print_tracing_info(outputStream* out);

  GCTimer* conc_timer();
  GCTimer* stw_timer();
  ShenandoahTracer* tracer() {return _tracer;}

  void set_conc_gc_aborted() { _conc_gc_aborted = true;}
  void clear_conc_gc_aborted() {_conc_gc_aborted = false;}

  void increase_cycle_counter();
  size_t cycle_counter() const;


  // Calculate the number of workers for initial marking
  static uint calc_workers_for_init_marking(uint active_workers,
                                            uint application_workers);

  // Calculate the number of workers for concurrent marking
  static uint calc_workers_for_conc_marking(uint active_workers,
                                            uint application_workers);

  // Calculate the number of workers for final marking
  static uint calc_workers_for_final_marking(uint active_workers,
                                             uint application_workers);

  // Calculate workers for concurrent evacuation (concurrent GC)
  static uint calc_workers_for_conc_evacuation(uint active_workers,
                                               uint application_workers);

  // Calculate workers for parallel evaculation (full GC)
  static uint calc_workers_for_parallel_evacuation(uint active_workers,
                                                   uint application_workers);

private:
  static uint calc_workers_for_java_threads(uint application_workers);
  static uint calc_workers_for_live_set(size_t live_data);

  static uint calc_default_active_workers(uint total_workers,
                                    uint min_workers,
                                    uint active_workers,
                                    uint application_workers,
                                    uint workers_by_java_threads,
                                    uint workers_by_liveset);

  static uint calc_workers_for_evacuation(bool full_gc,
                                    uint total_workers,
                                    uint active_workers,
                                    uint application_workers);

  void print_summary_sd(outputStream* out, const char* str, const HdrSeq* seq);
};


#endif // SHARE_VM_GC_SHENANDOAH_SHENANDOAH_COLLECTOR_POLICY_HPP
