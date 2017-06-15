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

#ifndef SHARE_VM_GC_SHENANDOAH_SHENANDOAHHEAP_INLINE_HPP
#define SHARE_VM_GC_SHENANDOAH_SHENANDOAHHEAP_INLINE_HPP

#include "gc_implementation/g1/concurrentMark.inline.hpp"
#include "memory/threadLocalAllocBuffer.inline.hpp"
#include "gc_implementation/shenandoah/brooksPointer.inline.hpp"
#include "gc_implementation/shenandoah/shenandoahBarrierSet.inline.hpp"
#include "gc_implementation/shenandoah/shenandoahHeap.hpp"
#include "gc_implementation/shenandoah/shenandoahHeapRegionSet.hpp"
#include "gc_implementation/shenandoah/shenandoahHeapRegion.inline.hpp"
#include "oops/oop.inline.hpp"
#include "runtime/atomic.hpp"
#include "runtime/prefetch.hpp"
#include "runtime/prefetch.inline.hpp"
#include "utilities/copy.hpp"

template <class T>
void SCMUpdateRefsClosure::do_oop_work(T* p) {
  T o = oopDesc::load_heap_oop(p);
  if (! oopDesc::is_null(o)) {
    oop obj = oopDesc::decode_heap_oop_not_null(o);
    _heap->update_oop_ref_not_null(p, obj);
  }
}

void SCMUpdateRefsClosure::do_oop(oop* p)       { do_oop_work(p); }
void SCMUpdateRefsClosure::do_oop(narrowOop* p) { do_oop_work(p); }

/*
 * Marks the object. Returns true if the object has not been marked before and has
 * been marked by this thread. Returns false if the object has already been marked,
 * or if a competing thread succeeded in marking this object.
 */
inline bool ShenandoahHeap::mark_next(oop obj) const {
#ifdef ASSERT
  if (! oopDesc::unsafe_equals(obj, oopDesc::bs()->read_barrier(obj))) {
    tty->print_cr("heap region containing obj:");
    ShenandoahHeapRegion* obj_region = heap_region_containing(obj);
    obj_region->print();
    tty->print_cr("heap region containing forwardee:");
    ShenandoahHeapRegion* forward_region = heap_region_containing(oopDesc::bs()->read_barrier(obj));
    forward_region->print();
  }
#endif

  assert(oopDesc::unsafe_equals(obj, oopDesc::bs()->read_barrier(obj)), "only mark forwarded copy of objects");
  return mark_next_no_checks(obj);
}

inline bool ShenandoahHeap::mark_next_no_checks(oop obj) const {
  HeapWord* addr = (HeapWord*) obj;
  return (! allocated_after_next_mark_start(addr)) && _next_mark_bit_map->parMark(addr);
}

inline bool ShenandoahHeap::is_marked_next(oop obj) const {
  HeapWord* addr = (HeapWord*) obj;
  return allocated_after_next_mark_start(addr) || _next_mark_bit_map->isMarked(addr);
}

inline bool ShenandoahHeap::is_marked_complete(oop obj) const {
  HeapWord* addr = (HeapWord*) obj;
  return allocated_after_complete_mark_start(addr) || _complete_mark_bit_map->isMarked(addr);
}

inline bool ShenandoahHeap::need_update_refs() const {
  return _need_update_refs;
}

inline size_t ShenandoahHeap::heap_region_index_containing(const void* addr) const {
  uintptr_t region_start = ((uintptr_t) addr);
  uintptr_t index = (region_start - (uintptr_t) base()) >> ShenandoahHeapRegion::region_size_shift();
#ifdef ASSERT
  if (!(index < _num_regions)) {
    tty->print_cr("heap region does not contain address, heap base: "PTR_FORMAT \
                  ", real bottom of first region: "PTR_FORMAT", num_regions: "SIZE_FORMAT", region_size: "SIZE_FORMAT,
                  p2i(base()),
                  p2i(_ordered_regions->get(0)->bottom()),
                  _num_regions,
                  ShenandoahHeapRegion::region_size_bytes());
  }
#endif
  assert(index < _num_regions, "heap region index must be in range");
  return index;
}

inline ShenandoahHeapRegion* ShenandoahHeap::heap_region_containing(const void* addr) const {
  size_t index = heap_region_index_containing(addr);
  ShenandoahHeapRegion* result = _ordered_regions->get(index);
#ifdef ASSERT
  if (!(addr >= result->bottom() && addr < result->end())) {
    tty->print_cr("heap region does not contain address, heap base: "PTR_FORMAT \
                  ", real bottom of first region: "PTR_FORMAT", num_regions: "SIZE_FORMAT,
                  p2i(base()),
                  p2i(_ordered_regions->get(0)->bottom()),
                  _num_regions);
  }
#endif
  assert(addr >= result->bottom() && addr < result->end(), "address must be in found region");
  return result;
}

template <class T>
inline oop ShenandoahHeap::update_oop_ref_not_null(T* p, oop obj) {
  if (in_collection_set(obj)) {
    oop forw = ShenandoahBarrierSet::resolve_oop_static_not_null(obj);
    assert(! oopDesc::unsafe_equals(forw, obj) || is_full_gc_in_progress() || cancelled_concgc(), "expect forwarded object");
    obj = forw;
    oopDesc::encode_store_heap_oop(p, obj);
  }
#ifdef ASSERT
  else {
    assert(oopDesc::unsafe_equals(obj, ShenandoahBarrierSet::resolve_oop_static_not_null(obj)), "expect not forwarded");
  }
#endif
  return obj;
}

template <class T>
inline oop ShenandoahHeap::maybe_update_oop_ref(T* p) {
  T o = oopDesc::load_heap_oop(p);
  if (! oopDesc::is_null(o)) {
    oop obj = oopDesc::decode_heap_oop_not_null(o);
    return maybe_update_oop_ref_not_null(p, obj);
  } else {
    return NULL;
  }
}

inline oop ShenandoahHeap::atomic_compare_exchange_oop(oop n, oop* addr, oop c) {
  return (oop) Atomic::cmpxchg_ptr(n, addr, c);
}

inline oop ShenandoahHeap::atomic_compare_exchange_oop(oop n, narrowOop* addr, oop c) {
  narrowOop cmp = oopDesc::encode_heap_oop(c);
  narrowOop val = oopDesc::encode_heap_oop(n);
  return oopDesc::decode_heap_oop((narrowOop) Atomic::cmpxchg(val, addr, cmp));
}

template <class T>
inline oop ShenandoahHeap::maybe_update_oop_ref_not_null(T* p, oop heap_oop) {

  assert((! is_in(p)) || (! in_collection_set(p))
         || is_full_gc_in_progress(),
         "never update refs in from-space, unless evacuation has been cancelled");

#ifdef ASSERT
  if (! is_in(heap_oop)) {
    print_heap_regions();
    tty->print_cr("object not in heap: "PTR_FORMAT", referenced by: "PTR_FORMAT, p2i((HeapWord*) heap_oop), p2i(p));
    assert(is_in(heap_oop), "object must be in heap");
  }
#endif
  assert(is_in(heap_oop), "only ever call this on objects in the heap");
  if (in_collection_set(heap_oop)) {
    oop forwarded_oop = ShenandoahBarrierSet::resolve_oop_static_not_null(heap_oop); // read brooks ptr
    assert(! oopDesc::unsafe_equals(forwarded_oop, heap_oop) || is_full_gc_in_progress(), "expect forwarded object");

    log_develop_trace(gc)("Updating old ref: "PTR_FORMAT" pointing to "PTR_FORMAT" to new ref: "PTR_FORMAT,
                          p2i(p), p2i(heap_oop), p2i(forwarded_oop));

    assert(forwarded_oop->is_oop(), "oop required");
    assert(is_in(forwarded_oop), "forwardee must be in heap");
    assert(oopDesc::bs()->is_safe(forwarded_oop), "forwardee must not be in collection set");
    // If this fails, another thread wrote to p before us, it will be logged in SATB and the
    // reference be updated later.
    oop result = atomic_compare_exchange_oop(forwarded_oop, p, heap_oop);

    if (oopDesc::unsafe_equals(result, heap_oop)) { // CAS successful.
      return forwarded_oop;
    } else {
      // Note: we used to assert the following here. This doesn't work because sometimes, during
      // marking/updating-refs, it can happen that a Java thread beats us with an arraycopy,
      // which first copies the array, which potentially contains from-space refs, and only afterwards
      // updates all from-space refs to to-space refs, which leaves a short window where the new array
      // elements can be from-space.
      // assert(oopDesc::is_null(result) ||
      //        oopDesc::unsafe_equals(result, ShenandoahBarrierSet::resolve_oop_static_not_null(result)),
      //       "expect not forwarded");
      return NULL;
    }
  } else {
    assert(oopDesc::unsafe_equals(heap_oop, ShenandoahBarrierSet::resolve_oop_static_not_null(heap_oop)),
           "expect not forwarded");
    return heap_oop;
  }
}

inline bool ShenandoahHeap::cancelled_concgc() const {
  return OrderAccess::load_acquire((jbyte*) &_cancelled_concgc) == 1;
}

inline bool ShenandoahHeap::try_cancel_concgc() {
  return Atomic::cmpxchg(1, &_cancelled_concgc, 0) == 0;
}

inline void ShenandoahHeap::clear_cancelled_concgc() {
  OrderAccess::release_store_fence(&_cancelled_concgc, 0);
}

inline HeapWord* ShenandoahHeap::allocate_from_gclab(Thread* thread, size_t size) {
  if (UseTLAB) {
    HeapWord* obj = thread->gclab().allocate(size);
    if (obj != NULL) {
      return obj;
    }
    // Otherwise...
    return allocate_from_gclab_slow(thread, size);
  } else {
    return NULL;
  }
}

inline void ShenandoahHeap::copy_object(oop p, HeapWord* s, size_t words) {
  assert(s != NULL, "allocation of brooks pointer must not fail");
  HeapWord* copy = s + BrooksPointer::word_size();

  guarantee(copy != NULL, "allocation of copy object must not fail");
  Copy::aligned_disjoint_words((HeapWord*) p, copy, words);
  BrooksPointer::initialize(oop(copy));

  log_develop_trace(gc, compaction)("copy object from "PTR_FORMAT" to: "PTR_FORMAT, p2i((HeapWord*) p), p2i(copy));
}

inline oop ShenandoahHeap::evacuate_object(oop p, Thread* thread, bool& evacuated) {
  evacuated = false;

  size_t required  = BrooksPointer::word_size() + p->size();

  assert(! heap_region_containing(p)->is_humongous(), "never evacuate humongous objects");

  bool alloc_from_gclab = true;
  HeapWord* filler = allocate_from_gclab(thread, required);
  if (filler == NULL) {
    filler = allocate_memory(required, _alloc_shared_gc);
    alloc_from_gclab = false;
  }

#ifdef ASSERT
  // Checking that current Java thread does not hold Threads_lock when we get here.
  // If that ever be the case, we'd deadlock in oom_during_evacuation.
  if ((! Thread::current()->is_GC_task_thread()) && (! Thread::current()->is_ConcurrentGC_thread())) {
    assert(! Threads_lock->owned_by_self()
           || SafepointSynchronize::is_at_safepoint(), "must not hold Threads_lock here");
  }
#endif

  if (filler == NULL) {
    oom_during_evacuation();
    // If this is a Java thread, it should have waited
    // until all GC threads are done, and then we
    // return the forwardee.
    oop resolved = ShenandoahBarrierSet::resolve_oop_static(p);
    return resolved;
  }

  HeapWord* copy = filler + BrooksPointer::word_size();
  copy_object(p, filler, required - BrooksPointer::word_size());

  oop copy_val = oop(copy);
  oop result = BrooksPointer::try_update_forwardee(p, copy_val);

  oop return_val;
  if (oopDesc::unsafe_equals(result, p)) {
    evacuated = true;
    return_val = copy_val;

    log_develop_trace(gc, compaction)("Copy of "PTR_FORMAT" to "PTR_FORMAT" succeeded \n",
                                      p2i((HeapWord*) p), p2i(copy));

#ifdef ASSERT
    assert(return_val->is_oop(), "expect oop");
    assert(p->klass() == return_val->klass(), err_msg("Should have the same class p: "PTR_FORMAT", copy: "PTR_FORMAT,
						      p2i((HeapWord*) p), p2i((HeapWord*) copy)));
#endif
  }  else {
    if (alloc_from_gclab) {
      thread->gclab().rollback(required);
    }
    log_develop_trace(gc, compaction)("Copy of "PTR_FORMAT" to "PTR_FORMAT" failed, use other: "PTR_FORMAT,
                                      p2i((HeapWord*) p), p2i(copy), p2i((HeapWord*) result));
    return_val = result;
  }

  return return_val;
}

inline bool ShenandoahHeap::requires_marking(const void* entry) const {
  return ! is_marked_next(oop(entry));
}

bool ShenandoahHeap::region_in_collection_set(size_t region_index) const {
  return _in_cset_fast_test_base[region_index] == 1;
}

bool ShenandoahHeap::in_collection_set(ShenandoahHeapRegion* r) const {
  return region_in_collection_set(r->region_number());
}

template <class T>
inline bool ShenandoahHeap::in_collection_set(T p) const {
  HeapWord* obj = (HeapWord*) p;
  assert(_in_cset_fast_test != NULL, "sanity");
  assert(is_in(obj), "should be in heap");

  // no need to subtract the bottom of the heap from obj,
  // _in_cset_fast_test is biased
  uintx index = ((uintx) obj) >> ShenandoahHeapRegion::region_size_shift();
  return _in_cset_fast_test[index];
}

inline bool ShenandoahHeap::concurrent_mark_in_progress() {
  return _concurrent_mark_in_progress != 0;
}

inline address ShenandoahHeap::concurrent_mark_in_progress_addr() {
  return (address) &(ShenandoahHeap::heap()->_concurrent_mark_in_progress);
}

inline address ShenandoahHeap::update_refs_in_progress_addr() {
  return (address) &(ShenandoahHeap::heap()->_update_refs_in_progress);
}

inline bool ShenandoahHeap::is_evacuation_in_progress() {
  return _evacuation_in_progress != 0;
}

inline bool ShenandoahHeap::allocated_after_next_mark_start(HeapWord* addr) const {
  uintx index = ((uintx) addr) >> ShenandoahHeapRegion::region_size_shift();
  HeapWord* top_at_mark_start = _next_top_at_mark_starts[index];
  bool alloc_after_mark_start = addr >= top_at_mark_start;
  return alloc_after_mark_start;
}

inline bool ShenandoahHeap::allocated_after_complete_mark_start(HeapWord* addr) const {
  uintx index = ((uintx) addr) >> ShenandoahHeapRegion::region_size_shift();
  HeapWord* top_at_mark_start = _complete_top_at_mark_starts[index];
  bool alloc_after_mark_start = addr >= top_at_mark_start;
  return alloc_after_mark_start;
}

template<class T>
inline void ShenandoahHeap::marked_object_iterate(ShenandoahHeapRegion* region, T* cl) {
  marked_object_iterate(region, cl, region->top());
}

template<class T>
inline void ShenandoahHeap::marked_object_safe_iterate(ShenandoahHeapRegion* region, T* cl) {
  marked_object_iterate(region, cl, region->concurrent_iteration_safe_limit());
}

template<class T>
inline void ShenandoahHeap::marked_object_iterate(ShenandoahHeapRegion* region, T* cl, HeapWord* limit) {
  assert(BrooksPointer::word_offset() < 0, "skip_delta calculation below assumes the forwarding ptr is before obj");

  CMBitMap* mark_bit_map = _complete_mark_bit_map;
  HeapWord* top_at_mark_start = complete_top_at_mark_start(region->bottom());

  size_t skip_bitmap_delta = BrooksPointer::word_size() + 1;
  size_t skip_objsize_delta = BrooksPointer::word_size() /* + actual obj.size() below */;
  HeapWord* start = region->bottom() + BrooksPointer::word_size();

  HeapWord* end = MIN2(top_at_mark_start + BrooksPointer::word_size(), _ordered_regions->end());
  HeapWord* addr = mark_bit_map->getNextMarkedWordAddress(start, end);

  intx dist = ShenandoahMarkScanPrefetch;
  if (dist > 0) {
    // Batched scan that prefetches the oop data, anticipating the access to
    // either header, oop field, or forwarding pointer. Not that we cannot
    // touch anything in oop, while it still being prefetched to get enough
    // time for prefetch to work. This is why we try to scan the bitmap linearly,
    // disregarding the object size. However, since we know forwarding pointer
    // preceeds the object, we can skip over it. Once we cannot trust the bitmap,
    // there is no point for prefetching the oop contents, as oop->size() will
    // touch it prematurely.

    // No variable-length arrays in standard C++, have enough slots to fit
    // the prefetch distance.
    static const int SLOT_COUNT = 256;
    guarantee(dist <= SLOT_COUNT, "adjust slot count");
    oop slots[SLOT_COUNT];

    bool aborting = false;
    int avail;
    do {
      avail = 0;
      for (int c = 0; (c < dist) && (addr < limit); c++) {
        Prefetch::read(addr, BrooksPointer::byte_offset());
        oop obj = oop(addr);
        slots[avail++] = obj;
        if (addr < top_at_mark_start) {
          addr += skip_bitmap_delta;
          addr = mark_bit_map->getNextMarkedWordAddress(addr, end);
        } else {
          // cannot trust mark bitmap anymore, finish the current stride,
          // and switch to accurate traversal
          addr += obj->size() + skip_objsize_delta;
          aborting = true;
        }
      }

      for (int c = 0; c < avail; c++) {
        do_marked_object(mark_bit_map, cl, slots[c]);
      }
    } while (avail > 0 && !aborting);

    // accurate traversal
    while (addr < limit) {
      oop obj = oop(addr);
      int size = obj->size();
      do_marked_object(mark_bit_map, cl, obj);
      addr += size + skip_objsize_delta;
    }
  } else {
    while (addr < limit) {
      oop obj = oop(addr);
      int size = obj->size();
      do_marked_object(mark_bit_map, cl, obj);
      addr += size + skip_objsize_delta;
      if (addr < top_at_mark_start) {
        addr = mark_bit_map->getNextMarkedWordAddress(addr, end);
      }
    }
  }
}

template<class T>
inline void ShenandoahHeap::do_marked_object(CMBitMap* bitmap, T* cl, oop obj) {
  assert(!oopDesc::is_null(obj), "sanity");
  assert(obj->is_oop(), "sanity");
  assert(is_in(obj), "sanity");
  assert(bitmap == _complete_mark_bit_map, "only iterate completed mark bitmap");
  assert(is_marked_complete(obj), "object expected to be marked");
  cl->do_object(obj);
}

template <class T>
class ShenandoahObjectToOopClosure : public ObjectClosure {
  T* _cl;
public:
  ShenandoahObjectToOopClosure(T* cl) : _cl(cl) {}

  void do_object(oop obj) {
    obj->oop_iterate(_cl);
  }
};

template <class T>
class ShenandoahObjectToOopBoundedClosure : public ObjectClosure {
  T* _cl;
  MemRegion _bounds;
public:
  ShenandoahObjectToOopBoundedClosure(T* cl, HeapWord* bottom, HeapWord* top) :
    _cl(cl), _bounds(bottom, top) {}

  void do_object(oop obj) {
    obj->oop_iterate(_cl, _bounds);
  }
};

template<class T>
inline void ShenandoahHeap::marked_object_oop_iterate(ShenandoahHeapRegion* region, T* cl, HeapWord* top) {
  if (region->is_humongous()) {
    HeapWord* bottom = region->bottom();
    if (top > bottom) {
      // Go to start of humongous region.
      size_t idx = region->region_number();
      while (! region->is_humongous_start()) {
        assert(idx > 0, "sanity");
        idx--;
        region = _ordered_regions->get(idx);
      }
      ShenandoahObjectToOopBoundedClosure<T> objs(cl, bottom, top);
      marked_object_iterate(region, &objs);
    }
  } else {
    ShenandoahObjectToOopClosure<T> objs(cl);
    marked_object_iterate(region, &objs, top);
  }
}

template<class T>
inline void ShenandoahHeap::marked_object_oop_iterate(ShenandoahHeapRegion* region, T* cl) {
  marked_object_oop_iterate(region, cl, region->top());
}

template<class T>
inline void ShenandoahHeap::marked_object_oop_safe_iterate(ShenandoahHeapRegion* region, T* cl) {
  marked_object_oop_iterate(region, cl, region->concurrent_iteration_safe_limit());
}
#endif // SHARE_VM_GC_SHENANDOAH_SHENANDOAHHEAP_INLINE_HPP
