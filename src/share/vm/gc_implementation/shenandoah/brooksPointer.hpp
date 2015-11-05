/*
 * Copyright (c) 2013, 2015, Red Hat, Inc. and/or its affiliates.
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

#ifndef SHARE_VM_GC_SHENANDOAH_BROOKSPOINTER_HPP
#define SHARE_VM_GC_SHENANDOAH_BROOKSPOINTER_HPP

#include "oops/oop.hpp"
#include "utilities/globalDefinitions.hpp"
#include "gc_implementation/shenandoah/shenandoahHeap.hpp"

class BrooksPointer {

public:
  static const uint BROOKS_POINTER_OBJ_SIZE = 1;
  static const int BYTE_OFFSET = -8;

private:

  HeapWord** _heap_word;

  BrooksPointer(HeapWord** heap_word);

public:

  bool check_forwardee_is_in_heap(oop forwardee);

  inline void set_forwardee(oop forwardee);
  inline HeapWord* cas_forwardee(HeapWord* old, HeapWord* forwardee);

  static inline BrooksPointer get(oop obj);
};

#endif // SHARE_VM_GC_SHENANDOAH_BROOKSPOINTER_HPP
