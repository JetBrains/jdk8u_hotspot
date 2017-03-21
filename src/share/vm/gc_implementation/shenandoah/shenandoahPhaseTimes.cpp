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
#include "gc_implementation/shenandoah/shenandoahPhaseTimes.hpp"
#include "gc_implementation/shenandoah/shenandoahWorkerDataArray.inline.hpp"
#include "runtime/os.hpp"

ShenandoahPhaseTimes::ShenandoahPhaseTimes(uint max_gc_threads) :
  _max_gc_threads(max_gc_threads)
{
  assert(max_gc_threads > 0, "Must have some GC threads");

  // Root scanning phases
  _gc_par_phases[ThreadRoots] = new ShenandoahWorkerDataArray<double>(max_gc_threads, "Thread Roots (ms):");
  _gc_par_phases[CodeCacheRoots] = new ShenandoahWorkerDataArray<double>(max_gc_threads, "CodeCache Roots (ms):");
  _gc_par_phases[StringTableRoots] = new ShenandoahWorkerDataArray<double>(max_gc_threads, "StringTable Roots (ms):");
  _gc_par_phases[UniverseRoots] = new ShenandoahWorkerDataArray<double>(max_gc_threads, "Universe Roots (ms):");
  _gc_par_phases[JNIRoots] = new ShenandoahWorkerDataArray<double>(max_gc_threads, "JNI Handles Roots (ms):");
  _gc_par_phases[JNIWeakRoots] = new ShenandoahWorkerDataArray<double>(max_gc_threads, "JNI Weak Roots (ms):");
  _gc_par_phases[ObjectSynchronizerRoots] = new ShenandoahWorkerDataArray<double>(max_gc_threads, "ObjectSynchronizer Roots (ms):");
  _gc_par_phases[FlatProfilerRoots] = new ShenandoahWorkerDataArray<double>(max_gc_threads, "FlatProfiler Roots (ms):");
  _gc_par_phases[ManagementRoots] = new ShenandoahWorkerDataArray<double>(max_gc_threads, "Management Roots (ms):");
  _gc_par_phases[SystemDictionaryRoots] = new ShenandoahWorkerDataArray<double>(max_gc_threads, "SystemDictionary Roots (ms):");
  _gc_par_phases[CLDGRoots] = new ShenandoahWorkerDataArray<double>(max_gc_threads, "CLDG Roots (ms):");
  _gc_par_phases[JVMTIRoots] = new ShenandoahWorkerDataArray<double>(max_gc_threads, "JVMTI Roots (ms):");
}

// record the time a phase took in seconds
void ShenandoahPhaseTimes::record_time_secs(GCParPhases phase, uint worker_i, double secs) {
  _gc_par_phases[phase]->set(worker_i, secs);
}

double ShenandoahPhaseTimes::average(uint i) {
  return _gc_par_phases[i]->average();
}
void ShenandoahPhaseTimes::reset(uint i) {
  _gc_par_phases[i]->reset();
}

void ShenandoahPhaseTimes::print() {
  for (uint i = 0; i < GCParPhasesSentinel; i++) {
    _gc_par_phases[i]->print_summary_on(tty);
  }
}

ShenandoahParPhaseTimesTracker::ShenandoahParPhaseTimesTracker(ShenandoahPhaseTimes* phase_times,
                                                               ShenandoahPhaseTimes::GCParPhases phase, uint worker_id) :
    _phase_times(phase_times), _phase(phase), _worker_id(worker_id) {
  if (_phase_times != NULL) {
    _start_time = os::elapsedTime();
  }
}

ShenandoahParPhaseTimesTracker::~ShenandoahParPhaseTimesTracker() {
  if (_phase_times != NULL) {
    _phase_times->record_time_secs(_phase, _worker_id, os::elapsedTime() - _start_time);
  }
}

