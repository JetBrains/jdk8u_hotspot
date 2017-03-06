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

#include "gc_implementation/shared/isGCActiveMark.hpp"
#include "gc_implementation/shared/parallelCleaning.hpp"
#include "gc_implementation/shenandoah/shenandoahBarrierSet.inline.hpp"
#include "gc_implementation/shenandoah/shenandoahCollectorPolicy.hpp"
#include "gc_implementation/shenandoah/shenandoahConcurrentMark.inline.hpp"
#include "gc_implementation/shenandoah/shenandoahOopClosures.inline.hpp"
#include "gc_implementation/shenandoah/shenandoahHeap.inline.hpp"
#include "gc_implementation/shenandoah/shenandoahRootProcessor.hpp"
#include "gc_implementation/shenandoah/shenandoah_specialized_oop_closures.hpp"
#include "gc_implementation/shenandoah/shenandoahOopClosures.inline.hpp"
#include "gc_implementation/shenandoah/brooksPointer.hpp"
#include "memory/referenceProcessor.hpp"
#include "gc_implementation/shenandoah/shenandoahTaskqueue.hpp"
#include "code/codeCache.hpp"
#include "classfile/symbolTable.hpp"
#include "classfile/systemDictionary.hpp"
#include "memory/iterator.inline.hpp"
#include "oops/oop.inline.hpp"
#include "utilities/taskqueue.hpp"

class ShenandoahInitMarkRootsClosure : public OopClosure {
private:
  SCMObjToScanQueue* _queue;
  ShenandoahHeap* _heap;

  template <class T>
  inline void do_oop_nv(T* p) {
    ShenandoahConcurrentMark::mark_through_ref<T, RESOLVE>(p, _heap, _queue);
  }

public:
  ShenandoahInitMarkRootsClosure(SCMObjToScanQueue* q) :
    _queue(q), _heap(ShenandoahHeap::heap()) {};

  void do_oop(narrowOop* p) { do_oop_nv(p); }
  void do_oop(oop* p)       { do_oop_nv(p); }
};

ShenandoahMarkRefsSuperClosure::ShenandoahMarkRefsSuperClosure(SCMObjToScanQueue* q, ReferenceProcessor* rp) :
  MetadataAwareOopClosure(rp),
  _queue(q),
  _heap((ShenandoahHeap*) Universe::heap())
{
}

class ShenandoahInitMarkRootsTask : public AbstractGangTask {
private:
  ShenandoahRootProcessor* _rp;
  bool _process_refs;
public:
  ShenandoahInitMarkRootsTask(ShenandoahRootProcessor* rp, bool process_refs) :
    AbstractGangTask("Shenandoah init mark roots task"),
    _rp(rp),
    _process_refs(process_refs) {
  }

  void work(uint worker_id) {
    assert(SafepointSynchronize::is_at_safepoint(), "Must be at a safepoint");

    ShenandoahHeap* heap = ShenandoahHeap::heap();
    SCMObjToScanQueueSet* queues = heap->concurrentMark()->task_queues();
    assert(queues->get_reserved() > worker_id, err_msg("Queue has not been reserved for worker id: %d", worker_id));

    SCMObjToScanQueue* q = queues->queue(worker_id);
    ShenandoahInitMarkRootsClosure mark_cl(q);
    CLDToOopClosure cldCl(&mark_cl);
    MarkingCodeBlobClosure blobsCl(&mark_cl, ! CodeBlobToOopClosure::FixRelocations);

    // The rationale for selecting the roots to scan is as follows:
    //   a. With unload_classes = true, we only want to scan the actual strong roots from the
    //      code cache. This will allow us to identify the dead classes, unload them, *and*
    //      invalidate the relevant code cache blobs. This could be only done together with
    //      class unloading.
    //   b. With unload_classes = false, we have to nominally retain all the references from code
    //      cache, because there could be the case of embedded class/oop in the generated code,
    //      which we will never visit during mark. Without code cache invalidation, as in (a),
    //      we risk executing that code cache blob, and crashing.
    //   c. With ShenandoahConcurrentCodeRoots, we avoid scanning the entire code cache here,
    //      and instead do that in concurrent phase under the relevant lock. This saves init mark
    //      pause time.

    ResourceMark m;
    if (heap->concurrentMark()->unload_classes()) {
      _rp->process_strong_roots(&mark_cl, _process_refs ? NULL : &mark_cl, &cldCl, &blobsCl, worker_id);
    } else {
      if (ShenandoahConcurrentCodeRoots) {
        CodeBlobClosure* code_blobs;
#ifdef ASSERT
        AssertToSpaceClosure assert_to_space_oops;
        CodeBlobToOopClosure assert_to_space(&assert_to_space_oops,
                                             !CodeBlobToOopClosure::FixRelocations);
        code_blobs = &assert_to_space;
#else
        code_blobs = NULL;
#endif
        _rp->process_all_roots(&mark_cl, _process_refs ? NULL : &mark_cl, &cldCl, code_blobs, worker_id);
      } else {
        _rp->process_all_roots(&mark_cl, _process_refs ? NULL : &mark_cl, &cldCl, &blobsCl, worker_id);
      }
    }
  }
};

class ShenandoahUpdateRootsTask : public AbstractGangTask {
private:
  ShenandoahRootProcessor* _rp;
public:
  ShenandoahUpdateRootsTask(ShenandoahRootProcessor* rp) :
    AbstractGangTask("Shenandoah update roots task"),
    _rp(rp) {
  }

  void work(uint worker_id) {
    assert(SafepointSynchronize::is_at_safepoint(), "Must be at a safepoint");

    ShenandoahHeap* heap = ShenandoahHeap::heap();
    SCMUpdateRefsClosure cl;
    CLDToOopClosure cldCl(&cl);

    CodeBlobClosure* code_blobs;
#ifdef ASSERT
    AssertToSpaceClosure assert_to_space_oops;
    CodeBlobToOopClosure assert_to_space(&assert_to_space_oops, !CodeBlobToOopClosure::FixRelocations);
    code_blobs = &assert_to_space;
#else
    code_blobs = NULL;
#endif
    _rp->process_all_roots(&cl, &cl, &cldCl, code_blobs, worker_id);
  }
};

class SCMConcurrentMarkingTask : public AbstractGangTask {
private:
  ShenandoahConcurrentMark* _cm;
  ParallelTaskTerminator* _terminator;
  bool _update_refs;

public:
  SCMConcurrentMarkingTask(ShenandoahConcurrentMark* cm, ParallelTaskTerminator* terminator, bool update_refs) :
    AbstractGangTask("Root Region Scan"), _cm(cm), _terminator(terminator), _update_refs(update_refs) {
  }


  void work(uint worker_id) {
    SCMObjToScanQueue* q = _cm->get_queue(worker_id);
    jushort* live_data = _cm->get_liveness(worker_id);
    ReferenceProcessor* rp;
    if (_cm->process_references()) {
      rp = ShenandoahHeap::heap()->ref_processor();
    } else {
      rp = NULL;
    }
    if (ShenandoahConcurrentCodeRoots && _cm->claim_codecache()) {
      if (! _cm->unload_classes()) {
        ShenandoahMarkResolveRefsClosure cl(q, rp);
        CodeBlobToOopClosure blobs(&cl, ! CodeBlobToOopClosure::FixRelocations);
        MutexLockerEx mu(CodeCache_lock, Mutex::_no_safepoint_check_flag);
        CodeCache::blobs_do(&blobs);
      }
    }

    _cm->mark_loop(worker_id, _terminator, rp,
                   true, // cancellable
                   true, // drain SATBs as we go
                   true, // count liveness
                   _cm->unload_classes(),
                   _update_refs);
  }
};

class SCMFinalMarkingTask : public AbstractGangTask {
private:
  ShenandoahConcurrentMark* _cm;
  ParallelTaskTerminator* _terminator;
  bool _update_refs;
  bool _count_live;
  bool _unload_classes;

public:
  SCMFinalMarkingTask(ShenandoahConcurrentMark* cm, ParallelTaskTerminator* terminator, bool update_refs, bool count_live, bool unload_classes) :
    AbstractGangTask("Shenandoah Final Marking"), _cm(cm), _terminator(terminator), _update_refs(update_refs), _count_live(count_live), _unload_classes(unload_classes) {
  }

  void work(uint worker_id) {
    // First drain remaining SATB buffers.
    // Notice that this is not strictly necessary for mark-compact. But since
    // it requires a StrongRootsScope around the task, we need to claim the
    // threads, and performance-wise it doesn't really matter. Adds about 1ms to
    // full-gc.
    _cm->drain_satb_buffers(worker_id, true);

    ReferenceProcessor* rp;
    if (_cm->process_references()) {
      rp = ShenandoahHeap::heap()->ref_processor();
    } else {
      rp = NULL;
    }

    _cm->mark_loop(worker_id, _terminator, rp,
                   false, // not cancellable
                   false, // do not drain SATBs, already drained
                   _count_live,
                   _unload_classes,
                   _update_refs);

    assert(_cm->task_queues()->is_empty(), "Should be empty");
  }
};

void ShenandoahConcurrentMark::mark_roots() {
  assert(Thread::current()->is_VM_thread(), "can only do this in VMThread");
  assert(SafepointSynchronize::is_at_safepoint(), "Must be at a safepoint");

  ShenandoahHeap* heap = ShenandoahHeap::heap();

  ClassLoaderDataGraph::clear_claimed_marks();
  WorkGang* workers = heap->workers();
  uint nworkers = workers->active_workers();

  assert(nworkers <= task_queues()->size(), "Just check");

  ShenandoahRootProcessor root_proc(heap, nworkers, ShenandoahCollectorPolicy::scan_thread_roots);
  TASKQUEUE_STATS_ONLY(reset_taskqueue_stats());
  task_queues()->reserve(nworkers);

  ShenandoahInitMarkRootsTask mark_roots(&root_proc, process_references());
  workers->run_task(&mark_roots);
  if (ShenandoahConcurrentCodeRoots) {
    clear_claim_codecache();
  }
}

void ShenandoahConcurrentMark::init_mark_roots() {
  assert(Thread::current()->is_VM_thread(), "can only do this in VMThread");
  assert(SafepointSynchronize::is_at_safepoint(), "Must be at a safepoint");

  ShenandoahHeap* heap = ShenandoahHeap::heap();

  // Set up ref processing and class unloading.
  ShenandoahCollectorPolicy* policy = heap->shenandoahPolicy();
  set_process_references(policy->process_references());
  set_unload_classes(policy->unload_classes());

  mark_roots();
}

void ShenandoahConcurrentMark::update_roots() {
  assert(SafepointSynchronize::is_at_safepoint(), "Must be at a safepoint");
  ShenandoahHeap* heap = ShenandoahHeap::heap();

  ClassLoaderDataGraph::clear_claimed_marks();
  uint nworkers = heap->workers()->active_workers();

  ShenandoahRootProcessor root_proc(heap, nworkers, ShenandoahCollectorPolicy::update_thread_roots);
  ShenandoahUpdateRootsTask update_roots(&root_proc);
  heap->workers()->run_task(&update_roots);
}

void ShenandoahConcurrentMark::final_update_roots() {
  assert(Thread::current()->is_VM_thread(), "can only do this in VMThread");
  assert(SafepointSynchronize::is_at_safepoint(), "Must be at a safepoint");

  COMPILER2_PRESENT(DerivedPointerTable::clear());

  update_roots();

  COMPILER2_PRESENT(DerivedPointerTable::update_pointers());
}


void ShenandoahConcurrentMark::initialize(uint workers) {
  _heap = ShenandoahHeap::heap();

  uint num_queues = MAX2(workers, 1U);

  _task_queues = new SCMObjToScanQueueSet((int) num_queues);

  for (uint i = 0; i < num_queues; ++i) {
    SCMObjToScanQueue* task_queue = new SCMObjToScanQueue();
    task_queue->initialize();
    _task_queues->register_queue(i, task_queue);
  }
  _process_references = false;
  _unload_classes = false;
  _claimed_codecache = 0;

  JavaThread::satb_mark_queue_set().set_buffer_size(ShenandoahSATBBufferSize);

  size_t max_regions = ShenandoahHeap::heap()->max_regions();
  _liveness_local = NEW_C_HEAP_ARRAY(jushort*, workers, mtGC);
  for (uint worker = 0; worker < workers; worker++) {
     _liveness_local[worker] = NEW_C_HEAP_ARRAY(jushort, max_regions, mtGC);
  }
}

void ShenandoahConcurrentMark::mark_from_roots() {
  ShenandoahHeap* sh = (ShenandoahHeap *) Universe::heap();
  WorkGang* workers = sh->workers();
  uint nworkers = workers->active_workers();

  bool update_refs = sh->need_update_refs();

  sh->shenandoahPolicy()->record_phase_start(ShenandoahCollectorPolicy::conc_mark);

  if (process_references()) {
    ReferenceProcessor* rp = sh->ref_processor();
    rp->set_active_mt_degree(nworkers);

    // enable ("weak") refs discovery
    rp->enable_discovery(true /*verify_no_refs*/, true);
    rp->setup_policy(sh->is_full_gc_in_progress()); // snapshot the soft ref policy to be used in this cycle
  }

  task_queues()->reserve(nworkers);

  if (UseShenandoahOWST) {
    ShenandoahTaskTerminator terminator(nworkers, task_queues());
    SCMConcurrentMarkingTask markingTask = SCMConcurrentMarkingTask(this, &terminator, update_refs);
    workers->run_task(&markingTask);
  } else {
    ParallelTaskTerminator terminator(nworkers, task_queues());
    SCMConcurrentMarkingTask markingTask = SCMConcurrentMarkingTask(this, &terminator, update_refs);
    workers->run_task(&markingTask);
  }

  assert(task_queues()->is_empty() || sh->cancelled_concgc(), "Should be empty when not cancelled");
  if (! sh->cancelled_concgc()) {
    TASKQUEUE_STATS_ONLY(print_taskqueue_stats());
  }

  TASKQUEUE_STATS_ONLY(reset_taskqueue_stats());

  sh->shenandoahPolicy()->record_phase_end(ShenandoahCollectorPolicy::conc_mark);
}

void ShenandoahConcurrentMark::finish_mark_from_roots() {
  assert(SafepointSynchronize::is_at_safepoint(), "Must be at a safepoint");

  IsGCActiveMark is_active;

  ShenandoahHeap* sh = (ShenandoahHeap *) Universe::heap();

  TASKQUEUE_STATS_ONLY(reset_taskqueue_stats());

  shared_finish_mark_from_roots(/* full_gc = */ false);

  sh->shenandoahPolicy()->record_phase_start(ShenandoahCollectorPolicy::update_roots);
  if (sh->need_update_refs()) {
    final_update_roots();
  }
  sh->shenandoahPolicy()->record_phase_end(ShenandoahCollectorPolicy::update_roots);

  TASKQUEUE_STATS_ONLY(print_taskqueue_stats());

#ifdef ASSERT
  verify_roots();

  if (ShenandoahDumpHeapAfterConcurrentMark) {
    sh->ensure_parsability(false);
    sh->print_all_refs("post-mark");
  }
#endif
}

class ResetRecentlyAllocated : public ShenandoahHeapRegionClosure {
public:
  bool doHeapRegion(ShenandoahHeapRegion* r) {
    ShenandoahHeap* sh = ShenandoahHeap::heap();
    r->set_recently_allocated(false);
    return false;
  }
};

void ShenandoahConcurrentMark::shared_finish_mark_from_roots(bool full_gc) {
  assert(SafepointSynchronize::is_at_safepoint(), "Must be at a safepoint");

  ShenandoahHeap* sh = ShenandoahHeap::heap();
  ShenandoahCollectorPolicy* policy = sh->shenandoahPolicy();

  uint nworkers = sh->workers()->active_workers();

  // Finally mark everything else we've got in our queues during the previous steps.
  // It does two different things for concurrent vs. mark-compact GC:
  // - For concurrent GC, it starts with empty task queues, drains the remaining
  //   SATB buffers, and then completes the marking closure.
  // - For mark-compact GC, it starts out with the task queues seeded by initial
  //   root scan, and completes the closure, thus marking through all live objects
  // The implementation is the same, so it's shared here.
  {
    policy->record_phase_start(full_gc ?
                               ShenandoahCollectorPolicy::full_gc_mark_drain_queues :
                               ShenandoahCollectorPolicy::drain_satb);
    bool count_live = !(ShenandoahNoLivenessFullGC && full_gc); // we do not need liveness data for full GC
    task_queues()->reserve(nworkers);

    SharedHeap::StrongRootsScope scope(sh, true);
    if (UseShenandoahOWST) {
      ShenandoahTaskTerminator terminator(nworkers, task_queues());
      SCMFinalMarkingTask task(this, &terminator, sh->need_update_refs(), count_live, unload_classes());
      sh->workers()->run_task(&task);
    } else {
      ParallelTaskTerminator terminator(nworkers, task_queues());
      SCMFinalMarkingTask task(this, &terminator, sh->need_update_refs(), count_live, unload_classes());
      sh->workers()->run_task(&task);
    }
    policy->record_phase_end(full_gc ?
                             ShenandoahCollectorPolicy::full_gc_mark_drain_queues :
                             ShenandoahCollectorPolicy::drain_satb);
  }

  assert(task_queues()->is_empty(), "Should be empty");

  // When we're done marking everything, we process weak references.
  policy->record_phase_start(full_gc ?
                             ShenandoahCollectorPolicy::full_gc_mark_weakrefs :
                             ShenandoahCollectorPolicy::weakrefs);
  if (process_references()) {
    weak_refs_work();
  }
  policy->record_phase_end(full_gc ?
                           ShenandoahCollectorPolicy::full_gc_mark_weakrefs :
                           ShenandoahCollectorPolicy::weakrefs);

  // And finally finish class unloading
  policy->record_phase_start(full_gc ?
                             ShenandoahCollectorPolicy::full_gc_mark_class_unloading :
                             ShenandoahCollectorPolicy::class_unloading);
  if (unload_classes()) {
    ShenandoahForwardedIsAliveClosure is_alive;
    // Unload classes and purge SystemDictionary.
    bool purged_class = SystemDictionary::do_unloading(&is_alive, false);
    ParallelCleaningTask unlink_task(&is_alive, true, true, nworkers, purged_class);
    sh->workers()->run_task(&unlink_task);
    ClassLoaderDataGraph::purge();
  }

  // Mark finished. All recently allocated regions are not recent anymore.
  {
    ResetRecentlyAllocated cl;
    sh->heap_region_iterate(&cl);
  }

  policy->record_phase_end(full_gc ?
                           ShenandoahCollectorPolicy::full_gc_mark_class_unloading :
                           ShenandoahCollectorPolicy::class_unloading);

  assert(task_queues()->is_empty(), "Should be empty");

}

#ifdef ASSERT
template <class T>
void ShenandoahVerifyRootsClosure1::do_oop_work(T* p) {
  ShenandoahHeap* heap = ShenandoahHeap::heap();
  T o = oopDesc::load_heap_oop(p);
  if (! oopDesc::is_null(o)) {
    oop obj = oopDesc::decode_heap_oop_not_null(o);
    if (! oopDesc::unsafe_equals(obj, ShenandoahBarrierSet::resolve_oop_static_not_null(obj))) {
      tty->print_cr("from-space marked: %s, to-space marked: %s, unload_classes: %s",
                    BOOL_TO_STR(heap->is_marked_next(obj)),
                    BOOL_TO_STR(heap->is_marked_next(ShenandoahBarrierSet::resolve_oop_static_not_null(obj))),
                    BOOL_TO_STR(heap->concurrentMark()->unload_classes()));
    }
    guarantee(oopDesc::unsafe_equals(obj, ShenandoahBarrierSet::resolve_oop_static_not_null(obj)), "oop must not be forwarded");
    guarantee(heap->is_marked_next(obj), "oop must be marked");
  }
}

void ShenandoahVerifyRootsClosure1::do_oop(oop* p) {
  do_oop_work(p);
}

void ShenandoahVerifyRootsClosure1::do_oop(narrowOop* p) {
  do_oop_work(p);
}

void ShenandoahConcurrentMark::verify_roots() {
  ShenandoahVerifyRootsClosure1 cl;
  CodeBlobToOopClosure blobsCl(&cl, false);
  CLDToOopClosure cldCl(&cl);
  ClassLoaderDataGraph::clear_claimed_marks();
  ShenandoahRootProcessor rp(ShenandoahHeap::heap(), 1);
  rp.process_all_roots(&cl, &cl, &cldCl, &blobsCl, 0);

}
#endif

class ShenandoahSATBThreadsClosure : public ThreadClosure {
  ShenandoahSATBBufferClosure* _satb_cl;
  int _thread_parity;

 public:
  ShenandoahSATBThreadsClosure(ShenandoahSATBBufferClosure* satb_cl) :
    _satb_cl(satb_cl),
    _thread_parity(SharedHeap::heap()->strong_roots_parity()) {}

  void do_thread(Thread* thread) {
    if (thread->is_Java_thread()) {
      if (thread->claim_oops_do(true, _thread_parity)) {
        JavaThread* jt = (JavaThread*)thread;
        jt->satb_mark_queue().apply_closure_and_empty(_satb_cl);
      }
    } else if (thread->is_VM_thread()) {
      if (thread->claim_oops_do(true, _thread_parity)) {
        JavaThread::satb_mark_queue_set().shared_satb_queue()->apply_closure_and_empty(_satb_cl);
      }
    }
  }
};

void ShenandoahConcurrentMark::drain_satb_buffers(uint worker_id, bool remark) {
  SCMObjToScanQueue* q = get_queue(worker_id);
  ShenandoahSATBBufferClosure cl(q);

  SATBMarkQueueSet& satb_mq_set = JavaThread::satb_mark_queue_set();
  while (satb_mq_set.apply_closure_to_completed_buffer(&cl));

  if (remark) {
    ShenandoahSATBThreadsClosure tc(&cl);
    Threads::threads_do(&tc);
  }
}

#if TASKQUEUE_STATS
void ShenandoahConcurrentMark::print_taskqueue_stats_hdr(outputStream* const st) {
  st->print_raw_cr("GC Task Stats");
  st->print_raw("thr "); TaskQueueStats::print_header(1, st); st->cr();
  st->print_raw("--- "); TaskQueueStats::print_header(2, st); st->cr();
}

void ShenandoahConcurrentMark::print_taskqueue_stats() const {
  if (! ShenandoahLogTrace) {
    return;
  }
  ResourceMark rm;
  outputStream* st = gclog_or_tty;
  print_taskqueue_stats_hdr(st);

  ShenandoahHeap* sh = (ShenandoahHeap*) Universe::heap();
  TaskQueueStats totals;
  const int n = _task_queues->size();
  for (int i = 0; i < n; ++i) {
    st->print(INT32_FORMAT_W(3), i);
    _task_queues->queue(i)->stats.print(st);
    st->cr();
    totals += _task_queues->queue(i)->stats;
  }
  st->print("tot "); totals.print(st); st->cr();
  DEBUG_ONLY(totals.verify());

}

void ShenandoahConcurrentMark::reset_taskqueue_stats() {
  ShenandoahHeap* sh = (ShenandoahHeap*) Universe::heap();
  const int n = task_queues()->size();
  for (int i = 0; i < n; ++i) {
    task_queues()->queue(i)->stats.reset();
  }
}
#endif // TASKQUEUE_STATS

// Weak Reference Closures
class ShenandoahCMDrainMarkingStackClosure: public VoidClosure {
  uint _worker_id;
  ParallelTaskTerminator* _terminator;

public:
  ShenandoahCMDrainMarkingStackClosure(uint worker_id, ParallelTaskTerminator* t):
    _worker_id(worker_id),
    _terminator(t) {
  }


  void do_void() {
    assert(SafepointSynchronize::is_at_safepoint(), "Must be at a safepoint");

    ShenandoahHeap* sh = ShenandoahHeap::heap();
    ShenandoahConcurrentMark* scm = sh->concurrentMark();
    ReferenceProcessor* rp;
    if (scm->process_references()) {
      rp = ShenandoahHeap::heap()->ref_processor();
    } else {
      rp = NULL;
    }

    scm->mark_loop(_worker_id, _terminator, rp,
                   false, // not cancellable
                   false, // do not drain SATBs
                   true,  // count liveness
                   scm->unload_classes(),
                   sh->need_update_refs());
  }
};


class ShenandoahCMKeepAliveClosure : public OopClosure {
private:
  SCMObjToScanQueue* _queue;
  ShenandoahHeap* _heap;

  template <class T>
  inline void do_oop_nv(T* p) {
    ShenandoahConcurrentMark::mark_through_ref<T, NONE>(p, _heap, _queue);
  }

public:
  ShenandoahCMKeepAliveClosure(SCMObjToScanQueue* q) :
    _queue(q), _heap(ShenandoahHeap::heap()) {};

  void do_oop(narrowOop* p) { do_oop_nv(p); }
  void do_oop(oop* p)       { do_oop_nv(p); }
};

class ShenandoahCMKeepAliveUpdateClosure : public OopClosure {
private:
  SCMObjToScanQueue* _queue;
  ShenandoahHeap* _heap;

  template <class T>
  inline void do_oop_nv(T* p) {
    ShenandoahConcurrentMark::mark_through_ref<T, SIMPLE>(p, _heap, _queue);
  }

public:
  ShenandoahCMKeepAliveUpdateClosure(SCMObjToScanQueue* q) :
    _queue(q), _heap(ShenandoahHeap::heap()) {};

  void do_oop(narrowOop* p) { do_oop_nv(p); }
  void do_oop(oop* p)       { do_oop_nv(p); }
};

class ShenandoahRefProcTaskProxy : public AbstractGangTask {

private:
  AbstractRefProcTaskExecutor::ProcessTask& _proc_task;
  ParallelTaskTerminator* _terminator;
public:

  ShenandoahRefProcTaskProxy(AbstractRefProcTaskExecutor::ProcessTask& proc_task,
                             ParallelTaskTerminator* t) :
    AbstractGangTask("Process reference objects in parallel"),
    _proc_task(proc_task),
    _terminator(t) {
  }

  void work(uint worker_id) {
    assert(SafepointSynchronize::is_at_safepoint(), "Must be at a safepoint");
    ShenandoahHeap* heap = ShenandoahHeap::heap();
    ShenandoahForwardedIsAliveClosure is_alive;
    ShenandoahCMDrainMarkingStackClosure complete_gc(worker_id, _terminator);
    if (heap->need_update_refs()) {
      ShenandoahCMKeepAliveUpdateClosure keep_alive(heap->concurrentMark()->get_queue(worker_id));
      _proc_task.work(worker_id, is_alive, keep_alive, complete_gc);
    } else {
      ShenandoahCMKeepAliveClosure keep_alive(heap->concurrentMark()->get_queue(worker_id));
      _proc_task.work(worker_id, is_alive, keep_alive, complete_gc);
    }
  }
};

class ShenandoahRefEnqueueTaskProxy : public AbstractGangTask {

private:
  AbstractRefProcTaskExecutor::EnqueueTask& _enqueue_task;

public:

  ShenandoahRefEnqueueTaskProxy(AbstractRefProcTaskExecutor::EnqueueTask& enqueue_task) :
    AbstractGangTask("Enqueue reference objects in parallel"),
    _enqueue_task(enqueue_task) {
  }

  void work(uint worker_id) {
    _enqueue_task.work(worker_id);
  }
};

class ShenandoahRefProcTaskExecutor : public AbstractRefProcTaskExecutor {

private:
  WorkGang* _workers;

public:

  ShenandoahRefProcTaskExecutor(WorkGang* workers) :
    _workers(workers) {
  }

  // Executes a task using worker threads.
  void execute(ProcessTask& task) {
    assert(SafepointSynchronize::is_at_safepoint(), "Must be at a safepoint");

    ShenandoahHeap* heap = ShenandoahHeap::heap();
    ShenandoahConcurrentMark* cm = heap->concurrentMark();
    uint nworkers = _workers->active_workers();
    cm->task_queues()->reserve(nworkers);
    if (UseShenandoahOWST) {
      ShenandoahTaskTerminator terminator(nworkers, cm->task_queues());
      ShenandoahRefProcTaskProxy proc_task_proxy(task, &terminator);
      _workers->run_task(&proc_task_proxy);
    } else {
      ParallelTaskTerminator terminator(nworkers, cm->task_queues());
      ShenandoahRefProcTaskProxy proc_task_proxy(task, &terminator);
      _workers->run_task(&proc_task_proxy);
    }
  }

  void execute(EnqueueTask& task) {
    ShenandoahRefEnqueueTaskProxy enqueue_task_proxy(task);
    _workers->run_task(&enqueue_task_proxy);
  }
};


void ShenandoahConcurrentMark::weak_refs_work() {
  assert(process_references(), "sanity");
  ShenandoahHeap* sh = (ShenandoahHeap*) Universe::heap();
  ReferenceProcessor* rp = sh->ref_processor();
  WorkGang* workers = sh->workers();
  uint nworkers = workers->active_workers();

  // Setup collector policy for softref cleaning.
  bool clear_soft_refs = sh->collector_policy()->use_should_clear_all_soft_refs(true /* bogus arg*/);
  log_develop_debug(gc, ref)("clearing soft refs: %s", BOOL_TO_STR(clear_soft_refs));
  rp->setup_policy(clear_soft_refs);
  rp->set_active_mt_degree(nworkers);

  uint serial_worker_id = 0;
  ShenandoahForwardedIsAliveClosure is_alive;

  assert(task_queues()->is_empty(), "Should be empty");

  ParallelTaskTerminator terminator(1, task_queues());
  ShenandoahCMDrainMarkingStackClosure complete_gc(serial_worker_id, &terminator);
  ShenandoahRefProcTaskExecutor executor(workers);

  log_develop_trace(gc, ref)("start processing references");

  if (sh->need_update_refs()) {
    ShenandoahCMKeepAliveUpdateClosure keep_alive(get_queue(serial_worker_id));
    rp->process_discovered_references(&is_alive, &keep_alive,
                                      &complete_gc, &executor,
                                      NULL, sh->shenandoahPolicy()->tracer()->gc_id());
  } else {
    ShenandoahCMKeepAliveClosure keep_alive(get_queue(serial_worker_id));
    rp->process_discovered_references(&is_alive, &keep_alive,
                                      &complete_gc, &executor,
                                      NULL, sh->shenandoahPolicy()->tracer()->gc_id());
  }

  assert(task_queues()->is_empty(), "Should be empty");

  log_develop_trace(gc, ref)("finished processing references");
  log_develop_trace(gc, ref)("start enqueuing references");

  rp->enqueue_discovered_references(&executor);

  log_develop_trace(gc, ref)("finished enqueueing references");

  rp->verify_no_references_recorded();
  assert(!rp->discovery_enabled(), "Post condition");
}

void ShenandoahConcurrentMark::cancel() {
  ShenandoahHeap* sh = ShenandoahHeap::heap();

  // Cancel weak-ref discovery.
  if (process_references()) {
    ReferenceProcessor* rp = sh->ref_processor();
    rp->abandon_partial_discovery();
    rp->disable_discovery();
  }

  // Clean up marking stacks.
  SCMObjToScanQueueSet* queues = task_queues();
  queues->clear();

  // Cancel SATB buffers.
  JavaThread::satb_mark_queue_set().abandon_partial_marking();
}

SCMObjToScanQueue* ShenandoahConcurrentMark::get_queue(uint worker_id) {
  assert(task_queues()->get_reserved() > worker_id, err_msg("No reserved queue for worker id: %d", worker_id));
  return _task_queues->queue(worker_id);
}

void ShenandoahConcurrentMark::clear_queue(SCMObjToScanQueue *q) {
  q->set_empty();
  q->overflow_stack()->clear();
  q->clear_buffer();
}

template <bool CANCELLABLE, bool DRAIN_SATB, bool COUNT_LIVENESS, bool CLASS_UNLOAD, bool UPDATE_REFS>
void ShenandoahConcurrentMark::mark_loop_prework(uint w, ParallelTaskTerminator *t, ReferenceProcessor *rp) {
  SCMObjToScanQueue* q = get_queue(w);

  jushort* ld;
  if (COUNT_LIVENESS) {
    ld = get_liveness(w);
    Copy::fill_to_bytes(ld, _heap->max_regions() * sizeof(jushort));
  } else {
    ld = NULL;
  }

  // TODO: We can clean up this if we figure out how to do templated oop closures that
  // play nice with specialized_oop_iterators.
  if (CLASS_UNLOAD) {
    if (UPDATE_REFS) {
      ShenandoahMarkUpdateRefsMetadataClosure cl(q, rp);
      mark_loop_work<ShenandoahMarkUpdateRefsMetadataClosure, CANCELLABLE, DRAIN_SATB, COUNT_LIVENESS>(&cl, ld, w, t);
    } else {
      ShenandoahMarkRefsMetadataClosure cl(q, rp);
      mark_loop_work<ShenandoahMarkRefsMetadataClosure, CANCELLABLE, DRAIN_SATB, COUNT_LIVENESS>(&cl, ld, w, t);
    }
  } else {
    if (UPDATE_REFS) {
      ShenandoahMarkUpdateRefsClosure cl(q, rp);
      mark_loop_work<ShenandoahMarkUpdateRefsClosure, CANCELLABLE, DRAIN_SATB, COUNT_LIVENESS>(&cl, ld, w, t);
    } else {
      ShenandoahMarkRefsClosure cl(q, rp);
      mark_loop_work<ShenandoahMarkRefsClosure, CANCELLABLE, DRAIN_SATB, COUNT_LIVENESS>(&cl, ld, w, t);
    }
  }

  if (COUNT_LIVENESS) {
    for (uint i = 0; i < _heap->max_regions(); i++) {
      ShenandoahHeapRegion *r = _heap->regions()->get(i);
      if (r != NULL) {
        jushort live = ld[i];
        if (live > 0) {
          r->increase_live_data_words(live);
        }
      }
    }
  }
}

template <class T, bool CANCELLABLE, bool DRAIN_SATB, bool COUNT_LIVENESS>
void ShenandoahConcurrentMark::mark_loop_work(T* cl, jushort* live_data, uint worker_id, ParallelTaskTerminator *terminator) {
  int seed = 17;
  uint stride = CANCELLABLE ? ShenandoahMarkLoopStride : 1;

  ShenandoahHeap* heap = ShenandoahHeap::heap();
  SCMObjToScanQueueSet* queues = task_queues();
  SCMObjToScanQueue* q;
  SCMTask t;

  /*
   * Process outstanding queues, if any.
   *
   * There can be more queues than workers. To deal with the imbalance, we claim
   * extra queues first. Since marking can push new tasks into the queue associated
   * with this worker id, we come back to process this queue in the normal loop.
   */
  assert(queues->get_reserved() == heap->workers()->active_workers(),
    "Need to reserve proper number of queues");

  q = queues->claim_next();
  while (q != NULL) {
    if (CANCELLABLE && heap->cancelled_concgc()) {
      ShenandoahCancelledTerminatorTerminator tt;
      while (!terminator->offer_termination(&tt));
      return;
    }

    for (uint i = 0; i < stride; i++) {
      if (try_queue(q, t)) {
        do_task<T, COUNT_LIVENESS>(q, cl, live_data, &t);
      } else {
        assert(q->is_empty(), "Must be empty");
        q = queues->claim_next();
        break;
      }
    }
  }

  q = get_queue(worker_id);

  /*
   * Normal marking loop:
   */
  while (true) {
    if (CANCELLABLE && heap->cancelled_concgc()) {
      ShenandoahCancelledTerminatorTerminator tt;
      while (!terminator->offer_termination(&tt));
      return;
    }

    for (uint i = 0; i < stride; i++) {
      if (try_queue(q, t) ||
              (DRAIN_SATB && try_draining_satb_buffer(q, t)) ||
              queues->steal(worker_id, &seed, t)) {
        do_task<T, COUNT_LIVENESS>(q, cl, live_data, &t);
      } else {
        if (terminator->offer_termination()) return;
      }
    }
  }
}

void ShenandoahConcurrentMark::set_process_references(bool pr) {
  _process_references = pr;
}

bool ShenandoahConcurrentMark::process_references() const {
  return _process_references;
}

void ShenandoahConcurrentMark::set_unload_classes(bool uc) {
  _unload_classes = uc;
}

bool ShenandoahConcurrentMark::unload_classes() const {
  return _unload_classes;
}

bool ShenandoahConcurrentMark::claim_codecache() {
  assert(ShenandoahConcurrentCodeRoots, "must not be called otherwise");
  jbyte old = Atomic::cmpxchg(1, &_claimed_codecache, 0);
  return old == 0;
}

void ShenandoahConcurrentMark::clear_claim_codecache() {
  assert(ShenandoahConcurrentCodeRoots, "must not be called otherwise");
  _claimed_codecache = 0;
}

jushort* ShenandoahConcurrentMark::get_liveness(uint worker_id) {
  return _liveness_local[worker_id];
}
