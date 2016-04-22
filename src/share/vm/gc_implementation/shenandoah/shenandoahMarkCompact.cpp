/*
 * Copyright (c) 2014, 2015, Red Hat, Inc. and/or its affiliates.
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

#include "code/codeCache.hpp"
#include "gc_implementation/shared/isGCActiveMark.hpp"
#include "gc_implementation/shenandoah/brooksPointer.hpp"
#include "gc_implementation/shenandoah/shenandoahCollectorPolicy.hpp"
#include "gc_implementation/shenandoah/shenandoahMarkCompact.hpp"
#include "gc_implementation/shenandoah/shenandoahBarrierSet.hpp"
#include "gc_implementation/shenandoah/shenandoahHeapRegionSet.hpp"
#include "gc_implementation/shenandoah/shenandoahHeap.hpp"
#include "gc_implementation/shenandoah/shenandoahHeap.inline.hpp"
#include "gc_implementation/shenandoah/shenandoahRootProcessor.hpp"
#include "gc_implementation/shenandoah/vm_operations_shenandoah.hpp"
#include "oops/oop.inline.hpp"
#include "runtime/biasedLocking.hpp"
#include "runtime/thread.hpp"
#include "utilities/copy.hpp"
#include "utilities/taskqueue.hpp"
#include "utilities/workgroup.hpp"

class ShenandoahMarkCompactBarrierSet : public ShenandoahBarrierSet {
public:
  ShenandoahMarkCompactBarrierSet(ShenandoahHeap* heap) : ShenandoahBarrierSet(heap) {
  }
  oop read_barrier(oop src) {
    return src;
  }
#ifdef ASSERT
  bool is_safe(oop o) {
    if (o == NULL) return true;
    if (! oopDesc::unsafe_equals(o, read_barrier(o))) {
      return false;
    }
    return true;
  }
  bool is_safe(narrowOop o) {
    Unimplemented();
    return true;
  }
#endif
};

class ClearInCollectionSetHeapRegionClosure: public ShenandoahHeapRegionClosure {
  bool doHeapRegion(ShenandoahHeapRegion* r) {
    r->set_top_at_mark_start(r->end());
    r->clearLiveData();
    r->set_concurrent_iteration_safe_limit(r->top());
    r->set_top_prev_mark_bitmap(r->top_at_mark_start());
    return false;
  }
};


void ShenandoahMarkCompact::do_mark_compact() {

  ShenandoahHeap* _heap = ShenandoahHeap::heap();

  COMPILER2_PRESENT(DerivedPointerTable::clear());

  _heap->set_full_gc_in_progress(true);

  assert(SafepointSynchronize::is_at_safepoint(), "must be at a safepoint");
  IsGCActiveMark is_active;

  assert(Thread::current()->is_VM_thread(), "Do full GC only while world is stopped");
  assert(_heap->is_bitmap_clear(), "require cleared bitmap");
  assert(!_heap->concurrent_mark_in_progress(), "can't do full-GC while marking is in progress");
  assert(!_heap->is_evacuation_in_progress(), "can't do full-GC while evacuation is in progress");

  _heap->shenandoahPolicy()->record_phase_start(ShenandoahCollectorPolicy::full_gc);

  ClearInCollectionSetHeapRegionClosure cl;
  _heap->heap_region_iterate(&cl, false, false);

  _heap->clear_cancelled_concgc();

  assert(_heap->is_bitmap_clear(), "require cleared bitmap");

  /*
  if (ShenandoahVerify) {
    // Full GC should only be called between regular concurrent cycles, therefore
    // those verifications should be valid.
    _heap->verify_heap_after_evacuation();
    _heap->verify_heap_after_update_refs();
  }
  */

  if (ShenandoahTraceFullGC) {
    tty->print_cr("Shenandoah-full-gc: start with heap used: "SIZE_FORMAT" MB", _heap->used() / M);
    tty->print_cr("Shenandoah-full-gc: phase 1: marking the heap");
    // _heap->print_heap_regions();
  }

  if (UseTLAB) {
    _heap->ensure_parsability(true);
  }

  CodeCache::gc_prologue();

  // We should save the marks of the currently locked biased monitors.
  // The marking doesn't preserve the marks of biased objects.
  //BiasedLocking::preserve_marks();

  BarrierSet* old_bs = oopDesc::bs();
  ShenandoahMarkCompactBarrierSet bs(_heap);
  oopDesc::set_bs(&bs);

  OrderAccess::fence();

  phase1_mark_heap();

  OrderAccess::fence();

  if (ShenandoahTraceFullGC) {
    tty->print_cr("Shenandoah-full-gc: phase 2: calculating target addresses");
  }
  ShenandoahHeapRegionSet* copy_queues[_heap->max_parallel_workers()];
  phase2_calculate_target_addresses(copy_queues);

  OrderAccess::fence();

  if (ShenandoahTraceFullGC) {
    tty->print_cr("Shenandoah-full-gc: phase 3: updating references");
  }

  // Don't add any more derived pointers during phase3
  COMPILER2_PRESENT(DerivedPointerTable::set_active(false));

  phase3_update_references();

  if (ShenandoahTraceFullGC) {
    tty->print_cr("Shenandoah-full-gc: phase 4: compacting objects");
  }

  phase4_compact_objects(copy_queues);

  CodeCache::gc_epilogue();
  JvmtiExport::gc_epilogue();

  // refs processing: clean slate
  // rp.enqueue_discovered_references();

  if (ShenandoahVerify) {
    _heap->verify_heap_after_evacuation();
  }

  if (ShenandoahTraceFullGC) {
    tty->print_cr("Shenandoah-full-gc: finish with heap used: "SIZE_FORMAT" MB", _heap->used() / M);
  }

  _heap->reset_mark_bitmap();
  _heap->_bytesAllocSinceCM = 0;

  _heap->set_need_update_refs(false);

  _heap->set_full_gc_in_progress(false);

  COMPILER2_PRESENT(DerivedPointerTable::update_pointers());

  _heap->shenandoahPolicy()->record_phase_end(ShenandoahCollectorPolicy::full_gc);

  oopDesc::set_bs(old_bs);
}

#ifdef ASSERT
class VerifyNotForwardedPointersClosure : public MetadataAwareOopClosure {
  void do_oop(oop* p) {
    oop obj = oopDesc::load_heap_oop(p);
    if (! oopDesc::is_null(obj)) {
      assert(oopDesc::unsafe_equals(obj, ShenandoahBarrierSet::resolve_oop_static_not_null(obj)),
             "expect forwarded oop");
      ShenandoahHeap* heap = ShenandoahHeap::heap();
      if (! heap->is_marked_current(obj)) {
        tty->print_cr("ref region humongous? %s", BOOL_TO_STR(heap->heap_region_containing(p)->is_humongous()));
      }
      assert(heap->is_marked_current(obj), "must be marked");
      assert(! heap->allocated_after_mark_start((HeapWord*) obj), "must be truly marked");
    }
  }
  void do_oop(narrowOop* p) {
    Unimplemented();
  }
};

class ShenandoahMCVerifyAfterMarkingObjectClosure : public ObjectClosure {
  void do_object(oop p) {
    ShenandoahHeap* heap = ShenandoahHeap::heap();
    assert(oopDesc::unsafe_equals(p, ShenandoahBarrierSet::resolve_oop_static_not_null(p)),
           "expect forwarded oop");
    assert(heap->is_marked_current(p), "must be marked");
    assert(! heap->allocated_after_mark_start((HeapWord*) p), "must be truly marked");
    VerifyNotForwardedPointersClosure cl;
    p->oop_iterate(&cl);
  }
};

class ShenandoahMCVerifyAfterMarkingRegionClosure : public ShenandoahHeapRegionClosure {
  bool doHeapRegion(ShenandoahHeapRegion* r) {
    ShenandoahMCVerifyAfterMarkingObjectClosure cl;
    if (! r->is_humongous_continuation()) {
      r->marked_object_iterate(&cl);
    }
    return false;
  }
};

class ShenandoahMCVerifyBeforeMarkingObjectClosure : public ObjectClosure {
public:
  bool marked;
  ShenandoahMCVerifyBeforeMarkingObjectClosure() : ObjectClosure(), marked(false) {
  }
  void do_object(oop p) {
    marked = true;
  }
};

class ShenandoahMCVerifyBeforeMarkingRegionClosure : public ShenandoahHeapRegionClosure {
public:
  bool doHeapRegion(ShenandoahHeapRegion* r) {
    ShenandoahMCVerifyBeforeMarkingObjectClosure cl;
    if (! r->is_humongous_continuation()) {
      r->marked_object_iterate(&cl);
    }
    assert(! cl.marked, "must not see marked objects");
    return false;
  }
};

#endif

void ShenandoahMarkCompact::phase1_mark_heap() {
  ShenandoahHeap* _heap = ShenandoahHeap::heap();

#ifdef ASSERT
  ShenandoahMCVerifyBeforeMarkingRegionClosure cl1;
  _heap->heap_region_iterate(&cl1);
#endif

  ShenandoahConcurrentMark* cm = _heap->concurrentMark();

  cm->prepare_unmarked_root_objs_no_derived_ptrs(true);
  cm->shared_finish_mark_from_roots();

  if (VerifyDuringGC) {
    HandleMark hm;  // handle scope
    COMPILER2_PRESENT(DerivedPointerTableDeactivate dpt_deact);
    //    Universe::heap()->prepare_for_verify();
    _heap->prepare_for_verify();
    // Note: we can verify only the heap here. When an object is
    // marked, the previous value of the mark word (including
    // identity hash values, ages, etc) is preserved, and the mark
    // word is set to markOop::marked_value - effectively removing
    // any hash values from the mark word. These hash values are
    // used when verifying the dictionaries and so removing them
    // from the mark word can make verification of the dictionaries
    // fail. At the end of the GC, the original mark word values
    // (including hash values) are restored to the appropriate
    // objects.
    //    Universe::heap()->verify(VerifySilently, VerifyOption_G1UseMarkWord);
    _heap->verify(true, VerifyOption_G1UseMarkWord);
  }

#ifdef ASSERT
  ShenandoahMCVerifyAfterMarkingRegionClosure cl;
  _heap->heap_region_iterate(&cl);
#endif
}

class ShenandoahPrepareForCompactionObjectClosure : public ObjectClosure {

private:

  ShenandoahHeap* _heap;
  ShenandoahHeapRegionSet* _to_regions;
  ShenandoahHeapRegion* _to_region;
  ShenandoahHeapRegion* _from_region;
  HeapWord* _compact_point;

public:

  ShenandoahPrepareForCompactionObjectClosure(ShenandoahHeapRegionSet* to_regions, ShenandoahHeapRegion* to_region) :
    _heap(ShenandoahHeap::heap()),
    _to_regions(to_regions),
    _to_region(to_region),
    _from_region(NULL),
    _compact_point(to_region->bottom()) {
  }

  void set_from_region(ShenandoahHeapRegion* from_region) {
    _from_region = from_region;
  }

  ShenandoahHeapRegion* to_region() const {
    return _to_region;
  }
  HeapWord* compact_point() const {
    return _compact_point;
  }
  void do_object(oop p) {
    assert(_from_region != NULL, "must set before work");
    assert(_heap->is_marked_current(p), "must be marked");
    assert(! _heap->allocated_after_mark_start((HeapWord*) p), "must be truly marked");
    size_t size = p->size();
    size_t obj_size = size + BrooksPointer::BROOKS_POINTER_OBJ_SIZE;
    if (_compact_point + obj_size > _to_region->end()) {
      // Object doesn't fit. Pick next to-region and start compacting there.
      _to_region->set_new_top(_compact_point);
      ShenandoahHeapRegion* new_to_region = _to_regions->next();
      if (new_to_region == NULL) {
        new_to_region = _from_region;
      }
      assert(new_to_region != _to_region, "must not reuse same to-region");
      assert(new_to_region != NULL, "must not be NULL");
      _to_region = new_to_region;
      _compact_point = _to_region->bottom();
    }
    assert(_compact_point + obj_size <= _to_region->end(), "must fit");
    // tty->print_cr("forwarding %p to %p", p, _compact_point + BrooksPointer::BROOKS_POINTER_OBJ_SIZE);
    assert(oopDesc::unsafe_equals(p, ShenandoahBarrierSet::resolve_oop_static_not_null(p)),
           "expect forwarded oop");
    BrooksPointer::get(p).set_forwardee(oop(_compact_point + BrooksPointer::BROOKS_POINTER_OBJ_SIZE));
    _compact_point += obj_size;
  }
};

class ShenandoahPrepareForCompactionTask : public AbstractGangTask {
private:

  ShenandoahHeapRegionSet** _copy_queues;
  ShenandoahHeapRegionSet* _from_regions;

  ShenandoahHeapRegion* next_from_region(ShenandoahHeapRegionSet* copy_queue) {
    ShenandoahHeapRegion* from_region = _from_regions->claim_next();
    while (from_region != NULL && from_region->is_humongous()) {
      from_region = _from_regions->claim_next();
    }
    if (from_region != NULL) {
      assert(copy_queue != NULL, "sanity");
      assert(! from_region->is_humongous(), "must not get humongous regions here");
      copy_queue->add_region(from_region);
    }
    return from_region;
  }

public:
  ShenandoahPrepareForCompactionTask(ShenandoahHeapRegionSet* from_regions, ShenandoahHeapRegionSet** copy_queues) :
    AbstractGangTask("Shenandoah Prepare For Compaction Task"),
    _from_regions(from_regions), _copy_queues(copy_queues) {
  }

  void work(uint worker_id) {
    ShenandoahHeapRegionSet* copy_queue = _copy_queues[worker_id];
    ShenandoahHeapRegion* from_region = next_from_region(copy_queue);
    if (from_region == NULL) return;
    ShenandoahHeapRegionSet* to_regions = new ShenandoahHeapRegionSet(ShenandoahHeap::heap()->max_regions());
    ShenandoahPrepareForCompactionObjectClosure cl(to_regions, from_region);
    while (from_region != NULL) {
      assert(from_region != NULL, "sanity");
      cl.set_from_region(from_region);
      from_region->marked_object_iterate(&cl);
      if (from_region != cl.to_region()) {
        assert(from_region != NULL, "sanity");
        to_regions->add_region(from_region);
      }
      from_region = next_from_region(copy_queue);
    }
    assert(cl.to_region() != NULL, "should not happen");
    cl.to_region()->set_new_top(cl.compact_point());
    while (to_regions->count() > 0) {
      ShenandoahHeapRegion* r = to_regions->next();
      if (r == NULL) {
        to_regions->print();
      }
      assert(r != NULL, "should not happen");
      r->set_new_top(r->bottom());
    }
    delete to_regions;
  }
};

void ShenandoahMarkCompact::phase2_calculate_target_addresses(ShenandoahHeapRegionSet** copy_queues) {
  ShenandoahHeap* heap = ShenandoahHeap::heap();

  // Initialize copy queues.
  for (int i = 0; i < heap->max_parallel_workers(); i++) {
    copy_queues[i] = new ShenandoahHeapRegionSet(heap->max_regions());
  }

  ShenandoahHeapRegionSet* from_regions = heap->regions();
  from_regions->clear_current_index();
  ShenandoahPrepareForCompactionTask prepare_task(from_regions, copy_queues);
  heap->workers()->run_task(&prepare_task);
}

class ShenandoahAdjustPointersClosure : public MetadataAwareOopClosure {
private:
  ShenandoahHeap* _heap;

public:

  ShenandoahAdjustPointersClosure() : _heap(ShenandoahHeap::heap()) {
  }

  void do_oop(oop* p) {
    oop obj = oopDesc::load_heap_oop(p);
    if (! oopDesc::is_null(obj)) {
      assert(_heap->is_marked_current(obj), "must be marked");
      oop forw = oop(BrooksPointer::get(obj).get_forwardee());
      oopDesc::store_heap_oop(p, forw);
    }
  }
  void do_oop(narrowOop* p) {
    Unimplemented();
  }
};

class ShenandoahAdjustPointersObjectClosure : public ObjectClosure {
private:
  ShenandoahAdjustPointersClosure* _cl;
  ShenandoahHeap* _heap;
public:
  ShenandoahAdjustPointersObjectClosure(ShenandoahAdjustPointersClosure* cl) :
    _cl(cl), _heap(ShenandoahHeap::heap()) {
  }
  void do_object(oop p) {
    assert(_heap->is_marked_current(p), "must be marked");
    p->oop_iterate(_cl);
  }
};

class ShenandoahAdjustPointersTask : public AbstractGangTask {
private:
  ShenandoahHeapRegionSet* _regions;
public:

  ShenandoahAdjustPointersTask(ShenandoahHeapRegionSet* regions) :
    AbstractGangTask("Shenandoah Adjust Pointers Task"),
    _regions(regions) {
  }

  void work(uint worker_id) {
    ShenandoahHeapRegion* r = _regions->claim_next();
    ShenandoahAdjustPointersClosure cl;
    ShenandoahAdjustPointersObjectClosure obj_cl(&cl);
    while (r != NULL) {
      if (! r->is_humongous_continuation()) {
        r->marked_object_iterate(&obj_cl);
      }
      r = _regions->claim_next();
    }
  }
};

class ShenandoahAdjustRootPointersTask : public AbstractGangTask {
private:
  ShenandoahRootProcessor* _rp;

public:

  ShenandoahAdjustRootPointersTask(ShenandoahRootProcessor* rp) :
    AbstractGangTask("Shenandoah Adjust Root Pointers Task"),
    _rp(rp) {
  }

  void work(uint worker_id) {
    ShenandoahAdjustPointersClosure cl;
    CLDToOopClosure adjust_cld_closure(&cl, true);
    CodeBlobToOopClosure adjust_code_closure(&cl,
                                             CodeBlobToOopClosure::FixRelocations);

    _rp->process_all_roots(&cl,
                           &adjust_cld_closure,
                           &adjust_code_closure);
  }
};

void ShenandoahMarkCompact::phase3_update_references() {
  ShenandoahHeap* heap = ShenandoahHeap::heap();

    // Need cleared claim bits for the roots processing
  ClassLoaderDataGraph::clear_claimed_marks();

  {
    heap->set_par_threads(heap->max_parallel_workers());
    ShenandoahRootProcessor rp(heap, heap->max_parallel_workers());
    ShenandoahAdjustRootPointersTask task(&rp);
    heap->workers()->run_task(&task);
    heap->set_par_threads(0);
  }

  // Now adjust pointers in remaining weak roots.  (All of which should
  // have been cleared if they pointed to non-surviving objects.)
  ShenandoahAdjustPointersClosure cl;
  heap->ref_processor()->weak_oops_do(&cl);

  ShenandoahAlwaysTrueClosure always_true;
  JNIHandles::weak_oops_do(&always_true, &cl);

  ShenandoahHeapRegionSet* regions = heap->regions();
  regions->clear_current_index();
  ShenandoahAdjustPointersTask adjust_pointers_task(regions);
  heap->workers()->run_task(&adjust_pointers_task);
}

class ShenandoahCompactObjectsClosure : public ObjectClosure {
private:
  ShenandoahHeap* _heap;
public:
  ShenandoahCompactObjectsClosure() : _heap(ShenandoahHeap::heap()) {
  }
  void do_object(oop p) {
    assert(_heap->is_marked_current(p), "must be marked");
    size_t size = p->size();
    HeapWord* compact_to = BrooksPointer::get(p).get_forwardee();
    HeapWord* compact_from = (HeapWord*) p;
    if (compact_from != compact_to) {
      Copy::aligned_conjoint_words(compact_from, compact_to, size);
    }
    oop new_obj = oop(compact_to);
    // new_obj->init_mark();
    _heap->initialize_brooks_ptr(new_obj);
  }
};

class ShenandoahCompactObjectsTask : public AbstractGangTask {
  ShenandoahHeapRegionSet** _regions;
public:
  ShenandoahCompactObjectsTask(ShenandoahHeapRegionSet** regions) :
    AbstractGangTask("Shenandoah Compact Objects Task"),
    _regions(regions) {
  }
  void work(uint worker_id) {
    ShenandoahHeapRegionSet* copy_queue = _regions[worker_id];
    copy_queue->clear_current_index();
    ShenandoahCompactObjectsClosure cl;
    ShenandoahHeapRegion* r = copy_queue->next();
    while (r != NULL) {
      assert(! r->is_humongous(), "must not get humongous regions here");
      r->marked_object_iterate(&cl);
      r->set_top(r->new_top());
      r = copy_queue->next();
    }
  }
};

class ShenandoahPostCompactClosure : public ShenandoahHeapRegionClosure {
  size_t _live;
  ShenandoahHeap* _heap;
public:

  ShenandoahPostCompactClosure() : _live(0), _heap(ShenandoahHeap::heap()) {
    _heap->clear_free_regions();
  }

  bool doHeapRegion(ShenandoahHeapRegion* r) {
    r->reset_top_at_prev_mark_start();
    r->set_is_in_collection_set(false);
    if (r->is_humongous()) {
      if (r->is_humongous_start()) {
        oop humongous_obj = oop(r->bottom() + BrooksPointer::BROOKS_POINTER_OBJ_SIZE);
        if (! _heap->is_marked_current(humongous_obj)) {
          _heap->reclaim_humongous_region_at(r);
        } else {
          _live += ShenandoahHeapRegion::RegionSizeBytes;
        }
      } else {
        _live += ShenandoahHeapRegion::RegionSizeBytes;
      }

    } else {
      size_t live = r->used();
      if (live == 0) {
        r->recycle();
        _heap->add_free_region(r);
      }
      r->setLiveData(live);
      _live += live;
    }
    return false;
  }

  size_t getLive() { return _live;}

};

void ShenandoahMarkCompact::phase4_compact_objects(ShenandoahHeapRegionSet** copy_queues) {
  ShenandoahHeap* heap = ShenandoahHeap::heap();
  ShenandoahCompactObjectsTask compact_task(copy_queues);
  heap->workers()->run_task(&compact_task);

  heap->clear_cset_fast_test();
  ShenandoahPostCompactClosure post_compact;
  heap->heap_region_iterate(&post_compact);

  // We just reset the top-at-prev-mark-start pointer. Thus
  // we also need to clear the bitmap, otherwise it would make
  // a mess later when clearing the prev bitmap.
  heap->prev_mark_bit_map()->clearAll();

  heap->set_used(post_compact.getLive());

  for (int i = 0; i < heap->max_parallel_workers(); i++) {
    delete copy_queues[i];
  }

}
