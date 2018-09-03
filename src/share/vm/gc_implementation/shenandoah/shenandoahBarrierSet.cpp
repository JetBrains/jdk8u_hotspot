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
#include "gc_implementation/shenandoah/shenandoahHeuristics.hpp"
#include "runtime/interfaceSupport.hpp"

class ShenandoahUpdateRefsForOopClosure: public ExtendedOopClosure {
private:
  ShenandoahHeap* _heap;
  ShenandoahBarrierSet* _bs;

  template <class T>
  inline void do_oop_work(T* p) {
    _heap->maybe_update_with_forwarded(p);
  }
public:
  ShenandoahUpdateRefsForOopClosure() : _heap(ShenandoahHeap::heap()), _bs(ShenandoahBarrierSet::barrier_set()) {
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

void ShenandoahBarrierSet::write_ref_array_work(MemRegion r) {
  ShouldNotReachHere();
}

template <class T>
void ShenandoahBarrierSet::write_ref_array_loop(HeapWord* start, size_t count) {
  assert(UseShenandoahGC && ShenandoahCloneBarrier, "Should be enabled");
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

  ShenandoahEvacOOMScope oom_evac_scope;
  if (UseCompressedOops) {
    write_ref_array_loop<narrowOop>(start, count);
  } else {
    write_ref_array_loop<oop>(start, count);
  }
}

template <class T>
void ShenandoahBarrierSet::write_ref_array_pre_work(T* dst, size_t count) {
  assert (UseShenandoahGC && ShenandoahSATBBarrier, "Should be enabled");

  shenandoah_assert_not_in_cset_loc_except(dst, _heap->cancelled_gc());

  if (! JavaThread::satb_mark_queue_set().is_active()) return;
  T* elem_ptr = dst;
  for (size_t i = 0; i < count; i++, elem_ptr++) {
    T heap_oop = oopDesc::load_heap_oop(elem_ptr);
    if (!oopDesc::is_null(heap_oop)) {
      enqueue(oopDesc::decode_heap_oop_not_null(heap_oop));
    }
  }
}

void ShenandoahBarrierSet::write_ref_array_pre(oop* dst, int count, bool dest_uninitialized) {
  if (! dest_uninitialized && ShenandoahSATBBarrier) {
    write_ref_array_pre_work(dst, (size_t)count);
  }
}

void ShenandoahBarrierSet::write_ref_array_pre(narrowOop* dst, int count, bool dest_uninitialized) {
  if (! dest_uninitialized && ShenandoahSATBBarrier) {
    write_ref_array_pre_work(dst, (size_t)count);
  }
}

template <class T>
void ShenandoahBarrierSet::write_ref_field_pre_static(T* field, oop newVal) {
  T heap_oop = oopDesc::load_heap_oop(field);

  shenandoah_assert_not_in_cset_loc_except(field, ShenandoahHeap::heap()->cancelled_gc());

  if (!oopDesc::is_null(heap_oop)) {
    ShenandoahBarrierSet::barrier_set()->enqueue(oopDesc::decode_heap_oop(heap_oop));
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
  shenandoah_assert_not_in_cset_loc_except(v, _heap->cancelled_gc());
  shenandoah_assert_not_forwarded_except  (v, o, o == NULL || _heap->cancelled_gc() || !_heap->is_concurrent_mark_in_progress());
  shenandoah_assert_not_in_cset_except    (v, o, o == NULL || _heap->cancelled_gc() || !_heap->is_concurrent_mark_in_progress());
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
  shenandoah_assert_correct(NULL, obj);
  ShenandoahUpdateRefsForOopClosure cl;
  obj->oop_iterate(&cl);
}

oop ShenandoahBarrierSet::read_barrier(oop src) {
  // Check for forwarded objects, because on Full GC path we might deal with
  // non-trivial fwdptrs that contain Full GC specific metadata. We could check
  // for is_full_gc_in_progress(), but this also covers the case of stable heap,
  // which provides a bit of performance improvement.
  if (ShenandoahReadBarrier && _heap->has_forwarded_objects()) {
    return ShenandoahBarrierSet::resolve_forwarded(src);
  } else {
    return src;
  }
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
  oop result = ShenandoahBarrierSet::barrier_set()->write_barrier_mutator(src);
  return (oopDesc*) result;
JRT_END

IRT_LEAF(oopDesc*, ShenandoahBarrierSet::write_barrier_IRT(oopDesc* src))
  oop result = ShenandoahBarrierSet::barrier_set()->write_barrier_mutator(src);
  return (oopDesc*) result;
IRT_END

oop ShenandoahBarrierSet::write_barrier_mutator(oop obj) {
  assert(UseShenandoahGC && ShenandoahWriteBarrier, "should be enabled");
  assert(_heap->is_gc_in_progress_mask(ShenandoahHeap::EVACUATION), "evac should be in progress");
  shenandoah_assert_in_cset(NULL, obj);

  oop fwd = resolve_forwarded_not_null(obj);
  if (oopDesc::unsafe_equals(obj, fwd)) {
    ShenandoahEvacOOMScope oom_evac_scope;
    bool evac;

    Thread* thread = Thread::current();
    oop res_oop = _heap->evacuate_object(obj, thread, evac);

    // Since we are already here and paid the price of getting through runtime call adapters
    // and acquiring oom-scope, it makes sense to try and evacuate more adjacent objects,
    // thus amortizing the overhead. For sparsely live heaps, scan costs easily dominate
    // total assist costs, and can introduce a lot of evacuation latency. This is why we
    // only scan for _nearest_ N objects, regardless if they are eligible for evac or not.
    // The scan itself should also avoid touching the non-marked objects below TAMS, because
    // their metadata (notably, klasses) may be incorrect already.

    size_t max = ShenandoahEvacAssist;
    if (max > 0) {
      ShenandoahMarkingContext* ctx = _heap->complete_marking_context();

      ShenandoahHeapRegion* r = _heap->heap_region_containing(obj);
      assert(r->is_cset(), "sanity");

      HeapWord* cur = (HeapWord*)obj + obj->size() + BrooksPointer::word_size();

      size_t count = 0;
      while ((cur < r->top()) && ctx->is_marked(oop(cur)) && (count++ < max)) {
        oop cur_oop = oop(cur);
        if (oopDesc::unsafe_equals(cur_oop, resolve_forwarded_not_null(cur_oop))) {
          _heap->evacuate_object(cur_oop, thread, evac);
        }
        cur = cur + cur_oop->size() + BrooksPointer::word_size();
      }
    }

    return res_oop;
  }
  return fwd;
}

oop ShenandoahBarrierSet::write_barrier(oop obj) {
  if (ShenandoahWriteBarrier) {
    if (!oopDesc::is_null(obj)) {
      bool evac_in_progress = _heap->is_evacuation_in_progress();
      oop fwd = resolve_forwarded_not_null(obj);
      if (evac_in_progress &&
          _heap->in_collection_set(obj) &&
          oopDesc::unsafe_equals(obj, fwd)) {
        Thread *t = Thread::current();
        bool evac;
        if (t->is_Worker_thread()) {
          return _heap->evacuate_object(obj, t, evac);
        } else {
          ShenandoahEvacOOMScope oom_evac_scope;
          return _heap->evacuate_object(obj, t, evac);
        }
      } else {
        return fwd;
      }
    }
  }
  return obj;
}

void ShenandoahBarrierSet::enqueue(oop obj) {
  // Filter marked objects before hitting the SATB queues. The same predicate would
  // be used by SATBMQ::filter to eliminate already marked objects downstream, but
  // filtering here helps to avoid wasteful SATB queueing work to begin with.
  if (!_heap->requires_marking(obj)) return;

  G1SATBCardTableModRefBS::enqueue(obj);
}
