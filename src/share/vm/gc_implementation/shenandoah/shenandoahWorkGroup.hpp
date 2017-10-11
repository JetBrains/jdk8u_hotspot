/*
 * Copyright (c) 2017, Red Hat, Inc. and/or its affiliates.
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

#ifndef SHARE_VM_GC_SHENANDOAH_SHENANDOAH_WORKGROUP_HPP
#define SHARE_VM_GC_SHENANDOAH_SHENANDOAH_WORKGROUP_HPP
 
#include "utilities/workgroup.hpp"
#include "memory/allocation.hpp"

class ShenandoahWorkGang;

class ShenandoahWorkerScope : public StackObj {
private:
  uint      _n_workers;
  ShenandoahWorkGang* _workers;
public:
  ShenandoahWorkerScope(ShenandoahWorkGang* workers, uint nworkers);
  ~ShenandoahWorkerScope();
};


class ShenandoahPushWorkerScope : StackObj {
private:
  uint      _n_workers;
  uint      _old_workers;
  ShenandoahWorkGang* _workers;

public:
  ShenandoahPushWorkerScope(ShenandoahWorkGang* workers, uint nworkers, bool do_check = true);
  ~ShenandoahPushWorkerScope();
};


class ShenandoahWorkGang : public FlexibleWorkGang {
public:
  ShenandoahWorkGang(const char* name, uint workers,
                   bool are_GC_task_threads,
                   bool are_ConcurrentGC_threads) :
    FlexibleWorkGang(name, workers, are_GC_task_threads, are_ConcurrentGC_threads) {
  }

  // Hide FlexibleWorkGang's implementation, avoid _active_workers == _total_workers
  // check
  void set_active_workers(uint v) {
    assert(v <= _total_workers,
           "Trying to set more workers active than there are");
    _active_workers = MIN2(v, _total_workers);
    assert(v != 0, "Trying to set active workers to 0");
    _active_workers = MAX2(1U, _active_workers);
  }
};

#endif // SHARE_VM_GC_SHENANDOAH_SHENANDOAHWORKGROUP_HPP
