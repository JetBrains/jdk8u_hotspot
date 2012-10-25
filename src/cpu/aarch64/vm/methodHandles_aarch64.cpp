/*
 * Copyright (c) 1997, 2012, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
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
#include "interpreter/interpreter.hpp"
#include "interpreter/interpreterRuntime.hpp"
#include "memory/allocation.inline.hpp"
#include "prims/methodHandles.hpp"

#define __ _masm->

#ifdef PRODUCT
#define BLOCK_COMMENT(str) /* nothing */
#else
#define BLOCK_COMMENT(str) __ block_comment(str)
#endif

#define BIND(label) bind(label); BLOCK_COMMENT(#label ":")

// Workaround for C++ overloading nastiness on '0' for RegisterOrConstant.
static RegisterOrConstant constant(int value) {
  return RegisterOrConstant(value);
}

void MethodHandles::load_klass_from_Class(MacroAssembler* _masm, Register klass_reg) { __ call_Unimplemented(); }

#ifdef ASSERT
static int check_nonzero(const char* xname, int x) {
  assert(x != 0, err_msg("%s should be nonzero", xname));
  return x;
}
#define NONZERO(x) check_nonzero(#x, x)
#else //ASSERT
#define NONZERO(x) (x)
#endif //PRODUCT

#ifdef ASSERT
void MethodHandles::verify_klass(MacroAssembler* _masm,
                                 Register obj, SystemDictionary::WKID klass_id,
                                 const char* error_message) { __ call_Unimplemented(); }

void MethodHandles::verify_ref_kind(MacroAssembler* _masm, int ref_kind, Register member_reg, Register temp) { __ call_Unimplemented(); }

#endif //ASSERT

void MethodHandles::jump_from_method_handle(MacroAssembler* _masm,
					    Register method, Register temp,
                                            bool for_compiler_entry) { __ call_Unimplemented(); }

void MethodHandles::jump_to_lambda_form(MacroAssembler* _masm,
                                        Register recv, Register method_temp,
                                        Register temp2,
                                        bool for_compiler_entry) { __ call_Unimplemented(); }


// Code generation
address MethodHandles::generate_method_handle_interpreter_entry(MacroAssembler* _masm,
                                                                vmIntrinsics::ID iid) { __ call_Unimplemented(); }

void MethodHandles::generate_method_handle_dispatch(MacroAssembler* _masm,
                                                    vmIntrinsics::ID iid,
                                                    Register receiver_reg,
                                                    Register member_reg,
                                                    bool for_compiler_entry) { __ call_Unimplemented(); }

#ifndef PRODUCT
void trace_method_handle_stub(const char* adaptername,
                              oop mh,
                              intptr_t* saved_regs,
                              intptr_t* entry_sp) {  }

// The stub wraps the arguments in a struct on the stack to avoid
// dealing with the different calling conventions for passing 6
// arguments.
struct MethodHandleStubArguments {
  const char* adaptername;
  oopDesc* mh;
  intptr_t* saved_regs;
  intptr_t* entry_sp;
};
void trace_method_handle_stub_wrapper(MethodHandleStubArguments* args) {  }

void MethodHandles::trace_method_handle(MacroAssembler* _masm, const char* adaptername) {  }
#endif //PRODUCT
