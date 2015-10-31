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

#ifndef SHARE_VM_GC_SHENANDOAH_SHENANDOAHCONCURRENTMARK_INLINE_HPP
#define SHARE_VM_GC_SHENANDOAH_SHENANDOAHCONCURRENTMARK_INLINE_HPP

#include "gc_implementation/shenandoah/brooksPointer.hpp"
#include "gc_implementation/shenandoah/shenandoahBarrierSet.inline.hpp"
#include "gc_implementation/shenandoah/shenandoahConcurrentMark.hpp"
#include "memory/iterator.inline.hpp"
#include "oops/oop.inline.hpp"
#include "runtime/prefetch.inline.hpp"

template <class T>
void ShenandoahMarkObjsClosure<T>::do_object(oop obj, int index) {

  assert(obj != NULL, "expect non-null object");

  assert(obj == ShenandoahBarrierSet::resolve_oop_static_not_null(obj), "expect forwarded obj in queue");

#ifdef ASSERT
  if (_heap->heap_region_containing(obj)->is_in_collection_set()) {
    tty->print_cr("trying to mark obj: "PTR_FORMAT" (%s) in dirty region: ", p2i((HeapWord*) obj), BOOL_TO_STR(_heap->is_marked_current(obj)));
    //      _heap->heap_region_containing(obj)->print();
    //      _heap->print_heap_regions();
  }
#endif
  assert(_heap->cancelled_concgc()
         || ! _heap->heap_region_containing(obj)->is_in_collection_set(),
         "we don't want to mark objects in from-space");
  assert(_heap->is_in(obj), "referenced objects must be in the heap. No?");
  assert(_heap->is_marked_current(obj), "only marked objects on task queue");

  // Calculate liveness of heap region containing object.
  if (index == -1) { // Normal oop
    uint region_idx = _heap->heap_region_index_containing(obj);
    if (region_idx == _last_region_idx) {
      _live_data += (obj->size() + BrooksPointer::BROOKS_POINTER_OBJ_SIZE) * HeapWordSize;
    } else {
      ShenandoahHeapRegion* r = _heap->heap_regions()[_last_region_idx];
      r->increase_live_data(_live_data);
      _last_region_idx = region_idx;
      _live_data = (obj->size() + BrooksPointer::BROOKS_POINTER_OBJ_SIZE) * HeapWordSize;
    }
    obj->oop_iterate(&_mark_refs);
  } else { // Chunked obj array processing
    assert(obj->is_objArray(), "expect object array");
    objArrayOop array = objArrayOop(obj);
    const int len = array->length();
    const int beg_index = index;
    assert(beg_index < len, "index too large");
    const int stride = MIN2(len - beg_index, (int) ObjArrayMarkingStride);
    const int end_index = beg_index + stride;
    // tty->print_cr("strided obj array scan: %p, %d -> %d", array, beg_index, end_index);
    // Now scan our stride
    array->oop_iterate_range(&_mark_refs, beg_index, end_index);
    // Push continuation
    if (end_index < len) {
      bool pushed = _queue->queue()->push(ObjArrayTask(obj, end_index));
      assert(pushed, "overflow queue should always succeed pushing");
    }
  }
}

template <class T>
inline bool ShenandoahConcurrentMark::try_queue(SCMObjToScanQueue* q, ShenandoahMarkObjsClosure<T>* cl) {
  ObjArrayTask task;
  if (q->pop_local(task)) {
    assert(task.obj() != NULL, "Can't mark null");
    cl->do_object(task.obj(), task.index());
    return true;
  } else if (q->pop_overflow(task)) {
    cl->do_object(task.obj(), task.index());
    return true;
  } else {
    return false;
  }
}

template <class T>
inline bool ShenandoahConcurrentMark::try_to_steal(uint worker_id, ShenandoahMarkObjsClosure<T>* cl, int *seed) {
  ObjArrayTask task;
  if (task_queues()->steal(worker_id, seed, task)) {
    cl->do_object(task.obj(), task.index());
    return true;
  } else
    return false;
}

class ShenandoahSATBBufferClosure : public SATBBufferClosure {
private:
  SCMObjToScanQueue* _queue;
  ShenandoahHeap* _heap;
public:
  ShenandoahSATBBufferClosure(SCMObjToScanQueue* q) :
    _queue(q), _heap(ShenandoahHeap::heap())
  {
  }

  void do_buffer(void** buffer, size_t size) {
    // tty->print_cr("draining one satb buffer");
    for (size_t i = 0; i < size; ++i) {
      void* entry = buffer[i];
      oop obj = oop(entry);
      // tty->print_cr("satb buffer entry: "PTR_FORMAT, p2i((HeapWord*) obj));
      if (!oopDesc::is_null(obj)) {
        obj = ShenandoahBarrierSet::resolve_oop_static_not_null(obj);
        ShenandoahConcurrentMark::mark_and_push(obj, _heap, _queue);
      }
    }
  }
};

inline bool ShenandoahConcurrentMark:: try_draining_an_satb_buffer(SCMObjToScanQueue* q) {
  ShenandoahSATBBufferClosure cl(q);
  SATBMarkQueueSet& satb_mq_set = JavaThread::satb_mark_queue_set();
  return satb_mq_set.apply_closure_to_completed_buffer(&cl);
}

inline void ShenandoahConcurrentMark::mark_and_push(oop obj, ShenandoahHeap* heap, SCMObjToScanQueue* q) {
  if (heap->mark_current(obj)) {
    if (ShenandoahTraceConcurrentMarking) {
      tty->print_cr("marked obj: "PTR_FORMAT, p2i((HeapWord*) obj));
    }
    if (obj->is_typeArray()) { // No references. Skip it.
      count_liveness(obj, heap);
      return;
    } else if (obj->is_objArray()) {
      /*
      oop cld = obj->klass()->klass_holder();
      if (cld != NULL) {
        q->push(ObjArrayTask(cld, -1));
      }
      */
      count_liveness(obj, heap);
      if (objArrayOop(obj)->length() > 0) {
        bool pushed = q->push(ObjArrayTask(obj, 0));
        assert(pushed, "overflow queue should always succeed pushing");
      }
    } else {
      bool pushed = q->push(ObjArrayTask(obj, -1));
      assert(pushed, "overflow queue should always succeed pushing");
    }
  }
#ifdef ASSERT
  else {
    if (ShenandoahTraceConcurrentMarking) {
      tty->print_cr("failed to mark obj (already marked): "PTR_FORMAT, p2i((HeapWord*) obj));
    }
    assert(heap->is_marked_current(obj), "make sure object is marked");
  }
#endif
}

inline void ShenandoahConcurrentMark::count_liveness(oop obj, ShenandoahHeap* heap) {

  size_t live = (obj->size() + BrooksPointer::BROOKS_POINTER_OBJ_SIZE) * HeapWordSize;
  ShenandoahHeapRegion* r = heap->heap_region_containing(obj);
  r->increase_live_data(live);
}

#endif // SHARE_VM_GC_SHENANDOAH_SHENANDOAHCONCURRENTMARK_INLINE_HPP
