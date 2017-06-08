/*
 * Copyright (c) 2017, Red Hat, Inc. and/or its affiliates.
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

#include "memory/allocation.hpp"
#include "gc_implementation/shenandoah/shenandoahHeap.hpp"
#include "gc_implementation/shenandoah/shenandoahHeap.inline.hpp"
#include "gc_implementation/shenandoah/shenandoahPrinter.hpp"

class PrintAllRefsOopClosure: public ExtendedOopClosure {
private:
  int _index;
  const char* _prefix;

public:
  PrintAllRefsOopClosure(const char* prefix) : _index(0), _prefix(prefix) {}

private:
  template <class T>
  inline void do_oop_work(T* p) {
    oop o = oopDesc::load_decode_heap_oop(p);
    if (o != NULL) {
      if (ShenandoahHeap::heap()->is_in(o) && o->is_oop()) {
        tty->print_cr("%s "INT32_FORMAT" ("PTR_FORMAT")-> "PTR_FORMAT" (marked: %s) (%s "PTR_FORMAT")",
                      _prefix, _index,
                      p2i(p), p2i(o),
                      BOOL_TO_STR(ShenandoahHeap::heap()->is_marked_complete(o)),
                      o->klass()->internal_name(), p2i(o->klass()));
      } else {
        tty->print_cr("%s "INT32_FORMAT" ("PTR_FORMAT" dirty -> "PTR_FORMAT" (not in heap, possibly corrupted or dirty)",
                      _prefix, _index,
                      p2i(p), p2i(o));
      }
    } else {
      tty->print_cr("%s "INT32_FORMAT" ("PTR_FORMAT") -> "PTR_FORMAT, _prefix, _index, p2i(p), p2i((HeapWord*) o));
    }
    _index++;
  }

public:
  void do_oop(oop* p) {
    do_oop_work(p);
  }

  void do_oop(narrowOop* p) {
    do_oop_work(p);
  }
};

class PrintAllRefsObjectClosure : public ObjectClosure {
  const char* _prefix;

public:
  PrintAllRefsObjectClosure(const char* prefix) : _prefix(prefix) {}

  void do_object(oop p) {
    if (ShenandoahHeap::heap()->is_in(p)) {
      tty->print_cr("%s object "PTR_FORMAT" (marked: %s) (%s "PTR_FORMAT") refers to:",
                    _prefix, p2i(p),
                    BOOL_TO_STR(ShenandoahHeap::heap()->is_marked_complete(p)),
                    p->klass()->internal_name(), p2i(p->klass()));
      PrintAllRefsOopClosure cl(_prefix);
      p->oop_iterate(&cl);
    }
  }
};

void ShenandoahPrinter::print_all_refs(const char* prefix) {
  tty->print_cr("printing all references in the heap");
  tty->print_cr("root references:");

  _heap->ensure_parsability(false);

  PrintAllRefsOopClosure cl(prefix);
  _heap->roots_iterate(&cl);

  tty->print_cr("heap references:");
  PrintAllRefsObjectClosure cl2(prefix);
  _heap->object_iterate(&cl2);
}
