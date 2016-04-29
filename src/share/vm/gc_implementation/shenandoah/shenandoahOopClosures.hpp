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

#ifndef SHARE_VM_GC_SHENANDOAH_SHENANDOAHOOPCLOSURES_HPP
#define SHARE_VM_GC_SHENANDOAH_SHENANDOAHOOPCLOSURES_HPP

class ShenandoahHeap;
class QHolder;

class ShenandoahMarkUpdateRefsClosure : public MetadataAwareOopClosure {
  QHolder* _queue;
  ShenandoahHeap* _heap;

public:
  ShenandoahMarkUpdateRefsClosure(QHolder* q);

  void do_oop_nv(narrowOop* p) {
    Unimplemented();
  }
  void do_oop_nv(oop* p);

  virtual void do_oop(narrowOop* p) { do_oop_nv(p); }
  virtual void do_oop(oop* p) { do_oop_nv(p); }

};

class ShenandoahMarkRefsClosure : public MetadataAwareOopClosure {
  QHolder* _queue;
  ShenandoahHeap* _heap;

public:
  ShenandoahMarkRefsClosure(QHolder* q);

  void do_oop_nv(narrowOop* p) {
    Unimplemented();
  }
  void do_oop_nv(oop* p);

  virtual void do_oop(narrowOop* p) { do_oop_nv(p); }
  virtual void do_oop(oop* p) { do_oop_nv(p); }

};

#endif // SHARE_VM_GC_SHENANDOAH_SHENANDOAHOOPCLOSURES_HPP
