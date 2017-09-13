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
#include "gc_interface/gcCause.hpp"
#include "memory/resourceArea.hpp"

// For now we just want to have a concurrent marking thread.
// Once we have that working we will build a concurrent evacuation thread.

class ShenandoahConcurrentThread: public ConcurrentGCThread {
  friend class VMStructs;

private:
  // While we could have a single lock for these, it may risk unblocking
  // full GC waiters when concurrent cycle finishes.
  Monitor _full_gc_lock;
  Monitor _conc_gc_lock;

 private:
  static SurrogateLockerThread* _slt;

public:
  void run();
  void stop();

private:
  volatile jbyte _do_concurrent_gc;
  volatile jbyte _do_full_gc;
  volatile jbyte _graceful_shutdown;
  GCCause::Cause _full_gc_cause;

  bool check_cancellation();
  void service_normal_cycle();
  void service_fullgc_cycle();

public:
  // Constructor
  ShenandoahConcurrentThread();
  ~ShenandoahConcurrentThread();

  static void makeSurrogateLockerThread(TRAPS);
  static SurrogateLockerThread* slt() { return _slt; }

  // Printing
  void print_on(outputStream* st) const;
  void print() const;

  void do_conc_gc();
  void do_full_gc(GCCause::Cause cause);

  bool try_set_full_gc();
  void reset_full_gc();
  bool is_full_gc();

  bool is_conc_gc_requested();
  void reset_conc_gc_requested();

  char* name() const { return (char*)"ShenandoahConcurrentThread";}
  void start();

  void prepare_for_graceful_shutdown();
  bool in_graceful_shutdown();
};

#endif // SHARE_VM_GC_SHENANDOAH_SHENANDOAHCONCURRENTTHREAD_HPP
