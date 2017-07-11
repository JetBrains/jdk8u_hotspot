/*
 * Copyright (c) 2017, Red Hat, Inc. and/or its affiliates.
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

#include "memory/allocation.hpp"
#include "gc_implementation/shenandoah/shenandoahHeap.hpp"
#include "gc_implementation/shenandoah/shenandoahHeap.inline.hpp"
#include "gc_implementation/shenandoah/shenandoahHeapRegion.hpp"
#include "gc_implementation/shenandoah/shenandoahHeapRegion.inline.hpp"
#include "gc_implementation/shenandoah/shenandoahVerifier.hpp"
#include "gc_implementation/shenandoah/brooksPointer.hpp"
#include "gc_implementation/shenandoah/shenandoahRootProcessor.hpp"
#include "gc_implementation/shenandoah/shenandoahTaskqueue.hpp"
#include "gc_implementation/shenandoah/shenandoahTaskqueue.inline.hpp"

class VerifyReachableHeapClosure : public ExtendedOopClosure {
private:
  const char* _phase;
  ShenandoahVerifier::VerifyOptions _options;
  ShenandoahVerifierStack* _stack;
  ShenandoahHeap* _heap;
  CMBitMap* _map;
  void* _interior_loc;
  oop _loc;

public:
  VerifyReachableHeapClosure(ShenandoahVerifierStack* stack, CMBitMap* map, const char* phase, ShenandoahVerifier::VerifyOptions options) :
          _stack(stack), _heap(ShenandoahHeap::heap()), _map(map), _loc(NULL), _interior_loc(NULL),
          _phase(phase), _options(options) {};

private:
  void print_obj(MessageBuffer& msg, oop obj) {
    ShenandoahHeapRegion *r = _heap->heap_region_containing(obj);
    stringStream ss;
    r->print_on(&ss);

    msg.append("  " PTR_FORMAT " - klass " PTR_FORMAT " %s\n", p2i(obj), p2i(obj->klass()), obj->klass()->external_name());
    msg.append("    %3s allocated after complete mark start\n", _heap->allocated_after_complete_mark_start((HeapWord *) obj) ? "" : "not");
    msg.append("    %3s allocated after next mark start\n",     _heap->allocated_after_next_mark_start((HeapWord *) obj)     ? "" : "not");
    msg.append("    %3s marked complete\n",      _heap->is_marked_complete(obj) ? "" : "not");
    msg.append("    %3s marked next\n",          _heap->is_marked_next(obj) ? "" : "not");
    msg.append("    %3s in collection set\n",    _heap->in_collection_set(obj) ? "" : "not");
    msg.append("  region: %s", ss.as_string());
  }

  void print_non_obj(MessageBuffer& msg, void* loc) {
    msg.append("  outside of Java heap\n");
    stringStream ss;
    os::print_location(&ss, (intptr_t) loc, false);
    msg.append("  %s\n", ss.as_string());
  }

  void print_failure(oop obj, const char* label) {
    ResourceMark rm;

    bool loc_in_heap = (_loc != NULL && _heap->is_in(_loc));

    MessageBuffer msg("Shenandoah verification failed; %s: %s\n\n", _phase, label);

    msg.append("Referenced from:\n");
    if (_interior_loc != NULL) {
      msg.append("  interior location: " PTR_FORMAT "\n", p2i(_interior_loc));
      if (loc_in_heap) {
        print_obj(msg, _loc);
      } else {
        print_non_obj(msg, _interior_loc);
      }
    } else {
      msg.append("  no location recorded, probably a plain heap scan\n");
    }
    msg.append("\n");

    msg.append("Object:\n");
    print_obj(msg, obj);
    msg.append("\n");

    oop fwd = BrooksPointer::forwardee(obj);
    if (!oopDesc::unsafe_equals(obj, fwd)) {
      msg.append("Forwardee:\n");
      print_obj(msg, fwd);
      msg.append("\n");
    }

    oop fwd2 = BrooksPointer::forwardee(fwd);
    if (!oopDesc::unsafe_equals(fwd, fwd2)) {
      msg.append("Second forwardee:\n");
      print_obj(msg, fwd2);
      msg.append("\n");
    }

    report_vm_error(__FILE__, __LINE__, msg.buffer());
  }

  void verify(oop obj, bool test, const char* label) {
    if (!test) {
      print_failure(obj, label);
    }
  }

  template <class T>
  void do_oop_work(T* p) {
    T o = oopDesc::load_heap_oop(p);
    if (!oopDesc::is_null(o)) {
      oop obj = oopDesc::decode_heap_oop_not_null(o);

      // Single threaded verification can use faster non-atomic stack and bitmap
      // methods.
      //
      // For performance reasons, only fully verify non-marked field values.
      // We are here when the host object for *p is already marked.

      HeapWord* addr = (HeapWord*) obj;
      if (_map->parMark(addr)) {
        verify_oop_at(p, obj);
        _stack->push(VerifierTask(obj));
      }
    }
  }

  void verify_oop(oop obj) {
    // Perform basic consistency checks first, so that we can call extended verification
    // report calling methods on obj and forwardee freely.
    //
    guarantee(_heap->is_in(obj),
              err_msg("oop must be in heap: " PTR_FORMAT, p2i(obj)));
    guarantee(check_obj_alignment(obj),
              err_msg("oop must be aligned: " PTR_FORMAT, p2i(obj)));
    guarantee(obj->is_oop(),
              err_msg("oop must be an oop: " PTR_FORMAT, p2i(obj)));
    guarantee(Metaspace::contains(obj->klass()),
              err_msg("klass pointer must go to metaspace: "
                              "obj = " PTR_FORMAT ", klass = " PTR_FORMAT, p2i(obj), p2i(obj->klass())));

    oop fwd = (oop) BrooksPointer::get_raw(obj);
    if (!oopDesc::unsafe_equals(obj, fwd)) {
      guarantee(_heap->is_in(fwd),
                err_msg("Forwardee must be in heap: "
                                "obj = " PTR_FORMAT ", forwardee = " PTR_FORMAT, p2i(obj), p2i(fwd)));
      guarantee(!oopDesc::is_null(fwd),
                err_msg("Forwardee is set: "
                                "obj = " PTR_FORMAT ", forwardee = " PTR_FORMAT, p2i(obj), p2i(fwd)));
      guarantee(check_obj_alignment(fwd),
                err_msg("Forwardee must be aligned: "
                                "obj = " PTR_FORMAT ", forwardee = " PTR_FORMAT, p2i(obj), p2i(fwd)));
      guarantee(fwd->is_oop(),
                err_msg("Forwardee must be an oop: "
                                "obj = " PTR_FORMAT ", forwardee = " PTR_FORMAT, p2i(obj), p2i(fwd)));
      guarantee(Metaspace::contains(fwd->klass()),
                err_msg("Forwardee klass pointer must go to metaspace: "
                                "obj = " PTR_FORMAT ", klass = " PTR_FORMAT, p2i(obj), p2i(obj->klass())));
      guarantee(obj->klass() == fwd->klass(),
                err_msg("Forwardee and Object klasses should agree: "
                                "obj = " PTR_FORMAT ", obj-klass = " PTR_FORMAT ", "
                        "fwd = " PTR_FORMAT ", fwd-klass = " PTR_FORMAT,
                        p2i(obj), p2i(obj->klass()), p2i(fwd), p2i(fwd->klass())));

      oop fwd2 = (oop) BrooksPointer::get_raw(fwd);
      verify(obj, oopDesc::unsafe_equals(fwd, fwd2),
             "Double forwarding");
    }

    // Verify that object and forwardee fit their regions:
    {
      ShenandoahHeapRegion *obj_reg = _heap->heap_region_containing(obj);

      HeapWord *obj_addr = (HeapWord *) obj;
      verify(obj, obj_addr < obj_reg->top(),
             "Object start should be within the region");

      if (!obj_reg->is_humongous()) {
        verify(obj, (obj_addr + obj->size()) <= obj_reg->top(),
               "Object end should be within the region");
      } else {
        size_t humongous_start = obj_reg->region_number();
        size_t humongous_end = humongous_start + (obj->size() / ShenandoahHeapRegion::region_size_words());
        for (size_t idx = humongous_start + 1; idx < humongous_end; idx++) {
          verify(obj, _heap->regions()->get(idx)->is_humongous_continuation(),
                 "Humongous object is in continuation that fits it");
        }
      }
    }

    {
      if (!oopDesc::unsafe_equals(obj, fwd)) {
        ShenandoahHeapRegion *fwd_reg = _heap->heap_region_containing(fwd);
        verify(obj, !fwd_reg->is_humongous(),
               "Should have no humongous forwardees");

        HeapWord *fwd_addr = (HeapWord *) fwd;
        verify(obj, fwd_addr < fwd_reg->top(),
               "Forwardee start should be within the region");
        verify(obj, (fwd_addr + fwd->size()) <= fwd_reg->top(),
               "Forwardee end should be within the region");
      }
    }

    switch (_options._verify_marked) {
      case ShenandoahVerifier::_verify_marked_disable:
        // skip
        break;
      case ShenandoahVerifier::_verify_marked_next:
        verify(obj, _heap->is_marked_next(obj),
               "Must be marked in next bitmap");
        break;
      case ShenandoahVerifier::_verify_marked_complete:
        verify(obj, _heap->is_marked_complete(obj),
               "Must be marked in complete bitmap");
        break;
      default:
        assert(false, "Unhandled mark verification");
    }

    switch (_options._verify_forwarded) {
      case ShenandoahVerifier::_verify_forwarded_disable:
        // skip
        break;
      case ShenandoahVerifier::_verify_forwarded_none: {
        verify(obj, oopDesc::unsafe_equals(obj, fwd),
               "Should not be forwarded");
        break;
      }
      case ShenandoahVerifier::_verify_forwarded_allow: {
        if (!oopDesc::unsafe_equals(obj, fwd)) {
          verify(obj, _heap->heap_region_containing(obj) != _heap->heap_region_containing(fwd),
                 "Forwardee should be in another region");
        }
        break;
      }
      default:
        assert(false, "Unhandled forwarding verification");
    }

    switch (_options._verify_cset) {
      case ShenandoahVerifier::_verify_cset_disable:
        // skip
        break;
      case ShenandoahVerifier::_verify_cset_none:
        verify(obj, !_heap->in_collection_set(obj),
               "Should not have references to collection set");
        break;
      case ShenandoahVerifier::_verify_cset_forwarded:
        if (_heap->in_collection_set(obj)) {
          verify(obj, !oopDesc::unsafe_equals(obj, fwd),
                 "Object in collection set, should have forwardee");
        }
        break;
      default:
        assert(false, "Unhandled cset verification");
    }
  }

public:

  /**
   * Verify object with known interior reference.
   * @param p interior reference where the object is referenced from; can be off-heap
   * @param obj verified object
   */
  template <class T>
  void verify_oop_at(T* p, oop obj) {
    _interior_loc = p;
    verify_oop(obj);
    _interior_loc = NULL;
  }

  /**
   * Verify object without known interior reference.
   * Useful when picking up the object at known offset in heap,
   * but without knowing what objects reference it.
   * @param obj verified object
   */
  void verify_oop_standalone(oop obj) {
    _interior_loc = NULL;
    verify_oop(obj);
    _interior_loc = NULL;
  }

  /**
   * Verify oop fields from this object.
   * @param obj host object for verified fields
   */
  void verify_oops_from(oop obj) {
    _loc = obj;
    obj->oop_iterate(this);
    _loc = NULL;
  }


  void do_oop(oop* p) { do_oop_work(p); }
  void do_oop(narrowOop* p) { do_oop_work(p); }
};

class CalculateRegionStatsClosure : public ShenandoahHeapRegionClosure {
private:
  size_t _used, _garbage;
public:
  CalculateRegionStatsClosure() : _used(0), _garbage(0) {};

  bool doHeapRegion(ShenandoahHeapRegion* r) {
    _used += r->used();
    _garbage += r->garbage();
    return false;
  }

  size_t used() { return _used; }
  size_t garbage() { return _garbage; }
};

class VerifyHeapRegionClosure : public ShenandoahHeapRegionClosure {
private:
  ShenandoahHeap* _heap;
public:
  VerifyHeapRegionClosure() : _heap(ShenandoahHeap::heap()) {};

  void print_failure(ShenandoahHeapRegion* r, const char* label) {
    ResourceMark rm;

    MessageBuffer msg("Shenandoah verification failed; %s\n\n", label);

    stringStream ss;
    r->print_on(&ss);
    msg.append("%s", ss.as_string());

    report_vm_error(__FILE__, __LINE__, msg.buffer());
  }

  void verify(ShenandoahHeapRegion* r, bool test, const char* msg) {
    if (!test) {
      print_failure(r, msg);
    }
  }

  bool doHeapRegion(ShenandoahHeapRegion* r) {
    verify(r, r->capacity() == ShenandoahHeapRegion::region_size_bytes(),
           "Capacity should match region size");

    verify(r, r->bottom() <= _heap->complete_top_at_mark_start(r->bottom()),
           "Region top should not be less than bottom");

    verify(r, _heap->complete_top_at_mark_start(r->bottom()) <= r->top(),
           "Complete TAMS should not be larger than top");

    verify(r, (r->get_live_data_bytes() <= r->capacity()),
           "Live data cannot be larger than capacity");

    verify(r, (r->garbage() <= r->capacity()) || (r->is_humongous_start()),
           "Garbage cannot be larger than capacity");

    verify(r, r->used() <= r->capacity(),
           "Used cannot be larger than capacity");

    verify(r, r->get_shared_allocs() <= r->capacity(),
           "Shared alloc count should not be larger than capacity");

    verify(r, r->get_tlab_allocs() <= r->capacity(),
           "TLAB alloc count should not be larger than capacity");

    verify(r, r->get_gclab_allocs() <= r->capacity(),
           "GCLAB alloc count should not be larger than capacity");

    verify(r, r->get_shared_allocs() + r->get_tlab_allocs() + r->get_gclab_allocs() == r->used(),
           "Accurate accounting: shared + TLAB + GCLAB = used");

    verify(r, !r->is_humongous_start() || !r->is_humongous_continuation(),
           "Region cannot be both humongous start and humongous continuation");

    verify(r, !r->is_pinned() || !r->in_collection_set(),
           "Region cannot be both pinned and in collection set");

    return false;
  }
};

class ShenandoahVerifierReachableTask : public AbstractGangTask {
private:
  const char* _label;
  ShenandoahRootProcessor* _rp;
  ShenandoahVerifier::VerifyOptions _options;
  ShenandoahHeap* _heap;
  CMBitMap* _bitmap;
  volatile jlong _processed;

public:
  ShenandoahVerifierReachableTask(CMBitMap* bitmap,
                                  ShenandoahRootProcessor* rp,
                                  const char* label,
                                  ShenandoahVerifier::VerifyOptions options) :
          AbstractGangTask("Shenandoah Parallel Verifier Reachable Task"),
          _heap(ShenandoahHeap::heap()), _rp(rp), _bitmap(bitmap), _processed(0),
          _label(label), _options(options) {};

  size_t processed() {
    return (size_t) _processed;
  }

  virtual void work(uint worker_id) {
    ResourceMark rm;
    ShenandoahVerifierStack stack;

    // On level 2, we need to only check the roots once.
    // On level 3, we want to check the roots, and seed the local stack.
    // It is a lesser evil to accept multiple root scans at level 3, because
    // extended parallelism would buy us out.
    if (((ShenandoahVerifyLevel == 2) && (worker_id == 0))
        || (ShenandoahVerifyLevel >= 3)) {
        VerifyReachableHeapClosure cl(&stack, _bitmap,
                                      MessageBuffer("%s, Roots", _label),
                                      _options);
        _rp->process_all_roots_slow(&cl);
    }

    jlong processed = 0;

    if (ShenandoahVerifyLevel >= 3) {
      VerifyReachableHeapClosure cl(&stack, _bitmap,
                                    MessageBuffer("%s, Reachable", _label),
                                    _options);
      while (!stack.is_empty()) {
        processed++;
        VerifierTask task = stack.pop();
        cl.verify_oops_from(task.obj());
      }
    }

    Atomic::add(processed, &_processed);
  }
};

class ShenandoahVerifierMarkedRegionTask : public AbstractGangTask {
private:
  const char* _label;
  ShenandoahVerifier::VerifyOptions _options;
  ShenandoahHeap *_heap;
  ShenandoahHeapRegionSet* _regions;
  CMBitMap* _bitmap;
  jlong _claimed;
  volatile jlong _processed;

public:
  ShenandoahVerifierMarkedRegionTask(ShenandoahHeapRegionSet* regions, CMBitMap* bitmap,
                                     const char* label,
                                     ShenandoahVerifier::VerifyOptions options) :
          AbstractGangTask("Shenandoah Parallel Verifier Marked Region"),
          _heap(ShenandoahHeap::heap()), _regions(regions), _bitmap(bitmap), _claimed(0), _processed(0),
          _label(label), _options(options) {};

  size_t processed() {
    return (size_t) _processed;
  }

  virtual void work(uint worker_id) {
    ShenandoahVerifierStack stack;
    VerifyReachableHeapClosure cl(&stack, _bitmap,
                                  MessageBuffer("%s, Marked", _label),
                                  _options);

    while (true) {
      size_t v = (size_t) (Atomic::add(1, &_claimed) - 1);
      if (v < _heap->num_regions()) {
        ShenandoahHeapRegion* r = _regions->get(v);
        if (!r->is_humongous()) {
          work_region(r, stack, cl);
        }
      } else {
        break;
      }
    }
  }

  virtual void work_region(ShenandoahHeapRegion *r, ShenandoahVerifierStack& stack, VerifyReachableHeapClosure& cl) {
    jlong processed = 0;
    CMBitMap* mark_bit_map = _heap->complete_mark_bit_map();
    HeapWord* tams = _heap->complete_top_at_mark_start(r->bottom());

    // Bitmaps, before TAMS
    if (tams > r->bottom()) {
      HeapWord* start = r->bottom() + BrooksPointer::word_size();
      HeapWord* addr = mark_bit_map->getNextMarkedWordAddress(start, tams);

      while (addr < tams) {
        verify_and_follow(addr, stack, cl, &processed);
        addr += BrooksPointer::word_size();
        if (addr < tams) {
          addr = mark_bit_map->getNextMarkedWordAddress(addr, tams);
        }
      }
    }

    // Size-based, after TAMS
    {
      HeapWord* limit = r->top();
      HeapWord* addr = tams + BrooksPointer::word_size();

      while (addr < limit) {
        verify_and_follow(addr, stack, cl, &processed);
        addr += oop(addr)->size() + BrooksPointer::word_size();
      }
    }

    Atomic::add(processed, &_processed);
  }

  void verify_and_follow(HeapWord *addr, ShenandoahVerifierStack &stack, VerifyReachableHeapClosure &cl, jlong *processed) {
    if (!_bitmap->parMark(addr)) return;

    // Verify the object itself:
    oop obj = oop(addr);
    cl.verify_oop_standalone(obj);

    // Verify everything reachable from that object too:
    stack.push(obj);
    while (!stack.is_empty()) {
      (*processed)++;
      VerifierTask task = stack.pop();
      cl.verify_oops_from(task.obj());
    }
  }
};

void ShenandoahVerifier::verify_at_safepoint(const char *label,
                                             VerifyForwarded forwarded, VerifyMarked marked,
                                             VerifyMatrix matrix, VerifyCollectionSet cset) {
  guarantee(SafepointSynchronize::is_at_safepoint(), "only when nothing else happens");
  guarantee(ShenandoahVerify, "only when enabled, and bitmap is initialized in ShenandoahHeap::initialize");

  log_info(gc)("Starting level " INTX_FORMAT " verification: %s", ShenandoahVerifyLevel, label);

  // Heap size checks
  {
    CalculateRegionStatsClosure cl;
    _heap->heap_region_iterate(&cl);
    size_t heap_used = _heap->used();
    guarantee(cl.used() == heap_used,
              err_msg("heap used size must be consistent: heap-used = " SIZE_FORMAT ", regions-used = " SIZE_FORMAT,
                      heap_used, cl.used()));
  }

  // Internal heap region checks
  if (ShenandoahVerifyLevel >= 1) {
    VerifyHeapRegionClosure cl;
    _heap->heap_region_iterate(&cl, true, true);
  }

  OrderAccess::fence();
  _heap->ensure_parsability(false);

  // Allocate temporary bitmap for storing marking wavefront:
  MemRegion mr = MemRegion(_verification_bit_map->startWord(), _verification_bit_map->endWord());
  _verification_bit_map->clear_range_large(mr);

  const VerifyOptions& options = ShenandoahVerifier::VerifyOptions(forwarded, marked, matrix, cset);

  ShenandoahRootProcessor rp(_heap, _heap->max_workers(),
                             ShenandoahCollectorPolicy::_num_phases); // no need for stats

  // Steps 1-2. Scan root set to get initial reachable set. Finish walking the reachable heap.
  // This verifies what application can see, since it only cares about reachable objects.
  size_t count_reachable = 0;
  {
    ShenandoahVerifierReachableTask task(_verification_bit_map, &rp, label, options);
    _heap->workers()->run_task(&task);
    count_reachable = task.processed();
  }

  // Step 3. Walk marked objects. Marked objects might be unreachable. This verifies what collector,
  // not the application, can see during the region scans. There is no reason to process the objects
  // that were already verified, e.g. those marked in verification bitmap. There is interaction with TAMS:
  // before TAMS, we verify the bitmaps, if available; after TAMS, we walk until the top(). It mimics
  // what marked_object_iterate is doing, without calling into that optimized (and possibly incorrect)
  // version

  size_t count_marked = 0;
  if (ShenandoahVerifyLevel >= 4 && marked == _verify_marked_complete) {
    ShenandoahVerifierMarkedRegionTask task(_heap->regions(), _verification_bit_map, label, options);
    _heap->workers()->run_task(&task);
    count_marked = task.processed();
  } else {
    guarantee(ShenandoahVerifyLevel < 4 || marked == _verify_marked_next || marked == _verify_marked_disable, "Should be");
  }

  log_info(gc)("Verification finished: " SIZE_FORMAT " reachable, " SIZE_FORMAT " marked", count_reachable, count_marked);
}

void ShenandoahVerifier::verify_generic(VerifyOption vo) {
  verify_at_safepoint(
          "Generic Verification",
          _verify_forwarded_allow,     // conservatively allow forwarded
          _verify_marked_disable,      // do not verify marked: lots ot time wasted checking dead allocations
          _verify_matrix_disable,      // matrix can be inconsistent here
          _verify_cset_disable         // cset may be inconsistent
  );
}

void ShenandoahVerifier::verify_before_concmark() {
  if (_heap->need_update_refs()) {
    verify_at_safepoint(
            "Before Mark",
            _verify_forwarded_allow,     // may have forwarded references
            _verify_marked_disable,      // do not verify marked: lots ot time wasted checking dead allocations
            _verify_matrix_disable,      // matrix is foobared
            _verify_cset_forwarded       // allow forwarded references to cset
    );
  } else {
    verify_at_safepoint(
            "Before Mark",
            _verify_forwarded_none,      // UR should have fixed up
            _verify_marked_disable,      // do not verify marked: lots ot time wasted checking dead allocations
            _verify_matrix_conservative, // UR should have fixed matrix
            _verify_cset_none            // UR should have fixed this
    );
  }
}

void ShenandoahVerifier::verify_after_concmark() {
  // No need, will unconditionally do evacuation
}

void ShenandoahVerifier::verify_before_evacuation() {
  verify_at_safepoint(
          "Before Evacuation",
          _verify_forwarded_none,      // no forwarded references
          _verify_marked_complete,     // bitmaps as precise as we can get
          _verify_matrix_disable,      // matrix might be foobared
          _verify_cset_none            // no cset, no references to it
  );
}

void ShenandoahVerifier::verify_after_evacuation() {
  verify_at_safepoint(
          "After Evacuation",
          _verify_forwarded_allow,     // objects are still forwarded
          _verify_marked_complete,     // bitmaps might be stale, but alloc-after-mark should be well
          _verify_matrix_disable,      // matrix is inconsistent here
          _verify_cset_forwarded       // all cset refs are fully forwarded
  );
}

void ShenandoahVerifier::verify_before_updaterefs() {
  verify_at_safepoint(
          "Before Updating References",
          _verify_forwarded_allow,     // forwarded references allowed
          _verify_marked_complete,     // bitmaps might be stale, but alloc-after-mark should be well
          _verify_matrix_disable,      // matrix is inconsistent here
          _verify_cset_forwarded       // all cset refs are fully forwarded
  );
}

void ShenandoahVerifier::verify_after_updaterefs() {
  verify_at_safepoint(
          "After Updating References",
          _verify_forwarded_none,      // no forwarded references
          _verify_marked_complete,     // bitmaps might be stale, but alloc-after-mark should be well
          _verify_matrix_conservative, // matrix is conservatively consistent
          _verify_cset_none            // no cset references, all updated
  );
}

void ShenandoahVerifier::verify_before_partial() {
  verify_at_safepoint(
          "Before Partial GC",
          _verify_forwarded_none,      // cannot have forwarded objects
          _verify_marked_complete,     // bitmaps might be stale, but alloc-after-mark should be well
          _verify_matrix_conservative, // matrix is conservatively consistent
          _verify_cset_none            // no cset references before partial
  );
}

void ShenandoahVerifier::verify_after_partial() {
  verify_at_safepoint(
          "After Partial GC",
          _verify_forwarded_none,      // cannot have forwarded objects
          _verify_marked_complete,     // bitmaps might be stale, but alloc-after-mark should be well
          _verify_matrix_conservative, // matrix is conservatively consistent
          _verify_cset_none            // no cset references left after partial
  );
}

void ShenandoahVerifier::verify_before_fullgc() {
  verify_at_safepoint(
          "Before Full GC",
          _verify_forwarded_allow,     // can have forwarded objects
          _verify_marked_disable,      // do not verify marked: lots ot time wasted checking dead allocations
          _verify_matrix_disable,      // matrix might be foobared
          _verify_cset_disable         // cset might be foobared
  );
}

void ShenandoahVerifier::verify_after_fullgc() {
  verify_at_safepoint(
          "After Full GC",
          _verify_forwarded_none,      // all objects are non-forwarded
          _verify_marked_complete,     // all objects are marked in complete bitmap
          _verify_matrix_conservative, // matrix is conservatively consistent
          _verify_cset_none            // no cset references
  );
}

void ShenandoahVerifier::verify_oop_fwdptr(oop obj, oop fwd) {
  guarantee(UseShenandoahGC, "must only be called when Shenandoah is used");

  ShenandoahHeap* heap = ShenandoahHeap::heap();

  guarantee(obj != NULL, "oop is not NULL");
  guarantee(heap->is_in(obj), err_msg("oop must point to a heap address: " PTR_FORMAT, p2i(obj)));

  guarantee(fwd != NULL, "forwardee is not NULL");
  if (!heap->is_in(fwd)) {
    ResourceMark rm;
    ShenandoahHeapRegion* r = heap->heap_region_containing(obj);
    stringStream obj_region;
    r->print_on(&obj_region);

    fatal(err_msg("forwardee must point to a heap address: " PTR_FORMAT " -> " PTR_FORMAT "\n %s",
          p2i(obj), p2i(fwd), obj_region.as_string()));
  }

  if (!oopDesc::unsafe_equals(fwd, obj) &&
      (heap->heap_region_containing(fwd) ==
       heap->heap_region_containing(obj))) {
    ResourceMark rm;
    ShenandoahHeapRegion* ro = heap->heap_region_containing(obj);
    stringStream obj_region;
    ro->print_on(&obj_region);

    ShenandoahHeapRegion* rf = heap->heap_region_containing(fwd);
    stringStream fwd_region;
    rf->print_on(&fwd_region);

    fatal(err_msg("forwardee should be self, or another region: " PTR_FORMAT " -> " PTR_FORMAT "\n %s %s",
          p2i(obj), p2i(fwd),
          obj_region.as_string(), fwd_region.as_string()));
  }

  if (!oopDesc::unsafe_equals(obj, fwd)) {
    oop fwd2 = oop(BrooksPointer::get_raw(fwd));
    if (!oopDesc::unsafe_equals(fwd, fwd2)) {
      // We should never be forwarded more than once.
      ResourceMark rm;

      ShenandoahHeapRegion* ro = heap->heap_region_containing(obj);
      stringStream obj_region;
      ro->print_on(&obj_region);

      ShenandoahHeapRegion* rf = heap->heap_region_containing(fwd);
      stringStream fwd_region;
      rf->print_on(&fwd_region);

      ShenandoahHeapRegion* rf2 = heap->heap_region_containing(fwd2);
      stringStream fwd2_region;
      rf2->print_on(&fwd2_region);

      fatal(err_msg("Multiple forwardings: " PTR_FORMAT " -> " PTR_FORMAT " -> " PTR_FORMAT "\n %s %s %s",
            p2i(obj), p2i(fwd), p2i(fwd2),
            obj_region.as_string(), fwd_region.as_string(), fwd2_region.as_string()));
    }
  }
}

void ShenandoahVerifier::verify_oop(oop obj) {
  oop fwd = oop(BrooksPointer::get_raw(obj));
  verify_oop_fwdptr(obj, fwd);
}
