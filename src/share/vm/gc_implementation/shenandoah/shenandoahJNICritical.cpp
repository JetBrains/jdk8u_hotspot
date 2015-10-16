/*
 * Copyright (c) 2015, Red Hat, Inc. and/or its affiliates.
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

#include "gc_implementation/shenandoah/shenandoahJNICritical.hpp"
#include "gc_implementation/shenandoah/shenandoahHeap.hpp"

#include "memory/gcLocker.hpp"
#include "runtime/mutexLocker.hpp"
#include "runtime/thread.hpp"
#include "runtime/vmThread.hpp"

class VM_ShenandoahJNICriticalOperation : public VM_Operation {
private:
  VM_Operation* _target;
public:
  VM_ShenandoahJNICriticalOperation(VM_Operation* target);
  VMOp_Type type() const;
  bool doit_prologue();
  void doit_epilogue();
  void doit();
  const char* name() const;
};

ShenandoahJNICritical::ShenandoahJNICritical() : _op_waiting_for_jni_critical(NULL) {
}

/*
 * This is called by the Java thread who leaves the last JNI critical block.
 */
void ShenandoahJNICritical::notify_jni_critical() {
  assert(Thread::current()->is_Java_thread(), "call only from Java thread");
  assert(_op_waiting_for_jni_critical != NULL, "must be waiting for jni critical notification");

  MonitorLockerEx ml(ShenandoahJNICritical_lock, true);

  VMThread::execute(_op_waiting_for_jni_critical);
  _op_waiting_for_jni_critical = NULL;

  ml.notify_all();

}

/*
 * This is called by the VM thread, if it determines that the task must wait
 * for JNI critical regions to be left.
 */
void ShenandoahJNICritical::set_waiting_for_jni_before_gc(VM_Operation* op) {
  assert(Thread::current()->is_VM_thread(), "call only from VM thread");
  _op_waiting_for_jni_critical = op;
}

/**
 * This is called by the Shenandoah concurrent thread in order
 * to execute a VM_Operation on the VM thread, that needs to perform
 * a JNI critical region check.
 */
void ShenandoahJNICritical::execute_in_vm_thread(VM_Operation* op) {
  MonitorLockerEx ml(ShenandoahJNICritical_lock, true);
  VM_ShenandoahJNICriticalOperation jni_op(op);
  VMThread::execute(&jni_op);
  while (_op_waiting_for_jni_critical != NULL) {
    ml.wait(true);
  }
}


VM_ShenandoahJNICriticalOperation::VM_ShenandoahJNICriticalOperation(VM_Operation* target)
  : _target(target) {
}

VM_Operation::VMOp_Type VM_ShenandoahJNICriticalOperation::type() const {
  return _target->type();
}

const char* VM_ShenandoahJNICriticalOperation::name() const {
  return _target->name();
}

bool VM_ShenandoahJNICriticalOperation::doit_prologue() {
  return _target->doit_prologue();
}

void VM_ShenandoahJNICriticalOperation::doit_epilogue() {
  _target->doit_epilogue();
}

void VM_ShenandoahJNICriticalOperation::doit() {
  if (! GC_locker::check_active_before_gc()) {
    _target->doit();
  } else {

    if (ShenandoahTraceJNICritical) {
      gclog_or_tty->print_cr("Deferring JNI critical op because of active JNI critical regions");
    }

    // This makes the GC background thread wait, and kick off evacuation as
    // soon as JNI notifies us that critical regions have all been left.
    ShenandoahHeap *sh = ShenandoahHeap::heap();
    sh->jni_critical()->set_waiting_for_jni_before_gc(this);
  }
}
