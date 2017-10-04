/*
 * Copyright (c) 2015, 2017, Red Hat, Inc. and/or its affiliates.
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

#include "classfile/javaClasses.hpp"
#include "classfile/systemDictionary.hpp"
#include "code/codeCache.hpp"
#include "gc_implementation/shenandoah/shenandoahRootProcessor.hpp"
#include "gc_implementation/shenandoah/shenandoahHeap.hpp"
#include "gc_implementation/shenandoah/shenandoahHeap.inline.hpp"
#include "gc_implementation/shenandoah/shenandoahFreeSet.hpp"
#include "gc_implementation/shenandoah/shenandoahBarrierSet.hpp"
#include "gc_implementation/shenandoah/shenandoahCollectorPolicy.hpp"
#include "gc_implementation/shenandoah/shenandoahPhaseTimes.hpp"
#include "memory/allocation.inline.hpp"
#include "runtime/fprofiler.hpp"
#include "runtime/mutex.hpp"
#include "services/management.hpp"

ShenandoahRootProcessor::ShenandoahRootProcessor(ShenandoahHeap* heap, uint n_workers,
                                                 ShenandoahCollectorPolicy::TimingPhase phase) :
  _process_strong_tasks(new SubTasksDone(SHENANDOAH_RP_PS_NumElements)),
  _srs(heap, true),
  _phase(phase),
  _coderoots_all_iterator(ShenandoahCodeRoots::iterator()),
  _om_iterator(ObjectSynchronizer::parallel_iterator())
{
  heap->shenandoahPolicy()->record_workers_start(_phase);
  _process_strong_tasks->set_n_threads(n_workers);
  heap->set_par_threads(n_workers);
}

ShenandoahRootProcessor::~ShenandoahRootProcessor() {
  delete _process_strong_tasks;
  ShenandoahHeap::heap()->shenandoahPolicy()->record_workers_end(_phase);
}

void ShenandoahRootProcessor::process_all_roots_slow(OopClosure* oops) {
  ShenandoahAlwaysTrueClosure always_true;

  CLDToOopClosure clds(oops);
  CodeBlobToOopClosure blobs(oops, !CodeBlobToOopClosure::FixRelocations);

  Threads::possibly_parallel_oops_do(oops, &clds, &blobs);
  CodeCache::blobs_do(&blobs);
  ClassLoaderDataGraph::cld_do(&clds);
  Universe::oops_do(oops);
  FlatProfiler::oops_do(oops);
  Management::oops_do(oops);
  JvmtiExport::oops_do(oops);
  JNIHandles::oops_do(oops);
  JNIHandles::weak_oops_do(&always_true, oops);
  ObjectSynchronizer::oops_do(oops);
  SystemDictionary::roots_oops_do(oops, oops);
  StringTable::oops_do(oops);
}

void ShenandoahRootProcessor::process_strong_roots(OopClosure* oops,
                                                   OopClosure* weak_oops,
                                                   CLDClosure* clds,
                                                   CodeBlobClosure* blobs,
                                                   uint worker_id) {

  process_java_roots(oops, clds, clds, NULL, blobs, worker_id);
  process_vm_roots(oops, NULL, weak_oops, worker_id);

  _process_strong_tasks->all_tasks_completed();
}

void ShenandoahRootProcessor::process_all_roots(OopClosure* oops,
                                                OopClosure* weak_oops,
                                                CLDClosure* clds,
                                                CodeBlobClosure* blobs,
                                                uint worker_id) {

  ShenandoahPhaseTimes* phase_times = ShenandoahHeap::heap()->shenandoahPolicy()->phase_times();
  process_java_roots(oops, NULL, clds, clds, NULL, worker_id);
  process_vm_roots(oops, oops, weak_oops, worker_id);

  if (blobs != NULL) {
    ShenandoahParPhaseTimesTracker timer(phase_times, ShenandoahPhaseTimes::CodeCacheRoots, worker_id);
    _coderoots_all_iterator.possibly_parallel_blobs_do(blobs);
  }

  _process_strong_tasks->all_tasks_completed();
}

void ShenandoahRootProcessor::process_java_roots(OopClosure* strong_roots,
						 CLDClosure* thread_clds,
                                                 CLDClosure* strong_clds,
                                                 CLDClosure* weak_clds,
                                                 CodeBlobClosure* strong_code,
                                                 uint worker_id)
{
  ShenandoahPhaseTimes* phase_times = ShenandoahHeap::heap()->shenandoahPolicy()->phase_times();
  // Iterating over the CLDG and the Threads are done early to allow us to
  // first process the strong CLDs and nmethods and then, after a barrier,
  // let the thread process the weak CLDs and nmethods.
  {
    ShenandoahParPhaseTimesTracker timer(phase_times, ShenandoahPhaseTimes::CLDGRoots, worker_id);
    _cld_iterator.root_cld_do(strong_clds, weak_clds);
  }

  {
    ShenandoahParPhaseTimesTracker timer(phase_times, ShenandoahPhaseTimes::ThreadRoots, worker_id);
    ResourceMark rm;
    Threads::possibly_parallel_oops_do(strong_roots, thread_clds, strong_code);
  }
}

void ShenandoahRootProcessor::process_vm_roots(OopClosure* strong_roots,
                                               OopClosure* weak_roots,
                                               OopClosure* jni_weak_roots,
                                               uint worker_id)
{
  ShenandoahPhaseTimes* phase_times = ShenandoahHeap::heap()->shenandoahPolicy()->phase_times();
  if (!_process_strong_tasks->is_task_claimed(SHENANDOAH_RP_PS_Universe_oops_do)) {
    ShenandoahParPhaseTimesTracker timer(phase_times, ShenandoahPhaseTimes::UniverseRoots, worker_id);
    Universe::oops_do(strong_roots);
  }

  if (!_process_strong_tasks->is_task_claimed(SHENANDOAH_RP_PS_JNIHandles_oops_do)) {
    ShenandoahParPhaseTimesTracker timer(phase_times, ShenandoahPhaseTimes::JNIRoots, worker_id);
    JNIHandles::oops_do(strong_roots);
  }

  if (!_process_strong_tasks->is_task_claimed(SHENANDOAH_RP_PS_FlatProfiler_oops_do)) {
    ShenandoahParPhaseTimesTracker timer(phase_times, ShenandoahPhaseTimes::FlatProfilerRoots, worker_id);
    FlatProfiler::oops_do(strong_roots);
  }
  if (!_process_strong_tasks->is_task_claimed(SHENANDOAH_RP_PS_Management_oops_do)) {
    ShenandoahParPhaseTimesTracker timer(phase_times, ShenandoahPhaseTimes::ManagementRoots, worker_id);
    Management::oops_do(strong_roots);
  }
  if (!_process_strong_tasks->is_task_claimed(SHENANDOAH_RP_PS_jvmti_oops_do)) {
    ShenandoahParPhaseTimesTracker timer(phase_times, ShenandoahPhaseTimes::JVMTIRoots, worker_id);
    JvmtiExport::oops_do(strong_roots);
  }
  if (!_process_strong_tasks->is_task_claimed(SHENANDOAH_RP_PS_SystemDictionary_oops_do)) {
    ShenandoahParPhaseTimesTracker timer(phase_times, ShenandoahPhaseTimes::SystemDictionaryRoots, worker_id);
    SystemDictionary::roots_oops_do(strong_roots, weak_roots);
  }
  if (jni_weak_roots != NULL) {
    if (!_process_strong_tasks->is_task_claimed(SHENANDOAH_RP_PS_JNIHandles_weak_oops_do)) {
      ShenandoahAlwaysTrueClosure always_true;
      ShenandoahParPhaseTimesTracker timer(phase_times, ShenandoahPhaseTimes::JNIWeakRoots, worker_id);
      JNIHandles::weak_oops_do(&always_true, jni_weak_roots);
    }
  }

  {
    ShenandoahParPhaseTimesTracker timer(phase_times, ShenandoahPhaseTimes::ObjectSynchronizerRoots, worker_id);
    if (ShenandoahFastSyncRoots && MonitorInUseLists) {
      ObjectSynchronizer::oops_do(strong_roots);
    } else {
      while(_om_iterator.parallel_oops_do(strong_roots));
    }
  }
  // All threads execute the following. A specific chunk of buckets
  // from the StringTable are the individual tasks.
  if (weak_roots != NULL) {
    ShenandoahParPhaseTimesTracker timer(phase_times, ShenandoahPhaseTimes::StringTableRoots, worker_id);
    StringTable::possibly_parallel_oops_do(weak_roots);
  }
}

ShenandoahRootEvacuator::ShenandoahRootEvacuator(ShenandoahHeap* heap, uint n_workers, ShenandoahCollectorPolicy::TimingPhase phase) :
  _process_strong_tasks(new SubTasksDone(SHENANDOAH_RP_PS_NumElements)),
  _srs(heap, true),
  _phase(phase),
  _coderoots_cset_iterator(ShenandoahCodeRoots::cset_iterator())
{
  _process_strong_tasks->set_n_threads(n_workers);
  heap->set_par_threads(n_workers);
  heap->shenandoahPolicy()->record_workers_start(_phase);
}

ShenandoahRootEvacuator::~ShenandoahRootEvacuator() {
  delete _process_strong_tasks;
  ShenandoahHeap::heap()->shenandoahPolicy()->record_workers_end(_phase);
}

void ShenandoahRootEvacuator::process_evacuate_roots(OopClosure* oops,
                                                     CodeBlobClosure* blobs,
                                                     uint worker_id) {
  ShenandoahHeap* heap = ShenandoahHeap::heap();

  {
    // Evacuate the PLL here so that the SurrogateLockerThread doesn't
    // have to. SurrogateLockerThread can execute write barrier in VMOperation
    // prolog. If the SLT runs into OOM during that evacuation, the VMOperation
    // may deadlock. Doing this evacuation the first thing makes that critical
    // OOM less likely to happen.  It is a bit excessive to perform WB by all
    // threads, but this guarantees the very first evacuation would be the PLL.
    //
    // This pre-evac can still silently fail with OOME here, and PLL would not
    // get evacuated. This would mean next VMOperation would try to evac PLL in
    // SLT thread. We make additional effort to recover from that OOME in SLT,
    // see ShenandoahHeap::oom_during_evacuation(). It seems to be the lesser evil
    // to do there, because we cannot trigger Full GC right here, when we are
    // in another VMOperation.
    oop pll = java_lang_ref_Reference::pending_list_lock();
    oopDesc::bs()->write_barrier(pll);
  }

  ShenandoahPhaseTimes* phase_times = heap->shenandoahPolicy()->phase_times();
  {
    ResourceMark rm;
    ShenandoahParPhaseTimesTracker timer(phase_times, ShenandoahPhaseTimes::ThreadRoots, worker_id);
    Threads::possibly_parallel_oops_do(oops, NULL, NULL);
  }

  if (blobs != NULL) {
    ShenandoahParPhaseTimesTracker timer(phase_times, ShenandoahPhaseTimes::CodeCacheRoots, worker_id);
    _coderoots_cset_iterator.possibly_parallel_blobs_do(blobs);
  }

  _process_strong_tasks->all_tasks_completed();
}


// Implemenation of ParallelCLDRootIterator
ParallelCLDRootIterator::ParallelCLDRootIterator() {
  assert(SafepointSynchronize::is_at_safepoint(), "Must at safepoint");
  ClassLoaderDataGraph::clear_claimed_marks();
}

void ParallelCLDRootIterator::root_cld_do(CLDClosure* strong, CLDClosure* weak) {
    ClassLoaderDataGraph::roots_cld_do(strong, weak);
}
