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

#include "gc_implementation/shared/hSpaceCounters.hpp"
#include "gc_implementation/shared/collectorCounters.hpp"
#include "gc_implementation/shared/generationCounters.hpp"
#include "gc_implementation/shenandoah/shenandoahMonitoringSupport.hpp"
#include "gc_implementation/shenandoah/shenandoahHeap.hpp"

class DummyGenerationCounters : public GenerationCounters {
public:
  DummyGenerationCounters():
    GenerationCounters("dummy", 0, 3, MinObjAlignmentInBytes * 3, MinObjAlignmentInBytes * 3, (size_t) 0) {
    // Nothing to do.
  }
};


ShenandoahMonitoringSupport::ShenandoahMonitoringSupport(ShenandoahHeap* heap) :
_concurrent_collection_counters(NULL),
_full_collection_counters(NULL)
{
  _concurrent_collection_counters = new CollectorCounters("Shenandoah concurrent collections", 0);
  _full_collection_counters = new CollectorCounters("Shenandoah full collections", 1);
  // We report young gen as unused.
  _young_gen_counters = new DummyGenerationCounters();
  _eden_space_counters = new HSpaceCounters("eden", 0, MinObjAlignmentInBytes, MinObjAlignmentInBytes, _young_gen_counters);
  _s0_space_counters = new HSpaceCounters("s0", 1, MinObjAlignmentInBytes, MinObjAlignmentInBytes, _young_gen_counters);
  _s1_space_counters = new HSpaceCounters("s1", 2, MinObjAlignmentInBytes, MinObjAlignmentInBytes, _young_gen_counters);

  _old_gen_counters = new GenerationCounters("old", 1, 1, heap->storage());
  _old_space_counters = new HSpaceCounters("old", 0, heap->max_capacity(), heap->min_capacity(), _old_gen_counters);
  if (UsePerfData) {
    _eden_space_counters->update_used(0);
    _s0_space_counters->update_used(0);
    _s1_space_counters->update_used(0);
  }
}

CollectorCounters* ShenandoahMonitoringSupport::full_collection_counters() {
  return _full_collection_counters;
}

CollectorCounters* ShenandoahMonitoringSupport::concurrent_collection_counters() {
  return _concurrent_collection_counters;
}

void ShenandoahMonitoringSupport::update_counters() {

  MemoryService::track_memory_usage();

  if (UsePerfData) {
    ShenandoahHeap* heap = ShenandoahHeap::heap();
    size_t used = heap->used();
    size_t capacity = heap->capacity();
    _old_gen_counters->update_all();
    _old_space_counters->update_all(capacity, used);
  }
}
