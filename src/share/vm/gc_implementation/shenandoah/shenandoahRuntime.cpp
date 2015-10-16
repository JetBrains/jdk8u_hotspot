/*
 * Copyright (c) 2014, 2015, Red Hat, Inc. and/or its affiliates.
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

#include "gc_implementation/shenandoah/shenandoahRuntime.hpp"
#include "gc_implementation/shenandoah/shenandoahBarrierSet.inline.hpp"
#include "runtime/interfaceSupport.hpp"
#include "oops/oop.inline.hpp"

JRT_LEAF(bool, ShenandoahRuntime::compare_and_swap_object(HeapWord* addr, oopDesc* newval, oopDesc* old))
  bool success;
  oop expected;
  do {
    expected = old;
    old = oopDesc::atomic_compare_exchange_oop(newval, addr, expected, true);
    success  = (old == expected);
  } while ((! success) && ShenandoahBarrierSet::resolve_oop_static(old) == ShenandoahBarrierSet::resolve_oop_static(expected));

  return success;
JRT_END
