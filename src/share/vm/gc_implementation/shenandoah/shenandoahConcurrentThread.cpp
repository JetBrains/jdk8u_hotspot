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

#include "gc_implementation/shenandoah/shenandoahConcurrentThread.hpp"
#include "gc_implementation/shenandoah/shenandoahHeap.inline.hpp"
#include "gc_implementation/shenandoah/shenandoahJNICritical.hpp"
#include "gc_implementation/shenandoah/vm_operations_shenandoah.hpp"
#include "gc_implementation/shenandoah/shenandoahMonitoringSupport.hpp"
#include "memory/iterator.hpp"
#include "memory/universe.hpp"
#include "runtime/vmThread.hpp"

SurrogateLockerThread* ShenandoahConcurrentThread::_slt = NULL;

ShenandoahConcurrentThread::ShenandoahConcurrentThread() :
  ConcurrentGCThread(),
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

  while (!_should_terminate) {
    if (_do_full_gc) {
      {
        if (_full_gc_cause == GCCause::_allocation_failure) {
          heap->shenandoahPolicy()->record_allocation_failure_gc();
        } else {
          heap->shenandoahPolicy()->record_user_requested_gc();
        }

        TraceCollectorStats tcs(heap->monitoring_support()->full_collection_counters());
        TraceMemoryManagerStats tmms(true, _full_gc_cause);
        VM_ShenandoahFullGC full_gc;
        heap->jni_critical()->execute_in_vm_thread(&full_gc);
      }
      MonitorLockerEx ml(ShenandoahFullGC_lock);
      _do_full_gc = false;
      ml.notify_all();
    } else if (heap->shenandoahPolicy()->should_start_concurrent_mark(heap->used(),
                                                               heap->capacity()))
      {

        TraceCollectorStats tcs(heap->monitoring_support()->concurrent_collection_counters());
        TraceMemoryManagerStats tmms(false, GCCause::_no_cause_specified);
        if (ShenandoahGCVerbose)
          tty->print("Capacity = "SIZE_FORMAT" Used = "SIZE_FORMAT"  doing initMark\n", heap->capacity(), heap->used());

        if (ShenandoahGCVerbose) tty->print("Starting a mark");

        {
          TraceCollectorStats tcs(heap->monitoring_support()->stw_collection_counters());
          VM_ShenandoahInitMark initMark;
          heap->shenandoahPolicy()->record_phase_start(ShenandoahCollectorPolicy::init_mark_gross);
          VMThread::execute(&initMark);
          heap->shenandoahPolicy()->record_phase_end(ShenandoahCollectorPolicy::init_mark_gross);
        }
        {
          TraceCollectorStats tcs(heap->monitoring_support()->concurrent_collection_counters());
          ShenandoahHeap::heap()->concurrentMark()->mark_from_roots();
        }

        {
          TraceCollectorStats tcs(heap->monitoring_support()->stw_collection_counters());
          VM_ShenandoahStartEvacuation finishMark;
          heap->shenandoahPolicy()->record_phase_start(ShenandoahCollectorPolicy::final_mark_gross);
          heap->jni_critical()->execute_in_vm_thread(&finishMark);
          heap->shenandoahPolicy()->record_phase_end(ShenandoahCollectorPolicy::final_mark_gross);
        }

        if (! _should_terminate) {
          TraceCollectorStats tcs(heap->monitoring_support()->concurrent_collection_counters());
          heap->do_evacuation();
        }

        if (heap->is_evacuation_in_progress()) {
          heap->set_evacuation_in_progress(false);
        }
        heap->shenandoahPolicy()->record_phase_start(ShenandoahCollectorPolicy::reset_bitmaps);
        heap->reset_mark_bitmap();
        heap->shenandoahPolicy()->record_phase_end(ShenandoahCollectorPolicy::reset_bitmaps);

      } else {
      Thread::current()->_ParkEvent->park(10) ;
      // yield();
    }
    heap->clear_cancelled_concgc();
    // Make sure the _do_full_gc flag changes are seen.
    OrderAccess::storeload();
  }
}

void ShenandoahConcurrentThread::do_full_gc(GCCause::Cause cause) {

  assert(Thread::current()->is_Java_thread(), "expect Java thread here");

  MonitorLockerEx ml(ShenandoahFullGC_lock);
  schedule_full_gc();
  _full_gc_cause = cause;
  while (_do_full_gc) {
    ml.wait();
    OrderAccess::storeload();
  }
  assert(_do_full_gc == false, "expect full GC to have completed");
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

void ShenandoahConcurrentThread::yield() {
  _sts.yield();
}

void ShenandoahConcurrentThread::safepoint_synchronize() {
  assert(UseShenandoahGC, "just checking");
  _sts.synchronize();
}

void ShenandoahConcurrentThread::safepoint_desynchronize() {
  assert(UseShenandoahGC, "just checking");
  _sts.desynchronize();
}

void ShenandoahConcurrentThread::makeSurrogateLockerThread(TRAPS) {
  assert(UseShenandoahGC, "SLT thread needed only for concurrent GC");
  assert(THREAD->is_Java_thread(), "must be a Java thread");
  assert(_slt == NULL, "SLT already created");
  _slt = SurrogateLockerThread::make(THREAD);
}

void ShenandoahConcurrentThread::shutdown() {
  _should_terminate = true;
}
