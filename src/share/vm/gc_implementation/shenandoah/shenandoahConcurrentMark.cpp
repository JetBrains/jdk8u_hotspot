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

#include "gc_implementation/shared/gcTimer.hpp"
#include "gc_implementation/shared/isGCActiveMark.hpp"
#include "gc_implementation/shenandoah/shenandoahBarrierSet.inline.hpp"
#include "gc_implementation/shenandoah/shenandoahConcurrentMark.inline.hpp"
#include "gc_implementation/shenandoah/shenandoahHeap.inline.hpp"
#include "gc_implementation/shenandoah/shenandoahRootProcessor.hpp"
#include "gc_implementation/shenandoah/shenandoahOopClosures.inline.hpp"
#include "gc_implementation/shenandoah/shenandoah_specialized_oop_closures.hpp"
#include "gc_implementation/shenandoah/brooksPointer.hpp"
#include "memory/referenceProcessor.hpp"
#include "memory/sharedHeap.hpp"
#include "code/codeCache.hpp"
#include "classfile/symbolTable.hpp"
#include "classfile/systemDictionary.hpp"
#include "memory/iterator.inline.hpp"
#include "oops/oop.inline.hpp"
#include "utilities/taskqueue.hpp"

class ShenandoahMarkUpdateRootsClosure : public OopClosure {
  SCMObjToScanQueue* _queue;
  ShenandoahHeap* _heap;

public:
  ShenandoahMarkUpdateRootsClosure(SCMObjToScanQueue* q) :
    _queue(q),
    _heap((ShenandoahHeap*) Universe::heap())
  {
  }

  void do_oop(narrowOop* p) {
    Unimplemented();
  }

  inline void do_oop(oop* p) {
    oop obj = oopDesc::load_heap_oop(p);
    if (! oopDesc::is_null(obj)) {
      obj = _heap->update_oop_ref_not_null(p, obj);
      ShenandoahConcurrentMark::mark_and_push(obj, _heap, _queue);
    }
  }

};

class ShenandoahMarkRootsClosure : public OopClosure {
  SCMObjToScanQueue* _queue;
  ShenandoahHeap* _heap;

public:
  ShenandoahMarkRootsClosure(SCMObjToScanQueue* q) :
    _queue(q),
    _heap((ShenandoahHeap*) Universe::heap())
  {
  }

  void do_oop(narrowOop* p) {
    Unimplemented();
  }

  inline void do_oop(oop* p) {
    oop obj = oopDesc::load_heap_oop(p);
    if (! oopDesc::is_null(obj)) {
      assert(obj == ShenandoahBarrierSet::resolve_oop_static_not_null(obj),
             "expect forwarded oop");
      ShenandoahConcurrentMark::mark_and_push(obj, _heap, _queue);
    }
  }

};


class SCMUpdateRefsClosure: public OopClosure {
private:
  ShenandoahHeap* _heap;
public:

  SCMUpdateRefsClosure() : _heap(ShenandoahHeap::heap()) {
  }

  inline void do_oop(oop* p) {
    oop obj = oopDesc::load_heap_oop(p);
    if (! oopDesc::is_null(obj)) {
      _heap->update_oop_ref_not_null(p, obj);
    }
  }

  void do_oop(narrowOop* p) {
    Unimplemented();
  }
};

// Mark the object and add it to the queue to be scanned
template <class T>
ShenandoahMarkObjsClosure<T>::ShenandoahMarkObjsClosure(QHolder* q) :
  _heap((ShenandoahHeap*)(Universe::heap())),
  _mark_refs(T(q)),
  _queue(q),
  _last_region_idx(0),
  _live_data(0)
{
}

template <class T>
ShenandoahMarkObjsClosure<T>::~ShenandoahMarkObjsClosure() {
  // tty->print_cr("got "SIZE_FORMAT" x region: "UINT32_FORMAT, _live_data_count, _last_region_idx);
  ShenandoahHeapRegion* r = _heap->heap_regions()[_last_region_idx];
  r->increase_live_data(_live_data);
}

ShenandoahMarkUpdateRefsClosure::ShenandoahMarkUpdateRefsClosure(QHolder* q) :
  MetadataAwareOopClosure(((ShenandoahHeap *) Universe::heap())->ref_processor()),
  _queue(q),
  _heap((ShenandoahHeap*) Universe::heap())
{
}

ShenandoahMarkRefsClosure::ShenandoahMarkRefsClosure(QHolder* q) :
  MetadataAwareOopClosure(((ShenandoahHeap *) Universe::heap())->ref_processor()),
  _queue(q),
  _heap((ShenandoahHeap*) Universe::heap())
{
}

class ShenandoahMarkRootsTask : public AbstractGangTask {
private:
  ShenandoahRootProcessor* _rp;
  bool _update_refs;
public:
  ShenandoahMarkRootsTask(ShenandoahRootProcessor* rp, bool update_refs) :
    AbstractGangTask("Shenandoah update roots task"), _update_refs(update_refs),
    _rp(rp) {
  }

  void work(uint worker_id) {
    // tty->print_cr("start mark roots worker: "INT32_FORMAT, worker_id);
    ShenandoahHeap* heap = ShenandoahHeap::heap();
    SCMObjToScanQueue* q = heap->concurrentMark()->get_queue(worker_id);
    OopClosure* cl;
    ShenandoahMarkUpdateRootsClosure mark_update_cl(q);
    ShenandoahMarkRootsClosure mark_cl(q);
    if (_update_refs) {
      cl = &mark_update_cl;
    } else {
      cl = &mark_cl;
    }
    CodeBlobToOopClosure blobsCl(cl, true);
    CLDToOopClosure cldCl(cl);

    ResourceMark m;
    if (ClassUnloadingWithConcurrentMark) {
      SCMUpdateRefsClosure uprefs;
      CodeBlobToOopClosure upcode(&uprefs, true);
      CLDToOopClosure upcld(&uprefs, false);
      _rp->process_roots(cl, &uprefs, &cldCl, &upcld, &cldCl, &blobsCl, &upcode);
    } else {
      _rp->process_all_roots(cl, &cldCl, &blobsCl);
      SCMUpdateRefsClosure uprefs;
      ShenandoahAlwaysTrueClosure always_true;
      JNIHandles::weak_oops_do(&always_true, &uprefs);
    }
    // tty->print_cr("finish mark roots worker: "INT32_FORMAT, worker_id);
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
    QHolder qh(q);
    if (_update_refs) {
      ShenandoahMarkObjsClosure<ShenandoahMarkUpdateRefsClosure> cl(&qh);
      _cm->concurrent_mark_loop(&cl, worker_id, q, _terminator);
    } else {
      ShenandoahMarkObjsClosure<ShenandoahMarkRefsClosure> cl(&qh);
      _cm->concurrent_mark_loop(&cl, worker_id, q,  _terminator);
    }
    ShenandoahHeap* heap = ShenandoahHeap::heap();
    if (ShenandoahTracePhases && heap->cancelled_concgc()) {
      tty->print_cr("Cancelled concurrent marking");
    }
  }
};

class SCMFinalMarkingTask : public AbstractGangTask {
private:
  ShenandoahConcurrentMark* _cm;
  ParallelTaskTerminator* _terminator;
  bool _update_refs;

public:
  SCMFinalMarkingTask(ShenandoahConcurrentMark* cm, ParallelTaskTerminator* terminator, bool update_refs) :
    AbstractGangTask("Shenandoah Final Marking"), _cm(cm), _terminator(terminator), _update_refs(update_refs) {
  }

  void work(uint worker_id) {

    SCMObjToScanQueue* q = _cm->get_queue(worker_id);
    QHolder qh(q);
    if (_update_refs) {
      ShenandoahMarkObjsClosure<ShenandoahMarkUpdateRefsClosure> cl(&qh);
      _cm->final_mark_loop(&cl, worker_id, q, _terminator);
    } else {
      ShenandoahMarkObjsClosure<ShenandoahMarkRefsClosure> cl(&qh);
      _cm->final_mark_loop(&cl, worker_id, q, _terminator);
    }
  }
};

void ShenandoahConcurrentMark::prepare_unmarked_root_objs() {

  ShenandoahHeap* heap = ShenandoahHeap::heap();
  bool update_refs = heap->need_update_refs();

  if (update_refs) {
    COMPILER2_PRESENT(DerivedPointerTable::clear());
  }

  prepare_unmarked_root_objs_no_derived_ptrs(update_refs);

  if (update_refs) {
    COMPILER2_PRESENT(DerivedPointerTable::update_pointers());
  }

}

void ShenandoahConcurrentMark::prepare_unmarked_root_objs_no_derived_ptrs(bool update_refs) {
  assert(Thread::current()->is_VM_thread(), "can only do this in VMThread");

  ShenandoahHeap* heap = ShenandoahHeap::heap();
  heap->set_par_threads(_max_conc_worker_id);
  heap->conc_workers()->set_active_workers(_max_conc_worker_id);
  ShenandoahRootProcessor root_proc(heap, _max_conc_worker_id);
  TASKQUEUE_STATS_ONLY(reset_taskqueue_stats());
  ShenandoahMarkRootsTask mark_roots(&root_proc, update_refs);
  heap->conc_workers()->run_task(&mark_roots);
  heap->set_par_threads(0);

  if (! ShenandoahProcessReferences) {
    ShenandoahMarkUpdateRootsClosure cl(get_queue(0));
    heap->weak_roots_iterate(&cl);
  }
  // tty->print_cr("all root marker threads done");
}


void ShenandoahConcurrentMark::initialize() {
  _max_conc_worker_id = MAX2((uint) ConcGCThreads, 1U);
  _task_queues = new SCMObjToScanQueueSet((int) _max_conc_worker_id);

  for (uint i = 0; i < _max_conc_worker_id; ++i) {
    SCMObjToScanQueue* task_queue = new SCMObjToScanQueue();
    task_queue->initialize();
    _task_queues->register_queue(i, task_queue);
  }
  JavaThread::satb_mark_queue_set().set_buffer_size(1014 /* G1SATBBufferSize */);
}

void ShenandoahConcurrentMark::mark_from_roots() {
  if (ShenandoahGCVerbose) {
    tty->print_cr("STOPPING THE WORLD: before marking");
    tty->print_cr("Starting markFromRoots");
  }

  ShenandoahHeap* sh = (ShenandoahHeap *) Universe::heap();

  bool update_refs = sh->need_update_refs();

  sh->shenandoahPolicy()->record_phase_start(ShenandoahCollectorPolicy::conc_mark);
  ParallelTaskTerminator terminator(_max_conc_worker_id, _task_queues);

  if (ShenandoahProcessReferences) {
    ReferenceProcessor* rp = sh->ref_processor();
    // enable ("weak") refs discovery
    rp->enable_discovery(true /*verify_no_refs*/, true);
    rp->setup_policy(false); // snapshot the soft ref policy to be used in this cycle
  }

  SCMConcurrentMarkingTask markingTask = SCMConcurrentMarkingTask(this, &terminator, update_refs);
  sh->set_par_threads(_max_conc_worker_id);
  sh->conc_workers()->set_active_workers(_max_conc_worker_id);
  sh->conc_workers()->run_task(&markingTask);
  sh->set_par_threads(0);

  if (ShenandoahGCVerbose) {
    tty->print("total workers = %u active workers = %u\n",
               sh->conc_workers()->total_workers(),
               sh->conc_workers()->active_workers());
    TASKQUEUE_STATS_ONLY(print_taskqueue_stats());
    TASKQUEUE_STATS_ONLY(reset_taskqueue_stats());
  }

  if (ShenandoahGCVerbose) {
    tty->print_cr("Finishing markFromRoots");
    tty->print_cr("RESUMING THE WORLD: after marking");
  }

  sh->shenandoahPolicy()->record_phase_end(ShenandoahCollectorPolicy::conc_mark);
}

class FinishDrainSATBBuffersTask : public AbstractGangTask {
private:
  ShenandoahConcurrentMark* _cm;
  ParallelTaskTerminator* _terminator;
public:
  FinishDrainSATBBuffersTask(ShenandoahConcurrentMark* cm, ParallelTaskTerminator* terminator) :
    AbstractGangTask("Finish draining SATB buffers"), _cm(cm), _terminator(terminator) {
  }

  void work(uint worker_id) {
    _cm->drain_satb_buffers(worker_id, true);
  }
};

void ShenandoahConcurrentMark::finish_mark_from_roots() {
  if (ShenandoahGCVerbose) {
    tty->print_cr("Starting finishMarkFromRoots");
  }

  IsGCActiveMark is_active;

  ShenandoahHeap* sh = (ShenandoahHeap *) Universe::heap();

  // Trace any (new) unmarked root references.
  sh->shenandoahPolicy()->record_phase_start(ShenandoahCollectorPolicy::rescan_roots);
  prepare_unmarked_root_objs();
  sh->shenandoahPolicy()->record_phase_end(ShenandoahCollectorPolicy::rescan_roots);
  sh->shenandoahPolicy()->record_phase_start(ShenandoahCollectorPolicy::drain_satb);
  {
    SharedHeap::StrongRootsScope scope(sh, true);
    ParallelTaskTerminator terminator(_max_conc_worker_id, _task_queues);
    // drain_satb_buffers(0, true);
    FinishDrainSATBBuffersTask drain_satb_buffers(this, &terminator);
    sh->set_par_threads(_max_conc_worker_id);
    sh->conc_workers()->set_active_workers(_max_conc_worker_id);
    sh->conc_workers()->run_task(&drain_satb_buffers);
    sh->set_par_threads(0);
    sh->shenandoahPolicy()->record_phase_end(ShenandoahCollectorPolicy::drain_satb);
  }

  // Finally mark everything else we've got in our queues during the previous steps.
  {
    sh->shenandoahPolicy()->record_phase_start(ShenandoahCollectorPolicy::drain_queues);
    ParallelTaskTerminator terminator(_max_conc_worker_id, _task_queues);
    SCMFinalMarkingTask markingTask = SCMFinalMarkingTask(this, &terminator, sh->need_update_refs());
    sh->set_par_threads(_max_conc_worker_id);
    sh->conc_workers()->run_task(&markingTask);
    sh->set_par_threads(0);
    sh->shenandoahPolicy()->record_phase_end(ShenandoahCollectorPolicy::drain_queues);
  }

#ifdef ASSERT
  for (int i = 0; i < (int) _max_conc_worker_id; i++) {
    assert(_task_queues->queue(i)->is_empty(), "Should be empty");
  }
#endif

  // When we're done marking everything, we process weak references.
  if (ShenandoahProcessReferences) {
    sh->shenandoahPolicy()->record_phase_start(ShenandoahCollectorPolicy::weakrefs);
    weak_refs_work();
    sh->shenandoahPolicy()->record_phase_end(ShenandoahCollectorPolicy::weakrefs);
  }

  // And finally finish class unloading
  if (ClassUnloadingWithConcurrentMark) {
    sh->shenandoahPolicy()->record_phase_start(ShenandoahCollectorPolicy::class_unloading);
    ShenandoahIsAliveClosure is_alive;
    // Unload classes and purge SystemDictionary.
    bool purged_class = SystemDictionary::do_unloading(&is_alive, true);
    // Unload nmethods.
    CodeCache::do_unloading(&is_alive, purged_class);
    // Prune dead klasses from subklass/sibling/implementor lists.
    Klass::clean_weak_klass_links(&is_alive);
    // Delete entries from dead interned strings.
    // Clean up unreferenced symbols in symbol table.
    sh->unlink_string_and_symbol_table(&is_alive);

    ClassLoaderDataGraph::purge();
    sh->shenandoahPolicy()->record_phase_end(ShenandoahCollectorPolicy::class_unloading);
  }

#ifdef ASSERT
  for (int i = 0; i < (int) _max_conc_worker_id; i++) {
    assert(_task_queues->queue(i)->is_empty(), "Should be empty");
  }
#endif

  if (ShenandoahGCVerbose) {
    tty->print_cr("Finishing finishMarkFromRoots");
#ifdef SLOWDEBUG
    for (int i = 0; i <(int)_max_conc_worker_id; i++) {
      tty->print("Queue: "INT32_FORMAT":", i);
      _task_queues->queue(i)->stats.print(tty, 10);
      tty->cr();
      _task_queues->queue(i)->stats.verify();
    }
#endif
  }

#ifdef ASSERT
  verify_roots();

  if (ShenandoahDumpHeapAfterConcurrentMark) {
    sh->ensure_parsability(false);
    sh->print_all_refs("post-mark");
  }
#endif
}

#ifdef ASSERT
void ShenandoahVerifyRootsClosure1::do_oop(oop* p) {
  oop obj = oopDesc::load_heap_oop(p);
  if (! oopDesc::is_null(obj)) {
    guarantee(ShenandoahHeap::heap()->is_marked_current(obj), "oop must be marked");
    guarantee(obj == ShenandoahBarrierSet::resolve_oop_static_not_null(obj), "oop must not be forwarded");
  }
}

void ShenandoahConcurrentMark::verify_roots() {
  ShenandoahVerifyRootsClosure1 cl;
  CodeBlobToOopClosure blobsCl(&cl, false);
  CLDToOopClosure cldCl(&cl);
  ClassLoaderDataGraph::clear_claimed_marks();
  ShenandoahRootProcessor rp(ShenandoahHeap::heap(), 1);
  rp.process_roots(&cl, &cl, &cldCl, &cldCl, &cldCl, &blobsCl, &blobsCl);

  ShenandoahAlwaysTrueClosure always_true;
  JNIHandles::weak_oops_do(&always_true, &cl);
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

  // tty->print_cr("start draining SATB buffers");

  ShenandoahHeap* sh = (ShenandoahHeap*) Universe::heap();
  SCMObjToScanQueue* q = get_queue(worker_id);
  ShenandoahSATBBufferClosure cl(q);

  SATBMarkQueueSet& satb_mq_set = JavaThread::satb_mark_queue_set();
  while (satb_mq_set.apply_closure_to_completed_buffer(&cl));

  if (remark) {
    ShenandoahSATBThreadsClosure tc(&cl);
    Threads::threads_do(&tc);
  }

  // tty->print_cr("end draining SATB buffers");

}

#if TASKQUEUE_STATS
void ShenandoahConcurrentMark::print_taskqueue_stats_hdr(outputStream* const st) {
  st->print_raw_cr("GC Task Stats");
  st->print_raw("thr "); TaskQueueStats::print_header(1, st); st->cr();
  st->print_raw("--- "); TaskQueueStats::print_header(2, st); st->cr();
}

void ShenandoahConcurrentMark::print_taskqueue_stats(outputStream* const st) const {
  print_taskqueue_stats_hdr(st);
  ShenandoahHeap* sh = (ShenandoahHeap*) Universe::heap();
  TaskQueueStats totals;
  const int n = sh->max_conc_workers();
  for (int i = 0; i < n; ++i) {
    st->print(INT32_FORMAT_W(3), i);
    _task_queues->queue(i)->stats.print(st);
    st->print("\n");
    totals += _task_queues->queue(i)->stats;
  }
  st->print_raw("tot "); totals.print(st); st->cr();
  DEBUG_ONLY(totals.verify());

}

void ShenandoahConcurrentMark::print_push_only_taskqueue_stats(outputStream* const st) const {
  print_taskqueue_stats_hdr(st);
  ShenandoahHeap* sh = (ShenandoahHeap*) Universe::heap();
  TaskQueueStats totals;
  const int n = sh->max_conc_workers();
  for (int i = 0; i < n; ++i) {
    st->print(INT32_FORMAT_W(3), i);
    _task_queues->queue(i)->stats.print(st);
    st->print("\n");
    totals += _task_queues->queue(i)->stats;
  }
  st->print_raw("tot "); totals.print(st); st->cr();
}

void ShenandoahConcurrentMark::reset_taskqueue_stats() {
  ShenandoahHeap* sh = (ShenandoahHeap*) Universe::heap();
  const int n = sh->max_conc_workers();
  for (int i = 0; i < n; ++i) {
    _task_queues->queue(i)->stats.reset();
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

    ShenandoahHeap* sh = ShenandoahHeap::heap();
    ShenandoahConcurrentMark* scm = sh->concurrentMark();
    SCMObjToScanQueue* q = scm->get_queue(_worker_id);
    QHolder qh(q);
    if (sh->need_update_refs()) {
      ShenandoahMarkObjsClosure<ShenandoahMarkUpdateRefsClosure> cl(&qh);
      scm->final_mark_loop(&cl, _worker_id, q, _terminator);
    } else {
      ShenandoahMarkObjsClosure<ShenandoahMarkRefsClosure> cl(&qh);
      scm->final_mark_loop(&cl, _worker_id, q, _terminator);
    }
  }
};


class ShenandoahCMKeepAliveClosure: public OopClosure {
  SCMObjToScanQueue* _queue;
  ShenandoahHeap* _sh;

public:
  ShenandoahCMKeepAliveClosure(SCMObjToScanQueue* q) :
    _queue(q) {
    _sh = (ShenandoahHeap*) Universe::heap();
  }

  void do_oop(narrowOop* p) {
    assert(false, "narrowOops Aren't implemented");
  }


  void do_oop(oop* p) {

    oop obj = oopDesc::load_heap_oop(p);
    assert(obj == oopDesc::bs()->read_barrier(obj), "only get updated oops in weak ref processing");

    if (! oopDesc::is_null(obj)) {
      if (Verbose && ShenandoahTraceWeakReferences) {
        gclog_or_tty->print_cr("\twe're looking at location "
                               "*"PTR_FORMAT" = "PTR_FORMAT,
                               p2i(p), p2i((void*) obj));
        obj->print();
      }
      ShenandoahConcurrentMark::mark_and_push(obj, _sh, _queue);
    }
  }

};

class ShenandoahCMKeepAliveUpdateClosure: public OopClosure {
  SCMObjToScanQueue* _queue;
  ShenandoahHeap* _sh;

public:
  ShenandoahCMKeepAliveUpdateClosure(SCMObjToScanQueue* q) :
    _queue(q) {
    _sh = (ShenandoahHeap*) Universe::heap();
  }

  void do_oop(narrowOop* p) {
    assert(false, "narrowOops Aren't implemented");
  }


  void do_oop(oop* p) {

    oop obj = oopDesc::load_heap_oop(p);
    if (! oopDesc::is_null(obj)) {
      obj = _sh->update_oop_ref_not_null(p, obj);
      assert(obj == oopDesc::bs()->read_barrier(obj), "only get updated oops in weak ref processing");
      if (Verbose && ShenandoahTraceWeakReferences) {
        gclog_or_tty->print_cr("\twe're looking at location "
                               "*"PTR_FORMAT" = "PTR_FORMAT,
                               p2i(p), p2i((void*) obj));
        obj->print();
      }
      ShenandoahConcurrentMark::mark_and_push(obj, _sh, _queue);
    }

  }

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
    ShenandoahHeap* heap = ShenandoahHeap::heap();
    ShenandoahIsAliveClosure is_alive;
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

  ShenandoahRefProcTaskExecutor() : _workers(ShenandoahHeap::heap()->conc_workers()) {
  }

  // Executes a task using worker threads.
  void execute(ProcessTask& task) {
    ShenandoahHeap* heap = ShenandoahHeap::heap();
    ShenandoahConcurrentMark* cm = heap->concurrentMark();
    ParallelTaskTerminator terminator(cm->max_conc_worker_id(), cm->task_queues());
    ShenandoahRefProcTaskProxy proc_task_proxy(task, &terminator);
    heap->set_par_threads(cm->max_conc_worker_id());
    _workers->run_task(&proc_task_proxy);
    heap->set_par_threads(0);
  }

  void execute(EnqueueTask& task) {
    ShenandoahHeap* heap = ShenandoahHeap::heap();
    ShenandoahConcurrentMark* cm = heap->concurrentMark();
    ShenandoahRefEnqueueTaskProxy enqueue_task_proxy(task);
    heap->set_par_threads(cm->max_conc_worker_id());
    _workers->run_task(&enqueue_task_proxy);
    heap->set_par_threads(0);
  }
};


void ShenandoahConcurrentMark::weak_refs_work() {
   ShenandoahHeap* sh = (ShenandoahHeap*) Universe::heap();
   ReferenceProcessor* rp = sh->ref_processor();

   // Setup collector policy for softref cleaning.
   bool clear_soft_refs = sh->collector_policy()->use_should_clear_all_soft_refs(true /* bogus arg*/);
   if (ShenandoahTraceWeakReferences) {
     tty->print_cr("clearing soft refs: %s", BOOL_TO_STR(clear_soft_refs));
   }
   rp->setup_policy(clear_soft_refs);

   uint serial_worker_id = 0;
   ShenandoahIsAliveClosure is_alive;
   ParallelTaskTerminator terminator(1, task_queues());
   ShenandoahCMDrainMarkingStackClosure complete_gc(serial_worker_id, &terminator);
   ShenandoahRefProcTaskExecutor executor;

   if (ShenandoahTraceWeakReferences) {
     gclog_or_tty->print_cr("start processing references");
   }

   if (sh->need_update_refs()) {
     ShenandoahCMKeepAliveUpdateClosure keep_alive(get_queue(serial_worker_id));
     rp->process_discovered_references(&is_alive, &keep_alive,
                                       &complete_gc, &executor,
                                       NULL,
                                       ShenandoahHeap::heap()->tracer()->gc_id());
   } else {
     ShenandoahCMKeepAliveClosure keep_alive(get_queue(serial_worker_id));
     rp->process_discovered_references(&is_alive, &keep_alive,
                                       &complete_gc, &executor,
                                       NULL,
                                       ShenandoahHeap::heap()->tracer()->gc_id());
   }

#ifdef ASSERT
   for (int i = 0; i < (int) _max_conc_worker_id; i++) {
     assert(_task_queues->queue(i)->is_empty(), "Should be empty");
   }
#endif

   if (ShenandoahTraceWeakReferences) {
     gclog_or_tty->print_cr("finished processing references");
     gclog_or_tty->print_cr("start enqueuing references");
   }

   rp->enqueue_discovered_references(&executor);

   if (ShenandoahTraceWeakReferences) {
     gclog_or_tty->print_cr("finished enqueueing references");
   }

   rp->verify_no_references_recorded();
   assert(!rp->discovery_enabled(), "Post condition");

}

void ShenandoahConcurrentMark::cancel() {
  ShenandoahHeap* sh = ShenandoahHeap::heap();

  // Cancel weak-ref discovery.
  if (ShenandoahProcessReferences) {
    ReferenceProcessor* rp = sh->ref_processor();
    rp->abandon_partial_discovery();
    rp->disable_discovery();
  }

  // Clean up marking stacks.
  SCMObjToScanQueueSet* queues = task_queues();
  for (uint i = 0; i < _max_conc_worker_id; ++i) {
    SCMObjToScanQueue* task_queue = queues->queue(i);
    task_queue->set_empty();
    task_queue->overflow_stack()->clear();
  }

  // Cancel SATB buffers.
  JavaThread::satb_mark_queue_set().abandon_partial_marking();
}
SCMObjToScanQueue* ShenandoahConcurrentMark::get_queue(uint worker_id) {
  worker_id = worker_id % _max_conc_worker_id;
  return _task_queues->queue(worker_id);
}

template <class T>
void ShenandoahConcurrentMark::concurrent_mark_loop(ShenandoahMarkObjsClosure<T>* cl,
                                                    uint worker_id,
                                                    SCMObjToScanQueue* q,
                                                    ParallelTaskTerminator* terminator) {
  ShenandoahHeap* heap = ShenandoahHeap::heap();
  int seed = 17;
  while (true) {
    if (heap->cancelled_concgc() ||
        (!try_queue(q, cl) &&
         !try_draining_an_satb_buffer(q) &&
         !try_to_steal(worker_id, cl, &seed))
        ) {
      if (terminator->offer_termination()) break;
    }
  }
}

template <class T>
void ShenandoahConcurrentMark::final_mark_loop(ShenandoahMarkObjsClosure<T>* cl,
                                               uint worker_id,
                                               SCMObjToScanQueue* q,
                                               ParallelTaskTerminator* terminator) {
  ShenandoahHeap* heap = ShenandoahHeap::heap();
  int seed = 17;
  while (true) {
    if (!try_queue(q, cl) &&
        !try_to_steal(worker_id, cl, &seed)) {
      if (terminator->offer_termination()) break;
    }
  }
}
