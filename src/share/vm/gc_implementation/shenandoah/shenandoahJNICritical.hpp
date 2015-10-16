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

#ifndef SHARE_VM_GC_SHENANDOAH_SHENANDOAHJNICRITICAL_HPP
#define SHARE_VM_GC_SHENANDOAH_SHENANDOAHJNICRITICAL_HPP

#include "gc_implementation/shared/vmGCOperations.hpp"
#include "memory/allocation.hpp"

class ShenandoahJNICritical : public CHeapObj<mtGC> {
private:
  VM_Operation* _op_waiting_for_jni_critical;

public:
  ShenandoahJNICritical();
  void notify_jni_critical();
  void set_waiting_for_jni_before_gc(VM_Operation* op);
  void execute_in_vm_thread(VM_Operation* op);
};


#endif // SHARE_VM_GC_SHENANDOAH_SHENANDOAHJNICRITICAL_HPP
