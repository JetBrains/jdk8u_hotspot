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

#ifndef SHARE_VM_GC_SHENANDOAH_SHENANDOAHHEAPREGIONCOUNTERS_HPP
#define SHARE_VM_GC_SHENANDOAH_SHENANDOAHHEAPREGIONCOUNTERS_HPP

#include "memory/allocation.hpp"

/**
 * This provides the following in JVMStat:
 *
 * constants:
 * - sun.gc.shenandoah.regions.timestamp    the timestamp for this sample
 * - sun.gc.shenandoah.regions.max_regions  maximum number of regions
 * - sun.gc.shenandoah.regions.region_size  size per region, in kilobytes
 *
 * variables:
 * - sun.gc.shenandoah.regions.status       current GC status:
 *     - bit 0 set when marking in progress
 *     - bit 1 set when evacuation in progress
 *     - bit 2 set when update refs in progress
 *
 * one variable counter per region, with $max_regions (see above) counters:
 * - sun.gc.shenandoah.regions.region.$i.data
 * where $ is the region number from 0 <= i < $max_regions
 *
 * .data is in the following format:
 * - bits 0-28   used memory in kilobytes
 * - bits 29-58  live memory in kilobytes
 * - bits 58-63  status
 *      - bit 58 set when region is not used yet
 *      - bit 59 set when region in collection set
 *      - bit 60 set when region is humongous
 *      - bit 61 set when region is recently allocated
 *      - bit 62 set when region is pinned
 */
class ShenandoahHeapRegionCounters : public CHeapObj<mtGC>  {
private:
  static const jlong USED_MASK   = 0x1fffffff; // bits 0-28
  static const jlong USED_SHIFT  = 0;

  static const jlong LIVE_MASK   = 0x1fffffff; // bits 29-58
  static const jlong LIVE_SHIFT  = 29;

  static const jlong FLAGS_MASK  = 0x3f;       // bits 58-63
  static const jlong FLAGS_SHIFT = 58;         // bits 58-63

  char* _name_space;
  PerfLongVariable** _regions_data;
  PerfLongVariable* _timestamp;
  PerfLongVariable* _status;
  volatile jlong _last_sample_millis;

public:
  ShenandoahHeapRegionCounters();
  ~ShenandoahHeapRegionCounters();
  void update();
};

#endif // SHARE_VM_GC_SHENANDOAH_SHENANDOAHHEAPREGIONCOUNTERS_HPP
