/*
 * Copyright (c) 2016, Red Hat, Inc. and/or its affiliates.
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

#ifndef SHARE_VM_GC_SHENANDOAH_SHENANDOAH_TASKQUEUE_HPP
#define SHARE_VM_GC_SHENANDOAH_SHENANDOAH_TASKQUEUE_HPP

#include "memory/padded.hpp"
#include "utilities/taskqueue.hpp"
#include "runtime/mutex.hpp"

class Thread;

template<class E, MEMFLAGS F, unsigned int N = TASKQUEUE_SIZE>
class BufferedOverflowTaskQueue: public OverflowTaskQueue<E, F, N>
{
public:
  typedef OverflowTaskQueue<E, F, N> taskqueue_t;

  BufferedOverflowTaskQueue() : _buf_empty(true) {};

  TASKQUEUE_STATS_ONLY(using taskqueue_t::stats;)

  // Push task t onto:
  //   - first, try buffer;
  //   - then, try the queue;
  //   - then, overflow stack.
  // Return true.
  inline bool push(E t);

  // Attempt to pop from the buffer; return true if anything was popped.
  inline bool pop_buffer(E &t);

  inline void clear_buffer()  { _buf_empty = true; }
  inline bool buffer_empty()  const { return _buf_empty; }
  inline bool is_empty()        const {
    return taskqueue_t::is_empty() && buffer_empty();
  }

private:
  bool _buf_empty;
  E _elem;
};

class ObjArrayFromToTask
{
public:
  ObjArrayFromToTask(oop o = NULL, int from = 0, int to = 0): _obj(o), _from(from), _to(to) { }
  ObjArrayFromToTask(oop o, size_t from, size_t to): _obj(o), _from(int(from)), _to(int(to)) {
    assert(from <= size_t(max_jint), "too big");
    assert(to <= size_t(max_jint), "too big");
    assert(from < to, "sanity");
  }
  ObjArrayFromToTask(const ObjArrayFromToTask& t): _obj(t._obj), _from(t._from), _to(t._to) { }

  ObjArrayFromToTask& operator =(const ObjArrayFromToTask& t) {
    _obj = t._obj;
    _from = t._from;
    _to = t._to;
    return *this;
  }
  volatile ObjArrayFromToTask&
  operator =(const volatile ObjArrayFromToTask& t) volatile {
    (void)const_cast<oop&>(_obj = t._obj);
    _from = t._from;
    _to = t._to;
    return *this;
  }

  inline oop obj()   const { return _obj; }
  inline int from() const { return _from; }
  inline int to() const { return _to; }

  DEBUG_ONLY(bool is_valid() const); // Tasks to be pushed/popped must be valid.

private:
  oop _obj;
  int _from, _to;
};

typedef BufferedOverflowTaskQueue<ObjArrayFromToTask, mtGC> ShenandoahBufferedOverflowTaskQueue;
typedef Padded<ShenandoahBufferedOverflowTaskQueue> SCMObjToScanQueue;
// typedef GenericTaskQueueSet<SCMObjToScanQueue, mtGC> SCMObjToScanQueueSet;


template <class T, MEMFLAGS F>
class ParallelClaimableQueueSet: public GenericTaskQueueSet<T, F> {
private:
  volatile jint     _claimed_index;
  debug_only(uint   _reserved;  )

public:
  using GenericTaskQueueSet<T, F>::size;

public:
  ParallelClaimableQueueSet(int n) : GenericTaskQueueSet<T, F>(n) {
    debug_only(_reserved = 0; )
  }

  void clear_claimed() { _claimed_index = 0; }
  T*   claim_next();

  // reserve queues that not for parallel claiming
  void reserve(uint n) {
    assert(n <= size(), "Sanity");
    _claimed_index = (jint)n;
    debug_only(_reserved = n;)
  }

  debug_only(uint get_reserved() const { return (uint)_reserved; })
};


template <class T, MEMFLAGS F>
T* ParallelClaimableQueueSet<T, F>::claim_next() {
  jint size = (jint)GenericTaskQueueSet<T, F>::size();

  if (_claimed_index >= size) {
    return NULL;
  }

  jint index = Atomic::add(1, &_claimed_index);

  if (index <= size) {
    return GenericTaskQueueSet<T, F>::queue((uint)index - 1);
  } else {
    return NULL;
  }
}

class SCMObjToScanQueueSet: public ParallelClaimableQueueSet<SCMObjToScanQueue, mtGC> {

public:
  SCMObjToScanQueueSet(int n) : ParallelClaimableQueueSet<SCMObjToScanQueue, mtGC>(n) {
  }

  bool is_empty();

  void clear();
};


/*
 * This is an enhanced implementation of Google's work stealing
 * protocol, which is described in the paper:
 * Understanding and improving JVM GC work stealing at the data center scale
 * (http://dl.acm.org/citation.cfm?id=2926706)
 *
 * Instead of a dedicated spin-master, our implementation will let spin-master to relinquish
 * the role before it goes to sleep/wait, so allows newly arrived thread to compete for the role.
 * The intention of above enhancement, is to reduce spin-master's latency on detecting new tasks
 * for stealing and termination condition.
 */

class ShenandoahTaskTerminator: public ParallelTaskTerminator {
private:
  Monitor*    _blocker;
  Thread*     _spin_master;


public:
  ShenandoahTaskTerminator(uint n_threads, TaskQueueSetSuper* queue_set) :
    ParallelTaskTerminator(n_threads, queue_set), _spin_master(NULL) {
    _blocker = new Monitor(Mutex::leaf, "ShenandoahTaskTerminator", false);
  }

  bool offer_termination(TerminatorTerminator* terminator);

private:
  size_t tasks_in_queue_set() { return _queue_set->tasks(); }


  /*
   * Perform spin-master task.
   * return true if termination condition is detected
   * otherwise, return false
   */
  bool do_spin_master_work();
};

#endif // SHARE_VM_GC_SHENANDOAH_SHENANDOAH_TASKQUEUE_HPP
