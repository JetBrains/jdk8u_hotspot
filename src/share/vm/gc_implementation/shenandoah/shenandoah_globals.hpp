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

#ifndef SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHENANDOAH_GLOBALS_HPP
#define SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHENANDOAH_GLOBALS_HPP

#include "runtime/globals.hpp"

#define SHENANDOAH_FLAGS(develop, develop_pd, product, product_pd, diagnostic, experimental, notproduct, manageable, product_rw) \
                                                                            \
  product(bool, UseShenandoahGC, false,                                     \
          "Use the Shenandoah garbage collector")                           \
                                                                            \
  product(bool, ShenandoahOptimizeFinals, true,                             \
          "Optimize barriers on final and stable fields/arrays")            \
                                                                            \
  product(uintx, ShenandoahHeapRegionSize, 0,                               \
          "Size of the Shenandoah regions.")                                \
                                                                            \
  develop(bool, ShenandoahDumpHeapBeforeConcurrentMark, false,              \
          "Dump the ShenanodahHeap Before Each ConcurrentMark")             \
                                                                            \
  develop(bool, ShenandoahDumpHeapAfterConcurrentMark, false,               \
          "Dump the ShenanodahHeap After Each Concurrent Mark")             \
                                                                            \
  product(bool, ShenandoahTraceFullGC, false,                               \
          "Trace Shenandoah full GC")                                       \
                                                                            \
  product(bool, ShenandoahTracePhases, false,                               \
          "Trace Shenandoah GC phases")                                     \
                                                                            \
  develop(bool, ShenandoahTraceJNICritical, false,                          \
          "Trace Shenandoah stalls for JNI critical regions")               \
                                                                            \
  product(bool, ShenandoahTraceHumongous, false,                            \
          "Trace Shenandoah humongous objects")                             \
                                                                            \
  develop(bool, ShenandoahTraceAllocations, false,                          \
          "Trace Shenandoah Allocations")                                   \
                                                                            \
  develop(bool, ShenandoahTraceBrooksPointers, false,                       \
          "Trace Brooks Pointer updates")                                   \
                                                                            \
  develop(bool, ShenandoahTraceEvacuations, false,                          \
          "Trace Shenandoah Evacuations")                                   \
                                                                            \
  develop(bool, ShenandoahVerifyWritesToFromSpace, false,                   \
          "Use Memory Protection to signal illegal writes to from space")   \
                                                                            \
  develop(bool, ShenandoahVerifyReadsToFromSpace, false,                    \
          "Use Memory Protection to signal illegal reads to from space")    \
                                                                            \
  develop(bool, ShenandoahTraceConcurrentMarking, false,                    \
          "Trace Concurrent Marking")                                       \
                                                                            \
  develop(bool, ShenandoahTraceUpdates, false,                              \
          "Trace Shenandoah Updates")                                       \
                                                                            \
  develop(bool, ShenandoahTraceTLabs, false,                                \
          "Trace TLabs in Shenandoah Heap")                                 \
                                                                            \
  product(bool, ShenandoahProcessReferences, true,                          \
          "Enable processing of (soft/weak/..) references in Shenandoah")   \
                                                                            \
  develop(bool, ShenandoahTraceWeakReferences, false,                       \
          "Trace Weak Reference Processing in Shenandoah Heap")             \
                                                                            \
  product(bool, ShenandoahGCVerbose, false,                                 \
          "Verbose information about the Shenandoah garbage collector")     \
                                                                            \
  product(bool, ShenandoahLogConfig, false,                                 \
          "Log information about Shenandoah's configuration settings")      \
                                                                            \
  develop(bool, ShenandoahVerify, false,                                    \
          "Verify the  Shenandoah garbage collector")                       \
                                                                            \
  product(bool, ShenandoahWriteBarrier, true,                               \
          "Turn on/off write barriers in Shenandoah")                       \
                                                                            \
  product(bool, ShenandoahReadBarrier, true,                                \
          "Turn on/off read barriers in Shenandoah")                        \
                                                                            \
  product(ccstr, ShenandoahGCHeuristics, "dynamic",                         \
          "The heuristics to use in Shenandoah GC; possible values: "       \
          "statusquo, aggressive, halfway, lazy, dynamic")                  \
                                                                            \
  product(uintx, ShenandoahGarbageThreshold, 60,                            \
          "Sets the percentage of garbage a region need to contain before " \
          "it can be marked for collection. Applies to "                    \
          "Shenandoah GC dynamic Heuristic mode only (ignored otherwise)")  \
                                                                            \
  product(uintx, ShenandoahFreeThreshold, 25,                               \
          "Set the percentage of heap free in relation to the total "       \
          "capacity before a region can enter the concurrent marking "      \
          "phase. Applies to Shenandoah GC dynamic Heuristic mode only "    \
          "(ignored otherwise)")                                            \
                                                                            \
  product(uintx, ShenandoahInitialFreeThreshold, 50,                        \
          "Set the percentage of heap free in relation to the total "       \
          "capacity before a region can enter the concurrent marking "      \
          "phase. Applies to Shenandoah GC dynamic Heuristic mode only "    \
          "(ignored otherwise)")                                            \
                                                                            \
  product(uintx, ShenandoahAllocationThreshold, 0,                          \
          "Set the number of bytes allocated since last GC cycle before"    \
          "a region can enter the concurrent marking "                      \
          "phase. Applies to Shenandoah GC dynamic Heuristic mode only "    \
          "(ignored otherwise)")                                            \
                                                                            \
  product(uintx, ShenandoahTargetHeapOccupancy, 80,                         \
          "Sets the target maximum percentage occupance of the heap we"     \
          "would like to maintain."                                         \
          "Shenandoah GC newadaptive Heuristic mode only.")                 \
                                                                            \
  product(uintx, ShenandoahAllocReserveRegions, 10,                         \
          "How many regions should be kept as allocation reserve, before "  \
          "Shenandoah attempts to grow the heap")                      \
                                                                            \
  product(bool, ShenandoahWarnings, false,                                  \
          "Print Shenandoah related warnings. Useful for Shenandoah devs.") \
                                                                            \
  product(bool, ShenandoahPrintCollectionSet, false,                        \
          "Print the collection set before each GC phase")                  \
                                                                            \
  develop(bool, VerifyStrictOopOperations, false,                           \
          "Verify that == and != are not used on oops. Only in fastdebug")  \
                                                                            \
  experimental(bool, ShenandoahTraceStringSymbolTableScrubbing, false,      \
          "Trace information string and symbol table scrubbing.")

SHENANDOAH_FLAGS(DECLARE_DEVELOPER_FLAG, DECLARE_PD_DEVELOPER_FLAG, DECLARE_PRODUCT_FLAG, DECLARE_PD_PRODUCT_FLAG, DECLARE_DIAGNOSTIC_FLAG, DECLARE_EXPERIMENTAL_FLAG, DECLARE_NOTPRODUCT_FLAG, DECLARE_MANAGEABLE_FLAG, DECLARE_PRODUCT_RW_FLAG)

#endif // SHARE_VM_GC_IMPLEMENTATION_SHENANDOAH_SHENANDOAH_GLOBALS_HPP
