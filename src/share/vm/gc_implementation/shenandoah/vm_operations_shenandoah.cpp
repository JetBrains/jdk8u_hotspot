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

void VM_ShenandoahInitMark::doit() {
  ShenandoahGCPauseMark mark(SvcGCMarker::OTHER);
  ShenandoahHeap::heap()->entry_init_mark();
}

void VM_ShenandoahFinalMarkStartEvac::doit() {
  ShenandoahGCPauseMark mark(SvcGCMarker::OTHER);
  ShenandoahHeap::heap()->entry_final_mark();
}

void VM_ShenandoahFullGC::doit() {
  ShenandoahGCPauseMark mark(SvcGCMarker::FULL);
  ShenandoahHeap::heap()->entry_full(_gc_cause);
}

void VM_ShenandoahInitUpdateRefs::doit() {
  ShenandoahGCPauseMark mark(SvcGCMarker::OTHER);
  ShenandoahHeap::heap()->entry_init_updaterefs();
}

void VM_ShenandoahFinalUpdateRefs::doit() {
  ShenandoahGCPauseMark mark(SvcGCMarker::OTHER);
  ShenandoahHeap::heap()->entry_final_updaterefs();
}

void VM_ShenandoahVerifyHeapAfterEvacuation::doit() {
  ShenandoahGCPauseMark mark(SvcGCMarker::OTHER);
  ShenandoahHeap::heap()->entry_verify_after_evac();
}
