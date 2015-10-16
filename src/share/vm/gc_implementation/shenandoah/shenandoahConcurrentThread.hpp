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

#ifndef SHARE_VM_GC_SHENANDOAH_SHENANDOAHCONCURRENTTHREAD_HPP
#define SHARE_VM_GC_SHENANDOAH_SHENANDOAHCONCURRENTTHREAD_HPP

#include "gc_implementation/shared/concurrentGCThread.hpp"
#include "gc_implementation/shared/suspendibleThreadSet.hpp"
#include "gc_interface/gcCause.hpp"
#include "memory/resourceArea.hpp"

class ShenandoahConcurrentThread: public ConcurrentGCThread {
  friend class VMStructs;

 public:
  virtual void run();

 private:
  volatile bool                    _concurrent_mark_started;
  volatile bool                    _concurrent_mark_in_progress;
  volatile bool                    _concurrent_mark_aborted;

  int _epoch;

  static SurrogateLockerThread* _slt;
  static SuspendibleThreadSet _sts;

  bool _do_full_gc;
  GCCause::Cause _full_gc_cause;

  void sleepBeforeNextCycle();

 public:
  // Constructor
  ShenandoahConcurrentThread();
  ~ShenandoahConcurrentThread();

  static void makeSurrogateLockerThread(TRAPS);
  static SurrogateLockerThread* slt() { return _slt; }

  // Printing
  void print_on(outputStream* st) const;
  void print() const;

  void set_cm_started();
  void clear_cm_started();
  bool cm_started();

  void set_cm_in_progress();
  void clear_cm_in_progress();
  bool cm_in_progress();

  void cm_abort() { _concurrent_mark_aborted = true;}
  bool cm_has_aborted() { return _concurrent_mark_aborted;}
  void clear_cm_aborted() { _concurrent_mark_aborted = false;}

  void do_full_gc(GCCause::Cause cause);

  void schedule_full_gc();

  // This flag returns true from the moment a marking cycle is
  // initiated (during the initial-mark pause when started() is set)
  // to the moment when the cycle completes (just after the next
  // marking bitmap has been cleared and in_progress() is
  // cleared). While this flag is true we will not start another cycle
  // so that cycles do not overlap. We cannot use just in_progress()
  // as the CM thread might take some time to wake up before noticing
  // that started() is set and set in_progress().
  bool during_cycle()      { return cm_started() || cm_in_progress(); }

  char* name() const { return (char*)"ShenandoahConcurrentThread";}
  void start();
  void yield();

  static void safepoint_synchronize();
  static void safepoint_desynchronize();

  void shutdown();
};

#endif // SHARE_VM_GC_SHENANDOAH_SHENANDOAHCONCURRENTTHREAD_HPP
