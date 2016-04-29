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
#include "opto/callnode.hpp"
#include "opto/phaseX.hpp"
#include "opto/shenandoahSupport.hpp"

Node* ShenandoahBarrierNode::skip_through_barrier(Node* n) {
  if (n->is_ShenandoahBarrier()) {
    return n->in(ValueIn);
  } else {
    return n;
  }
}

bool ShenandoahBarrierNode::needs_barrier(PhaseTransform* phase, ShenandoahBarrierNode* orig, Node* n, Node* rb_mem, bool allow_fromspace) {
  Unique_Node_List visited;
  return needs_barrier_impl(phase, orig, n, rb_mem, allow_fromspace, visited);
}

bool ShenandoahBarrierNode::needs_barrier_impl(PhaseTransform* phase, ShenandoahBarrierNode* orig, Node* n, Node* rb_mem, bool allow_fromspace, Unique_Node_List &visited) {

  if (visited.member(n)) {
    return false; // Been there.
  }
  visited.push(n);

  if (n->is_Allocate()) {
    return false;
  }

  if (n->is_CallJava()) {
    return true;
  }

  const Type* type = phase->type(n);
  if (type->higher_equal(TypePtr::NULL_PTR)) {
    return false;
  }
  if (type->isa_oopptr() && type->is_oopptr()->const_oop() != NULL) {
    return false;
  }

  if (ShenandoahOptimizeFinals) {
    const TypeAryPtr* ary = type->isa_aryptr();
    if (ary && ary->is_stable() && allow_fromspace) {
      return false;
    }
  }

  if (n->is_CheckCastPP() || n->is_ConstraintCast()) {
    return needs_barrier_impl(phase, orig, n->in(1), rb_mem, allow_fromspace, visited);
  }
  if (n->is_Parm()) {
    return true;
  }
  if (n->is_Proj()) {
    return needs_barrier_impl(phase, orig, n->in(0), rb_mem, allow_fromspace, visited);
  }
  if (n->is_Phi()) {
    bool need_barrier = false;
    for (uint i = 1; i < n->req() && ! need_barrier; i++) {
      Node* input = n->in(i);
      if (input == NULL) {
        need_barrier = true; // Phi not complete yet?
      } else if (needs_barrier_impl(phase, orig, input, rb_mem, allow_fromspace, visited)) {
        need_barrier = true;
      }
    }
    return need_barrier;
  }
  if (n->is_CMove()) {
    return needs_barrier_impl(phase, orig, n->in(CMoveNode::IfFalse), rb_mem, allow_fromspace, visited) ||
           needs_barrier_impl(phase, orig, n->in(CMoveNode::IfTrue ), rb_mem, allow_fromspace, visited);
  }
  if (n->Opcode() == Op_CreateEx) {
    return true;
  }
  if (n->Opcode() == Op_ShenandoahWriteBarrier) {
    return false;
  }
  if (n->Opcode() == Op_ShenandoahReadBarrier) {
    if (rb_mem == n->in(Memory)) {
      return false;
    } else {
      return true;
    }
  }

  if (n->Opcode() == Op_LoadP) {
    return true;
  }
  if (n->Opcode() == Op_GetAndSetP) {
    return true;
  }
#ifdef ASSERT
  tty->print("need barrier on?: "); n->dump();
#endif
  return true;
}

bool ShenandoahReadBarrierNode::dominates_memory_rb_impl(PhaseTransform* phase,
                                                         Node* b1,
                                                         Node* b2,
                                                         Node* current,
                                                         Unique_Node_List &visited) {
  if (current == NULL) {
    return false; // Incomplete phi. Try again later.
  } else if (visited.member(current)) {
    // We have already seen it.
    return true;
  }
  visited.push(current);

  if (current == b1) {
    return true;
  } else if (current == phase->C->immutable_memory()) {
    return false;
  } else if (current->isa_Phi()) {
    bool dominates = true;
    for (uint i = 1; i < current->req() && dominates == true; i++) {
      Node* in = current->in(i);
      dominates = dominates && dominates_memory_rb_impl(phase, b1, b2, in, visited);
    }
    return dominates;
  } else if (current->Opcode() == Op_ShenandoahWriteBarrier) {
    const Type* in_type = current->bottom_type();
    const Type* this_type = b2->bottom_type();
    if (is_independent(in_type, this_type)) {
      Node* in = current->in(Memory);
      return dominates_memory_rb_impl(phase, b1, b2, in, visited);
    } else {
      return false;
    }
  } else if (current->Opcode() == Op_ShenandoahWBMemProj) {
    Node* in = current->in(0);
    return dominates_memory_rb_impl(phase, b1, b2, in, visited);
  } else if (current->is_top()) {
    return true; // Dead path
  } else if (current->is_Proj()) {
    return dominates_memory_rb_impl(phase, b1, b2, current->in(0), visited);
  } else if (current->is_Call()) {
    return false; // TODO: Maybe improve by looking at the call's memory effects?
  } else if (current->is_MemBar()) {
    return false; // TODO: Do we need to stop at *any* membar?
  } else if (current->is_MergeMem()) {
    const TypePtr* adr_type = phase->type(b2)->is_ptr()->add_offset(BrooksPointer::BYTE_OFFSET);
    uint alias_idx = phase->C->get_alias_index(adr_type);
    Node* mem_in = current->as_MergeMem()->memory_at(alias_idx);
    return dominates_memory_rb_impl(phase, b1, b2, current->in(TypeFunc::Memory), visited);
  } else {
#ifdef ASSERT
    current->dump();
#endif
    ShouldNotReachHere();
    return false;
  }
}

bool ShenandoahReadBarrierNode::dominates_memory_rb(PhaseTransform* phase, Node* b1, Node* b2) {
  Unique_Node_List visited;
  return dominates_memory_rb_impl(phase, b1->in(Memory), b2, b2->in(Memory), visited);
}

bool ShenandoahReadBarrierNode::is_independent(const Type* in_type, const Type* this_type) const {
  assert(in_type->isa_oopptr(), "expect oop ptr");
  assert(this_type->isa_oopptr(), "expect oop ptr");

  ciKlass* in_kls = in_type->is_oopptr()->klass();
  ciKlass* this_kls = this_type->is_oopptr()->klass();
  if ((!in_kls->is_subclass_of(this_kls)) &&
      (!this_kls->is_subclass_of(in_kls))) {
    return true;
  }
  return false;
}

Node* ShenandoahReadBarrierNode::Ideal(PhaseGVN *phase, bool can_reshape) {

  if (! can_reshape) {
    return NULL;
  }

  if (in(Memory) == phase->C->immutable_memory()) return NULL;

  // If memory input is a MergeMem, take the appropriate slice out of it.
  Node* mem_in = in(Memory);
  if (mem_in->isa_MergeMem()) {
    const TypePtr* adr_type = bottom_type()->is_ptr()->add_offset(BrooksPointer::BYTE_OFFSET);
    uint alias_idx = phase->C->get_alias_index(adr_type);
    mem_in = mem_in->as_MergeMem()->memory_at(alias_idx);
    set_req(Memory, mem_in);
    return this;
  }

  Node* input = in(Memory);
  if (input->Opcode() == Op_ShenandoahWBMemProj) {
    input = input->in(0);
    if (input->is_top()) return NULL; // Dead path.
    assert(input->Opcode() == Op_ShenandoahWriteBarrier, "expect write barrier");
    const Type* in_type = phase->type(input);
    const Type* this_type = phase->type(this);
    if (is_independent(in_type, this_type)) {
      phase->igvn_rehash_node_delayed(input);
      set_req(Memory, input->in(Memory));
      return this;
    }
  }
  return NULL;
}

bool ShenandoahBarrierNode::has_barrier_users(Node* n, Unique_Node_List &visited) {
  if (visited.member(n)) {
    return false;
  }
  visited.push(n);

  bool has_users = false;
  for (DUIterator_Fast jmax, j = n->fast_outs(jmax); j < jmax && ! has_users; j++) {
    Node* o = n->fast_out(j);
    if (o->Opcode() == Op_ShenandoahReadBarrier ||
        o->Opcode() == Op_ShenandoahWriteBarrier) {
      has_users = true;
    } else if (o->isa_Phi()) {
      has_users = has_barrier_users(o, visited);
    } else if (o->Opcode() == Op_MergeMem) {
      // Not a user. ?
    } else {
      ShouldNotReachHere();
    }
  }
  return has_users;
}

Node* ShenandoahWriteBarrierNode::Ideal(PhaseGVN *phase, bool can_reshape) {

  if (! can_reshape) return NULL;

  if (in(Memory) == phase->C->immutable_memory()) return NULL;

  Node* mem_in = in(Memory);
  if (mem_in->isa_MergeMem()) {
    const TypePtr* adr_type = bottom_type()->is_ptr()->add_offset(BrooksPointer::BYTE_OFFSET);
    uint alias_idx = phase->C->get_alias_index(adr_type);
    mem_in = mem_in->as_MergeMem()->memory_at(alias_idx);
    set_req(Memory, mem_in);
    return this;
  }

  Node* mem_proj = find_out_with(Op_ShenandoahWBMemProj);
  if (mem_proj == NULL) {
    set_req(Memory, phase->C->immutable_memory());
    return this;
  }

  Unique_Node_List visited;
  if (! has_barrier_users(mem_proj, visited)) {
    phase->igvn_rehash_node_delayed(in(Memory));
    set_req(Memory, phase->C->immutable_memory());
    return this;
  }
  return NULL;
}

bool ShenandoahBarrierNode::dominates_control(PhaseTransform* phase,
                                              Node* c1,
                                              Node* c2) {
  if (c1 == c2) {
    return true;
  }
  if (c1 == NULL) {
    return true;
  }
  //ShouldNotReachHere();
  return false;
}

bool ShenandoahBarrierNode::dominates_memory_impl(PhaseTransform* phase,
                                                  Node* b1,
                                                  Node* b2,
                                                  Node* current,
                                                  Unique_Node_List &visited) {
  if (current == NULL) {
    return false;
  } else if (visited.member(current)) {
    // We have already seen it.
    return true;
  }

  visited.push(current);

  if (current == b1) {
    return true;
  } else if (current == b2) {
    return false;
  } else if (current == phase->C->immutable_memory()) {
    return false;
  } else if (current->isa_Phi()) {
    bool dominates = true;
    for (uint i = 1; i < current->req() && dominates == true; i++) {
      Node* in = current->in(i);
      dominates = dominates && dominates_memory_impl(phase, b1, b2, in, visited);
    }
    return dominates;
  } else if (current->Opcode() == Op_ShenandoahWriteBarrier) {
    // Follow through memory input.
    Node* in = current->in(Memory);
    return dominates_memory_impl(phase, b1, b2, in, visited);
  } else if (current->Opcode() == Op_ShenandoahWBMemProj) {
    // Follow through memory input.
    Node* in = current->in(0);
    return dominates_memory_impl(phase, b1, b2, in, visited);
  } else if (current->is_top()) {
    return true; // Dead path
  } else if (current->is_Proj()) {
    return dominates_memory_impl(phase, b1, b2, current->in(0), visited);
  } else if (current->is_Call()) {
    return dominates_memory_impl(phase, b1, b2, current->in(TypeFunc::Memory), visited);
  } else if (current->is_MemBar()) {
    return dominates_memory_impl(phase, b1, b2, current->in(TypeFunc::Memory), visited);
  } else if (current->is_MergeMem()) {
    const TypePtr* adr_type = phase->type(b2)->is_ptr()->add_offset(BrooksPointer::BYTE_OFFSET);
    uint alias_idx = phase->C->get_alias_index(adr_type);
    Node* mem_in = current->as_MergeMem()->memory_at(alias_idx);
    return dominates_memory_impl(phase, b1, b2, current->in(TypeFunc::Memory), visited);
  } else {
    // tty->print_cr("what else can we see here:");
#ifdef ASSERT
    current->dump();
#endif
    ShouldNotReachHere();
    return false;
  }
}

/**
 * Determines if b1 dominates b2 through memory inputs. It returns true if:
 * - b1 can be reached by following each branch in b2's memory input (through phis, etc)
 * - or we get back to b2 (i.e. through a loop) without seeing b1
 * In all other cases, (in particular, if we reach immutable_memory without having seen b1)
 * we return false.
 */
bool ShenandoahBarrierNode::dominates_memory(PhaseTransform* phase, Node* b1, Node* b2) {
  Unique_Node_List visited;
  return dominates_memory_impl(phase, b1->in(Memory), b2, b2->in(Memory), visited);
}

Node* ShenandoahBarrierNode::Identity_impl(PhaseTransform* phase) {
  Node* n = in(ValueIn);

  Node* rb_mem = Opcode() == Op_ShenandoahReadBarrier ? in(Memory) : NULL;
  if (! needs_barrier(phase, this, n, rb_mem, _allow_fromspace)) {
    return n;
  }

  // Try to find a write barrier sibling with identical inputs that we can fold into.
  for (DUIterator i = n->outs(); n->has_out(i); i++) {
    Node* sibling = n->out(i);
    if (sibling == this) {
      continue;
    }
    if (sibling->Opcode() != Op_ShenandoahWriteBarrier) {
      continue;
    }
    assert(sibling->in(ValueIn) == in(ValueIn), "sanity");
    assert(sibling->Opcode() == Op_ShenandoahWriteBarrier, "sanity");

    if (dominates_control(phase, sibling->in(Control), in(Control)) &&
        dominates_memory(phase, sibling, this)) {

      return sibling;
    }

  }
  return this;
}

Node* ShenandoahBarrierNode::Identity(PhaseTransform* phase) {

  Node* replacement = Identity_impl(phase);
  if (replacement != this) {
    // If we have a memory projection, we first need to make it go away.
    Node* mem_proj = find_out_with(Op_ShenandoahWBMemProj);
    if (mem_proj != NULL) {
      phase->igvn_rehash_node_delayed(mem_proj);
      return this;
    }
  }
  return replacement;
}

Node* ShenandoahReadBarrierNode::Identity(PhaseTransform* phase) {

  Node* id = ShenandoahBarrierNode::Identity(phase);

  if (id == this && phase->is_IterGVN()) {
    Node* n = in(ValueIn);
    // No success in super call. Try to combine identical read barriers.
    for (DUIterator i = n->outs(); n->has_out(i); i++) {
      Node* sibling = n->out(i);
      if (sibling == this || sibling->Opcode() != Op_ShenandoahReadBarrier) {
        continue;
      }
      assert(sibling->in(ValueIn)  == in(ValueIn), "sanity");
      if (phase->is_IterGVN()->hash_find(sibling) &&
          sibling->bottom_type() == bottom_type() &&
          sibling->in(Control) == in(Control) &&
          dominates_memory_rb(phase, sibling, this)) {
        return sibling;
      }
    }
  }
  return id;
}

const Type* ShenandoahBarrierNode::Value(PhaseTransform* phase) const {
  // Either input is TOP ==> the result is TOP
  const Type *t1 = phase->type(in(Memory));
  if (t1 == Type::TOP) return Type::TOP;
  const Type *t2 = phase->type(in(ValueIn));
  if( t2 == Type::TOP ) return Type::TOP;

  Node* input = in(ValueIn);
  const Type* type = phase->type(input);
  return type;
}

#ifdef ASSERT
uint ShenandoahBarrierNode::num_mem_projs() {
  uint num_mem_proj = 0;
  for (DUIterator_Fast imax, i = fast_outs(imax); i < imax; i++) {
    Node* use = fast_out(i);
    if (use->Opcode() == Op_ShenandoahWBMemProj) {
      num_mem_proj++;
    }
  }
  return num_mem_proj;
}

void ShenandoahBarrierNode::check_invariants() {
}
#endif

uint ShenandoahBarrierNode::hash() const {
  return TypeNode::hash() + _allow_fromspace;
}

uint ShenandoahBarrierNode::cmp(const Node& n) const {
  return _allow_fromspace == ((ShenandoahBarrierNode&) n)._allow_fromspace
    && TypeNode::cmp(n);
}

uint ShenandoahBarrierNode::size_of() const {
  return sizeof(*this);
}

Node* ShenandoahWBMemProjNode::Identity(PhaseTransform* phase) {

  Node* wb = in(0);
  if (wb->is_top()) return phase->C->top(); // Dead path.

  assert(wb->Opcode() == Op_ShenandoahWriteBarrier, "expect write barrier");
  if (wb->as_ShenandoahBarrier()->Identity_impl(phase) != wb) {
    // If the parent write barrier would go away, make this mem proj go away first.
    // Poke parent to give it a chance to go away too.
    phase->igvn_rehash_node_delayed(wb);
    return wb->in(ShenandoahBarrierNode::Memory);
  }

  // We can't do the below unless the graph is fully constructed.
  if (! phase->is_IterGVN()) {
    return this;
  }

  // If the mem projection has no barrier users, it's not needed anymore.
  Unique_Node_List visited;
  if (! ShenandoahWriteBarrierNode::has_barrier_users(this, visited)) {
    phase->igvn_rehash_node_delayed(wb);
    return wb->in(ShenandoahBarrierNode::Memory);
  }

  return this;
}

Node* ShenandoahBarrierNode::Ideal_DU_postCCP( PhaseCCP *ccp ) {
  return MemNode::Ideal_common_DU_postCCP(ccp, this, in(ValueIn));
}
