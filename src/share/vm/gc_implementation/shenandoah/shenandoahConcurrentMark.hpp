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

#ifndef SHARE_VM_GC_SHENANDOAH_SHENANDOAHCONCURRENTMARK_HPP
#define SHARE_VM_GC_SHENANDOAH_SHENANDOAHCONCURRENTMARK_HPP

#include "utilities/taskqueue.hpp"
#include "utilities/workgroup.hpp"
#include "gc_implementation/shenandoah/shenandoahTaskqueue.hpp"

typedef BufferedOverflowTaskQueue<ObjArrayFromToTask, mtGC> ShenandoahBufferedOverflowTaskQueue;
typedef Padded<ShenandoahBufferedOverflowTaskQueue> SCMObjToScanQueue;

class ShenandoahConcurrentMark;

#ifdef ASSERT
class ShenandoahVerifyRootsClosure1 : public OopClosure {
private:
  template <class T>
  inline void do_oop_work(T* p);

public:
  void do_oop(oop* p);
  void do_oop(narrowOop* p);
};
#endif

template <class T, bool CL>
class ShenandoahMarkObjsClosure {
  ShenandoahHeap* _heap;
  T _mark_refs;
  SCMObjToScanQueue* _queue;
  uint _last_region_idx;
  size_t _live_data;
public:
  ShenandoahMarkObjsClosure(SCMObjToScanQueue* q, ReferenceProcessor* rp);
  ~ShenandoahMarkObjsClosure();

  inline void do_object_or_array(oop obj, int from, int to);
  inline void do_array(objArrayOop array, int from, int to);
  inline void count_liveness(oop obj);
};

class ShenandoahConcurrentMark: public CHeapObj<mtGC> {

private:
  // The per-worker-thread work queues
  SCMObjToScanQueueSet* _task_queues;

  bool _process_references;
  bool _unload_classes;

  jbyte _claimed_codecache;

public:
  // We need to do this later when the heap is already created.
  void initialize(uint workers);

  void set_process_references(bool pr);
  bool process_references() const;

  void set_unload_classes(bool uc);
  bool unload_classes() const;

  bool claim_codecache();
  void clear_claim_codecache();

  static inline void mark_and_push(oop obj, ShenandoahHeap* heap, SCMObjToScanQueue* q);

  void mark_from_roots();

  // Prepares unmarked root objects by marking them and putting
  // them into the marking task queue.
  void init_mark_roots();
  void mark_roots();
  void update_roots();
  void final_update_roots();

  void shared_finish_mark_from_roots(bool full_gc);
  void finish_mark_from_roots();
  // Those are only needed public because they're called from closures.

  template <class T, bool CL>
  void concurrent_mark_loop(ShenandoahMarkObjsClosure<T, CL>* cl, uint worker_id, SCMObjToScanQueue* q, ParallelTaskTerminator* t);

  template <class T, bool CL>
  void final_mark_loop(ShenandoahMarkObjsClosure<T, CL>* cl, uint worker_id, SCMObjToScanQueue* q, ParallelTaskTerminator* t);

  inline bool try_queue(SCMObjToScanQueue* q, ObjArrayFromToTask &task);

  SCMObjToScanQueue* get_queue(uint worker_id);
  void clear_queue(SCMObjToScanQueue *q);

  inline bool try_draining_satb_buffer(SCMObjToScanQueue *q, ObjArrayFromToTask &task);
  void drain_satb_buffers(uint worker_id, bool remark = false);
  SCMObjToScanQueueSet* task_queues() { return _task_queues;}

  void cancel();

private:

#ifdef ASSERT
  void verify_roots();
#endif

  void weak_refs_work();

  /**
   * Process assigned queue and others if there are any to be claimed.
   * Return false if the process is terminated by concurrent gc cancellation.
   */
  template <class T, bool CL>
  bool concurrent_process_queues(ShenandoahHeap* heap, SCMObjToScanQueue* q, ShenandoahMarkObjsClosure<T, CL>* cl);

#if TASKQUEUE_STATS
  static void print_taskqueue_stats_hdr(outputStream* const st = tty);
  void print_taskqueue_stats() const;
  void reset_taskqueue_stats();
#endif // TASKQUEUE_STATS

};

#endif // SHARE_VM_GC_SHENANDOAH_SHENANDOAHCONCURRENTMARK_HPP
