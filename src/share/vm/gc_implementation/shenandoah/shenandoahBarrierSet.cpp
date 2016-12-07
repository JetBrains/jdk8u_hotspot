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
#include "gc_implementation/shenandoah/shenandoahBarrierSet.hpp"
#include "gc_implementation/shenandoah/shenandoahHeap.inline.hpp"
#include "runtime/interfaceSupport.hpp"

class UpdateRefsForOopClosure: public ExtendedOopClosure {

private:
  ShenandoahHeap* _heap;
  template <class T>
  inline void do_oop_work(T* p) {
    _heap->maybe_update_oop_ref(p);
  }
public:
  UpdateRefsForOopClosure() {
    _heap = ShenandoahHeap::heap();
  }

  void do_oop(oop* p)       {
    do_oop_work(p);
  }

  void do_oop(narrowOop* p) {
    do_oop_work(p);
  }

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
  return _heap->concurrent_mark_in_progress() && _heap->need_update_refs();
}

void ShenandoahBarrierSet::write_ref_array_work(MemRegion r) {
  ShouldNotReachHere();
}

void ShenandoahBarrierSet::write_ref_array(HeapWord* start, size_t count) {
  if (! need_update_refs_barrier()) return;
  if (UseCompressedOops) {
    narrowOop* dst = (narrowOop*) start;
    for (size_t i = 0; i < count; i++, dst++) {
      _heap->maybe_update_oop_ref(dst);
    }
  } else {
    oop* dst = (oop*) start;
    for (size_t i = 0; i < count; i++, dst++) {
      _heap->maybe_update_oop_ref(dst);
    }
  }
}

template <class T>
void ShenandoahBarrierSet::write_ref_array_pre_work(T* dst, int count) {

#ifdef ASSERT
    if (_heap->is_in(dst) &&
        _heap->in_collection_set(dst) &&
        ! _heap->cancelled_concgc()) {
      tty->print_cr("dst = "PTR_FORMAT, p2i(dst));
      _heap->heap_region_containing((HeapWord*) dst)->print();
      assert(false, "We should have fixed this earlier");
    }
#endif

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
  if (! dest_uninitialized) {
    write_ref_array_pre_work(dst, count);
  }
}

void ShenandoahBarrierSet::write_ref_array_pre(narrowOop* dst, int count, bool dest_uninitialized) {
  if (! dest_uninitialized) {
    write_ref_array_pre_work(dst, count);
  }
}

template <class T>
void ShenandoahBarrierSet::write_ref_field_pre_static(T* field, oop newVal) {
  T heap_oop = oopDesc::load_heap_oop(field);

#ifdef ASSERT
  ShenandoahHeap* heap = ShenandoahHeap::heap();
    if (heap->is_in(field) &&
        heap->in_collection_set(field) &&
        ! heap->cancelled_concgc()) {
      tty->print_cr("field = "PTR_FORMAT, p2i(field));
      tty->print_cr("in_cset: %s", BOOL_TO_STR(heap->in_collection_set(field)));
      heap->heap_region_containing((HeapWord*)field)->print();
      tty->print_cr("marking: %s, evacuating: %s",
                    BOOL_TO_STR(heap->concurrent_mark_in_progress()),
                    BOOL_TO_STR(heap->is_evacuation_in_progress()));
      assert(false, "We should have fixed this earlier");
    }
#endif

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
#ifdef ASSERT
  ShenandoahHeap* heap = ShenandoahHeap::heap();
  if (!(heap->cancelled_concgc() || !heap->in_collection_set(v))) {
    tty->print_cr("field not in collection set: "PTR_FORMAT, p2i(v));
    tty->print_cr("containing heap region:");
    ShenandoahHeap::heap()->heap_region_containing(v)->print();
  }
  assert(heap->cancelled_concgc() || !heap->in_collection_set(v), "only write to to-space");
  if (! need_update_refs_barrier()) return;
  assert(o == NULL || oopDesc::unsafe_equals(o, resolve_oop_static(o)), "only write to-space values");
  assert(o == NULL || !heap->in_collection_set(o), "only write to-space values");
#endif
}

void ShenandoahBarrierSet::write_region_work(MemRegion mr) {

  if (! need_update_refs_barrier()) return;

  // This is called for cloning an object (see jvm.cpp) after the clone
  // has been made. We are not interested in any 'previous value' because
  // it would be NULL in any case. But we *are* interested in any oop*
  // that potentially need to be updated.

  oop obj = oop(mr.start());
  assert(obj->is_oop(), "must be an oop");
  UpdateRefsForOopClosure cl;
  obj->oop_iterate(&cl);
}

oop ShenandoahBarrierSet::read_barrier(oop src) {
  return ShenandoahBarrierSet::resolve_oop_static(src);
}

bool ShenandoahBarrierSet::obj_equals(oop obj1, oop obj2) {
  bool eq = oopDesc::unsafe_equals(obj1, obj2);
  if (! eq) {
    OrderAccess::loadload();
    obj1 = resolve_oop_static(obj1);
    obj2 = resolve_oop_static(obj2);
    eq = oopDesc::unsafe_equals(obj1, obj2);
  }
  return eq;
}

bool ShenandoahBarrierSet::obj_equals(narrowOop obj1, narrowOop obj2) {
  return obj_equals(oopDesc::decode_heap_oop(obj1), oopDesc::decode_heap_oop(obj2));
}

#ifdef ASSERT
bool ShenandoahBarrierSet::is_safe(oop o) {
  if (o == NULL) return true;
  if (_heap->in_collection_set(o)) {
    return false;
  }
  if (! oopDesc::unsafe_equals(o, read_barrier(o))) {
    return false;
  }
  return true;
}

bool ShenandoahBarrierSet::is_safe(narrowOop o) {
  return is_safe(oopDesc::decode_heap_oop(o));
}
#endif

oop ShenandoahBarrierSet::resolve_and_maybe_copy_oop_work(oop src) {
  assert(src != NULL, "only evacuated non NULL oops");

  if (_heap->in_collection_set(src)) {
    return resolve_and_maybe_copy_oop_work2(src);
  } else {
    return src;
  }
}

oop ShenandoahBarrierSet::resolve_and_maybe_copy_oop_work2(oop src) {
  assert(src != NULL, "only evacuated non NULL oops");
  assert(_heap->in_collection_set(src), "only evacuate objects in collection set");
  assert(! _heap->heap_region_containing(src)->is_humongous(), "never evacuate humongous objects");
  // TODO: Consider passing thread from caller.
  oop dst = _heap->evacuate_object(src, Thread::current());

  log_develop_trace(gc, compaction)("src = "PTR_FORMAT" dst = "PTR_FORMAT" src-2 = "PTR_FORMAT,
                                    p2i(src), p2i(dst), p2i(((HeapWord*) src) - 2));

  assert(_heap->is_in(dst), "result should be in the heap");
  return dst;
}

oop ShenandoahBarrierSet::resolve_and_maybe_copy_oopHelper(oop src) {
  assert(src != NULL, "checked before");
  bool evac = _heap->is_evacuation_in_progress();
  OrderAccess::loadload();
  src = resolve_oop_static_not_null(src);
  if (! evac) {
    return src;
  } else {
    return resolve_and_maybe_copy_oop_work(src);
  }
}

JRT_LEAF(oopDesc*, ShenandoahBarrierSet::write_barrier_c2(oopDesc* src))
  oop result = ((ShenandoahBarrierSet*) oopDesc::bs())->resolve_and_maybe_copy_oop_work2(src);
  return (oopDesc*) result;
JRT_END

oop ShenandoahBarrierSet::write_barrier(oop src) {
  if (! oopDesc::is_null(src)) {
    assert(_heap->is_in(src), "sanity");
    assert(src != NULL, "checked before");
    oop result = resolve_and_maybe_copy_oopHelper(src);
    assert(_heap->is_in(result) /*&& result->is_oop()*/, "resolved oop must be NULL, or a valid oop in the heap");
    return result;
  } else {
    return NULL;
  }
}

#ifdef ASSERT
void ShenandoahBarrierSet::verify_safe_oop(oop p) {
  ShenandoahHeap* heap = ShenandoahHeap::heap();
  if (p == NULL) return;
  if (heap->in_collection_set(p) &&
      ! heap->cancelled_concgc()) {
    tty->print_cr("oop = "PTR_FORMAT", resolved: "PTR_FORMAT", marked-next %s, marked-complete: %s",
                  p2i(p),
                  p2i(read_barrier(p)),
                  BOOL_TO_STR(heap->is_marked_next(p)),
                  BOOL_TO_STR(heap->is_marked_complete(p)));
    tty->print_cr("in_cset: %s", BOOL_TO_STR(heap->in_collection_set(p)));
    heap->heap_region_containing((HeapWord*) p)->print();
    tty->print_cr("top-at-mark-start: %p", heap->next_top_at_mark_start((HeapWord*) p));
    tty->print_cr("top-at-prev-mark-start: %p", heap->complete_top_at_mark_start((HeapWord*) p));
    tty->print_cr("marking: %s, evacuating: %s", BOOL_TO_STR(heap->concurrent_mark_in_progress()), BOOL_TO_STR(heap->is_evacuation_in_progress()));
    assert(false, "We should have fixed this earlier");
  }
}
#endif
