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
  ShenandoahVerifierStack* _stack;
  ShenandoahHeap* _heap;
  CMBitMap* _map;
  const char* _phase;
  ShenandoahVerifier::VerifyForwarded _verify_forwarded;
  ShenandoahVerifier::VerifyMarked _verify_marked;
  ShenandoahVerifier::VerifyMatrix _verify_matrix;
  ShenandoahVerifier::VerifyCollectionSet _verify_cset;
  void* _interior_loc;
  oop _loc;
public:
  VerifyReachableHeapClosure(ShenandoahVerifierStack* stack, CMBitMap* map,
                             const char* phase,
                             ShenandoahVerifier::VerifyForwarded forwarded,
                             ShenandoahVerifier::VerifyMarked marked,
                             ShenandoahVerifier::VerifyMatrix matrix,
                             ShenandoahVerifier::VerifyCollectionSet cset) :
          _stack(stack), _heap(ShenandoahHeap::heap()), _map(map), _loc(NULL), _interior_loc(NULL), _phase(phase),
          _verify_forwarded(forwarded), _verify_marked(marked), _verify_matrix(matrix), _verify_cset(cset) {};

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

  void print_failure(oop obj, const char* label) {
    bool loc_in_heap = (_loc != NULL && _heap->is_in(_loc));

    MessageBuffer msg("Shenandoah verification failed; %s: %s\n\n", _phase, label);

    msg.append("Referenced from:\n");
    if (_interior_loc != NULL) {
      msg.append("  interior location: " PTR_FORMAT "\n", p2i(_interior_loc));
    } else {
      msg.append("  no location recorded, probably a plain heap scan\n");
    }

    if (loc_in_heap) {
      print_obj(msg, _loc);
    } else {
      msg.append("  outside of Java heap\n");
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
      if (!_map->isMarked(addr)) {
        _map->mark(addr);
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

    switch (_verify_marked) {
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

    switch (_verify_forwarded) {
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

    switch (_verify_cset) {
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

void ShenandoahVerifier::verify_at_safepoint(const char *label,
                                             VerifyForwarded forwarded, VerifyMarked marked,
                                             VerifyMatrix matrix, VerifyCollectionSet cset) {
  guarantee(SafepointSynchronize::is_at_safepoint(), "only when nothing else happens");
  guarantee(ShenandoahVerify, "only when enabled, and bitmap is initialized in ShenandoahHeap::initialize");

  log_info(gc)("Starting verification: %s", label);

  // Internal heap region checks
  {
    VerifyHeapRegionClosure cl;
    _heap->heap_region_iterate(&cl, true, true);
  }

  // Heap size checks
  {
    CalculateRegionStatsClosure cl;
    _heap->heap_region_iterate(&cl);
    size_t heap_used = _heap->used();
    guarantee(cl.used() == heap_used,
              err_msg("heap used size must be consistent: heap-used = " SIZE_FORMAT ", regions-used = " SIZE_FORMAT,
                      heap_used, cl.used()));
  }

  OrderAccess::fence();
  _heap->ensure_parsability(false);

  // Allocate temporary bitmap for storing marking wavefront:
  MemRegion mr = MemRegion(_verification_bit_map->startWord(), _verification_bit_map->endWord());
  _verification_bit_map->clear_range_large(mr);

  // Initialize a single queue
  ShenandoahVerifierStack stack;

  // Step 1. Scan root set to get initial reachable set.
  ShenandoahRootProcessor rp(_heap, 1,
                             ShenandoahCollectorPolicy::_num_phases); // no need for stats

  {
    VerifyReachableHeapClosure cl(&stack, _verification_bit_map, MessageBuffer("%s, Roots", label),
                                  forwarded, marked, matrix, cset);
    CLDToOopClosure cld_cl(&cl);
    CodeBlobToOopClosure code_cl(&cl, ! CodeBlobToOopClosure::FixRelocations);
    rp.process_all_roots(&cl, &cl, &cld_cl, &code_cl, 0);
  }

  // Step 2. Finish walking the reachable heap. This verifies what application can see, since it
  // only cares about reachable objects.
  {
    VerifyReachableHeapClosure cl(&stack, _verification_bit_map, MessageBuffer("%s, Reachable", label),
                                  forwarded, marked, matrix, cset);
    while (!stack.is_empty()) {
      VerifierTask task = stack.pop();
      cl.verify_oops_from(task.obj());
    }
  }

  // Step 3. Walk marked objects. Marked objects might be unreachable. This verifies what collector,
  // not the application, can see during the region scans. There is no reason to process the objects
  // that were already verified, e.g. those marked in verification bitmap. There is interaction with TAMS:
  // before TAMS, we verify the bitmaps, if available; after TAMS, we walk until the top(). It mimics
  // what marked_object_iterate is doing, without calling into that optimized (and possibly incorrect)
  // version

  if (marked == _verify_marked_complete) {
    VerifyReachableHeapClosure cl(&stack, _verification_bit_map, MessageBuffer("%s, Unreachable", label),
                                  forwarded, marked, matrix, cset);

    CMBitMap* mark_bit_map = _heap->complete_mark_bit_map();

    ShenandoahHeapRegionSet* set = _heap->regions();
    for (size_t idx = 0; idx < _heap->num_regions(); idx++) {
      ShenandoahHeapRegion* r = set->get(idx);
      if (r->is_humongous()) continue;

      HeapWord* tams = _heap->complete_top_at_mark_start(r->bottom());

      // Bitmaps, before TAMS
      if (tams > r->bottom()) {
        HeapWord* start = r->bottom() + BrooksPointer::word_size();
        HeapWord* addr = mark_bit_map->getNextMarkedWordAddress(start, tams);

        while (addr < tams) {
          if (!_verification_bit_map->isMarked(addr)) {
            _verification_bit_map->mark(addr);
            verify_and_follow(stack, cl, addr);
          }
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
          if (!_verification_bit_map->isMarked(addr)) {
            _verification_bit_map->mark(addr);
            verify_and_follow(stack, cl, addr);
          }
          addr += oop(addr)->size() + BrooksPointer::word_size();
        }
      }
    }
  } else {
    guarantee(marked == _verify_marked_next || marked == _verify_marked_disable, "Should be");
  }

  log_info(gc)("Verification finished: %s", label);
}

void ShenandoahVerifier::verify_and_follow(ShenandoahVerifierStack &stack,
                                           VerifyReachableHeapClosure &cl,
                                           HeapWord *addr) {
  // Verify the object itself:
  oop obj = oop(addr);
  cl.verify_oop_standalone(obj);

  // Verify everything reachable from that object too:
  stack.push(obj);
  while (!stack.is_empty()) {
    VerifierTask task = stack.pop();
    cl.verify_oops_from(task.obj());
  }
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
