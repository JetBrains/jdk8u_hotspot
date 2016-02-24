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

#ifndef SHARE_VM_OPTO_SHENANDOAH_SUPPORT_HPP
#define SHARE_VM_OPTO_SHENANDOAH_SUPPORT_HPP

#include "gc_implementation/shenandoah/brooksPointer.hpp"
#include "memory/allocation.hpp"
#include "opto/addnode.hpp"
#include "opto/memnode.hpp"
#include "opto/multnode.hpp"
#include "opto/node.hpp"

class PhaseTransform;


class ShenandoahBarrierNode : public TypeNode {
private:
  bool _allow_fromspace;
public:

public:
  enum { Control,
         Memory,
         ValueIn
  };

  ShenandoahBarrierNode(Node* ctrl, Node* mem, Node* obj, bool allow_fromspace)
    : TypeNode(obj->bottom_type(), 3),
      _allow_fromspace(allow_fromspace) {

    init_req(Control, ctrl);
    init_req(Memory, mem);
    init_req(ValueIn, obj);

    init_class_id(Class_ShenandoahBarrier);
  }

  static Node* skip_through_barrier(Node* n);

  virtual const class TypePtr* adr_type() const {
    const TypePtr* adr_type = bottom_type()->is_ptr()->add_offset(BrooksPointer::BYTE_OFFSET);
    assert(adr_type->offset() == BrooksPointer::BYTE_OFFSET, "sane offset");
    assert(Compile::current()->alias_type(adr_type)->is_rewritable(), "brooks ptr must be rewritable");
    return adr_type;
  }

  virtual uint  ideal_reg() const { return Op_RegP; }
  virtual uint match_edge(uint idx) const {
    return idx >= ValueIn;
  }

  virtual Node* Identity(PhaseTransform* phase);
  Node* Identity_impl(PhaseTransform* phase);

  virtual Node *Ideal_DU_postCCP( PhaseCCP * );

  virtual const Type* Value(PhaseTransform* phase) const;
  virtual bool depends_only_on_test() const {
    return true;
  };
#ifdef ASSERT
  void check_invariants();
  uint num_mem_projs();
#endif

  static bool needs_barrier(PhaseTransform* phase, ShenandoahBarrierNode* orig, Node* n, Node* rb_mem, bool allow_fromspace);

  static bool has_barrier_users(Node* n, Unique_Node_List &visited);

  uint hash() const;
  uint cmp(const Node& n) const;
  uint size_of() const;

private:
  static bool needs_barrier_impl(PhaseTransform* phase, ShenandoahBarrierNode* orig, Node* n, Node* rb_mem, bool allow_fromspace, Unique_Node_List &visited);


  bool dominates_control(PhaseTransform* phase, Node* c1, Node* c2);
  bool dominates_memory(PhaseTransform* phase, Node* b1, Node* b2);
  bool dominates_memory_impl(PhaseTransform* phase, Node* b1, Node* b2, Node* current, Unique_Node_List &visisted);
};

class ShenandoahReadBarrierNode : public ShenandoahBarrierNode {
public:
  ShenandoahReadBarrierNode(Node* ctrl, Node* mem, Node* obj)
    : ShenandoahBarrierNode(ctrl, mem, obj, true) {
  }
  ShenandoahReadBarrierNode(Node* ctrl, Node* mem, Node* obj, bool allow_fromspace)
    : ShenandoahBarrierNode(ctrl, mem, obj, allow_fromspace) {
  }

  virtual Node *Ideal(PhaseGVN *phase, bool can_reshape);
  virtual Node* Identity(PhaseTransform* phase);
  virtual int Opcode() const;

private:
  bool is_independent(const Type* in_type, const Type* this_type) const;
  bool dominates_memory_rb(PhaseTransform* phase, Node* b1, Node* b2);
  bool dominates_memory_rb_impl(PhaseTransform* phase, Node* b1, Node* b2, Node* current, Unique_Node_List &visited);
};

class ShenandoahWriteBarrierNode : public ShenandoahBarrierNode {
public:
  ShenandoahWriteBarrierNode(Node* ctrl, Node* mem, Node* obj)
    : ShenandoahBarrierNode(ctrl, mem, obj, true) {
  }

  virtual int Opcode() const;
  virtual Node *Ideal(PhaseGVN *phase, bool can_reshape);
};

class ShenandoahWBMemProjNode : public ProjNode {
public:
  enum {SWBMEMPROJCON = (uint)-3};
  ShenandoahWBMemProjNode(Node *src) : ProjNode( src, SWBMEMPROJCON) {
    assert(src->Opcode() == Op_ShenandoahWriteBarrier || src->is_Mach(), "epxect wb");
#ifdef ASSERT
    in(0)->as_ShenandoahBarrier()->check_invariants();
#endif
  }
  virtual Node* Identity(PhaseTransform* phase);

  virtual int Opcode() const;
  virtual bool      is_CFG() const  { return false; }
  virtual const Type *bottom_type() const {return Type::MEMORY;}
  virtual const TypePtr *adr_type() const {
    Node* ctrl = in(0);
    if (ctrl == NULL)  return NULL; // node is dead
    assert(ctrl->Opcode() == Op_ShenandoahWriteBarrier || ctrl->is_Mach(), "expect wb");
    return ctrl->adr_type();
  }

  virtual uint ideal_reg() const { return 0;} // memory projections don't have a register
  virtual const Type *Value( PhaseTransform *phase ) const {
    return bottom_type();
  }
#ifndef PRODUCT
  virtual void dump_spec(outputStream *st) const {};
#endif
};

#endif // SHARE_VM_OPTO_SHENANDOAH_SUPPORT_HPP
