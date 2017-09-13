/*
 * Copyright (c) 2012, 2013, Oracle and/or its affiliates. All rights reserved.
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

#include "precompiled.hpp"
#include "gc_implementation/shared/gcTimer.hpp"
#include "gc_implementation/shared/gcTrace.hpp"
#include "gc_implementation/shenandoah/shenandoahHeap.hpp"
#include "gc_implementation/shenandoah/shenandoahGCTraceTime.hpp"
#include "runtime/globals.hpp"
#include "runtime/os.hpp"
#include "runtime/safepoint.hpp"
#include "runtime/thread.inline.hpp"
#include "runtime/timer.hpp"
#include "utilities/ostream.hpp"
#include "utilities/ticks.inline.hpp"


ShenandoahGCTraceTime::ShenandoahGCTraceTime(const char* title, bool doit, GCTimer* timer, GCId gc_id, bool print_heap) :
    _title(title), _doit(doit), _timer(timer), _start_counter(), _heap(ShenandoahHeap::heap()), _print_heap(print_heap) {
  if (_doit || _timer != NULL) {
    _start_counter.stamp();
  }

  if (_timer != NULL) {
    _timer->register_gc_phase_start(title, _start_counter);
  }

  if (_doit) {
    _bytes_before = _heap->used();

    gclog_or_tty->date_stamp(PrintGCDateStamps);
    gclog_or_tty->stamp(PrintGCTimeStamps);
    if (PrintGCID && !gc_id.is_undefined()) {
      gclog_or_tty->print("#%u: ", gc_id.id());
    }
    gclog_or_tty->print("[%s", title);
    gclog_or_tty->flush();
  }
}

ShenandoahGCTraceTime::~ShenandoahGCTraceTime() {
  Ticks stop_counter;

  if (_doit || _timer != NULL) {
    stop_counter.stamp();
  }

  if (_timer != NULL) {
    _timer->register_gc_phase_end(stop_counter);
  }

  if (_doit) {
    const Tickspan duration = stop_counter - _start_counter;
    double secs = TicksToTimeHelper::seconds(duration);

    size_t bytes_after = _heap->used();
    size_t capacity = _heap->capacity();

    if (_print_heap) {
      gclog_or_tty->print(" " SIZE_FORMAT "%s->" SIZE_FORMAT "%s(" SIZE_FORMAT  "%s)",
                          byte_size_in_proper_unit(_bytes_before),
                          proper_unit_for_byte_size(_bytes_before),
                          byte_size_in_proper_unit(bytes_after),
                          proper_unit_for_byte_size(bytes_after),
                          byte_size_in_proper_unit(capacity),
                          proper_unit_for_byte_size(capacity));
    }

    gclog_or_tty->print_cr(", %.3f ms]", secs * 1000);
    gclog_or_tty->flush();
  }
}
