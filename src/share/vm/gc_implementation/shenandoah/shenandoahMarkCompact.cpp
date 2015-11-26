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
#include "gc_implementation/shenandoah/shenandoahMarkCompact.hpp"
#include "gc_implementation/shenandoah/shenandoahBarrierSet.hpp"
#include "gc_implementation/shenandoah/shenandoahHeap.hpp"
#include "gc_implementation/shenandoah/shenandoahHeap.inline.hpp"
#include "gc_implementation/shenandoah/shenandoahRootProcessor.hpp"
#include "gc_implementation/shenandoah/vm_operations_shenandoah.hpp"
#include "gc_implementation/shared/markSweep.inline.hpp"
#include "oops/oop.inline.hpp"
#include "runtime/biasedLocking.hpp"
#include "runtime/thread.hpp"
#include "utilities/copy.hpp"
#include "utilities/taskqueue.hpp"
#include "utilities/workgroup.hpp"



void ShenandoahMarkCompact::allocate_stacks() {
  GenMarkSweep::_preserved_count_max = 0;
  GenMarkSweep::_preserved_marks = NULL;
  GenMarkSweep::_preserved_count = 0;
}

void ShenandoahMarkCompact::do_mark_compact() {

  COMPILER2_PRESENT(DerivedPointerTable::clear());

  ShenandoahHeap* _heap = ShenandoahHeap::heap();

  assert(SafepointSynchronize::is_at_safepoint(), "must be at a safepoint");
  IsGCActiveMark is_active;

  assert(Thread::current()->is_VM_thread(), "Do full GC only while world is stopped");
  assert(_heap->is_bitmap_clear(), "require cleared bitmap");
  assert(!_heap->concurrent_mark_in_progress(), "can't do full-GC while marking is in progress");
  assert(!_heap->is_evacuation_in_progress(), "can't do full-GC while evacuation is in progress");

  _heap->shenandoahPolicy()->record_phase_start(ShenandoahCollectorPolicy::full_gc);

  // We need to clear the is_in_collection_set flag in all regions.
  ShenandoahHeapRegion** regions = _heap->heap_regions();
  size_t num_regions = _heap->num_regions();
  for (size_t i = 0; i < num_regions; i++) {
    regions[i]->set_is_in_collection_set(false);
  }
  _heap->clear_cset_fast_test();

  /*
  if (ShenandoahVerify) {
    // Full GC should only be called between regular concurrent cycles, therefore
    // those verifications should be valid.
    _heap->verify_heap_after_evacuation();
    _heap->verify_heap_after_update_refs();
  }
  */

  if (ShenandoahTraceFullGC) {
    gclog_or_tty->print_cr("Shenandoah-full-gc: start with heap used: "SIZE_FORMAT" MB", _heap->used() / M);
    gclog_or_tty->print_cr("Shenandoah-full-gc: phase 1: marking the heap");
    // _heap->print_heap_regions();
  }

  if (UseTLAB) {
    _heap->ensure_parsability(true);
  }

  MemRegion mr = _heap->reserved_region();
  ReferenceProcessor rp(mr, false, 0, false, 0, true, NULL);

  // hook up weak ref data so it can be used during Mark-Sweep
  assert(MarkSweep::ref_processor() == NULL, "no stomping");
  bool clear_all_softrefs = _heap->collector_policy()->use_should_clear_all_soft_refs(true /*ignored*/);
  GenMarkSweep::_ref_processor = &rp;

  rp.enable_discovery(true, true);
  rp.setup_policy(clear_all_softrefs);

  CodeCache::gc_prologue();
  allocate_stacks();

  // We should save the marks of the currently locked biased monitors.
  // The marking doesn't preserve the marks of biased objects.
  BiasedLocking::preserve_marks();

  phase1_mark_heap();

  if (ShenandoahTraceFullGC) {
    gclog_or_tty->print_cr("Shenandoah-full-gc: phase 2: calculating target addresses");
  }
  phase2_calculate_target_addresses();

  if (ShenandoahTraceFullGC) {
    gclog_or_tty->print_cr("Shenandoah-full-gc: phase 3: updating references");
  }

  // Don't add any more derived pointers during phase3
  COMPILER2_PRESENT(DerivedPointerTable::set_active(false));

  phase3_update_references();

  if (ShenandoahTraceFullGC) {
    gclog_or_tty->print_cr("Shenandoah-full-gc: phase 4: compacting objects");
  }

  phase4_compact_objects();

  MarkSweep::restore_marks();
  BiasedLocking::restore_marks();
  GenMarkSweep::deallocate_stacks();

  CodeCache::gc_epilogue();
  JvmtiExport::gc_epilogue();

  // refs processing: clean slate
  rp.enqueue_discovered_references();

  GenMarkSweep::_ref_processor = NULL;

  if (ShenandoahVerify) {
    _heap->verify_heap_after_evacuation();
  }

  if (ShenandoahTraceFullGC) {
    gclog_or_tty->print_cr("Shenandoah-full-gc: finish with heap used: "SIZE_FORMAT" MB", _heap->used() / M);
  }

  _heap->_bytesAllocSinceCM = 0;

  _heap->set_need_update_refs(false);

  COMPILER2_PRESENT(DerivedPointerTable::update_pointers());

  _heap->shenandoahPolicy()->record_phase_end(ShenandoahCollectorPolicy::full_gc);
}

class UpdateRefsClosure: public ExtendedOopClosure {
public:
  virtual void do_oop(oop* p) {
    oop obj = oopDesc::load_heap_oop(p);
    if (! oopDesc::is_null(obj)) {
      ShenandoahBarrierSet::resolve_and_update_oop_static(p, obj);
    }
  }
  virtual void do_oop(narrowOop* p) {
    Unimplemented();
  }
};

void ShenandoahMarkCompact::phase1_mark_heap() {
  ShenandoahHeap* _heap = ShenandoahHeap::heap();
  ReferenceProcessor* ref_proc = GenMarkSweep::ref_processor();

  {
    MarkingCodeBlobClosure follow_code_closure(&MarkSweep::follow_root_closure, CodeBlobToOopClosure::FixRelocations);

    UpdateRefsClosure uprefs;
    CLDToOopClosure cld_uprefs(&uprefs, false);
    CodeBlobToOopClosure code_uprefs(&uprefs, CodeBlobToOopClosure::FixRelocations);

    // Need cleared claim bits for the roots processing
    ClassLoaderDataGraph::clear_claimed_marks();
    ShenandoahRootProcessor rp(_heap, 1);
    rp.process_roots(&MarkSweep::follow_root_closure, &uprefs,
                     &MarkSweep::follow_cld_closure, &cld_uprefs, &MarkSweep::follow_cld_closure,
                     &follow_code_closure, &code_uprefs);

  }

  _heap->shenandoahPolicy()->record_phase_start(ShenandoahCollectorPolicy::weakrefs);

  const ReferenceProcessorStats& stats =
    ref_proc->process_discovered_references(&MarkSweep::is_alive,
                                            &GenMarkSweep::keep_alive,
                                            &MarkSweep::follow_stack_closure,
                                            NULL,
                                            NULL,
                                            _heap->tracer()->gc_id());

  //     heap->tracer()->report_gc_reference_stats(stats);

  _heap->shenandoahPolicy()->record_phase_end(ShenandoahCollectorPolicy::weakrefs);

  // Unload classes and purge the SystemDictionary.
  bool purged_class = SystemDictionary::do_unloading(&MarkSweep::is_alive);

  // Unload nmethods.
  CodeCache::do_unloading(&MarkSweep::is_alive, purged_class);

  // Prune dead klasses from subklass/sibling/implementor lists.
  Klass::clean_weak_klass_links(&MarkSweep::is_alive);

  // Delete entries for dead interned string and clean up unreferenced symbols in symbol table.
  _heap->unlink_string_and_symbol_table(&MarkSweep::is_alive);

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
    if (!VerifySilently) {
      gclog_or_tty->print(" VerifyDuringGC:(full)[Verifying ");
    }
    //    Universe::heap()->verify(VerifySilently, VerifyOption_G1UseMarkWord);
    _heap->verify(VerifySilently, VerifyOption_G1UseMarkWord);
    if (!VerifySilently) {
      gclog_or_tty->print_cr("]");
    }
  }
}

class ShenandoahPrepareForCompaction : public ShenandoahHeapRegionClosure {
  CompactPoint _cp;
  ShenandoahHeap* _heap;
  bool _dead_humongous;

public:
  ShenandoahPrepareForCompaction() :
    _heap(ShenandoahHeap::heap()),
    _dead_humongous(false) {
  }

  bool doHeapRegion(ShenandoahHeapRegion* r) {
    // We need to save the contents
    if (!r->is_humongous()) {
      if (_cp.space == NULL) {
        _cp.space = r;
        _cp.threshold = r->initialize_threshold();
      }
      _dead_humongous = false;
      r->prepare_for_compaction(&_cp);
    }  else {
      if (r->is_humongous_start()) {
        oop obj = oop(r->bottom() + BrooksPointer::BROOKS_POINTER_OBJ_SIZE);
        if (obj->is_gc_marked()) {
          obj->forward_to(obj);
          _dead_humongous = false;
        } else {
          if (_cp.space == NULL) {
            _cp.space = r;
            _cp.threshold = r->initialize_threshold();
          }
          _dead_humongous = true;
          guarantee(r->region_number() >= ((ShenandoahHeapRegion*)_cp.space)->region_number(),
                    "only reset regions that are not yet used for compaction");
          r->reset();
          r->prepare_for_compaction(&_cp);
        }
      } else {
        assert(r->is_humongous_continuation(), "expect humongous continuation");
        if (_dead_humongous) {
          guarantee(r->region_number() > ((ShenandoahHeapRegion*)_cp.space)->region_number(),
                    "only reset regions that are not yet used for compaction");
          r->reset();
          r->prepare_for_compaction(&_cp);
        }
      }
    }
    return false;
  }
};

void ShenandoahMarkCompact::phase2_calculate_target_addresses() {
  ShenandoahPrepareForCompaction prepare;
  ShenandoahHeap::heap()->heap_region_iterate(&prepare);
}


class ShenandoahMarkCompactAdjustPointersClosure : public ShenandoahHeapRegionClosure {
  bool doHeapRegion(ShenandoahHeapRegion* r) {
    if (r->is_humongous()) {
      if (r->is_humongous_start()) {
        // We must adjust the pointers on the single H object.
        oop obj = oop(r->bottom() + BrooksPointer::BROOKS_POINTER_OBJ_SIZE);
        assert(obj->is_gc_marked(), "should be marked");
        // point all the oops to the new location
        obj->adjust_pointers();
      }
    } else {
      r->adjust_pointers();
    }
    return false;
  }
};

void ShenandoahMarkCompact::phase3_update_references() {
  ShenandoahHeap* heap = ShenandoahHeap::heap();

    // Need cleared claim bits for the roots processing
  ClassLoaderDataGraph::clear_claimed_marks();

  CodeBlobToOopClosure adjust_code_closure(&MarkSweep::adjust_pointer_closure,
                                           CodeBlobToOopClosure::FixRelocations);

  {
    ShenandoahRootProcessor rp(heap, 1);
    rp.process_all_roots(&MarkSweep::adjust_pointer_closure,
                         &MarkSweep::adjust_cld_closure,
                         &adjust_code_closure);
  }

  // Now adjust pointers in remaining weak roots.  (All of which should
  // have been cleared if they pointed to non-surviving objects.)
  GenMarkSweep::ref_processor()->weak_oops_do(&MarkSweep::adjust_pointer_closure);

  ShenandoahAlwaysTrueClosure always_true;
  JNIHandles::weak_oops_do(&always_true, &MarkSweep::adjust_pointer_closure);

  //  if (G1StringDedup::is_enabled()) {
  //    G1StringDedup::oops_do(&MarkSweep::adjust_pointer_closure);
  //  }

  MarkSweep::adjust_marks();

  ShenandoahMarkCompactAdjustPointersClosure apc;
  heap->heap_region_iterate(&apc);
}

class ShenandoahCleanupObjectClosure : public ObjectClosure {
  void  do_object(oop p) {
    ShenandoahHeap::heap()->initialize_brooks_ptr(p);
  }
};

class CompactObjectsClosure : public ShenandoahHeapRegionClosure {

public:

  CompactObjectsClosure() {
  }

  bool doHeapRegion(ShenandoahHeapRegion* r) {
    if (r->is_humongous()) {
      if (r->is_humongous_start()) {
        oop obj = oop(r->bottom() + BrooksPointer::BROOKS_POINTER_OBJ_SIZE);
        assert(obj->is_gc_marked(), "expect marked humongous object");
        obj->init_mark();
      }
    } else {
      r->compact();
    }

    return false;
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
    if (r->is_humongous()) {
      _live += ShenandoahHeapRegion::RegionSizeBytes;

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

void ShenandoahMarkCompact::phase4_compact_objects() {
  ShenandoahHeap* heap = ShenandoahHeap::heap();
  CompactObjectsClosure coc;
  heap->heap_region_iterate(&coc);

  ShenandoahCleanupObjectClosure cleanup;
  heap->object_iterate(&cleanup);

  ShenandoahPostCompactClosure post_compact;
  heap->heap_region_iterate(&post_compact);

  // We just reset the top-at-prev-mark-start pointer. Thus
  // we also need to clear the bitmap, otherwise it would make
  // a mess later when clearing the prev bitmap.
  heap->prev_mark_bit_map()->clearAll();

  heap->set_used(post_compact.getLive());
}
