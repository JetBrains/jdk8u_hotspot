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

#include "gc_implementation/shenandoah/shenandoahTaskqueue.hpp"

class ShenandoahHeap;

enum UpdateRefsMode {
  NONE,       // No reference updating
  RESOLVE,    // Only a read-barrier (no reference updating)
  SIMPLE,     // Reference updating using simple store
  CONCURRENT  // Reference updating using CAS
};

class ShenandoahMarkRefsSuperClosure : public MetadataAwareOopClosure {
private:
  SCMObjToScanQueue* _queue;
  ShenandoahHeap* _heap;
public:
  ShenandoahMarkRefsSuperClosure(SCMObjToScanQueue* q, ReferenceProcessor* rp);

  template <class T, UpdateRefsMode UPDATE_MODE>
  void work(T *p);
};

class ShenandoahMarkUpdateRefsClosure : public ShenandoahMarkRefsSuperClosure {
public:
  ShenandoahMarkUpdateRefsClosure(SCMObjToScanQueue* q, ReferenceProcessor* rp) :
          ShenandoahMarkRefsSuperClosure(q, rp) {};

  template <class T>
  inline void do_oop_nv(T* p)       { work<T, CONCURRENT>(p); }
  virtual void do_oop(narrowOop* p) { do_oop_nv(p); }
  virtual void do_oop(oop* p)       { do_oop_nv(p); }
  inline bool do_metadata_nv()      { return false; }
  virtual bool do_metadata()        { return false; }
};

class ShenandoahMarkUpdateRefsMetadataClosure : public ShenandoahMarkRefsSuperClosure {
public:
  ShenandoahMarkUpdateRefsMetadataClosure(SCMObjToScanQueue* q, ReferenceProcessor* rp) :
          ShenandoahMarkRefsSuperClosure(q, rp) {};

  template <class T>
  inline void do_oop_nv(T* p)       { work<T, CONCURRENT>(p); }
  virtual void do_oop(narrowOop* p) { do_oop_nv(p); }
  virtual void do_oop(oop* p)       { do_oop_nv(p); }
  inline bool do_metadata_nv()      { return true; }
  virtual bool do_metadata()        { return true; }
};

class ShenandoahMarkRefsClosure : public ShenandoahMarkRefsSuperClosure {
public:
  ShenandoahMarkRefsClosure(SCMObjToScanQueue* q, ReferenceProcessor* rp) :
    ShenandoahMarkRefsSuperClosure(q, rp) {};

  template <class T>
  inline void do_oop_nv(T* p)       { work<T, NONE>(p); }
  virtual void do_oop(narrowOop* p) { do_oop_nv(p); }
  virtual void do_oop(oop* p)       { do_oop_nv(p); }
  inline bool do_metadata_nv()      { return false; }
  virtual bool do_metadata()        { return false; }
};

class ShenandoahMarkResolveRefsClosure : public ShenandoahMarkRefsSuperClosure {
public:
  ShenandoahMarkResolveRefsClosure(SCMObjToScanQueue* q, ReferenceProcessor* rp) :
    ShenandoahMarkRefsSuperClosure(q, rp) {};

  template <class T>
  inline void do_oop_nv(T* p)       { work<T, RESOLVE>(p); }
  virtual void do_oop(narrowOop* p) { do_oop_nv(p); }
  virtual void do_oop(oop* p)       { do_oop_nv(p); }
  inline bool do_metadata_nv()      { return false; }
  virtual bool do_metadata()        { return false; }
};

class ShenandoahMarkRefsMetadataClosure : public ShenandoahMarkRefsSuperClosure {
public:
  ShenandoahMarkRefsMetadataClosure(SCMObjToScanQueue* q, ReferenceProcessor* rp) :
    ShenandoahMarkRefsSuperClosure(q, rp) {};

  template <class T>
  inline void do_oop_nv(T* p)       { work<T, NONE>(p); }
  virtual void do_oop(narrowOop* p) { do_oop_nv(p); }
  virtual void do_oop(oop* p)       { do_oop_nv(p); }
  inline bool do_metadata_nv()      { return true; }
  virtual bool do_metadata()        { return true; }
};

#endif // SHARE_VM_GC_SHENANDOAH_SHENANDOAHOOPCLOSURES_HPP
