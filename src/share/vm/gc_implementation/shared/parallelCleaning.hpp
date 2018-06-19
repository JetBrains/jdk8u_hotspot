/*
 * Copyright (c) 2016, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
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

#ifndef SHARE_VM_GC_IMPLEMENTATION_SHARED_PARALLELCLEANING_HPP
#define SHARE_VM_GC_IMPLEMENTATION_SHARED_PARALLELCLEANING_HPP

#include "classfile/metadataOnStackMark.hpp"
#include "classfile/symbolTable.hpp"
#include "code/codeCache.hpp"
#include "gc_interface/collectedHeap.hpp"
#include "memory/resourceArea.hpp"
#include "utilities/workgroup.hpp"

class StringSymbolTableUnlinkTask : public AbstractGangTask {
private:
  BoolObjectClosure* _is_alive;
  int _initial_string_table_size;
  int _initial_symbol_table_size;

  bool  _process_strings;
  int _strings_processed;
  int _strings_removed;

  bool  _process_symbols;
  int _symbols_processed;
  int _symbols_removed;

  bool _do_in_parallel;
public:
  StringSymbolTableUnlinkTask(BoolObjectClosure* is_alive, bool process_strings, bool process_symbols) :
    AbstractGangTask("String/Symbol Unlinking"),
    _is_alive(is_alive),
    _do_in_parallel(Universe::heap()->use_parallel_gc_threads()),
    _process_strings(process_strings), _strings_processed(0), _strings_removed(0),
    _process_symbols(process_symbols), _symbols_processed(0), _symbols_removed(0) {

    _initial_string_table_size = StringTable::the_table()->table_size();
    _initial_symbol_table_size = SymbolTable::the_table()->table_size();
    if (process_strings) {
      StringTable::clear_parallel_claimed_index();
    }
    if (process_symbols) {
      SymbolTable::clear_parallel_claimed_index();
    }
  }

  ~StringSymbolTableUnlinkTask() {
    guarantee(!_process_strings || !_do_in_parallel || StringTable::parallel_claimed_index() >= _initial_string_table_size,
              err_msg("claim value "INT32_FORMAT" after unlink less than initial string table size "INT32_FORMAT,
                      StringTable::parallel_claimed_index(), _initial_string_table_size));
    guarantee(!_process_symbols || !_do_in_parallel || SymbolTable::parallel_claimed_index() >= _initial_symbol_table_size,
              err_msg("claim value "INT32_FORMAT" after unlink less than initial symbol table size "INT32_FORMAT,
                      SymbolTable::parallel_claimed_index(), _initial_symbol_table_size));

    if (G1TraceStringSymbolTableScrubbing) {
      gclog_or_tty->print_cr("Cleaned string and symbol table, "
                             "strings: "SIZE_FORMAT" processed, "SIZE_FORMAT" removed, "
                             "symbols: "SIZE_FORMAT" processed, "SIZE_FORMAT" removed",
                             strings_processed(), strings_removed(),
                             symbols_processed(), symbols_removed());
    }
  }

  void work(uint worker_id) {
    if (_do_in_parallel) {
      int strings_processed = 0;
      int strings_removed = 0;
      int symbols_processed = 0;
      int symbols_removed = 0;
      if (_process_strings) {
        StringTable::possibly_parallel_unlink(_is_alive, &strings_processed, &strings_removed);
        Atomic::add(strings_processed, &_strings_processed);
        Atomic::add(strings_removed, &_strings_removed);
      }
      if (_process_symbols) {
        SymbolTable::possibly_parallel_unlink(&symbols_processed, &symbols_removed);
        Atomic::add(symbols_processed, &_symbols_processed);
        Atomic::add(symbols_removed, &_symbols_removed);
      }
    } else {
      if (_process_strings) {
        StringTable::unlink(_is_alive, &_strings_processed, &_strings_removed);
      }
      if (_process_symbols) {
        SymbolTable::unlink(&_symbols_processed, &_symbols_removed);
      }
    }
  }

  size_t strings_processed() const { return (size_t)_strings_processed; }
  size_t strings_removed()   const { return (size_t)_strings_removed; }

  size_t symbols_processed() const { return (size_t)_symbols_processed; }
  size_t symbols_removed()   const { return (size_t)_symbols_removed; }
};

class CodeCacheUnloadingTask VALUE_OBJ_CLASS_SPEC {
private:
  static Monitor* _lock;

  BoolObjectClosure* const _is_alive;
  const bool               _unloading_occurred;
  const uint               _num_workers;

  // Variables used to claim nmethods.
  nmethod* _first_nmethod;
  volatile nmethod* _claimed_nmethod;

  // The list of nmethods that need to be processed by the second pass.
  volatile nmethod* _postponed_list;
  volatile uint     _num_entered_barrier;

 public:
  CodeCacheUnloadingTask(uint num_workers, BoolObjectClosure* is_alive, bool unloading_occurred) :
      _is_alive(is_alive),
      _unloading_occurred(unloading_occurred),
      _num_workers(num_workers),
      _first_nmethod(NULL),
      _claimed_nmethod(NULL),
      _postponed_list(NULL),
      _num_entered_barrier(0)
  {
    nmethod::increase_unloading_clock();
    _first_nmethod = CodeCache::alive_nmethod(CodeCache::first());
    _claimed_nmethod = (volatile nmethod*)_first_nmethod;
  }

  ~CodeCacheUnloadingTask() {
    CodeCache::verify_clean_inline_caches();

    CodeCache::set_needs_cache_clean(false);
    guarantee(CodeCache::scavenge_root_nmethods() == NULL, "Must be");

    CodeCache::verify_icholder_relocations();
  }

 private:
  void add_to_postponed_list(nmethod* nm) {
      nmethod* old;
      do {
        old = (nmethod*)_postponed_list;
        nm->set_unloading_next(old);
      } while ((nmethod*)Atomic::cmpxchg_ptr(nm, &_postponed_list, old) != old);
  }

  void clean_nmethod(nmethod* nm) {
    bool postponed = nm->do_unloading_parallel(_is_alive, _unloading_occurred);

    if (postponed) {
      // This nmethod referred to an nmethod that has not been cleaned/unloaded yet.
      add_to_postponed_list(nm);
    }

    // Mark that this thread has been cleaned/unloaded.
    // After this call, it will be safe to ask if this nmethod was unloaded or not.
    nm->set_unloading_clock(nmethod::global_unloading_clock());
  }

  void clean_nmethod_postponed(nmethod* nm) {
    nm->do_unloading_parallel_postponed(_is_alive, _unloading_occurred);
  }

  static const int MaxClaimNmethods = 16;

  void claim_nmethods(nmethod** claimed_nmethods, int *num_claimed_nmethods) {
    nmethod* first;
    nmethod* last;

    do {
      *num_claimed_nmethods = 0;

      first = last = (nmethod*)_claimed_nmethod;

      if (first != NULL) {
        for (int i = 0; i < MaxClaimNmethods; i++) {
          last = CodeCache::alive_nmethod(CodeCache::next(last));

          if (last == NULL) {
            break;
          }

          claimed_nmethods[i] = last;
          (*num_claimed_nmethods)++;
        }
      }

    } while ((nmethod*)Atomic::cmpxchg_ptr(last, &_claimed_nmethod, first) != first);
  }

  nmethod* claim_postponed_nmethod() {
    nmethod* claim;
    nmethod* next;

    do {
      claim = (nmethod*)_postponed_list;
      if (claim == NULL) {
        return NULL;
      }

      next = claim->unloading_next();

    } while ((nmethod*)Atomic::cmpxchg_ptr(next, &_postponed_list, claim) != claim);

    return claim;
  }

 public:
  // Mark that we're done with the first pass of nmethod cleaning.
  void barrier_mark(uint worker_id) {
    MonitorLockerEx ml(_lock, Mutex::_no_safepoint_check_flag);
    _num_entered_barrier++;
    if (_num_entered_barrier == _num_workers) {
      ml.notify_all();
    }
  }

  // See if we have to wait for the other workers to
  // finish their first-pass nmethod cleaning work.
  void barrier_wait(uint worker_id) {
    if (_num_entered_barrier < _num_workers) {
      MonitorLockerEx ml(_lock, Mutex::_no_safepoint_check_flag);
      while (_num_entered_barrier < _num_workers) {
          ml.wait(Mutex::_no_safepoint_check_flag, 0, false);
      }
    }
  }

  // Cleaning and unloading of nmethods. Some work has to be postponed
  // to the second pass, when we know which nmethods survive.
  void work_first_pass(uint worker_id) {
    // The first nmethods is claimed by the first worker.
    if (worker_id == 0 && _first_nmethod != NULL) {
      clean_nmethod(_first_nmethod);
      _first_nmethod = NULL;
    }

    int num_claimed_nmethods;
    nmethod* claimed_nmethods[MaxClaimNmethods];

    while (true) {
      claim_nmethods(claimed_nmethods, &num_claimed_nmethods);

      if (num_claimed_nmethods == 0) {
        break;
      }

      for (int i = 0; i < num_claimed_nmethods; i++) {
        clean_nmethod(claimed_nmethods[i]);
      }
    }

    // The nmethod cleaning helps out and does the CodeCache part of MetadataOnStackMark.
    // Need to retire the buffers now that this thread has stopped cleaning nmethods.
    MetadataOnStackMark::retire_buffer_for_thread(Thread::current());
  }

  void work_second_pass(uint worker_id) {
    nmethod* nm;
    // Take care of postponed nmethods.
    while ((nm = claim_postponed_nmethod()) != NULL) {
      clean_nmethod_postponed(nm);
    }
  }
};

class KlassCleaningTask : public StackObj {
  BoolObjectClosure*                      _is_alive;
  volatile jint                           _clean_klass_tree_claimed;
  ClassLoaderDataGraphKlassIteratorAtomic _klass_iterator;

 public:
  KlassCleaningTask(BoolObjectClosure* is_alive) :
      _is_alive(is_alive),
      _clean_klass_tree_claimed(0),
      _klass_iterator() {
  }

 private:
  bool claim_clean_klass_tree_task() {
    if (_clean_klass_tree_claimed) {
      return false;
    }

    return Atomic::cmpxchg(1, (jint*)&_clean_klass_tree_claimed, 0) == 0;
  }

  InstanceKlass* claim_next_klass() {
    Klass* klass;
    do {
      klass =_klass_iterator.next_klass();
    } while (klass != NULL && !klass->oop_is_instance());

    return (InstanceKlass*)klass;
  }

public:

  void clean_klass(InstanceKlass* ik) {
    ik->clean_weak_instanceklass_links(_is_alive);

    if (JvmtiExport::has_redefined_a_class()) {
      InstanceKlass::purge_previous_versions(ik);
    }
  }

  void work() {
    ResourceMark rm;

    // One worker will clean the subklass/sibling klass tree.
    if (claim_clean_klass_tree_task()) {
      Klass::clean_subklass_tree(_is_alive);
    }

    // All workers will help cleaning the classes,
    InstanceKlass* klass;
    while ((klass = claim_next_klass()) != NULL) {
      clean_klass(klass);
    }
  }
};

class ParallelCleaningTask;

class ParallelCleaningTimes {
  friend class ParallelCleaningTask;
private:
  // All times are in microseconds, making room for ~2 hrs in jint
  jint _sync, _codecache_work, _tables_work, _klass_work;

public:
  ParallelCleaningTimes() :
          _sync(0),
          _codecache_work(0),
          _tables_work(0),
          _klass_work(0) {};

  jint sync_us()           const { return _sync; }
  jint codecache_work_us() const { return _codecache_work; }
  jint tables_work_us()    const { return _tables_work; }
  jint klass_work_us()     const { return _klass_work; }
};

class ParallelCleaningTaskTimer {
  volatile jint* _timer_us;
  jlong start;
public:
  ParallelCleaningTaskTimer(jint* timer_us) : _timer_us(timer_us) {
    start = os::javaTimeNanos();
  }
  ~ParallelCleaningTaskTimer() {
    jlong ns = os::javaTimeNanos() - start;
    jlong us = ns / 1000;
    assert (us < max_jint, "overflow");
    Atomic::add((jint) us, _timer_us);
  }
};

// To minimize the remark pause times, the tasks below are done in parallel.
class ParallelCleaningTask : public AbstractGangTask {
  friend class ParallelCleaningTimes;
private:
  ParallelCleaningTimes       _times;
  StringSymbolTableUnlinkTask _string_symbol_task;
  CodeCacheUnloadingTask      _code_cache_task;
  KlassCleaningTask           _klass_cleaning_task;

public:
  // The constructor is run in the VMThread.
  ParallelCleaningTask(BoolObjectClosure* is_alive, bool process_strings, bool process_symbols, uint num_workers, bool unloading_occurred) :
      AbstractGangTask("Parallel Cleaning"),
      _string_symbol_task(is_alive, process_strings, process_symbols),
      _code_cache_task(num_workers, is_alive, unloading_occurred),
      _klass_cleaning_task(is_alive) {
  }

  void pre_work_verification() {
    // The VM Thread will have registered Metadata during the single-threaded phase of MetadataStackOnMark.
    assert(Thread::current()->is_VM_thread()
           || !MetadataOnStackMark::has_buffer_for_thread(Thread::current()), "Should be empty");
  }

  void post_work_verification() {
    assert(!MetadataOnStackMark::has_buffer_for_thread(Thread::current()), "Should be empty");
  }

  ParallelCleaningTimes times() {
    return _times;
  }

  // The parallel work done by all worker threads.
  void work(uint worker_id) {
    {
      ParallelCleaningTaskTimer timer(&_times._codecache_work);
      // Do first pass of code cache cleaning.
      _code_cache_task.work_first_pass(worker_id);
    }

    {
      ParallelCleaningTaskTimer timer(&_times._sync);
      // Let the threads mark that the first pass is done.
      _code_cache_task.barrier_mark(worker_id);
    }

    {
      ParallelCleaningTaskTimer timer(&_times._tables_work);
      // Clean the Strings and Symbols.
      _string_symbol_task.work(worker_id);
    }

    {
      ParallelCleaningTaskTimer timer(&_times._sync);
      // Wait for all workers to finish the first code cache cleaning pass.
      _code_cache_task.barrier_wait(worker_id);
    }

    {
      ParallelCleaningTaskTimer timer(&_times._codecache_work);
      // Do the second code cache cleaning work, which realize on
      // the liveness information gathered during the first pass.
      _code_cache_task.work_second_pass(worker_id);
    }

    {
      ParallelCleaningTaskTimer timer(&_times._klass_work);
      // Clean all klasses that were not unloaded.
      _klass_cleaning_task.work();
    }
  }

};

#endif // SHARE_VM_GC_IMPLEMENTATION_SHARED_PARALLELCLEANING_HPP
