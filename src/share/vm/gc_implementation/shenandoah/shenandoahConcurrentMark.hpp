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

typedef OverflowTaskQueue<ObjArrayTask, mtGC> ShenandoahOverflowTaskQueue;
typedef Padded<ShenandoahOverflowTaskQueue> SCMObjToScanQueue;
typedef GenericTaskQueueSet<SCMObjToScanQueue, mtGC> SCMObjToScanQueueSet;

class ShenandoahConcurrentMark;

#ifdef ASSERT
class ShenandoahVerifyRootsClosure1 : public OopClosure {
  void do_oop(oop* p);

  void do_oop(narrowOop* p) {
    Unimplemented();
  }
};
#endif

class ShenandoahMarkRefsClosure : public MetadataAwareOopClosure {
  SCMObjToScanQueue* _queue;
  ShenandoahHeap* _heap;
  bool _update_refs;
  ShenandoahConcurrentMark* _scm;

public:
  ShenandoahMarkRefsClosure(SCMObjToScanQueue* q, bool update_refs);

  void do_oop(narrowOop* p);

  inline void do_oop(oop* p);

};

class ShenandoahMarkObjsClosure {
  ShenandoahHeap* _heap;
  ShenandoahMarkRefsClosure _mark_refs;
  SCMObjToScanQueue* _queue;
  uint _last_region_idx;
  size_t _live_data;
  size_t _live_data_count;
public:
  ShenandoahMarkObjsClosure(SCMObjToScanQueue* q, bool update_refs);
  ~ShenandoahMarkObjsClosure();

  inline void do_object(oop obj, int index);
};

class ShenandoahConcurrentMark: public CHeapObj<mtGC> {

private:
  // The per-worker-thread work queues
  SCMObjToScanQueueSet* _task_queues;

  bool                    _aborted;
  uint _max_conc_worker_id;
  ParallelTaskTerminator* _terminator;

public:
  // We need to do this later when the heap is already created.
  void initialize();

  static inline void mark_and_push(oop obj, ShenandoahHeap* heap, SCMObjToScanQueue* q);

  void mark_from_roots();

  // Prepares unmarked root objects by marking them and putting
  // them into the marking task queue.
  void prepare_unmarked_root_objs();
  void prepare_unmarked_root_objs_no_derived_ptrs(bool update_refs);

  void finish_mark_from_roots();
  // Those are only needed public because they're called from closures.

  SCMObjToScanQueue* get_queue(uint worker_id);
  inline bool try_queue(SCMObjToScanQueue* q, ShenandoahMarkObjsClosure* cl);
  inline bool try_to_steal(uint worker_id, ShenandoahMarkObjsClosure* cl, int *seed);
  inline bool try_draining_an_satb_buffer(uint worker_id);
  void drain_satb_buffers(uint worker_id, bool remark = false);
  SCMObjToScanQueueSet* task_queues() { return _task_queues;}
  uint max_conc_worker_id() { return _max_conc_worker_id; }

  void cancel();

private:

#ifdef ASSERT
  void verify_roots();
#endif

  bool drain_one_satb_buffer(uint worker_id);
  void weak_refs_work();

  ParallelTaskTerminator* terminator() { return _terminator;}

#if TASKQUEUE_STATS
  static void print_taskqueue_stats_hdr(outputStream* const st = gclog_or_tty);
  void print_taskqueue_stats(outputStream* const st = gclog_or_tty) const;
  void print_push_only_taskqueue_stats(outputStream* const st = gclog_or_tty) const;
  void reset_taskqueue_stats();
#endif // TASKQUEUE_STATS

};

#endif // SHARE_VM_GC_SHENANDOAH_SHENANDOAHCONCURRENTMARK_HPP
