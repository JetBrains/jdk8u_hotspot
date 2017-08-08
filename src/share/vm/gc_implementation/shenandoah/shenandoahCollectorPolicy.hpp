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

#ifndef SHARE_VM_GC_SHENANDOAH_SHENANDOAHCOLLECTORPOLICY_HPP
#define SHARE_VM_GC_SHENANDOAH_SHENANDOAHCOLLECTORPOLICY_HPP

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

    // Per-thread timer block, should have "roots" counters in consistent order
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

    // Per-thread timer block, should have "roots" counters in consistent order
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

    finish_queues,
    weakrefs,
    weakrefs_process,
    weakrefs_enqueue,
    purge,
    purge_class_unload,
    purge_par,
    purge_par_codecache,
    purge_par_symbstring,
    purge_par_rmt,
    purge_par_classes,
    purge_par_sync,
    purge_cldg,
    prepare_evac,
    recycle_regions,

    // Per-thread timer block, should have "roots" counters in consistent order
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

    init_update_refs_gross,
    init_update_refs,

    final_update_refs_gross,
    final_update_refs,
    final_update_refs_finish_work,

    // Per-thread timer block, should have "roots" counters in consistent order
    final_update_refs_roots,
    final_update_refs_thread_roots,
    final_update_refs_code_roots,
    final_update_refs_string_table_roots,
    final_update_refs_universe_roots,
    final_update_refs_jni_roots,
    final_update_refs_jni_weak_roots,
    final_update_refs_synchronizer_roots,
    final_update_refs_flat_profiler_roots,
    final_update_refs_management_roots,
    final_update_refs_system_dict_roots,
    final_update_refs_cldg_roots,
    final_update_refs_jvmti_roots,

    final_update_refs_recycle,

    full_gc,
    full_gc_heapdumps,
    full_gc_prepare,

    // Per-thread timer block, should have "roots" counters in consistent order
    full_gc_roots,
    full_gc_thread_roots,
    full_gc_code_roots,
    full_gc_string_table_roots,
    full_gc_universe_roots,
    full_gc_jni_roots,
    full_gc_jni_weak_roots,
    full_gc_synchronizer_roots,
    full_gc_flat_profiler_roots,
    full_gc_management_roots,
    full_gc_system_dictionary_roots,
    full_gc_cldg_roots,
    full_gc_jvmti_roots,

    full_gc_mark,
    full_gc_mark_finish_queues,
    full_gc_weakrefs,
    full_gc_weakrefs_process,
    full_gc_weakrefs_enqueue,
    full_gc_purge,
    full_gc_purge_class_unload,
    full_gc_purge_par,
    full_gc_purge_par_codecache,
    full_gc_purge_par_symbstring,
    full_gc_purge_par_rmt,
    full_gc_purge_par_classes,
    full_gc_purge_par_sync,
    full_gc_purge_cldg,
    full_gc_calculate_addresses,
    full_gc_adjust_pointers,
    full_gc_copy_objects,
    full_gc_resize_tlabs,

    // Longer concurrent phases at the end
    conc_mark,
    conc_preclean,
    conc_evac,
    conc_update_refs,
    conc_reset_bitmaps,

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

  size_t _degenerated_uprefs;
  size_t _successful_uprefs;

  volatile jbyte _in_shutdown;

  ShenandoahHeuristics* _heuristics;
  ShenandoahTracer* _tracer;

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

  void record_gc_start();
  void record_gc_end();

  // TODO: This is different from gc_end: that one encompasses one VM operation.
  // These two encompass the entire cycle.
  void record_cycle_start();
  void record_cycle_end();

  void record_phase_start(TimingPhase phase);
  void record_phase_end(TimingPhase phase);
  void record_phase_time(TimingPhase phase, jint time_us);

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

  // Returns true when there should be a separate concurrent reference
  // updating phase after evacuation.
  bool should_start_update_refs();
  bool update_refs();

  bool handover_cancelled_marking();
  bool handover_cancelled_uprefs();

  void record_cm_cancelled();
  void record_cm_success();
  void record_cm_degenerated();
  void record_uprefs_cancelled();
  void record_uprefs_success();
  void record_uprefs_degenerated();

  void record_peak_occupancy();

  void record_shutdown();
  bool is_at_shutdown();

  void choose_collection_set(ShenandoahCollectionSet* collection_set, int* connections=NULL);
  void choose_free_set(ShenandoahFreeSet* free_set);

  bool process_references();
  bool unload_classes();

  void print_tracing_info(outputStream* out);

  ShenandoahTracer* tracer() {return _tracer;}

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


#endif // SHARE_VM_GC_SHENANDOAH_SHENANDOAHCOLLECTORPOLICY_HPP
