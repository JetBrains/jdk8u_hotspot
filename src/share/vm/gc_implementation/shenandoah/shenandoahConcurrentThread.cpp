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
#include "memory/iterator.hpp"
#include "memory/universe.hpp"
#include "runtime/vmThread.hpp"

SurrogateLockerThread* ShenandoahConcurrentThread::_slt = NULL;

ShenandoahConcurrentThread::ShenandoahConcurrentThread() :
  ConcurrentGCThread(),
  _epoch(0),
  _concurrent_mark_started(false),
  _concurrent_mark_in_progress(false),
  _do_full_gc(false),
  _concurrent_mark_aborted(false)
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

        VM_ShenandoahFullGC full_gc;
        heap->jni_critical()->execute_in_vm_thread(&full_gc);
      }
      MonitorLockerEx ml(ShenandoahFullGC_lock);
      _do_full_gc = false;
      ml.notify_all();
    } else if (heap->shenandoahPolicy()->should_start_concurrent_mark(heap->used(),
                                                               heap->capacity()))
      {

        if (ShenandoahGCVerbose)
          tty->print("Capacity = "SIZE_FORMAT" Used = "SIZE_FORMAT"  doing initMark\n", heap->capacity(), heap->used());

        if (ShenandoahGCVerbose) tty->print("Starting a mark");

        VM_ShenandoahInitMark initMark;
        VMThread::execute(&initMark);

        if (ShenandoahConcurrentMarking) {
          ShenandoahHeap::heap()->concurrentMark()->mark_from_roots();

          VM_ShenandoahStartEvacuation finishMark;
          heap->jni_critical()->execute_in_vm_thread(&finishMark);
        }

        if (cm_has_aborted()) {
          clear_cm_aborted();
          assert(heap->is_bitmap_clear(), "need to continue with clear mark bitmap");
          assert(! heap->concurrent_mark_in_progress(), "concurrent mark must have terminated");
          continue;
        }
        if (! _should_terminate) {
          // If we're not concurrently evacuating, evacuation is done
          // from VM_ShenandoahFinishMark within the VMThread above.
          if (ShenandoahConcurrentEvacuation) {
            VM_ShenandoahEvacuation evacuation;
            evacuation.doit();
          }
        }

        if (heap->shenandoahPolicy()->update_refs_early() && ! _should_terminate && ! heap->cancelled_concgc()) {
          if (ShenandoahConcurrentUpdateRefs) {
            VM_ShenandoahUpdateRefs update_refs;
            VMThread::execute(&update_refs);
            heap->update_references();
          }
        } else {
          if (heap->is_evacuation_in_progress()) {
            heap->set_evacuation_in_progress(false);
          }
          heap->shenandoahPolicy()->record_phase_start(ShenandoahCollectorPolicy::reset_bitmaps);
          heap->reset_mark_bitmap();
          heap->shenandoahPolicy()->record_phase_end(ShenandoahCollectorPolicy::reset_bitmaps);
        }

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

void ShenandoahConcurrentThread::set_cm_started() {
    assert(!_concurrent_mark_in_progress, "cycle in progress");
    _concurrent_mark_started = true;
}

void ShenandoahConcurrentThread::clear_cm_started() {
    assert(_concurrent_mark_in_progress, "must be starting a cycle");
    _concurrent_mark_started = false;
}

bool ShenandoahConcurrentThread::cm_started() {
  return _concurrent_mark_started;
}

void ShenandoahConcurrentThread::set_cm_in_progress() {
  assert(_concurrent_mark_started, "must be starting a cycle");
  _concurrent_mark_in_progress = true;
}

void ShenandoahConcurrentThread::clear_cm_in_progress() {
  assert(!_concurrent_mark_started, "must not be starting a new cycle");
  _concurrent_mark_in_progress = false;
}

bool ShenandoahConcurrentThread::cm_in_progress() {
  return _concurrent_mark_in_progress;
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
