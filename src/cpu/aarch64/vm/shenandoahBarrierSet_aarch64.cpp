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

#include "gc_implementation/shenandoah/brooksPointer.hpp"
#include "gc_implementation/shenandoah/shenandoahBarrierSet.inline.hpp"

#include "asm/macroAssembler.hpp"
#include "interpreter/interpreter.hpp"

#define __ masm->

#ifndef CC_INTERP
void ShenandoahBarrierSet::compile_resolve_oop_runtime(MacroAssembler* masm, Register dst) {
  __ enter();
  __ push_call_clobbered_registers();

  __ mov(c_rarg0, dst);
  __ mov(rscratch1, CAST_FROM_FN_PTR(address, ShenandoahBarrierSet::resolve_oop_static));
  __ blrt(rscratch1, 1, 0, 1);
  __ mov(rscratch1, r0);

  __ pop_call_clobbered_registers();
  __ mov(dst, rscratch1);
  __ leave();
}

void ShenandoahBarrierSet::interpreter_read_barrier(MacroAssembler* masm, Register dst) {
  if (ShenandoahReadBarrier) {
    Label is_null;
    __ cbz(dst, is_null);
    interpreter_read_barrier_not_null(masm, dst);
    __ bind(is_null);
  }
}

void ShenandoahBarrierSet::interpreter_read_barrier_not_null(MacroAssembler* masm, Register dst) {
  if (ShenandoahReadBarrier) {
    if (ShenandoahVerifyReadsToFromSpace) {
      compile_resolve_oop_runtime(masm, dst);
      return;
    }
    __ ldr(dst, Address(dst, BrooksPointer::byte_offset()));
  }
}

void ShenandoahBarrierSet::interpreter_write_barrier(MacroAssembler* masm, Register dst) {

  if (! ShenandoahWriteBarrier) {
    return interpreter_read_barrier(masm, dst);
  }

  __ shenandoah_write_barrier(dst);
}

void ShenandoahHeap::compile_prepare_oop(MacroAssembler* masm, Register obj) {
  __ add(obj, obj, BrooksPointer::byte_size());
  __ str(obj, Address(obj, -1 * HeapWordSize));
}

void ShenandoahBarrierSet::asm_acmp_barrier(MacroAssembler* masm,
                                            Register op1, Register op2) {
  Label done;
  __ br(Assembler::EQ, done);
  // The object may have been evacuated, but we won't see it without a
  // membar here.
  __ membar(Assembler::LoadStore|Assembler::LoadLoad);
  interpreter_read_barrier(masm, op1);
  interpreter_read_barrier(masm, op2);
  __ cmp(op1, op2);
  __ bind(done);
}

#endif
