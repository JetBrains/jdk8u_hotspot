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

#ifndef SHARE_VM_GC_SHENANDOAH_VM_OPERATIONS_SHENANDOAH_HPP
#define SHARE_VM_GC_SHENANDOAH_VM_OPERATIONS_SHENANDOAH_HPP

#include "gc_implementation/shenandoah/shenandoahConcurrentMark.hpp"
#include "gc_implementation/shared/vmGCOperations.hpp"

// VM_operations for the Shenandoah Collector.
//
// VM_ShenandoahOperation
//   - VM_ShenandoahInitMark: initiate concurrent marking
//   - VM_ShenandoahReferenceOperation:
//       - VM_ShenandoahStartEvacuation: finish up concurrent marking, and start evacuation
//       - VM_ShenandoahFullGC: do full GC

class VM_ShenandoahOperation : public VM_Operation {
public:
  VM_ShenandoahOperation() {};
};

class VM_ShenandoahReferenceOperation : public VM_ShenandoahOperation {
private:
  BasicLock _pending_list_basic_lock;
public:
  VM_ShenandoahReferenceOperation() : VM_ShenandoahOperation() {};
  bool doit_prologue();
  void doit_epilogue();
};

class VM_ShenandoahInitMark: public VM_ShenandoahOperation {
public:
  VM_ShenandoahInitMark() : VM_ShenandoahOperation() {};
  VM_Operation::VMOp_Type type() const { return VMOp_ShenandoahInitMark; }
  const char* name()             const { return "Shenandoah Initial Marking"; }
  virtual void doit();
};

class VM_ShenandoahStartEvacuation: public VM_ShenandoahReferenceOperation {
public:
  VM_ShenandoahStartEvacuation() : VM_ShenandoahReferenceOperation() {};
  VM_Operation::VMOp_Type type() const { return VMOp_ShenandoahStartEvacuation; }
  const char* name()             const { return "Start Shenandoah evacuation"; }
  virtual  void doit();
};

class VM_ShenandoahFullGC : public VM_ShenandoahReferenceOperation {
private:
  GCCause::Cause _gc_cause;
public:
  VM_ShenandoahFullGC(GCCause::Cause gc_cause) : VM_ShenandoahReferenceOperation(), _gc_cause(gc_cause) {};
  VM_Operation::VMOp_Type type() const { return VMOp_ShenandoahFullGC; }
  const char* name()             const { return "Shenandoah Full GC"; }
  virtual void doit();
};

class VM_ShenandoahVerifyHeapAfterEvacuation: public VM_ShenandoahOperation {
public:
  VM_ShenandoahVerifyHeapAfterEvacuation() : VM_ShenandoahOperation() {};
  VM_Operation::VMOp_Type type() const { return VMOp_ShenandoahVerifyHeapAfterEvacuation; }
  const char* name()             const { return "Shenandoah verify heap after evacuation"; }
  virtual void doit();
};

#endif //SHARE_VM_GC_SHENANDOAH_VM_OPERATIONS_SHENANDOAH_HPP
