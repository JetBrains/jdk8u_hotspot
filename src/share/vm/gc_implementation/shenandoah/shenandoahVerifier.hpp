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

#ifndef SHARE_VM_GC_SHENANDOAH_SHENANDOAHVERIFY_HPP
#define SHARE_VM_GC_SHENANDOAH_SHENANDOAHVERIFY_HPP

#include "memory/allocation.hpp"
#include "gc_implementation/g1/concurrentMark.hpp"

class Thread;
class ShenandoahHeapRegionSet;
class ShenandoahHeap;

class ShenandoahVerifier : public CHeapObj<mtGC> {
private:
  ShenandoahHeap* _heap;
  CMBitMap* _verification_bit_map;
public:
  typedef enum {
    // Disable matrix verification completely
    _verify_matrix_disable,

    // Conservative matrix verification: all connected objects should have matrix
    // connections. The verification is conservative, because it allows matrix
    // connection that do not have actual heap connections.
    _verify_matrix_conservative,

    // Precise matrix verification: all connected objects should have matrix connections,
    // *and* every matrix connection should have at least a pair a connected objects.
    // TODO: implement this, if needed
    _verify_matrix_precise,
  } VerifyMatrix;

  typedef enum {
    // Disable marked objects verification.
    _verify_marked_disable,

    // Objects should be marked in "next" bitmap.
    _verify_marked_next,

    // Objects should be marked in "complete" bitmap.
    _verify_marked_complete,
  } VerifyMarked;

  typedef enum {
    // Disable forwarded objects verification.
    _verify_forwarded_disable,

    // Objects should not have forwardees.
    _verify_forwarded_none,

    // Objects may have forwardees.
    _verify_forwarded_allow,
  } VerifyForwarded;

  typedef enum {
    // Disable collection set verification.
    _verify_cset_disable,

    // Should have no references to cset.
    _verify_cset_none,

    // May have references to cset, all should be forwarded.
    // Note: Allowing non-forwarded references to cset is equivalent
    // to _verify_cset_disable.
    _verify_cset_forwarded,
  } VerifyCollectionSet;

private:
  void verify_reachable_at_safepoint(const char *label,
                                     VerifyForwarded forwarded,
                                     VerifyMarked marked,
                                     VerifyMatrix matrix,
                                     VerifyCollectionSet cset);

public:
  ShenandoahVerifier(ShenandoahHeap* heap, CMBitMap* verification_bitmap) :
          _heap(heap), _verification_bit_map(verification_bitmap) {};

  void verify_before_concmark();
  void verify_after_concmark();
  void verify_before_evacuation();
  void verify_after_evacuation();
  void verify_before_updaterefs();
  void verify_after_updaterefs();
  void verify_before_fullgc();
  void verify_after_fullgc();
  void verify_before_partial();
  void verify_after_partial();
  void verify_generic(VerifyOption option);
};

#endif // SHARE_VM_GC_SHENANDOAH_SHENANDOAHVERIFY_HPP
