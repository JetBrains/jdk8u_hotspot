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

#include "gc_implementation/shenandoah/shenandoahMarkCompact.hpp"
#include "gc_implementation/shenandoah/vm_operations_shenandoah.hpp"
#include "gc_implementation/shenandoah/shenandoahHeap.inline.hpp"

VM_Operation::VMOp_Type VM_ShenandoahInitMark::type() const {
  return VMOp_ShenandoahInitMark;
}

const char* VM_ShenandoahInitMark::name() const {
  return "Shenandoah Initial Marking";
}

void VM_ShenandoahInitMark::doit() {
  ShenandoahHeap *sh = (ShenandoahHeap*) Universe::heap();
  sh->shenandoahPolicy()->record_phase_start(ShenandoahCollectorPolicy::init_mark);

  assert(sh->is_bitmap_clear(), "need clear marking bitmap");

  if (ShenandoahGCVerbose)
    tty->print("vm_ShenandoahInitMark\n");
  sh->start_concurrent_marking();
  if (UseTLAB) {
    sh->shenandoahPolicy()->record_phase_start(ShenandoahCollectorPolicy::resize_tlabs);
    sh->resize_all_tlabs();
    sh->shenandoahPolicy()->record_phase_end(ShenandoahCollectorPolicy::resize_tlabs);
  }

  sh->shenandoahPolicy()->record_phase_end(ShenandoahCollectorPolicy::init_mark);

}

VM_Operation::VMOp_Type VM_ShenandoahFullGC::type() const {
  return VMOp_ShenandoahFullGC;
}

void VM_ShenandoahFullGC::doit() {

  ShenandoahMarkCompact::do_mark_compact();
  ShenandoahHeap *sh = ShenandoahHeap::heap();
  if (UseTLAB) {
    sh->shenandoahPolicy()->record_phase_start(ShenandoahCollectorPolicy::resize_tlabs);
    sh->resize_all_tlabs();
    sh->shenandoahPolicy()->record_phase_end(ShenandoahCollectorPolicy::resize_tlabs);
  }
}

const char* VM_ShenandoahFullGC::name() const {
  return "Shenandoah Full GC";
}


bool VM_ShenandoahReferenceOperation::doit_prologue() {
  ShenandoahHeap *sh = (ShenandoahHeap*) Universe::heap();
  sh->acquire_pending_refs_lock();
  return true;
}

void VM_ShenandoahReferenceOperation::doit_epilogue() {
  ShenandoahHeap *sh = ShenandoahHeap::heap();
  sh->release_pending_refs_lock();
}

void VM_ShenandoahStartEvacuation::doit() {

  // We need to do the finish mark here, so that a JNI critical region
  // can't divide it from evacuation start. It is critical that we
  // evacuate roots right after finishing marking, so that we don't
  // get unmarked objects in the roots.
  ShenandoahHeap *sh = ShenandoahHeap::heap();
  if (! sh->cancelled_concgc()) {
    if (ShenandoahGCVerbose)
      tty->print("vm_ShenandoahFinalMark\n");

    sh->shenandoahPolicy()->record_phase_start(ShenandoahCollectorPolicy::final_mark);
    sh->concurrentMark()->finish_mark_from_roots();
    sh->stop_concurrent_marking();
    sh->shenandoahPolicy()->record_phase_end(ShenandoahCollectorPolicy::final_mark);

    sh->shenandoahPolicy()->record_phase_start(ShenandoahCollectorPolicy::prepare_evac);
    sh->prepare_for_concurrent_evacuation();
    sh->shenandoahPolicy()->record_phase_end(ShenandoahCollectorPolicy::prepare_evac);

    sh->set_evacuation_in_progress(true);

    // From here on, we need to update references.
    sh->set_need_update_refs(true);

    sh->shenandoahPolicy()->record_phase_start(ShenandoahCollectorPolicy::init_evac);
    sh->evacuate_and_update_roots();
    sh->shenandoahPolicy()->record_phase_end(ShenandoahCollectorPolicy::init_evac);

  } else {
    sh->concurrentMark()->cancel();
    sh->stop_concurrent_marking();
    sh->recycle_dirty_regions();
  }
}

VM_Operation::VMOp_Type VM_ShenandoahStartEvacuation::type() const {
  return VMOp_ShenandoahStartEvacuation;
}

const char* VM_ShenandoahStartEvacuation::name() const {
  return "Start shenandoah evacuation";
}

VM_Operation::VMOp_Type VM_ShenandoahVerifyHeapAfterEvacuation::type() const {
  return VMOp_ShenandoahVerifyHeapAfterEvacuation;
}

const char* VM_ShenandoahVerifyHeapAfterEvacuation::name() const {
  return "Shenandoah verify heap after evacuation";
}

void VM_ShenandoahVerifyHeapAfterEvacuation::doit() {

  ShenandoahHeap *sh = ShenandoahHeap::heap();
  sh->verify_heap_after_evacuation();

}
