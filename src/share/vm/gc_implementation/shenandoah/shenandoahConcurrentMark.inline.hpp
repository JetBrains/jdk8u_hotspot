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
#include "gc_implementation/shenandoah/shenandoahTaskqueue.inline.hpp"
#include "memory/iterator.inline.hpp"
#include "oops/oop.inline.hpp"
#include "runtime/prefetch.inline.hpp"

template <class T, bool CL>
void ShenandoahMarkObjsClosure<T, CL>::do_object_or_array(oop obj, int from, int to) {
  assert(obj != NULL, "expect non-null object");

  assert(oopDesc::unsafe_equals(obj, ShenandoahBarrierSet::resolve_oop_static_not_null(obj)), "expect forwarded obj in queue");

#ifdef ASSERT
  if (! oopDesc::bs()->is_safe(obj)) {
    tty->print_cr("trying to mark obj: "PTR_FORMAT" (%s) in dirty region: ", p2i((HeapWord*) obj), BOOL_TO_STR(_heap->is_marked_next(obj)));
    //      _heap->heap_region_containing(obj)->print();
    //      _heap->print_heap_regions();
  }
#endif
  assert(_heap->cancelled_concgc()
         || oopDesc::bs()->is_safe(obj),
         "we don't want to mark objects in from-space");
  assert(_heap->is_in(obj), "referenced objects must be in the heap. No?");
  assert(_heap->is_marked_next(obj), "only marked objects on task queue");

  if (from == -1) {
    count_liveness(obj);
    if (obj->is_objArray()) {
      // Case 1: Array instance and no task bounds set. Must be the first time
      // we visit it. Process its metadata, and submit the chunked array task
      // with proper bounds.
      _mark_refs.do_klass(obj->klass());
      objArrayOop array = objArrayOop(obj);
      if (array->length() > 0) {
        do_array(array, 0, array->length());
      }
    } else {
     // Case 2: Normal oop, process as usual.
      obj->oop_iterate(&_mark_refs);
    }
  } else {
    // Case 3: Array chunk, has sensible (from, to) bounds. Process it.
    assert(obj->is_objArray(), "expect object array");
    objArrayOop array = objArrayOop(obj);
    do_array(array, from, to);
  }
}

template <class T, bool CL>
inline void ShenandoahMarkObjsClosure<T, CL>::count_liveness(oop obj) {
  if (!CL) return; // no need to count liveness!
  uint region_idx = _heap->heap_region_index_containing(obj);
  if (region_idx == _last_region_idx) {
    _live_data += (obj->size() + BrooksPointer::word_size()) * HeapWordSize;
  } else {
    ShenandoahHeapRegion* r = _heap->regions()->get(_last_region_idx);
    r->increase_live_data(_live_data);
    _last_region_idx = region_idx;
    _live_data = (obj->size() + BrooksPointer::word_size()) * HeapWordSize;
  }
}

template <class T, bool CL>
inline void ShenandoahMarkObjsClosure<T, CL>::do_array(objArrayOop array, int from, int to) {
  assert (from < to, "sanity");
  assert (ObjArrayMarkingStride > 0, "sanity");

  // Fork out tasks until we hit the leaf task. Larger tasks would go to the
  // "stealing" part of the queue, which will seed other workers efficiently.
  while ((to - from) > (int)ObjArrayMarkingStride) {
    int mid = from + (to - from) / 2;
    bool pushed = _queue->push(ObjArrayFromToTask(array, mid, to));
    assert(pushed, "overflow queue should always succeed pushing");
    to = mid;
  }

  // Execute the leaf task
  array->oop_iterate_range(&_mark_refs, from, to);
}

template <class T, bool CL>
inline bool ShenandoahConcurrentMark::try_queue(SCMObjToScanQueue* q, ShenandoahMarkObjsClosure<T, CL>* cl) {
  ObjArrayFromToTask task;
  if (q->pop_buffer(task) ||
          q->pop_local(task) ||
          q->pop_overflow(task)) {
    assert(task.obj() != NULL, "Can't mark null");
    cl->do_object_or_array(task.obj(), task.from(), task.to());
    return true;
  } else {
    return false;
  }
}

template <class T, bool CL>
inline bool ShenandoahConcurrentMark::try_to_steal(uint worker_id, ShenandoahMarkObjsClosure<T, CL>* cl, int *seed) {
  ObjArrayFromToTask task;
  if (task_queues()->steal(worker_id, seed, task)) {
    cl->do_object_or_array(task.obj(), task.from(), task.to());
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
    for (size_t i = 0; i < size; ++i) {
      void* entry = buffer[i];
      oop obj = oop(entry);
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
#ifdef ASSERT
  if (! oopDesc::bs()->is_safe(obj)) {
    tty->print_cr("obj in cset: %s, obj: "PTR_FORMAT", forw: "PTR_FORMAT,
                  BOOL_TO_STR(heap->in_collection_set(obj)),
                  p2i(obj),
                  p2i(ShenandoahBarrierSet::resolve_oop_static_not_null(obj)));
    heap->heap_region_containing((HeapWord*) obj)->print();
  }
#endif
  assert(oopDesc::bs()->is_safe(obj), "no ref in cset");
  assert(Universe::heap()->is_in(obj), err_msg("We shouldn't be calling this on objects not in the heap: "PTR_FORMAT, p2i(obj)));
  if (heap->mark_next(obj)) {
#ifdef ASSERT
    log_develop_trace(gc, marking)("marked obj: "PTR_FORMAT, p2i((HeapWord*) obj));

    if (! oopDesc::bs()->is_safe(obj)) {
       tty->print_cr("trying to mark obj: "PTR_FORMAT" (%s) in dirty region: ", p2i((HeapWord*) obj), BOOL_TO_STR(heap->is_marked_next(obj)));
      //      _heap->heap_region_containing(obj)->print();
      //      _heap->print_heap_regions();
    }
#endif
    assert(heap->cancelled_concgc()
           || oopDesc::bs()->is_safe(obj),
           "we don't want to mark objects in from-space");

    bool pushed = q->push(ObjArrayFromToTask(obj, -1, -1));
    assert(pushed, "overflow queue should always succeed pushing");

  }
#ifdef ASSERT
  else {
    log_develop_trace(gc, marking)("failed to mark obj (already marked): "PTR_FORMAT, p2i((HeapWord*) obj));
    assert(heap->is_marked_next(obj), "make sure object is marked");
  }
#endif
}

#endif // SHARE_VM_GC_SHENANDOAH_SHENANDOAHCONCURRENTMARK_INLINE_HPP
