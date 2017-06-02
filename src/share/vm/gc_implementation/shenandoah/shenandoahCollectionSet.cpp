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

#include "precompiled.hpp"
#include "gc_implementation/shenandoah/shenandoahCollectionSet.hpp"
#include "gc_implementation/shenandoah/shenandoahHeap.hpp"
#include "gc_implementation/shenandoah/shenandoahHeapRegion.hpp"

ShenandoahCollectionSet::ShenandoahCollectionSet(size_t max_regions) :
  ShenandoahHeapRegionSet(max_regions),
  _garbage(0), _live_data(0) {
}

ShenandoahCollectionSet::~ShenandoahCollectionSet() {
}

void ShenandoahCollectionSet::add_region(ShenandoahHeapRegion* r) {
  assert(SafepointSynchronize::is_at_safepoint(), "Must be at a safepoint");
  ShenandoahHeapRegionSet::add_region(r);
  r->set_in_collection_set(true);

  _garbage += r->garbage();
  _live_data += r->get_live_data_bytes();
}

void ShenandoahCollectionSet::clear() {
  assert(SafepointSynchronize::is_at_safepoint(), "Must be at a safepoint");

  ShenandoahHeapRegionSet::clear();
  ShenandoahHeap::heap()->clear_cset_fast_test();
  _garbage = 0;
  _live_data = 0;
}
