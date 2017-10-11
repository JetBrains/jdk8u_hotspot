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
#include "gc_implementation/shared/vmGCOperations.hpp"
#include "gc_implementation/shenandoah/shenandoahGCTraceTime.hpp"
#include "gc_implementation/shenandoah/shenandoahPhaseTimings.hpp"
#include "gc_implementation/shenandoah/shenandoahMarkCompact.hpp"
#include "gc_implementation/shenandoah/shenandoahConcurrentMark.inline.hpp"
#include "gc_implementation/shenandoah/shenandoahHeap.inline.hpp"
#include "gc_implementation/shenandoah/shenandoahUtils.hpp"
#include "gc_implementation/shenandoah/shenandoahVerifier.hpp"
#include "gc_implementation/shenandoah/shenandoahWorkGroup.hpp"
#include "gc_implementation/shenandoah/shenandoahWorkerPolicy.hpp"
#include "gc_implementation/shenandoah/vm_operations_shenandoah.hpp"

void VM_ShenandoahInitMark::doit() {
  ShenandoahGCPauseMark mark(ShenandoahPhaseTimings::init_mark, SvcGCMarker::OTHER);
  ShenandoahHeap* sh = ShenandoahHeap::heap();

  GCTraceTime time("Pause Init Mark", PrintGC, sh->gc_timer(), sh->tracer()->gc_id());

  ShenandoahWorkGang*       workers = sh->workers();

  // Calculate workers for initial marking
  uint nworkers = ShenandoahWorkerPolicy::calc_workers_for_init_marking();

  ShenandoahWorkerScope scope(workers, nworkers);

  assert(sh->is_next_bitmap_clear(), "need clear marking bitmap");

  sh->start_concurrent_marking();
}

void VM_ShenandoahFullGC::doit() {
  ShenandoahGCPauseMark mark(ShenandoahPhaseTimings::full_gc, SvcGCMarker::FULL);
  ShenandoahMarkCompact::do_mark_compact(_gc_cause);
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
  ShenandoahGCPauseMark mark(ShenandoahPhaseTimings::final_mark, SvcGCMarker::OTHER);

  ShenandoahHeap *sh = ShenandoahHeap::heap();

  // It is critical that we
  // evacuate roots right after finishing marking, so that we don't
  // get unmarked objects in the roots.
  // Setup workers for final marking
  ShenandoahWorkGang* workers = sh->workers();
  uint n_workers = ShenandoahWorkerPolicy::calc_workers_for_final_marking();
  ShenandoahWorkerScope scope(workers, n_workers);

  if (! sh->cancelled_concgc()) {
    GCTraceTime time("Pause Final Mark", PrintGC, sh->gc_timer(), sh->tracer()->gc_id());
    sh->concurrentMark()->finish_mark_from_roots();
    sh->stop_concurrent_marking();

    {
      ShenandoahGCPhase prepare_evac(ShenandoahPhaseTimings::prepare_evac);
      sh->prepare_for_concurrent_evacuation();
    }

    // If collection set has candidates, start evacuation.
    // Otherwise, bypass the rest of the cycle.
    if (!sh->collection_set()->is_empty()) {
      sh->set_evacuation_in_progress_at_safepoint(true);
      // From here on, we need to update references.
      sh->set_need_update_refs(true);

      ShenandoahGCPhase init_evac(ShenandoahPhaseTimings::init_evac);
      sh->evacuate_and_update_roots();
    }
  } else {
    GCTraceTime time("Cancel concurrent mark", PrintGC, sh->gc_timer(), sh->tracer()->gc_id());
    sh->concurrentMark()->cancel();
    sh->stop_concurrent_marking();
  }
}

void VM_ShenandoahInitUpdateRefs::doit() {
  ShenandoahGCPauseMark mark(ShenandoahPhaseTimings::init_update_refs, SvcGCMarker::OTHER);

  ShenandoahHeap *sh = ShenandoahHeap::heap();
  GCTraceTime time("Pause Init Update Refs", PrintGC, sh->gc_timer(), sh->tracer()->gc_id());

  sh->prepare_update_refs();
}

void VM_ShenandoahFinalUpdateRefs::doit() {
  ShenandoahGCPauseMark mark(ShenandoahPhaseTimings::final_update_refs, SvcGCMarker::OTHER);

  ShenandoahHeap *sh = ShenandoahHeap::heap();
  GCTraceTime time("Pause Final Update Refs", PrintGC, sh->gc_timer(), sh->tracer()->gc_id());

  ShenandoahWorkGang* workers = sh->workers();
  uint n_workers = ShenandoahWorkerPolicy::calc_workers_for_final_update_ref();
  ShenandoahWorkerScope scope(workers, n_workers);

  sh->finish_update_refs();
}

void VM_ShenandoahVerifyHeapAfterEvacuation::doit() {
  ShenandoahGCPauseMark mark(ShenandoahPhaseTimings::pause_other, SvcGCMarker::OTHER);
  ShenandoahHeap::heap()->verifier()->verify_after_evacuation();
}
