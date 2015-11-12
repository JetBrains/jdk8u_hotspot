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
    _current_size->set_value(0);
    // Nothing to do.
  }
};


ShenandoahMonitoringSupport::ShenandoahMonitoringSupport(ShenandoahHeap* heap) :
_concurrent_collection_counters(NULL),
_stw_collection_counters(NULL),
_full_collection_counters(NULL)
{
  _concurrent_collection_counters = new CollectorCounters("Shenandoah concurrent phases", 0);
  _stw_collection_counters = new CollectorCounters("Shenandoah pauses", 1);
  _full_collection_counters = new CollectorCounters("Shenandoah full GC pauses", 2);

  // We report young gen as unused.
  _heap_counters = new GenerationCounters("heap", 0, 1, heap->storage());
  _space_counters = new HSpaceCounters("heap", 0, heap->max_capacity(), heap->min_capacity(), _heap_counters);
}

CollectorCounters* ShenandoahMonitoringSupport::stw_collection_counters() {
  return _stw_collection_counters;
}

CollectorCounters* ShenandoahMonitoringSupport::concurrent_collection_counters() {
  return _concurrent_collection_counters;
}

CollectorCounters* ShenandoahMonitoringSupport::full_collection_counters() {
  return _full_collection_counters;
}

void ShenandoahMonitoringSupport::update_counters() {

  MemoryService::track_memory_usage();

  if (UsePerfData) {
    ShenandoahHeap* heap = ShenandoahHeap::heap();
    size_t used = heap->used();
    size_t capacity = heap->capacity();
    _heap_counters->update_all();
    _space_counters->update_all(capacity, used);
  }
}
