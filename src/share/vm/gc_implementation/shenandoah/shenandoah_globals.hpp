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

#ifndef SHARE_VM_GC_SHENANDOAH_SHENANDOAH_GLOBALS_HPP
#define SHARE_VM_GC_SHENANDOAH_SHENANDOAH_GLOBALS_HPP

#include "runtime/globals.hpp"

#define SHENANDOAH_FLAGS(develop, \
                         develop_pd, \
                         product, \
                         product_pd, \
                         diagnostic, \
                         experimental, \
                         notproduct, \
                         manageable, \
                         product_rw) \
                                                                            \
  product(bool, UseShenandoahGC, false,                                     \
          "Use the Shenandoah garbage collector")                           \
                                                                            \
  product(bool, ShenandoahOptimizeFinals, true,                             \
          "Optimize barriers on final and stable fields/arrays. "           \
          "Turn it off for maximum compatibility with reflection or JNI "   \
          "code that manipulates final fields."                             \
          "Defaults to true. ")                                        \
                                                                            \
  product(uintx, ShenandoahHeapRegionSize, 0,                               \
          "Size of the Shenandoah regions. "                                \
          "Determined automatically by default.")                           \
                                                                            \
  experimental(uintx, ShenandoahMinRegionSize, 256 * K,                     \
          "Minimum heap region size. ")                                     \
                                                                            \
  experimental(uintx, ShenandoahMaxRegionSize, 32 * M,                      \
          "Maximum heap region size. ")                                     \
                                                                            \
  experimental(size_t, ShenandoahTargetNumRegions, 2048,                    \
          "Target number of regions. We try to get around that many "       \
          "regions, based on ShenandoahMinRegionSize and "                  \
          "ShenandoahMaxRegionSizeSize. ")                                  \
                                                                            \
  product(bool, UseShenandoahMatrix, false,                                 \
          "Keep a connection matrix and use this to drive collection sets") \
                                                                            \
  product(ccstr, ShenandoahGCHeuristics, "adaptive",                        \
          "The heuristics to use in Shenandoah GC. Possible values: "       \
          "adaptive (adapt to maintain the given amount of free memory), "  \
          "dynamic (start concurrent GC based on amount of free memory, "   \
          "allocation threshold, etc), "                                    \
          "passive (do not start concurrent GC, wait for Full GC) "         \
          "aggressive (run concurrent GC continuously, evacuate everything), " \
          "Defaults to adaptive")                                            \
                                                                            \
  experimental(ccstr, ShenandoahUpdateRefsEarly, "adaptive",                \
          "Run a separate concurrent reference updating phase after"        \
          "concurrent evacuation. Possible values: 'on', 'off', 'adaptive'")\
                                                                            \
  product(uintx, ShenandoahRefProcFrequency, 5,                             \
          "How often should (weak, soft, etc) references be processed. "    \
          "References get processed at every Nth GC cycle. "                \
          "Set to 0 to disable reference processing. "                      \
          "Defaults to process references every 5 cycles.")                 \
                                                                            \
  product(uintx, ShenandoahUnloadClassesFrequency, 5,                       \
          "How often should classes get unloaded. "                         \
          "Class unloading is performed at every Nth GC cycle. "            \
          "Set to 0 to disable concurrent class unloading. "                \
          "Defaults to unload classes every 5 cycles.")                     \
                                                                            \
  product(bool, ShenandoahLogTrace, false,                                  \
          "Turns on logging in Shenandoah at trace level. ")                \
                                                                            \
  product(bool, ShenandoahLogDebug, false,                                  \
          "Turns on logging in Shenandoah at debug level. ")                \
                                                                            \
  product(bool, ShenandoahLogInfo, false,                                   \
          "Turns on logging in Shenandoah at info level. ")                 \
                                                                            \
  product(bool, ShenandoahLogWarning, false,                                \
          "Turns on logging in Shenandoah at warning level. ")              \
                                                                            \
  product_rw(uintx, ShenandoahFullGCThreshold, 3,                           \
          "How many cycles in a row to do degenerated marking on "          \
          "cancelled GC before triggering a full-gc"                        \
          "Defaults to 3")                                                  \
                                                                            \
  product_rw(uintx, ShenandoahGarbageThreshold, 60,                         \
          "Sets the percentage of garbage a region need to contain before " \
          "it can be marked for collection. Applies to "                    \
          "Shenandoah GC dynamic Heuristic mode only (ignored otherwise). " \
          "Defaults to 60%.")                                               \
                                                                            \
  product_rw(uintx, ShenandoahFreeThreshold, 10,                            \
          "Set the percentage of free heap at which a GC cycle is started. " \
          "Applies to Shenandoah GC dynamic Heuristic mode only "           \
          "(ignored otherwise). Defaults to 10%.")                          \
                                                                            \
  product_rw(uintx, ShenandoahCSetThreshold, 40,                            \
          "Set the approximate target percentage of the heap for the"       \
          "collection set. Defaults to 40%.")                               \
  product_rw(uintx, ShenandoahAllocationThreshold, 0,                       \
          "Set percentage of memory allocated since last GC cycle before "  \
          "a new GC cycle is started. "                                     \
          "Applies to Shenandoah GC dynamic Heuristic mode only "           \
          "(ignored otherwise). Defauls to 0%.")                            \
									    \
  experimental(uintx, ShenandoahMergeUpdateRefsMinGap, 100,                 \
               "If GC is currently running in separate update-refs mode "   \
               "this numbers gives the threshold when to switch to "        \
               "merged update-refs mode. Number is percentage relative to"  \
               "duration(marking)+duration(update-refs).")                  \
                                                                            \
  experimental(uintx, ShenandoahMergeUpdateRefsMaxGap, 200,                 \
               "If GC is currently running in merged update-refs mode "     \
               "this numbers gives the threshold when to switch to "        \
               "separate update-refs mode. Number is percentage relative "  \
               "to duration(marking)+duration(update-refs).")               \
                                                                            \
  experimental(double, ShenandoahGCWorkerPerJavaThread, 0.5,                \
          "Set GC worker to Java thread ratio when "                        \
          "UseDynamicNumberOfGCThreads is enabled")                         \
                                                                            \
  experimental(uintx, ShenandoahInitFreeThreshold, 30,                      \
               "Initial remaining free threshold for adaptive heuristics")  \
                                                                            \
  experimental(uintx, ShenandoahMinFreeThreshold, 3,                        \
               "Minimum remaining free threshold for adaptive heuristics")  \
                                                                            \
  experimental(uintx, ShenandoahMaxFreeThreshold, 70,                       \
               "Maximum remaining free threshold for adaptive heuristics")  \
                                                                            \
  experimental(uintx, ShenandoahImmediateThreshold, 90,                     \
               "If mark identifies more than this much immediate garbage "  \
               "regions, it shall recycle them, and shall not continue the "\
               "rest of the GC cycle. The value is in percents of total "   \
               "number of candidates for collection set. Setting this "     \
               "threshold to 100% effectively disables this shortcut.")     \
                                                                            \
  experimental(uintx, ShenandoahHappyCyclesThreshold, 3,                    \
          "How many successful marking cycles before improving free "       \
               "threshold for adaptive heuristics")                         \
                                                                            \
  experimental(uintx, ShenandoahMarkLoopStride, 1000,                       \
          "How many items are processed during one marking step")           \
                                                                            \
  experimental(bool, ShenandoahConcurrentScanCodeRoots, true,               \
          "Scan code roots concurrently, instead of during a pause")        \
                                                                            \
  experimental(bool, ShenandoahConcurrentEvacCodeRoots, false,              \
          "Evacuate code roots concurrently, instead of during a pause. "   \
          "This requires ShenandoahBarriersForConst to be enabled.")        \
                                                                            \
  experimental(uintx, ShenandoahCodeRootsStyle, 1,                          \
          "Use this style to scan code cache:"                              \
          " 0 - sequential iterator;"                                       \
          " 1 - parallel iterator;"                                         \
          " 2 - parallel iterator with filters;")                           \
                                                                            \
  experimental(bool, ShenandoahBarriersForConst, false,                     \
          "Emit barriers for constant oops in generated code, improving "   \
          "throughput. If no barriers are emitted, GC will need to "        \
          "pre-evacuate code roots before returning from STW, adding to "   \
          "pause time.")                                                    \
                                                                            \
  experimental(bool, ShenandoahDontIncreaseWBFreq, true,                    \
          "Common 2 WriteBarriers or WriteBarrier and a ReadBarrier only "  \
          "if the resulting WriteBarrier isn't executed more frequently")   \
                                                                            \
  experimental(bool, ShenandoahNoLivenessFullGC, true,                      \
          "Skip liveness counting for mark during full GC.")                \
                                                                            \
  experimental(bool, ShenandoahWriteBarrierToIR, true,                      \
          "Convert write barrier to IR instead of using assembly blob")     \
                                                                            \
  experimental(bool, ShenandoahWriteBarrierCsetTestInIR, true,              \
          "Perform cset test in IR rather than in the stub")                \
                                                                            \
  experimental(bool, UseShenandoahOWST, true,                               \
          "Use Shenandoah work stealing termination protocol")              \
                                                                            \
  experimental(size_t, ShenandoahSATBBufferSize, 1 * K,                     \
          "Number of entries in an SATB log buffer.")                       \
                                                                            \
  product_rw(int, ShenandoahRegionSamplingRate, 40,                         \
          "Sampling rate for heap region sampling. "                        \
          "Number of milliseconds between samples")                         \
                                                                            \
  product_rw(bool, ShenandoahRegionSampling, false,                         \
          "Turns on heap region sampling via JVMStat")                      \
                                                                            \
  diagnostic(bool, ShenandoahWriteBarrier, true,                            \
          "Turn on/off write barriers in Shenandoah")                       \
                                                                            \
  diagnostic(bool, ShenandoahReadBarrier, true,                             \
          "Turn on/off read barriers in Shenandoah")                        \
                                                                            \
  diagnostic(bool, ShenandoahStoreCheck, false,                             \
          "Emit additional code that checks objects are written to only"    \
          " in to-space")                                                   \
                                                                            \
  diagnostic(bool, ShenandoahVerify, false,                                 \
          "Verify the Shenandoah garbage collector")                        \
                                                                            \
  diagnostic(intx, ShenandoahVerifyLevel, 4,                                \
          "Shenandoah verification level: "                                 \
          "0 = basic heap checks; "                                         \
          "1 = previous level, plus basic region checks; "                  \
          "2 = previous level, plus all roots; "                            \
          "3 = previous level, plus all reachable objects; "                \
          "4 = previous level, plus all marked objects")                    \
                                                                            \
  develop(bool, VerifyStrictOopOperations, false,                           \
          "Verify that == and != are not used on oops. Only in fastdebug")  \
                                                                            \
  develop(bool, ShenandoahVerifyOptoBarriers, false,                        \
          "Verify no missing barriers in c2")                               \
                                                                            \
  product(bool, ShenandoahAlwaysPreTouch, false,                            \
          "Pre-touch heap memory, overrides global AlwaysPreTouch")         \
                                                                            \
  experimental(intx, ShenandoahMarkScanPrefetch, 32,                        \
          "How many objects to prefetch ahead when traversing mark bitmaps." \
          "Set to 0 to disable prefetching.")                               \
                                                                            \
  experimental(intx, ShenandoahFullGCTries, 3,                              \
          "How many times to try to do Full GC on allocation failure."      \
          "Set to 0 to never try, and fail instead.")                       \
                                                                            \
  experimental(bool, ShenandoahFastSyncRoots, true,                         \
          "Enable fast synchronizer roots scanning")                        \
                                                                            \
  experimental(bool, ShenandoahPreclean, true,                              \
              "Do preclean phase before final mark")                        \
                                                                            \

SHENANDOAH_FLAGS(DECLARE_DEVELOPER_FLAG, \
                 DECLARE_PD_DEVELOPER_FLAG,     \
                 DECLARE_PRODUCT_FLAG,          \
                 DECLARE_PD_PRODUCT_FLAG,       \
                 DECLARE_DIAGNOSTIC_FLAG,       \
                 DECLARE_EXPERIMENTAL_FLAG,     \
                 DECLARE_NOTPRODUCT_FLAG,       \
                 DECLARE_MANAGEABLE_FLAG,       \
                 DECLARE_PRODUCT_RW_FLAG)

#endif // SHARE_VM_GC_SHENANDOAH_SHENANDOAH_GLOBALS_HPP
