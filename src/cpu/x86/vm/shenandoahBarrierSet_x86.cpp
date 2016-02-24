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

  __ push(rscratch1);

  if (dst != rax) {
    __ push(rax);
  }
  if (dst != rbx) {
    __ push(rbx);
  }
  if (dst != rcx) {
    __ push(rcx);
  }
  if (dst != rdx) {
    __ push(rdx);
  }
  if (dst != rdi) {
    __ push(rdi);
  }
  if (dst != rsi) {
    __ push(rsi);
  }
  if (dst != rbp) {
    __ push(rbp);
  }
  if (dst != r8) {
    __ push(r8);
  }
  if (dst != r9) {
    __ push(r9);
  }
  if (dst != r11) {
    __ push(r11);
  }
  if (dst != r12) {
    __ push(r12);
  }
  if (dst != r13) {
    __ push(r13);
  }
  if (dst != r14) {
    __ push(r14);
  }
  if (dst != r15) {
    __ push(r15);
  }

  __ subptr(rsp, 128);
  __ movdbl(Address(rsp, 0), xmm0);
  __ movdbl(Address(rsp, 8), xmm1);
  __ movdbl(Address(rsp, 16), xmm2);
  __ movdbl(Address(rsp, 24), xmm3);
  __ movdbl(Address(rsp, 32), xmm4);
  __ movdbl(Address(rsp, 40), xmm5);
  __ movdbl(Address(rsp, 48), xmm6);
  __ movdbl(Address(rsp, 56), xmm7);
  __ movdbl(Address(rsp, 64), xmm8);
  __ movdbl(Address(rsp, 72), xmm9);
  __ movdbl(Address(rsp, 80), xmm10);
  __ movdbl(Address(rsp, 88), xmm11);
  __ movdbl(Address(rsp, 96), xmm12);
  __ movdbl(Address(rsp, 104), xmm13);
  __ movdbl(Address(rsp, 112), xmm14);
  __ movdbl(Address(rsp, 120), xmm15);

  __ mov(c_rarg1, dst);
  __ super_call_VM_leaf(CAST_FROM_FN_PTR(address, ShenandoahBarrierSet::resolve_oop_static), c_rarg1);
  __ mov(rscratch1, rax);

  __ movdbl(xmm0, Address(rsp, 0));
  __ movdbl(xmm1, Address(rsp, 8));
  __ movdbl(xmm2, Address(rsp, 16));
  __ movdbl(xmm3, Address(rsp, 24));
  __ movdbl(xmm4, Address(rsp, 32));
  __ movdbl(xmm5, Address(rsp, 40));
  __ movdbl(xmm6, Address(rsp, 48));
  __ movdbl(xmm7, Address(rsp, 56));
  __ movdbl(xmm8, Address(rsp, 64));
  __ movdbl(xmm9, Address(rsp, 72));
  __ movdbl(xmm10, Address(rsp, 80));
  __ movdbl(xmm11, Address(rsp, 88));
  __ movdbl(xmm12, Address(rsp, 96));
  __ movdbl(xmm13, Address(rsp, 104));
  __ movdbl(xmm14, Address(rsp, 112));
  __ movdbl(xmm15, Address(rsp, 120));
  __ addptr(rsp, 128);

  if (dst != r15) {
    __ pop(r15);
  }
  if (dst != r14) {
    __ pop(r14);
  }
  if (dst != r13) {
    __ pop(r13);
  }
  if (dst != r12) {
    __ pop(r12);
  }
  if (dst != r11) {
    __ pop(r11);
  }
  if (dst != r9) {
    __ pop(r9);
  }
  if (dst != r8) {
    __ pop(r8);
  }
  if (dst != rbp) {
    __ pop(rbp);
  }
  if (dst != rsi) {
    __ pop(rsi);
  }
  if (dst != rdi) {
    __ pop(rdi);
  }
  if (dst != rdx) {
    __ pop(rdx);
  }
  if (dst != rcx) {
    __ pop(rcx);
  }
  if (dst != rbx) {
    __ pop(rbx);
  }
  if (dst != rax) {
    __ pop(rax);
  }

  __ mov(dst, rscratch1);

  __ pop(rscratch1);
}

void ShenandoahBarrierSet::interpreter_read_barrier(MacroAssembler* masm, Register dst) {
  if (ShenandoahReadBarrier) {

    Label is_null;
    __ testptr(dst, dst);
    __ jcc(Assembler::zero, is_null);
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
    __ movptr(dst, Address(dst, BrooksPointer::BYTE_OFFSET));
  }
}

void ShenandoahBarrierSet::interpreter_write_barrier(MacroAssembler* masm, Register dst) {

  if (! ShenandoahWriteBarrier) {
    return interpreter_read_barrier(masm, dst);
  }

  assert(dst != rscratch1, "different regs");
  //assert(dst != rscratch2, "Need rscratch2");

  Label done;

  Address evacuation_in_progress = Address(r15_thread, in_bytes(JavaThread::evacuation_in_progress_offset()));

  __ cmpb(evacuation_in_progress, 0);

  // Now check if evacuation is in progress.
  interpreter_read_barrier_not_null(masm, dst);

  __ jcc(Assembler::equal, done);
  __ push(rscratch1);
  __ push(rscratch2);

  __ movptr(rscratch1, dst);
  __ shrptr(rscratch1, ShenandoahHeapRegion::RegionSizeShift);
  __ movptr(rscratch2, (intptr_t) ShenandoahHeap::in_cset_fast_test_addr());
  __ movbool(rscratch2, Address(rscratch2, rscratch1, Address::times_1));
  __ testb(rscratch2, 0x1);

  __ pop(rscratch2);
  __ pop(rscratch1);

  __ jcc(Assembler::zero, done);

  __ push(rscratch1);

  // Save possibly live regs.
  if (dst != rax) {
    __ push(rax);
  }
  if (dst != rbx) {
    __ push(rbx);
  }
  if (dst != rcx) {
    __ push(rcx);
  }
  if (dst != rdx) {
    __ push(rdx);
  }
  if (dst != c_rarg1) {
    __ push(c_rarg1);
  }

  __ subptr(rsp, 2 * wordSize);
  __ movdbl(Address(rsp, 0), xmm0);

  // Call into runtime
  __ super_call_VM_leaf(CAST_FROM_FN_PTR(address, ShenandoahBarrierSet::write_barrier_interp), dst);
  __ mov(rscratch1, rax);

  // Restore possibly live regs.
  __ movdbl(xmm0, Address(rsp, 0));
  __ addptr(rsp, 2 * Interpreter::stackElementSize);

  if (dst != c_rarg1) {
    __ pop(c_rarg1);
  }
  if (dst != rdx) {
    __ pop(rdx);
  }
  if (dst != rcx) {
    __ pop(rcx);
  }
  if (dst != rbx) {
    __ pop(rbx);
  }
  if (dst != rax) {
    __ pop(rax);
  }

  // Move result into dst reg.
  __ mov(dst, rscratch1);

  __ pop(rscratch1);

  __ bind(done);
}

void ShenandoahBarrierSet::asm_acmp_barrier(MacroAssembler* masm, Register op1, Register op2) {
  Label done;
  __ jccb(Assembler::equal, done);
  interpreter_read_barrier(masm, op1);
  interpreter_read_barrier(masm, op2);
  __ cmpptr(op1, op2);
  __ bind(done);
}

void ShenandoahHeap::compile_prepare_oop(MacroAssembler* masm, Register obj) {
  __ incrementq(obj, BrooksPointer::BROOKS_POINTER_OBJ_SIZE * HeapWordSize);
  __ movptr(Address(obj, -1 * HeapWordSize), obj);
}
#endif
