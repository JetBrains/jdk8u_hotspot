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
// For now we are just doing two pauses.  The initial marking pause, and the final finish up marking and perform evacuation pause.
//    VM_ShenandoahInitMark
//    VM_ShenandoahFinishMark

class VM_ShenandoahInitMark: public VM_Operation {

public:
  virtual VMOp_Type type() const;
  virtual void doit();

  virtual const char* name() const;
};

class VM_ShenandoahReferenceOperation : public VM_Operation {
  bool doit_prologue();
  void doit_epilogue();

};

class VM_ShenandoahStartEvacuation: public VM_ShenandoahReferenceOperation {

 public:
  VMOp_Type type() const;
  void doit();
  const char* name() const;

};

class VM_ShenandoahFullGC : public VM_ShenandoahReferenceOperation {
 public:
  VMOp_Type type() const;
  void doit();
  const char* name() const;
};

class VM_ShenandoahVerifyHeapAfterEvacuation: public VM_Operation {

 public:
  virtual VMOp_Type type() const;
  virtual void doit();

  virtual const char* name() const;

};

#endif //SHARE_VM_GC_SHENANDOAH_VM_OPERATIONS_SHENANDOAH_HPP
