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
#include "opto/graphKit.hpp"
#include "opto/machnode.hpp"
#include "opto/memnode.hpp"
#include "opto/multnode.hpp"
#include "opto/node.hpp"

class PhaseGVN;


class ShenandoahBarrierNode : public TypeNode {
private:
  bool _allow_fromspace;

#ifdef ASSERT
  enum verify_type {
    ShenandoahLoad,
    ShenandoahStore,
    ShenandoahValue,
    ShenandoahNone,
  };

  static bool verify_helper(Node* in, Node_Stack& phis, VectorSet& visited, verify_type t, bool trace, Unique_Node_List& barriers_used);
#endif

public:

public:
  enum { Control,
         Memory,
         ValueIn
  };

  ShenandoahBarrierNode(Node* ctrl, Node* mem, Node* obj, bool allow_fromspace)
    : TypeNode(obj->bottom_type()->isa_oopptr() ? obj->bottom_type()->is_oopptr()->cast_to_nonconst() : obj->bottom_type(), 3),
      _allow_fromspace(allow_fromspace) {

    init_req(Control, ctrl);
    init_req(Memory, mem);
    init_req(ValueIn, obj);

    init_class_id(Class_ShenandoahBarrier);
  }

  static Node* skip_through_barrier(Node* n);

  static const TypeOopPtr* brooks_pointer_type(const Type* t) {
    return t->is_oopptr()->cast_to_nonconst()->add_offset(BrooksPointer::byte_offset())->is_oopptr();
  }

  virtual const TypePtr* adr_type() const {
    if (bottom_type() == Type::TOP) {
      return NULL;
    }
    //const TypePtr* adr_type = in(MemNode::Address)->bottom_type()->is_ptr();
    const TypePtr* adr_type = brooks_pointer_type(bottom_type());
    assert(adr_type->offset() == BrooksPointer::byte_offset(), "sane offset");
    assert(Compile::current()->alias_type(adr_type)->is_rewritable(), "brooks ptr must be rewritable");
    return adr_type;
  }

  virtual uint  ideal_reg() const { return Op_RegP; }
  virtual uint match_edge(uint idx) const {
    return idx >= ValueIn;
  }

  Node* Identity_impl(PhaseTransform* phase);

  virtual const Type* Value(PhaseTransform* phase) const;
  virtual bool depends_only_on_test() const {
    return true;
  };

  static bool is_evacuation_in_progress_test(Node *n);
  static bool is_gc_state_load(Node *n);

  static bool needs_barrier(PhaseTransform* phase, ShenandoahBarrierNode* orig, Node* n, Node* rb_mem, bool allow_fromspace);

#ifdef ASSERT
  static void report_verify_failure(const char* msg, Node* n1 = NULL, Node* n2 = NULL);
  static void verify(RootNode* root);
  static void verify_raw_mem(RootNode* root);
#endif
#ifndef PRODUCT
  virtual void dump_spec(outputStream *st) const;
#endif
  static void do_cmpp_if(GraphKit& kit, Node*& taken_branch, Node*& untaken_branch, Node*& taken_memory, Node*& untaken_memory);
  static const TypePtr* fix_addp_type(const TypePtr* res, Node* base);

protected:
  uint hash() const;
  uint cmp(const Node& n) const;
  uint size_of() const;

private:
  static bool needs_barrier_impl(PhaseTransform* phase, ShenandoahBarrierNode* orig, Node* n, Node* rb_mem, bool allow_fromspace, Unique_Node_List &visited);


  static bool dominates_memory(PhaseTransform* phase, Node* b1, Node* b2, bool linear);
  static bool dominates_memory_impl(PhaseTransform* phase, Node* b1, Node* b2, Node* current, bool linear);

public:
  static bool is_dominator(Node *d_c, Node *n_c, Node* d, Node* n, PhaseIdealLoop* phase);
  static bool is_dominator_same_ctrl(Node* c, Node* d, Node* n, PhaseIdealLoop* phase);
};

class ShenandoahReadBarrierNode : public ShenandoahBarrierNode {
public:
  ShenandoahReadBarrierNode(Node* ctrl, Node* mem, Node* obj)
    : ShenandoahBarrierNode(ctrl, mem, obj, true) {
    assert(UseShenandoahGC && (ShenandoahReadBarrier ||
                               ShenandoahWriteBarrier ||
                               ShenandoahAcmpBarrier),
           "should be enabled");
  }
  ShenandoahReadBarrierNode(Node* ctrl, Node* mem, Node* obj, bool allow_fromspace)
    : ShenandoahBarrierNode(ctrl, mem, obj, allow_fromspace) {
    assert(UseShenandoahGC && (ShenandoahReadBarrier ||
                               ShenandoahWriteBarrier ||
                               ShenandoahAcmpBarrier),
           "should be enabled");
  }

  virtual Node *Ideal(PhaseGVN *phase, bool can_reshape);
  virtual Node* Identity(PhaseTransform* phase);
  virtual int Opcode() const;

  bool is_independent(Node* mem);

private:
  static bool is_independent(const Type* in_type, const Type* this_type);
  static bool dominates_memory_rb(PhaseTransform* phase, Node* b1, Node* b2, bool linear);
  static bool dominates_memory_rb_impl(PhaseTransform* phase, Node* b1, Node* b2, Node* current, bool linear);
};

class ShenandoahWriteBarrierNode : public ShenandoahBarrierNode {
public:
  ShenandoahWriteBarrierNode(Compile* C, Node* ctrl, Node* mem, Node* obj)
    : ShenandoahBarrierNode(ctrl, mem, obj, false) {
    assert(UseShenandoahGC && ShenandoahWriteBarrier, "should be enabled");
    C->add_shenandoah_barrier(this);
  }

  virtual int Opcode() const;
  virtual Node *Ideal(PhaseGVN *phase, bool can_reshape);
  virtual Node* Identity(PhaseTransform* phase);
  virtual bool depends_only_on_test() const { return false; }

  static bool should_process_phi(Node* phi, int alias, Compile* C);
  static void fix_memory_uses(Node* mem, Node* replacement, Node* rep_proj, Node* rep_ctrl, int alias, PhaseIdealLoop* phase);
  static MergeMemNode* allocate_merge_mem(Node* mem, int alias, Node* rep_proj, Node* rep_ctrl, PhaseIdealLoop* phase);
  static MergeMemNode* clone_merge_mem(Node* u, Node* mem, int alias, Node* rep_proj, Node* rep_ctrl, DUIterator& i, PhaseIdealLoop* phase);
  static Node* find_raw_mem(Node* ctrl, Node* wb, const Node_List& memory_nodes, PhaseIdealLoop* phase);
  static void collect_memory_nodes(int alias, Node_List& memory_nodes, PhaseIdealLoop* phase);
  static void fix_raw_mem(Node* ctrl, Node* region, Node* raw_mem, Node* raw_mem_for_ctrl,
                          Node* raw_mem_phi, Node_List& memory_nodes,
                          Unique_Node_List& uses,
                          PhaseIdealLoop* phase);
  static Node* get_ctrl(Node* n, PhaseIdealLoop* phase);
  static Node* ctrl_or_self(Node* n, PhaseIdealLoop* phase);
  static bool mem_is_valid(Node* m, Node* c, PhaseIdealLoop* phase);

  // virtual void set_req( uint i, Node *n ) {
  //   if (i == MemNode::Memory) { assert(n == Compiler::current()->immutable_memory(), "set only immutable mem on wb"); }
  //   Node::set_req(i, n);
  // }
};

class ShenandoahWBMemProjNode : public ProjNode {
public:
  enum {SWBMEMPROJCON = (uint)-3};
  ShenandoahWBMemProjNode(Node *src) : ProjNode( src, SWBMEMPROJCON) {
    assert(UseShenandoahGC && ShenandoahWriteBarrier, "should be enabled");
    assert(src->Opcode() == Op_ShenandoahWriteBarrier || src->is_Mach(), "epxect wb");
  }
  virtual Node* Identity(PhaseTransform* phase);

  virtual int Opcode() const;
  virtual bool      is_CFG() const  { return false; }
  virtual const Type *bottom_type() const {return Type::MEMORY;}
  virtual const TypePtr *adr_type() const {
    Node* wb = in(0);
    if (wb == NULL || wb->is_top())  return NULL; // node is dead
    assert(wb->Opcode() == Op_ShenandoahWriteBarrier || (wb->is_Mach() && wb->as_Mach()->ideal_Opcode() == Op_ShenandoahWriteBarrier), "expect wb");
    return ShenandoahBarrierNode::brooks_pointer_type(wb->bottom_type());
  }

  virtual uint ideal_reg() const { return 0;} // memory projections don't have a register
  virtual const Type *Value(PhaseTransform* phase ) const {
    return bottom_type();
  }
#ifndef PRODUCT
  virtual void dump_spec(outputStream *st) const {};
#endif
};

#endif // SHARE_VM_OPTO_SHENANDOAH_SUPPORT_HPP
