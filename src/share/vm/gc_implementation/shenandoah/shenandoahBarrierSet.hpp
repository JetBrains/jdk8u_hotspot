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

#ifndef SHARE_VM_GC_SHENANDOAH_SHENANDOAHBARRIERSET_HPP
#define SHARE_VM_GC_SHENANDOAH_SHENANDOAHBARRIERSET_HPP

#include "gc_implementation/shenandoah/shenandoahHeap.hpp"
#include "memory/barrierSet.hpp"

class ShenandoahBarrierSet: public BarrierSet {
private:

  ShenandoahHeap* _heap;
  static inline oop get_shenandoah_forwardee_helper(oop p);

public:

  ShenandoahBarrierSet(ShenandoahHeap* heap);

  void print_on(outputStream* st) const;

  bool is_a(BarrierSet::Name bsn);

  bool has_read_prim_array_opt();
  bool has_read_prim_barrier();
  bool has_read_ref_array_opt();
  bool has_read_ref_barrier();
  bool has_read_region_opt();
  bool has_write_prim_array_opt();
  bool has_write_prim_barrier();
  bool has_write_ref_array_opt();
  bool has_write_ref_barrier();
  bool has_write_ref_pre_barrier();
  bool has_write_region_opt();
  bool is_aligned(HeapWord* hw);
  void read_prim_array(MemRegion mr);
  void read_prim_field(HeapWord* hw, size_t s);
  bool read_prim_needs_barrier(HeapWord* hw, size_t s);
  void read_ref_array(MemRegion mr);

  void read_ref_field(void* v);

  bool read_ref_needs_barrier(void* v);
  void read_region(MemRegion mr);
  void resize_covered_region(MemRegion mr);
  void write_prim_array(MemRegion mr);
  void write_prim_field(HeapWord* hw, size_t s , juint x, juint y);
  bool write_prim_needs_barrier(HeapWord* hw, size_t s, juint x, juint y);
  void write_ref_array_work(MemRegion mr);

  template <class T> void
  write_ref_array_pre_work(T* dst, int count);

  void write_ref_array_pre(oop* dst, int count, bool dest_uninitialized);

  void write_ref_array_pre(narrowOop* dst, int count, bool dest_uninitialized);


  template <class T> static void write_ref_field_pre_static(T* field, oop newVal);

  // We export this to make it available in cases where the static
  // type of the barrier set is known.  Note that it is non-virtual.
  template <class T> inline void inline_write_ref_field_pre(T* field, oop newVal);

  // These are the more general virtual versions.
  void write_ref_field_pre_work(oop* field, oop new_val);
  void write_ref_field_pre_work(narrowOop* field, oop new_val);
  void write_ref_field_pre_work(void* field, oop new_val);

  void write_ref_field_work(void* v, oop o, bool release = false);
  void write_region_work(MemRegion mr);

  virtual oop read_barrier(oop src);

  oop resolve_and_update_oop(oop* p, oop obj);
  oop resolve_and_update_oop(narrowOop* p, oop obj);

  template <class T>
  static inline oop resolve_and_update_oop_static(T p, oop obj);

  static inline oop resolve_oop_static_not_null(oop p);

  static inline oop resolve_oop_static(oop p);

  static inline oop resolve_oop_static_no_check(oop p);


  oop resolve_and_maybe_copy_oopHelper(oop src);
  oop resolve_and_maybe_copy_oop_work(oop src);
  oop resolve_and_maybe_copy_oop_work2(oop src);
  virtual oop write_barrier(oop src);

  bool obj_equals(oop obj1, oop obj2);
  bool obj_equals(narrowOop obj1, narrowOop obj2);

#ifdef ASSERT
  virtual bool is_safe(oop o);
  virtual bool is_safe(narrowOop o);
#endif

  static oopDesc* write_barrier_c2(oopDesc* src);
  static oopDesc* write_barrier_interp(oopDesc* src);
  static oopDesc* write_barrier_c1(JavaThread* thread, oopDesc* src);

private:
  bool need_update_refs_barrier();

#ifndef CC_INTERP
public:
  virtual void interpreter_read_barrier(MacroAssembler* masm, Register dst);
  virtual void interpreter_read_barrier_not_null(MacroAssembler* masm, Register dst);
  void interpreter_write_barrier(MacroAssembler* masm, Register dst);
  void asm_acmp_barrier(MacroAssembler* masm, Register op1, Register op2);

private:
  void compile_resolve_oop_runtime(MacroAssembler* masm, Register dst);

#endif
};

#endif //SHARE_VM_GC_SHENANDOAH_SHENANDOAHBARRIERSET_HPP
