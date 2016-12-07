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
#include "gc_implementation/shared/gcTraceTime.hpp"
#include "gc_implementation/shenandoah/shenandoahConcurrentThread.hpp"
#include "gc_implementation/shenandoah/shenandoahCollectorPolicy.hpp"
#include "gc_implementation/shenandoah/shenandoahHeap.inline.hpp"
#include "gc_implementation/shenandoah/shenandoahMonitoringSupport.hpp"
#include "gc_implementation/shenandoah/vm_operations_shenandoah.hpp"
#include "memory/iterator.hpp"
#include "memory/universe.hpp"
#include "runtime/vmThread.hpp"

SurrogateLockerThread* ShenandoahConcurrentThread::_slt = NULL;

ShenandoahConcurrentThread::ShenandoahConcurrentThread() :
  ConcurrentGCThread(),
  _full_gc_lock(Mutex::leaf, "ShenandoahFullGC_lock", true),
  _do_full_gc(false)
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

  GCTimer* gc_timer = heap->gc_timer();
  GCTracer* gc_tracer = heap->shenandoahPolicy()->tracer();
  while (! _should_terminate) {
    if (_do_full_gc) {
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
      MonitorLockerEx ml(&_full_gc_lock);
      _do_full_gc = false;
      ml.notify_all();
    } else if (heap->shenandoahPolicy()->should_start_concurrent_mark(heap->used(),
                                                               heap->capacity()))
      {

        gc_timer->register_gc_start();

        heap->shenandoahPolicy()->increase_cycle_counter();

        TraceCollectorStats tcs(heap->monitoring_support()->concurrent_collection_counters());
        TraceMemoryManagerStats tmms(false, GCCause::_no_cause_specified);

        {
          TraceCollectorStats tcs(heap->monitoring_support()->stw_collection_counters());
          VM_ShenandoahInitMark initMark;
          heap->shenandoahPolicy()->record_phase_start(ShenandoahCollectorPolicy::init_mark_gross);
          VMThread::execute(&initMark);
          heap->shenandoahPolicy()->record_phase_end(ShenandoahCollectorPolicy::init_mark_gross);
        }
        {
          // GCTraceTime time("Concurrent marking", ShenandoahLogInfo, true, gc_timer, gc_tracer->gc_id());
          TraceCollectorStats tcs(heap->monitoring_support()->concurrent_collection_counters());
          ShenandoahHeap::heap()->concurrentMark()->mark_from_roots();
        }

        {
          TraceCollectorStats tcs(heap->monitoring_support()->stw_collection_counters());
          VM_ShenandoahStartEvacuation finishMark;
          heap->shenandoahPolicy()->record_phase_start(ShenandoahCollectorPolicy::final_mark_gross);
          VMThread::execute(&finishMark);
          heap->shenandoahPolicy()->record_phase_end(ShenandoahCollectorPolicy::final_mark_gross);
        }

        if (! _should_terminate) {
          // GCTraceTime time("Concurrent evacuation ", ShenandoahLogInfo, true, gc_timer, gc_tracer->gc_id());
          TraceCollectorStats tcs(heap->monitoring_support()->concurrent_collection_counters());
          heap->do_evacuation();
        }

        if (heap->is_evacuation_in_progress()) {
          MutexLocker mu(Threads_lock);
          heap->set_evacuation_in_progress(false);
        }
        heap->shenandoahPolicy()->record_phase_start(ShenandoahCollectorPolicy::reset_bitmaps);
        heap->reset_next_mark_bitmap(heap->conc_workers());
        heap->shenandoahPolicy()->record_phase_end(ShenandoahCollectorPolicy::reset_bitmaps);

        gc_timer->register_gc_end();
      } else {
      Thread::current()->_ParkEvent->park(10) ;
      // yield();
    }

    // Make sure the _do_full_gc flag changes are seen.
    OrderAccess::storeload();
  }
  terminate();
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

void ShenandoahConcurrentThread::do_full_gc(GCCause::Cause cause) {

  assert(Thread::current()->is_Java_thread(), "expect Java thread here");

  MonitorLockerEx ml(&_full_gc_lock);
  schedule_full_gc();
  _full_gc_cause = cause;
  while (_do_full_gc) {
    ml.wait();
    OrderAccess::storeload();
  }
  assert(!_do_full_gc, "expect full GC to have completed");
}

void ShenandoahConcurrentThread::schedule_full_gc() {
  _do_full_gc = true;
}

void ShenandoahConcurrentThread::print() const {
  print_on(tty);
}

void ShenandoahConcurrentThread::print_on(outputStream* st) const {
  st->print("Shenandoah Concurrent Thread");
  Thread::print_on(st);
  st->cr();
}

void ShenandoahConcurrentThread::sleepBeforeNextCycle() {
  assert(false, "Wake up in the GC thread that never sleeps :-)");
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
