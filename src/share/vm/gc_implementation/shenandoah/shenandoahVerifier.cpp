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
  SCMObjToScanQueue* _queue;
  ShenandoahHeap* _heap;
  CMBitMap* _map;
  const char* _phase;
  ShenandoahVerifier::VerifyForwarded _verify_forwarded;
  ShenandoahVerifier::VerifyMarked _verify_marked;
  ShenandoahVerifier::VerifyMatrix _verify_matrix;
  oop _loc;
public:
  VerifyReachableHeapClosure(SCMObjToScanQueue* queue, CMBitMap* map,
                             const char* phase,
                             ShenandoahVerifier::VerifyForwarded forwarded,
                             ShenandoahVerifier::VerifyMarked marked,
                             ShenandoahVerifier::VerifyMatrix matrix) :
          _queue(queue), _heap(ShenandoahHeap::heap()), _map(map), _loc(NULL), _phase(phase),
          _verify_forwarded(forwarded), _verify_marked(marked), _verify_matrix(matrix) {};

  typedef FormatBuffer<8192> large_buf;

  void print_obj(large_buf& msg, oop obj) {
    ShenandoahHeapRegion *r = _heap->heap_region_containing(obj);
    stringStream ss;
    r->print_on(&ss);

    msg.append("  " PTR_FORMAT " - klass " PTR_FORMAT " %s\n", p2i(obj), p2i(obj->klass()), obj->klass()->external_name());
    msg.append("    %3s allocated after mark\n", _heap->allocated_after_complete_mark_start((HeapWord *) obj) ? "" : "not");
    msg.append("    %3s marked complete\n",      _heap->is_marked_complete(obj) ? "" : "not");
    msg.append("    %3s marked next\n",          _heap->is_marked_next(obj) ? "" : "not");
    msg.append("    %3s in collection set\n",    _heap->in_collection_set(obj) ? "" : "not");
    msg.append("  region: %s", ss.as_string());
  }

  template <class T>
  void verify(T* p, oop obj, bool test, const char* label) {
    if (!test) {
      bool loc_in_heap = (_loc != NULL && _heap->is_in(_loc));

      large_buf msg("%s: %s \n\n", _phase, label);

      msg.append("Referenced from: \n");
      msg.append("  interior location: " PTR_FORMAT "\n", p2i(p));

      if (loc_in_heap) {
        print_obj(msg, _loc);
      } else {
        msg.append("  outside of Java heap\n");
      }
      msg.append("\n");

      msg.append("Object: \n");
      print_obj(msg, obj);
      msg.append("\n");

      oop fwd = BrooksPointer::forwardee(obj);
      if (!oopDesc::unsafe_equals(obj, fwd)) {
        msg.append("Forwardee: \n");
        print_obj(msg, fwd);
        msg.append("\n");
      }

      oop fwd2 = BrooksPointer::forwardee(fwd);
      if (!oopDesc::unsafe_equals(fwd, fwd2)) {
        msg.append("Second forwardee: \n");
        print_obj(msg, fwd2);
        msg.append("\n");
      }

      bool verification_passed = false;
      guarantee(verification_passed, msg.buffer());
    }
  }

  template <class T>
  void do_oop_work(T* p) {
    T o = oopDesc::load_heap_oop(p);
    if (!oopDesc::is_null(o)) {
      oop obj = oopDesc::decode_heap_oop_not_null(o);

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

      oop fwd = BrooksPointer::forwardee(obj);
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

        oop fwd2 = BrooksPointer::forwardee(fwd);
        verify(p, obj, oopDesc::unsafe_equals(fwd, fwd2),
               "Double forwarding");
      }

      switch (_verify_marked) {
        case ShenandoahVerifier::_verify_marked_disable:
          // skip
          break;
        case ShenandoahVerifier::_verify_marked_next:
          verify(p, obj, _heap->is_marked_next(obj),
                 "Must be marked in next bitmap");
          break;
        case ShenandoahVerifier::_verify_marked_complete:
          verify(p, obj, _heap->is_marked_complete(obj),
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
          verify(p, obj, oopDesc::unsafe_equals(obj, fwd),
                 "Should not be forwarded");
          verify(p, obj, !_heap->in_collection_set(obj),
                 "Cannot have references to collection set");
          break;
        }
        case ShenandoahVerifier::_verify_forwarded_allow: {
          if (!oopDesc::unsafe_equals(obj, fwd)) {
            verify(p, obj, _heap->heap_region_containing(obj) != _heap->heap_region_containing(fwd),
                   "Forwardee should be in another region");
          } else {
            verify(p, obj, !_heap->in_collection_set(obj),
                   "Object in collection set, should have forwardee");
          }
          break;
        }
        default:
          assert(false, "Unhandled forwarding verification");
      }

      // Single threaded verification can use faster non-atomic version:
      HeapWord* addr = (HeapWord*) obj;
      if (!_map->isMarked(addr)) {
        _map->mark(addr);
        _queue->push(SCMTask(obj));
      }
    }
  }

  void do_oop(oop* p) { do_oop_work(p); }
  void do_oop(narrowOop* p) { do_oop_work(p); }
  void set_loc(oop o) { _loc = o; }
};

void ShenandoahVerifier::verify_reachable_at_safepoint(const char *label,
                                                       VerifyForwarded forwarded, VerifyMarked marked,
                                                       VerifyMatrix matrix) {
  guarantee(SafepointSynchronize::is_at_safepoint(), "only when nothing else happens");
  guarantee(ShenandoahVerify, "only when enabled, and bitmap is initialized in ShenandoahHeap::initialize");

  log_info(gc)("Starting verification: %s", label);

  // Basic checks
  {
    size_t calculated_used = _heap->calculateUsed();
    size_t heap_used = _heap->used();
    guarantee(calculated_used == heap_used,
              err_msg("heap used size must be consistent heap-used: " SIZE_FORMAT " regions-used: " SIZE_FORMAT,
              heap_used, calculated_used));
  }

  OrderAccess::fence();
  _heap->ensure_parsability(false);

  // Allocate temporary bitmap for storing marking wavefront:
  MemRegion mr = MemRegion(_verification_bit_map->startWord(), _verification_bit_map->endWord());
  _verification_bit_map->clear_range_large(mr);

  // Initialize a single queue
  SCMObjToScanQueue* q = new SCMObjToScanQueue();
  q->initialize();

  // Scan root set
  ShenandoahRootProcessor rp(_heap, 1,
                             ShenandoahCollectorPolicy::_num_phases); // no need for stats

  {
    VerifyReachableHeapClosure cl(q, _verification_bit_map, label, forwarded, marked, _verify_matrix_disable);
    CLDToOopClosure cld_cl(&cl);
    CodeBlobToOopClosure code_cl(&cl, ! CodeBlobToOopClosure::FixRelocations);
    rp.process_all_roots(&cl, &cl, &cld_cl, &code_cl, 0);
  }

  // Finish the scan
  {
    VerifyReachableHeapClosure cl(q, _verification_bit_map, label, forwarded, marked, matrix);
    SCMTask task;
    while ((q->pop_buffer(task) ||
            q->pop_local(task) ||
            q->pop_overflow(task))) {
      oop obj = task.obj();
      assert(!oopDesc::is_null(obj), "must not be null");
      cl.set_loc(obj);
      obj->oop_iterate(&cl);
    }
  }

  log_info(gc)("Verification finished: %s", label);
}

void ShenandoahVerifier::verify_generic(VerifyOption vo) {
  // _vo == UsePrevMarking -> use "prev" marking information,
  // _vo == UseNextMarking -> use "next" marking information,
  // _vo == UseMarkWord    -> use mark word from object header.

  VerifyMarked mark_verify;
  switch (vo) {
    case VerifyOption_G1UsePrevMarking:
      mark_verify = _verify_marked_complete;
      break;
    case VerifyOption_G1UseNextMarking:
      mark_verify = _verify_marked_next;
      break;
    default:
      mark_verify = _verify_marked_disable;
      break;
  }

  verify_reachable_at_safepoint(
          "Generic Verification",
          _verify_forwarded_allow,     // conservatively allow forwarded
          mark_verify,                 // (selector above)
          _verify_matrix_disable       // matrix can be inconsistent here
  );
}

void ShenandoahVerifier::verify_before_concmark() {
  if (_heap->need_update_refs()) {
    verify_reachable_at_safepoint(
            "Before Mark",
            _verify_forwarded_allow,     // may have forwarded references
            _verify_marked_disable,      // bitmaps are foobared
            _verify_matrix_disable       // matrix is foobared
    );
  } else {
    verify_reachable_at_safepoint(
            "Before Mark",
            _verify_forwarded_none,      // UR should have fixed up
            _verify_marked_disable,      // bitmaps are foobared
            _verify_matrix_conservative  // UR should have fixed matrix
    );
  }
}

void ShenandoahVerifier::verify_after_concmark() {
  // No need, will unconditionally do evacuation
}

void ShenandoahVerifier::verify_before_evacuation() {
  verify_reachable_at_safepoint(
          "Before Evacuation",
          _verify_forwarded_none,      // no forwarded references
          _verify_marked_complete,     // all objects are marked
          _verify_matrix_disable       // matrix might be foobared
  );
}

void ShenandoahVerifier::verify_after_evacuation() {
  verify_reachable_at_safepoint(
          "After Evacuation",
          _verify_forwarded_allow,     // objects are still forwarded
          _verify_marked_disable,      // cannot trust bitmaps
          _verify_matrix_disable       // matrix is inconsistent here
  );
}

void ShenandoahVerifier::verify_before_updaterefs() {
  verify_reachable_at_safepoint(
          "Before Updating References",
          _verify_forwarded_allow,     // forwarded references allowed
          _verify_marked_complete,     // all objects are marked
          _verify_matrix_disable       // matrix is inconsistent here
  );
}

void ShenandoahVerifier::verify_after_updaterefs() {
  verify_reachable_at_safepoint(
          "After Updating References",
          _verify_forwarded_none,      // no forwarded references
          _verify_marked_complete,     // all objects are marked
          _verify_matrix_conservative  // matrix is conservatively consistent
  );
}

void ShenandoahVerifier::verify_before_partial() {
  verify_reachable_at_safepoint(
          "Before Partial GC",
          _verify_forwarded_none,      // cannot have forwarded objects
          _verify_marked_disable,      // cannot trust bitmaps
          _verify_matrix_conservative  // matrix is conservatively consistent
  );
}

void ShenandoahVerifier::verify_after_partial() {
  verify_reachable_at_safepoint(
          "After Partial GC",
          _verify_forwarded_none,      // cannot have forwarded objects
          _verify_marked_disable,      // cannot trust bitmaps
          _verify_matrix_conservative  // matrix is conservatively consistent
  );
}

void ShenandoahVerifier::verify_before_fullgc() {
  verify_reachable_at_safepoint(
          "Before Full GC",
          _verify_forwarded_allow,     // can have forwarded objects
          _verify_marked_disable,      // bitmaps might be foobared
          _verify_matrix_disable       // matrix might be foobared
  );
}

void ShenandoahVerifier::verify_after_fullgc() {
  verify_reachable_at_safepoint(
          "After Full GC",
          _verify_forwarded_none,      // all objects are non-forwarded
          _verify_marked_complete,     // all objects are marked in complete bitmap
          _verify_matrix_conservative  // matrix is conservatively consistent
  );
}

