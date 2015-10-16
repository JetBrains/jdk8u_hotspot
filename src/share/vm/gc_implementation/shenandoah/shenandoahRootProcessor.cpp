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

#include "precompiled.hpp"

#include "classfile/symbolTable.hpp"
#include "classfile/systemDictionary.hpp"
#include "code/codeCache.hpp"
#include "gc_implementation/shenandoah/shenandoahRootProcessor.hpp"
#include "gc_implementation/shenandoah/shenandoahHeap.hpp"
#include "memory/allocation.inline.hpp"
#include "runtime/fprofiler.hpp"
#include "runtime/mutex.hpp"
#include "services/management.hpp"

ShenandoahRootProcessor::ShenandoahRootProcessor(ShenandoahHeap* heap, uint n_workers) :
  _process_strong_tasks(new SubTasksDone(SHENANDOAH_RP_PS_NumElements)),
  _srs(heap, true)
{
  _process_strong_tasks->set_n_threads(n_workers);
}

ShenandoahRootProcessor::~ShenandoahRootProcessor() {
  delete _process_strong_tasks;
}

void ShenandoahRootProcessor::process_roots(OopClosure* strong_oops,
                                            OopClosure* weak_oops,
                                            CLDClosure* strong_clds,
                                            CLDClosure* weak_clds,
                                            CLDClosure* thread_stack_clds,
                                            CodeBlobClosure* strong_code) {
  process_java_roots(strong_oops, thread_stack_clds, strong_clds, weak_clds, strong_code, 0);
  process_vm_roots(strong_oops, weak_oops, 0);

  if (!_process_strong_tasks->is_task_claimed(SHENANDOAH_RP_PS_CodeCache_oops_do)) {
    CodeCache::blobs_do(strong_code);
  }

  if (!_process_strong_tasks->is_task_claimed(SHENANDOAH_RP_PS_JNIHandles_weak_oops_do)) {
    ShenandoahAlwaysTrueClosure always_true;
    JNIHandles::weak_oops_do(&always_true, weak_oops);
  }

  _process_strong_tasks->all_tasks_completed();
}

void ShenandoahRootProcessor::process_strong_roots(OopClosure* oops,
                                           CLDClosure* clds,
                                           CodeBlobClosure* blobs) {

  process_java_roots(oops, clds, clds, NULL, blobs, 0);
  process_vm_roots(oops, NULL, 0);

  _process_strong_tasks->all_tasks_completed();
}

void ShenandoahRootProcessor::process_all_roots(OopClosure* oops,
                                        CLDClosure* clds,
                                        CodeBlobClosure* blobs) {

  process_java_roots(oops, NULL, clds, clds, NULL, 0);
  process_vm_roots(oops, oops, 0);

  if (!_process_strong_tasks->is_task_claimed(SHENANDOAH_RP_PS_CodeCache_oops_do)) {
    CodeCache::blobs_do(blobs);
  }

  _process_strong_tasks->all_tasks_completed();
}

void ShenandoahRootProcessor::process_java_roots(OopClosure* strong_roots,
                                                 CLDClosure* thread_stack_clds,
                                                 CLDClosure* strong_clds,
                                                 CLDClosure* weak_clds,
                                                 CodeBlobClosure* strong_code,
                                                 uint worker_i)
{
  //assert(thread_stack_clds == NULL || weak_clds == NULL, "There is overlap between those, only one may be set");
  // Iterating over the CLDG and the Threads are done early to allow us to
  // first process the strong CLDs and nmethods and then, after a barrier,
  // let the thread process the weak CLDs and nmethods.
  if (!_process_strong_tasks->is_task_claimed(SHENANDOAH_RP_PS_ClassLoaderDataGraph_oops_do)) {
    ClassLoaderDataGraph::roots_cld_do(strong_clds, weak_clds);
  }

  ResourceMark rm;
  Threads::possibly_parallel_oops_do(strong_roots, thread_stack_clds, strong_code);
}

void ShenandoahRootProcessor::process_vm_roots(OopClosure* strong_roots,
                                               OopClosure* weak_roots,
                                               uint worker_i)
{
  if (!_process_strong_tasks->is_task_claimed(SHENANDOAH_RP_PS_Universe_oops_do)) {
    Universe::oops_do(strong_roots);
  }

  if (!_process_strong_tasks->is_task_claimed(SHENANDOAH_RP_PS_JNIHandles_oops_do)) {
    JNIHandles::oops_do(strong_roots);
  }
  if (!_process_strong_tasks-> is_task_claimed(SHENANDOAH_RP_PS_ObjectSynchronizer_oops_do)) {
    ObjectSynchronizer::oops_do(strong_roots);
  }
  if (!_process_strong_tasks->is_task_claimed(SHENANDOAH_RP_PS_FlatProfiler_oops_do)) {
    FlatProfiler::oops_do(strong_roots);
  }
  if (!_process_strong_tasks->is_task_claimed(SHENANDOAH_RP_PS_Management_oops_do)) {
    Management::oops_do(strong_roots);
  }
  if (!_process_strong_tasks->is_task_claimed(SHENANDOAH_RP_PS_jvmti_oops_do)) {
    JvmtiExport::oops_do(strong_roots);
  }
  if (!_process_strong_tasks->is_task_claimed(SHENANDOAH_RP_PS_SystemDictionary_oops_do)) {
    SystemDictionary::roots_oops_do(strong_roots, weak_roots);
  }
  // All threads execute the following. A specific chunk of buckets
  // from the StringTable are the individual tasks.
  if (weak_roots != NULL) {
    StringTable::possibly_parallel_oops_do(weak_roots);
  }
}
