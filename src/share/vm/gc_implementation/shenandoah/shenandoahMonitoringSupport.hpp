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

#ifndef SHARE_VM_GC_SHENANDOAH_SHENANDOAHMONITORINGSUPPORT_HPP
#define SHARE_VM_GC_SHENANDOAH_SHENANDOAHMONITORINGSUPPORT_HPP

#include "memory/allocation.hpp"

class GenerationCounters;
class HSpaceCounters;
class ShenandoahHeap;
class CollectorCounters;

class ShenandoahMonitoringSupport : public CHeapObj<mtGC> {
private:
  CollectorCounters*   _concurrent_collection_counters;
  CollectorCounters*   _full_collection_counters;

  GenerationCounters* _young_gen_counters;
  GenerationCounters* _old_gen_counters;

  HSpaceCounters* _eden_space_counters;
  HSpaceCounters* _s0_space_counters;
  HSpaceCounters* _s1_space_counters;
  HSpaceCounters* _old_space_counters;

public:
 ShenandoahMonitoringSupport(ShenandoahHeap* heap);
 CollectorCounters* full_collection_counters();
 CollectorCounters* concurrent_collection_counters();
 void update_counters();
};

#endif // SHARE_VM_GC_SHENANDOAH_SHENANDOAHMONITORINGSUPPORT_HPP
