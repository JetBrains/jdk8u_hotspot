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
#include "gc_implementation/shenandoah/shenandoahGCTraceTime.hpp"
#include "gc_implementation/shenandoah/shenandoahConcurrentMark.inline.hpp"
#include "gc_implementation/shenandoah/shenandoahConcurrentThread.hpp"
#include "gc_implementation/shenandoah/shenandoahCollectorPolicy.hpp"
#include "gc_implementation/shenandoah/shenandoahHeap.inline.hpp"
#include "gc_implementation/shenandoah/shenandoahMonitoringSupport.hpp"
#include "gc_implementation/shenandoah/shenandoahUtils.hpp"
#include "gc_implementation/shenandoah/vm_operations_shenandoah.hpp"
#include "memory/iterator.hpp"
#include "memory/universe.hpp"
#include "runtime/vmThread.hpp"

SurrogateLockerThread* ShenandoahConcurrentThread::_slt = NULL;

ShenandoahConcurrentThread::ShenandoahConcurrentThread() :
  ConcurrentGCThread(),
  _full_gc_lock(Mutex::leaf, "ShenandoahFullGC_lock", true),
  _do_full_gc(0),
  _full_gc_cause(GCCause::_no_cause_specified),
  _graceful_shutdown(0)
{
  create_and_start();
}

ShenandoahConcurrentThread::~ShenandoahConcurrentThread() {
  // This is here so that super is called.
}

void ShenandoahConcurrentThread::run() {
  initialize_in_thread();

  wait_for_universe_init();

  // Wait until we have the surrogate locker thread in place.
  {
    MutexLockerEx x(CGC_lock, true);
    while(_slt == NULL && !_should_terminate) {
      CGC_lock->wait(true, 200);
    }
  }

  ShenandoahHeap* heap = ShenandoahHeap::heap();

  double last_shrink_time = os::elapsedTime();

  // Shrink period avoids constantly polling regions for shrinking.
  // Having a period 10x lower than the delay would mean we hit the
  // shrinking with lag of less than 1/10-th of true delay.
  // ShenandoahUncommitDelay is in msecs, but shrink_period is in seconds.
  double shrink_period = (double)ShenandoahUncommitDelay / 1000 / 10;

  while (!in_graceful_shutdown() && !_should_terminate) {
    bool conc_gc_requested = heap->shenandoahPolicy()->should_start_concurrent_mark(heap->used(), heap->capacity());
    bool full_gc_requested = is_full_gc();
    bool gc_requested = conc_gc_requested || full_gc_requested;

    if (full_gc_requested) {
      service_fullgc_cycle();
    } else if (conc_gc_requested) {
      service_normal_cycle();
    }

    if (gc_requested) {
      // Counters are already updated on allocation path. Makes sense to update them
      // them here only when GC happened.
      heap->monitoring_support()->update_counters();

      // Coming out of (cancelled) concurrent GC, reset these for sanity
      if (heap->is_evacuation_in_progress()) {
        heap->set_evacuation_in_progress_concurrently(false);
      }

      if (heap->is_update_refs_in_progress()) {
        heap->set_update_refs_in_progress(false);
      }
    } else {
      Thread::current()->_ParkEvent->park(10);
    }

    // Try to uncommit stale regions
    double current = os::elapsedTime();
    if (current - last_shrink_time > shrink_period) {
      heap->handle_heap_shrinkage();
      last_shrink_time = current;
    }

    // Make sure the _do_full_gc flag changes are seen.
    OrderAccess::storeload();
  }

  // Wait for the actual stop(), can't leave run_service() earlier.
  while (! _should_terminate) {
    Thread::current()->_ParkEvent->park(10);
  }
  terminate();
}

void ShenandoahConcurrentThread::service_normal_cycle() {
  if (check_cancellation()) return;

  ShenandoahHeap* heap = ShenandoahHeap::heap();

  GCTimer* gc_timer = heap->gc_timer();
  GCTracer* gc_tracer = heap->tracer();

  ShenandoahGCSession session;
  gc_tracer->report_gc_start(GCCause::_no_cause_specified, gc_timer->gc_start());

  // Cycle started
  heap->shenandoahPolicy()->record_cycle_start();

  // Capture peak occupancy right after starting the cycle
  heap->shenandoahPolicy()->record_peak_occupancy();

  TraceCollectorStats tcs(heap->monitoring_support()->concurrent_collection_counters());
  TraceMemoryManagerStats tmms(false, GCCause::_no_cause_specified);

  // Start initial mark under STW:
  {
    // Workers are setup by VM_ShenandoahInitMark
    TraceCollectorStats tcs(heap->monitoring_support()->stw_collection_counters());
    ShenandoahGCPhase total_phase(ShenandoahCollectorPolicy::total_pause_gross);
    ShenandoahGCPhase init_mark_phase(ShenandoahCollectorPolicy::init_mark_gross);
    VM_ShenandoahInitMark initMark;
    VMThread::execute(&initMark);
  }

  if (check_cancellation()) return;

  // Continue concurrent mark:
  {
    // Setup workers for concurrent marking phase
    FlexibleWorkGang* workers = heap->workers();
    uint n_workers = ShenandoahCollectorPolicy::calc_workers_for_conc_marking(workers->active_workers(),
                                                                              (uint) Threads::number_of_non_daemon_threads());
    ShenandoahWorkerScope scope(workers, n_workers);

    GCTraceTime time("Concurrent marking", PrintGC, gc_timer, gc_tracer->gc_id(), true);
    TraceCollectorStats tcs(heap->monitoring_support()->concurrent_collection_counters());
    ShenandoahHeap::heap()->concurrentMark()->mark_from_roots();
  }

  // Allocations happen during concurrent mark, record peak after the phase:
  heap->shenandoahPolicy()->record_peak_occupancy();

  // Possibly hand over remaining marking work to final-mark phase.
  bool clear_full_gc = false;
  if (heap->cancelled_concgc()) {
    heap->shenandoahPolicy()->record_cm_cancelled();
    if (_full_gc_cause == GCCause::_allocation_failure &&
        heap->shenandoahPolicy()->handover_cancelled_marking()) {
      heap->clear_cancelled_concgc();
      clear_full_gc = true;
      heap->shenandoahPolicy()->record_cm_degenerated();
    } else {
      return;
    }
  } else {
    heap->shenandoahPolicy()->record_cm_success();

    // If not cancelled, can try to concurrently pre-clean
    if (ShenandoahPreclean) {
      if (heap->concurrentMark()->process_references()) {
        GCTraceTime time("Concurrent precleaning", PrintGC, gc_timer, gc_tracer->gc_id(), true);
        ShenandoahGCPhase conc_preclean(ShenandoahCollectorPolicy::conc_preclean);

        heap->concurrentMark()->preclean_weak_refs();

        // Allocations happen during concurrent preclean, record peak after the phase:
        heap->shenandoahPolicy()->record_peak_occupancy();
      }
    }
  }

  // Proceed to complete marking under STW, and start evacuation:
  {
    // Workers are setup by VM_ShenandoahFinalMarkStartEvac
    TraceCollectorStats tcs(heap->monitoring_support()->stw_collection_counters());
    ShenandoahGCPhase total_phase(ShenandoahCollectorPolicy::total_pause_gross);
    ShenandoahGCPhase final_mark_phase(ShenandoahCollectorPolicy::final_mark_gross);
    VM_ShenandoahFinalMarkStartEvac finishMark;
    VMThread::execute(&finishMark);
  }

  if (check_cancellation()) return;

  // If we handed off remaining marking work above, we need to kick off waiting Java threads
  if (clear_full_gc) {
    reset_full_gc();
  }

  // Perform concurrent evacuation, if required.
  // This phase can be skipped if there is nothing to evacuate. If so, evac_in_progress would be unset
  // by collection set preparation code.
  if (heap->is_evacuation_in_progress()) {
    // Setup workers for concurrent evacuation phase
    FlexibleWorkGang* workers = heap->workers();
    uint n_workers = ShenandoahCollectorPolicy::calc_workers_for_conc_evacuation(workers->active_workers(),
                                                                                 (uint) Threads::number_of_non_daemon_threads());
    ShenandoahWorkerScope scope(workers, n_workers);

    GCTraceTime time("Concurrent evacuation", PrintGC, gc_timer, gc_tracer->gc_id(), true);
    TraceCollectorStats tcs(heap->monitoring_support()->concurrent_collection_counters());
    heap->do_evacuation();

    // Allocations happen during evacuation, record peak after the phase:
    heap->shenandoahPolicy()->record_peak_occupancy();

    // Do an update-refs phase if required.
    if (check_cancellation()) return;
  }

  // Perform update-refs phase, if required.
  // This phase can be skipped if there was nothing evacuated. If so, need_update_refs would be unset
  // by collection set preparation code. However, adaptive heuristics need to record "success" when
  // this phase is skipped. Therefore, we conditionally execute all ops, leaving heuristics adjustments
  // intact.
  if (heap->shenandoahPolicy()->should_start_update_refs()) {

    bool do_it = heap->need_update_refs();
    if (do_it) {
      {
        ShenandoahGCPhase total_phase(ShenandoahCollectorPolicy::total_pause_gross);
        ShenandoahGCPhase init_update_refs_phase(ShenandoahCollectorPolicy::init_update_refs_gross);
        VM_ShenandoahInitUpdateRefs init_update_refs;
        VMThread::execute(&init_update_refs);
      }

      {
        GCTraceTime time("Concurrent update references ", PrintGC, gc_timer, gc_tracer->gc_id(), true);
        heap->concurrent_update_heap_references();
      }
    }

    // Allocations happen during update-refs, record peak after the phase:
    heap->shenandoahPolicy()->record_peak_occupancy();

    clear_full_gc = false;
    if (heap->cancelled_concgc()) {
      heap->shenandoahPolicy()->record_uprefs_cancelled();
      if (_full_gc_cause == GCCause::_allocation_failure &&
          heap->shenandoahPolicy()->handover_cancelled_uprefs()) {
        clear_full_gc = true;
        heap->shenandoahPolicy()->record_uprefs_degenerated();
      } else {
        return;
      }
    } else {
      heap->shenandoahPolicy()->record_uprefs_success();
    }

    if (do_it) {
      ShenandoahGCPhase total(ShenandoahCollectorPolicy::total_pause_gross);
      ShenandoahGCPhase final_update_refs_phase(ShenandoahCollectorPolicy::final_update_refs_gross);
      VM_ShenandoahFinalUpdateRefs final_update_refs;
      VMThread::execute(&final_update_refs);
    }
  } else {
    // If update-refs were skipped, need to do another verification pass after evacuation.
    if (ShenandoahVerify && !check_cancellation()) {
      VM_ShenandoahVerifyHeapAfterEvacuation verify_after_evacuation;
      VMThread::execute(&verify_after_evacuation);
    }
  }

  // Prepare for the next normal cycle:
  if (check_cancellation()) return;

  if (clear_full_gc) {
    reset_full_gc();
  }

  {
    GCTraceTime time("Concurrent reset bitmaps", PrintGC, gc_timer, gc_tracer->gc_id(), true);
    ShenandoahGCPhase phase(ShenandoahCollectorPolicy::conc_reset_bitmaps);
    FlexibleWorkGang* workers = heap->workers();
    ShenandoahPushWorkerScope scope(workers, heap->max_workers());
    heap->reset_next_mark_bitmap(workers);
  }

  // Allocations happen during bitmap cleanup, record peak after the phase:
  heap->shenandoahPolicy()->record_peak_occupancy();

  // Cycle is complete
  heap->shenandoahPolicy()->record_cycle_end();

  gc_tracer->report_gc_end(gc_timer->gc_end(), gc_timer->time_partitions());
}

bool ShenandoahConcurrentThread::check_cancellation() {
  ShenandoahHeap* heap = ShenandoahHeap::heap();
  if (heap->cancelled_concgc()) {
    assert (is_full_gc() || in_graceful_shutdown(), "Cancel GC either for Full GC, or gracefully exiting");
    return true;
  }
  return false;
}


void ShenandoahConcurrentThread::stop() {
  {
    MutexLockerEx ml(Terminator_lock);
    _should_terminate = true;
  }

  {
    MutexLockerEx ml(CGC_lock, Mutex::_no_safepoint_check_flag);
    CGC_lock->notify_all();
  }

  {
    MutexLockerEx ml(Terminator_lock);
    while (!_has_terminated) {
      Terminator_lock->wait();
    }
  }
}

void ShenandoahConcurrentThread::service_fullgc_cycle() {
  ShenandoahHeap* heap = ShenandoahHeap::heap();

  {
    if (_full_gc_cause == GCCause::_allocation_failure) {
      heap->shenandoahPolicy()->record_allocation_failure_gc();
    } else {
      heap->shenandoahPolicy()->record_user_requested_gc();
    }

    TraceCollectorStats tcs(heap->monitoring_support()->full_collection_counters());
    TraceMemoryManagerStats tmms(true, _full_gc_cause);
    VM_ShenandoahFullGC full_gc(_full_gc_cause);
    VMThread::execute(&full_gc);
  }

  reset_full_gc();
}

void ShenandoahConcurrentThread::do_full_gc(GCCause::Cause cause) {
  assert(Thread::current()->is_Java_thread(), "expect Java thread here");

  if (try_set_full_gc()) {
    _full_gc_cause = cause;

    // Now that full GC is scheduled, we can abort everything else
    ShenandoahHeap::heap()->cancel_concgc(cause);
  } else {
    GCCause::Cause last_cause = _full_gc_cause;
    if (last_cause != cause) {
      switch (cause) {
        // These GC causes take precedence:
        case GCCause::_allocation_failure:
          log_info(gc)("Full GC was already pending with cause: %s; new cause is %s, overwriting",
                       GCCause::to_string(last_cause),
                       GCCause::to_string(cause));
          _full_gc_cause = cause;
          break;
        // Other GC causes can be ignored
        default:
          log_info(gc)("Full GC is already pending with cause: %s; new cause was %s, ignoring",
                       GCCause::to_string(last_cause),
                       GCCause::to_string(cause));
          break;
      }
    }
  }

  MonitorLockerEx ml(&_full_gc_lock);
  while (is_full_gc()) {
    ml.wait();
  }
  assert(!is_full_gc(), "expect full GC to have completed");
}

void ShenandoahConcurrentThread::reset_full_gc() {
  OrderAccess::release_store_fence(&_do_full_gc, 0);
  MonitorLockerEx ml(&_full_gc_lock);
  ml.notify_all();
}

bool ShenandoahConcurrentThread::try_set_full_gc() {
  jbyte old = Atomic::cmpxchg(1, &_do_full_gc, 0);
  return old == 0; // success
}

bool ShenandoahConcurrentThread::is_full_gc() {
  return OrderAccess::load_acquire(&_do_full_gc) == 1;
}

void ShenandoahConcurrentThread::print() const {
  print_on(tty);
}

void ShenandoahConcurrentThread::print_on(outputStream* st) const {
  st->print("Shenandoah Concurrent Thread");
  Thread::print_on(st);
  st->cr();
}

void ShenandoahConcurrentThread::start() {
  create_and_start();
}

void ShenandoahConcurrentThread::makeSurrogateLockerThread(TRAPS) {
  assert(UseShenandoahGC, "SLT thread needed only for concurrent GC");
  assert(THREAD->is_Java_thread(), "must be a Java thread");
  assert(_slt == NULL, "SLT already created");
  _slt = SurrogateLockerThread::make(THREAD);
}

void ShenandoahConcurrentThread::prepare_for_graceful_shutdown() {
  OrderAccess::release_store_fence(&_graceful_shutdown, 1);
}

bool ShenandoahConcurrentThread::in_graceful_shutdown() {
  return OrderAccess::load_acquire(&_graceful_shutdown) == 1;
}
