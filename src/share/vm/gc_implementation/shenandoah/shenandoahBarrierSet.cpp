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

#include "precompiled.hpp"
#include "gc_implementation/g1/g1SATBCardTableModRefBS.hpp"
#include "gc_implementation/shenandoah/shenandoahAsserts.hpp"
#include "gc_implementation/shenandoah/shenandoahBarrierSet.hpp"
#include "gc_implementation/shenandoah/shenandoahCollectorPolicy.hpp"
#include "gc_implementation/shenandoah/shenandoahHeap.inline.hpp"
#include "runtime/interfaceSupport.hpp"

class ShenandoahUpdateRefsForOopClosure: public ExtendedOopClosure {
private:
  ShenandoahHeap* _heap;
  template <class T>
  inline void do_oop_work(T* p) {
    _heap->maybe_update_with_forwarded(p);
  }
public:
  ShenandoahUpdateRefsForOopClosure() : _heap(ShenandoahHeap::heap()) {
    assert(UseShenandoahGC && ShenandoahCloneBarrier, "should be enabled");
  }
  void do_oop(oop* p)       { do_oop_work(p); }
  void do_oop(narrowOop* p) { do_oop_work(p); }
};

ShenandoahBarrierSet::ShenandoahBarrierSet(ShenandoahHeap* heap) :
  BarrierSet(),
  _heap(heap)
{
  _kind = BarrierSet::ShenandoahBarrierSet;
}

void ShenandoahBarrierSet::print_on(outputStream* st) const {
  st->print("ShenandoahBarrierSet");
}

bool ShenandoahBarrierSet::is_a(BarrierSet::Name bsn) {
  return bsn == BarrierSet::ShenandoahBarrierSet;
}

bool ShenandoahBarrierSet::has_read_prim_array_opt() {
  return true;
}

bool ShenandoahBarrierSet::has_read_prim_barrier() {
  return false;
}

bool ShenandoahBarrierSet::has_read_ref_array_opt() {
  return true;
}

bool ShenandoahBarrierSet::has_read_ref_barrier() {
  return false;
}

bool ShenandoahBarrierSet::has_read_region_opt() {
  return true;
}

bool ShenandoahBarrierSet::has_write_prim_array_opt() {
  return true;
}

bool ShenandoahBarrierSet::has_write_prim_barrier() {
  return false;
}

bool ShenandoahBarrierSet::has_write_ref_array_opt() {
  return true;
}

bool ShenandoahBarrierSet::has_write_ref_barrier() {
  return true;
}

bool ShenandoahBarrierSet::has_write_ref_pre_barrier() {
  return true;
}

bool ShenandoahBarrierSet::has_write_region_opt() {
  return true;
}

bool ShenandoahBarrierSet::is_aligned(HeapWord* hw) {
  return true;
}

void ShenandoahBarrierSet::read_prim_array(MemRegion mr) {
  Unimplemented();
}

void ShenandoahBarrierSet::read_prim_field(HeapWord* hw, size_t s){
  Unimplemented();
}

bool ShenandoahBarrierSet::read_prim_needs_barrier(HeapWord* hw, size_t s) {
  return false;
}

void ShenandoahBarrierSet::read_ref_array(MemRegion mr) {
  Unimplemented();
}

void ShenandoahBarrierSet::read_ref_field(void* v) {
  //    tty->print_cr("read_ref_field: v = "PTR_FORMAT, v);
  // return *v;
}

bool ShenandoahBarrierSet::read_ref_needs_barrier(void* v) {
  Unimplemented();
  return false;
}

void ShenandoahBarrierSet::read_region(MemRegion mr) {
  Unimplemented();
}

void ShenandoahBarrierSet::resize_covered_region(MemRegion mr) {
  Unimplemented();
}

void ShenandoahBarrierSet::write_prim_array(MemRegion mr) {
  Unimplemented();
}

void ShenandoahBarrierSet::write_prim_field(HeapWord* hw, size_t s , juint x, juint y) {
  Unimplemented();
}

bool ShenandoahBarrierSet::write_prim_needs_barrier(HeapWord* hw, size_t s, juint x, juint y) {
  Unimplemented();
  return false;
}

bool ShenandoahBarrierSet::need_update_refs_barrier() {
  if (_heap->shenandoahPolicy()->update_refs()) {
    return _heap->is_update_refs_in_progress();
  } else {
    return _heap->is_concurrent_mark_in_progress() && _heap->has_forwarded_objects();
  }
}

void ShenandoahBarrierSet::write_ref_array_work(MemRegion r) {
  ShouldNotReachHere();
}

template <class T>
void ShenandoahBarrierSet::write_ref_array_loop(HeapWord* start, size_t count) {
  assert(UseShenandoahGC && ShenandoahCloneBarrier, "Should be enabled");
  ShenandoahEvacOOMScope oom_evac_scope;
  ShenandoahUpdateRefsForOopClosure cl;
  T* dst = (T*) start;
  for (size_t i = 0; i < count; i++) {
    cl.do_oop(dst++);
  }
}

void ShenandoahBarrierSet::write_ref_array(HeapWord* start, size_t count) {
  assert(UseShenandoahGC, "should be enabled");
  if (!ShenandoahCloneBarrier) return;
  if (!need_update_refs_barrier()) return;

  if (UseCompressedOops) {
    write_ref_array_loop<narrowOop>(start, count);
  } else {
    write_ref_array_loop<oop>(start, count);
  }
}

template <class T>
void ShenandoahBarrierSet::write_ref_array_pre_work(T* dst, int count) {
  assert (UseShenandoahGC && ShenandoahSATBBarrier, "Should be enabled");

  shenandoah_assert_not_in_cset_loc_except(dst, _heap->cancelled_concgc());

  if (! JavaThread::satb_mark_queue_set().is_active()) return;
  T* elem_ptr = dst;
  for (int i = 0; i < count; i++, elem_ptr++) {
    T heap_oop = oopDesc::load_heap_oop(elem_ptr);
    if (!oopDesc::is_null(heap_oop)) {
      G1SATBCardTableModRefBS::enqueue(oopDesc::decode_heap_oop_not_null(heap_oop));
    }
  }
}

void ShenandoahBarrierSet::write_ref_array_pre(oop* dst, int count, bool dest_uninitialized) {
  if (! dest_uninitialized && ShenandoahSATBBarrier) {
    write_ref_array_pre_work(dst, count);
  }
}

void ShenandoahBarrierSet::write_ref_array_pre(narrowOop* dst, int count, bool dest_uninitialized) {
  if (! dest_uninitialized && ShenandoahSATBBarrier) {
    write_ref_array_pre_work(dst, count);
  }
}

template <class T>
void ShenandoahBarrierSet::write_ref_field_pre_static(T* field, oop newVal) {
  T heap_oop = oopDesc::load_heap_oop(field);

  shenandoah_assert_not_in_cset_loc_except(field, ShenandoahHeap::heap()->cancelled_concgc());

  if (!oopDesc::is_null(heap_oop)) {
    G1SATBCardTableModRefBS::enqueue(oopDesc::decode_heap_oop(heap_oop));
  }
}

template <class T>
inline void ShenandoahBarrierSet::inline_write_ref_field_pre(T* field, oop newVal) {
  write_ref_field_pre_static(field, newVal);
}

// These are the more general virtual versions.
void ShenandoahBarrierSet::write_ref_field_pre_work(oop* field, oop new_val) {
  write_ref_field_pre_static(field, new_val);
}

void ShenandoahBarrierSet::write_ref_field_pre_work(narrowOop* field, oop new_val) {
  write_ref_field_pre_static(field, new_val);
}

void ShenandoahBarrierSet::write_ref_field_pre_work(void* field, oop new_val) {
  guarantee(false, "Not needed");
}

void ShenandoahBarrierSet::write_ref_field_work(void* v, oop o, bool release) {
  shenandoah_assert_not_in_cset_loc_except(v, _heap->cancelled_concgc());
  shenandoah_assert_not_forwarded_except  (v, o, o == NULL || _heap->cancelled_concgc() || !_heap->is_concurrent_mark_in_progress());
  shenandoah_assert_not_in_cset_except    (v, o, o == NULL || _heap->cancelled_concgc() || !_heap->is_concurrent_mark_in_progress());
}

void ShenandoahBarrierSet::write_region_work(MemRegion mr) {
  assert(UseShenandoahGC, "should be enabled");
  if (!ShenandoahCloneBarrier) return;
  if (! need_update_refs_barrier()) return;

  // This is called for cloning an object (see jvm.cpp) after the clone
  // has been made. We are not interested in any 'previous value' because
  // it would be NULL in any case. But we *are* interested in any oop*
  // that potentially need to be updated.

  ShenandoahEvacOOMScope oom_evac_scope;
  oop obj = oop(mr.start());
  assert(obj->is_oop(), "must be an oop");
  ShenandoahUpdateRefsForOopClosure cl;
  obj->oop_iterate(&cl);
}

oop ShenandoahBarrierSet::read_barrier(oop src) {
  return ShenandoahBarrierSet::resolve_forwarded(src);
}

bool ShenandoahBarrierSet::obj_equals(oop obj1, oop obj2) {
  bool eq = oopDesc::unsafe_equals(obj1, obj2);
  if (! eq && ShenandoahAcmpBarrier) {
    OrderAccess::loadload();
    obj1 = resolve_forwarded(obj1);
    obj2 = resolve_forwarded(obj2);
    eq = oopDesc::unsafe_equals(obj1, obj2);
  }
  return eq;
}

bool ShenandoahBarrierSet::obj_equals(narrowOop obj1, narrowOop obj2) {
  return obj_equals(oopDesc::decode_heap_oop(obj1), oopDesc::decode_heap_oop(obj2));
}

JRT_LEAF(oopDesc*, ShenandoahBarrierSet::write_barrier_JRT(oopDesc* src))
  oop result = ((ShenandoahBarrierSet*)oopDesc::bs())->write_barrier(src);
  return (oopDesc*) result;
JRT_END

IRT_LEAF(oopDesc*, ShenandoahBarrierSet::write_barrier_IRT(oopDesc* src))
  oop result = ((ShenandoahBarrierSet*)oopDesc::bs())->write_barrier(src);
  return (oopDesc*) result;
IRT_END

oop ShenandoahBarrierSet::write_barrier(oop obj) {
  if (ShenandoahWriteBarrier) {
    if (!oopDesc::is_null(obj)) {
      bool evac_in_progress = _heap->is_evacuation_in_progress();
      OrderAccess::loadload();
      oop fwd = resolve_forwarded_not_null(obj);
      if (evac_in_progress &&
          _heap->in_collection_set(obj) &&
          oopDesc::unsafe_equals(obj, fwd)) {
        ShenandoahEvacOOMScope oom_evac_scope;
        bool evac;
        return _heap->evacuate_object(obj, Thread::current(), evac);
      } else {
        return fwd;
      }
    }
  }
  return obj;
}

#ifdef ASSERT
void ShenandoahBarrierSet::verify_safe_oop(oop p) {
  shenandoah_assert_not_in_cset_except(NULL, p, (p == NULL) || ShenandoahHeap::heap()->cancelled_concgc());
}

void ShenandoahBarrierSet::verify_safe_oop(narrowOop p) {
  verify_safe_oop(oopDesc::decode_heap_oop(p));
}
#endif
