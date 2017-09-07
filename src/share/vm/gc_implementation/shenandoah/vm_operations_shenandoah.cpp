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
#include "gc_implementation/shenandoah/shenandoahGCTraceTime.hpp"
#include "gc_implementation/shenandoah/shenandoahCollectorPolicy.hpp"
#include "gc_implementation/shenandoah/shenandoahMarkCompact.hpp"
#include "gc_implementation/shenandoah/shenandoahConcurrentMark.inline.hpp"
#include "gc_implementation/shenandoah/shenandoahHeap.inline.hpp"
#include "gc_implementation/shenandoah/shenandoahUtils.hpp"
#include "gc_implementation/shenandoah/shenandoahVerifier.hpp"
#include "gc_implementation/shenandoah/shenandoahWorkGroup.hpp"
#include "gc_implementation/shenandoah/vm_operations_shenandoah.hpp"

void VM_ShenandoahInitMark::doit() {
  ShenandoahHeap* sh = ShenandoahHeap::heap();

  GCTraceTime time("Pause Init Mark", PrintGC, sh->gc_timer(), sh->tracer()->gc_id());
  ShenandoahGCPhase total_phase(ShenandoahCollectorPolicy::total_pause);
  ShenandoahGCPhase init_mark_phase(ShenandoahCollectorPolicy::init_mark);

  FlexibleWorkGang*       workers = sh->workers();

  // Calculate workers for initial marking
  uint nworkers = ShenandoahCollectorPolicy::calc_workers_for_init_marking(
    workers->active_workers(), (uint)Threads::number_of_non_daemon_threads());

  ShenandoahWorkerScope scope(workers, nworkers);

  assert(sh->is_next_bitmap_clear(), "need clear marking bitmap");

  sh->start_concurrent_marking();
}

void VM_ShenandoahFullGC::doit() {
  ShenandoahHeap *sh = ShenandoahHeap::heap();
  sh->shenandoahPolicy()->record_gc_start();
  ShenandoahMarkCompact::do_mark_compact(_gc_cause);
  sh->shenandoahPolicy()->record_gc_end();
}

bool VM_ShenandoahReferenceOperation::doit_prologue() {
  if (Thread::current()->is_Java_thread()) {
    InstanceRefKlass::acquire_pending_list_lock(&_pending_list_basic_lock);
  } else {
    ShenandoahHeap *sh = (ShenandoahHeap*) Universe::heap();
    sh->acquire_pending_refs_lock();
  }
  return true;
}

void VM_ShenandoahReferenceOperation::doit_epilogue() {
  if (Thread::current()->is_Java_thread()) {
    InstanceRefKlass::release_and_notify_pending_list_lock(&_pending_list_basic_lock);
  } else {
    ShenandoahHeap *sh = ShenandoahHeap::heap();
    sh->release_pending_refs_lock();
  }
}

void VM_ShenandoahFinalMarkStartEvac::doit() {

  ShenandoahHeap *sh = ShenandoahHeap::heap();
  ShenandoahGCPhase total(ShenandoahCollectorPolicy::total_pause);
  ShenandoahGCPhase final_mark(ShenandoahCollectorPolicy::final_mark);

  sh->shenandoahPolicy()->record_gc_start();

  // It is critical that we
  // evacuate roots right after finishing marking, so that we don't
  // get unmarked objects in the roots.
  // Setup workers for final marking
  FlexibleWorkGang* workers = sh->workers();
  uint n_workers = ShenandoahCollectorPolicy::calc_workers_for_final_marking(workers->active_workers(),
                                                                             (uint)Threads::number_of_non_daemon_threads());
  ShenandoahWorkerScope scope(workers, n_workers);

  if (! sh->cancelled_concgc()) {
    GCTraceTime time("Pause Final Mark", PrintGC, sh->gc_timer(), sh->tracer()->gc_id(), true);
    sh->concurrentMark()->finish_mark_from_roots();
    sh->stop_concurrent_marking();

    {
      ShenandoahGCPhase prepare_evac(ShenandoahCollectorPolicy::prepare_evac);
      sh->prepare_for_concurrent_evacuation();
    }

    // If collection set has candidates, start evacuation.
    // Otherwise, bypass the rest of the cycle.
    if (!sh->collection_set()->is_empty()) {
      sh->set_evacuation_in_progress_at_safepoint(true);
      // From here on, we need to update references.
      sh->set_need_update_refs(true);

      ShenandoahGCPhase init_evac(ShenandoahCollectorPolicy::init_evac);
      sh->evacuate_and_update_roots();
    }
  } else {
    GCTraceTime time("Cancel concurrent mark", PrintGC, sh->gc_timer(), sh->tracer()->gc_id());
    sh->concurrentMark()->cancel();
    sh->stop_concurrent_marking();
  }

  sh->shenandoahPolicy()->record_gc_end();
}

void VM_ShenandoahInitUpdateRefs::doit() {
  ShenandoahHeap *sh = ShenandoahHeap::heap();
  GCTraceTime time("Pause Init Update Refs", PrintGC, sh->gc_timer(), sh->tracer()->gc_id());
  ShenandoahGCPhase total(ShenandoahCollectorPolicy::total_pause);
  ShenandoahGCPhase init_update_refs(ShenandoahCollectorPolicy::init_update_refs);

  sh->prepare_update_refs();
}

void VM_ShenandoahFinalUpdateRefs::doit() {
  ShenandoahHeap *sh = ShenandoahHeap::heap();
  GCTraceTime time("Pause Final Update Refs", PrintGC, sh->gc_timer(), sh->tracer()->gc_id(), true);
  ShenandoahGCPhase total(ShenandoahCollectorPolicy::total_pause);
  ShenandoahGCPhase final_update_refs(ShenandoahCollectorPolicy::final_update_refs);

  sh->finish_update_refs();
}

void VM_ShenandoahVerifyHeapAfterEvacuation::doit() {
  ShenandoahHeap *sh = ShenandoahHeap::heap();
  sh->verifier()->verify_after_evacuation();
}
