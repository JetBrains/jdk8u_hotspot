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
class ShenandoahStrDedupQueue;
class ShenandoahMarkingContext;

enum UpdateRefsMode {
  NONE,       // No reference updating
  RESOLVE,    // Only a read-barrier (no reference updating)
  SIMPLE,     // Reference updating using simple store
  CONCURRENT  // Reference updating using CAS
};

class ShenandoahMarkRefsSuperClosure : public MetadataAwareOopClosure {
private:
  ShenandoahObjToScanQueue* _queue;
  ShenandoahStrDedupQueue*  _dedup_queue;
  ShenandoahHeap* _heap;
  ShenandoahMarkingContext* const _mark_context;

public:
  ShenandoahMarkRefsSuperClosure(ShenandoahObjToScanQueue* q, ReferenceProcessor* rp);
  ShenandoahMarkRefsSuperClosure(ShenandoahObjToScanQueue* q, ShenandoahStrDedupQueue* dq, ReferenceProcessor* rp);

  template <class T, UpdateRefsMode UPDATE_MODE, bool STRING_DEDUP>
  void work(T *p);
};

class ShenandoahMarkUpdateRefsClosure : public ShenandoahMarkRefsSuperClosure {
public:
  ShenandoahMarkUpdateRefsClosure(ShenandoahObjToScanQueue* q, ReferenceProcessor* rp) :
          ShenandoahMarkRefsSuperClosure(q, rp) {};

  template <class T>
  inline void do_oop_nv(T* p)       { work<T, CONCURRENT, false /* string dedup */>(p); }
  virtual void do_oop(narrowOop* p) { do_oop_nv(p); }
  virtual void do_oop(oop* p)       { do_oop_nv(p); }
  inline bool do_metadata_nv()      { return false; }
  virtual bool do_metadata()        { return false; }
};

class ShenandoahMarkUpdateRefsDedupClosure : public ShenandoahMarkRefsSuperClosure {
public:
  ShenandoahMarkUpdateRefsDedupClosure(ShenandoahObjToScanQueue* q, ShenandoahStrDedupQueue* dq, ReferenceProcessor* rp) :
          ShenandoahMarkRefsSuperClosure(q, dq, rp) {};

  template <class T>
  inline void do_oop_nv(T* p)       { work<T, CONCURRENT, true /* string dedup */>(p); }
  virtual void do_oop(narrowOop* p) { do_oop_nv(p); }
  virtual void do_oop(oop* p)       { do_oop_nv(p); }
  inline bool do_metadata_nv()      { return false; }
  virtual bool do_metadata()        { return false; }
};

class ShenandoahMarkUpdateRefsMetadataClosure : public ShenandoahMarkRefsSuperClosure {
public:
  ShenandoahMarkUpdateRefsMetadataClosure(ShenandoahObjToScanQueue* q, ReferenceProcessor* rp) :
    ShenandoahMarkRefsSuperClosure(q, rp) {};

  template <class T>
  inline void do_oop_nv(T* p)       { work<T, CONCURRENT, false /* string dedup */>(p); }
  virtual void do_oop(narrowOop* p) { do_oop_nv(p); }
  virtual void do_oop(oop* p)       { do_oop_nv(p); }
  inline bool do_metadata_nv()      { return true; }
  virtual bool do_metadata()        { return true; }
};

class ShenandoahMarkUpdateRefsMetadataDedupClosure : public ShenandoahMarkRefsSuperClosure {
public:
  ShenandoahMarkUpdateRefsMetadataDedupClosure(ShenandoahObjToScanQueue* q, ShenandoahStrDedupQueue* dq, ReferenceProcessor* rp) :
  ShenandoahMarkRefsSuperClosure(q, dq, rp) {};

  template <class T>
  inline void do_oop_nv(T* p)       { work<T, CONCURRENT, true /* string dedup */>(p); }
  virtual void do_oop(narrowOop* p) { do_oop_nv(p); }
  virtual void do_oop(oop* p)       { do_oop_nv(p); }
  inline bool do_metadata_nv()      { return true; }
  virtual bool do_metadata()        { return true; }
};

class ShenandoahMarkRefsClosure : public ShenandoahMarkRefsSuperClosure {
public:
  ShenandoahMarkRefsClosure(ShenandoahObjToScanQueue* q, ReferenceProcessor* rp) :
    ShenandoahMarkRefsSuperClosure(q, rp) {};

  template <class T>
  inline void do_oop_nv(T* p)       { work<T, NONE, false /* string dedup */>(p); }
  virtual void do_oop(narrowOop* p) { do_oop_nv(p); }
  virtual void do_oop(oop* p)       { do_oop_nv(p); }
  inline bool do_metadata_nv()      { return false; }
  virtual bool do_metadata()        { return false; }
};

class ShenandoahMarkRefsDedupClosure : public ShenandoahMarkRefsSuperClosure {
public:
  ShenandoahMarkRefsDedupClosure(ShenandoahObjToScanQueue* q, ShenandoahStrDedupQueue* dq, ReferenceProcessor* rp) :
    ShenandoahMarkRefsSuperClosure(q, dq, rp) {};

  template <class T>
  inline void do_oop_nv(T* p)       { work<T, NONE, true /* string dedup */>(p); }
  virtual void do_oop(narrowOop* p) { do_oop_nv(p); }
  virtual void do_oop(oop* p)       { do_oop_nv(p); }
  inline bool do_metadata_nv()      { return false; }
  virtual bool do_metadata()        { return false; }
};

class ShenandoahMarkResolveRefsClosure : public ShenandoahMarkRefsSuperClosure {
public:
  ShenandoahMarkResolveRefsClosure(ShenandoahObjToScanQueue* q, ReferenceProcessor* rp) :
    ShenandoahMarkRefsSuperClosure(q, rp) {};

  template <class T>
  inline void do_oop_nv(T* p)       { work<T, RESOLVE, false /* string dedup */>(p); }
  virtual void do_oop(narrowOop* p) { do_oop_nv(p); }
  virtual void do_oop(oop* p)       { do_oop_nv(p); }
  inline bool do_metadata_nv()      { return false; }
  virtual bool do_metadata()        { return false; }
};

class ShenandoahMarkResolveRefsDedupClosure : public ShenandoahMarkRefsSuperClosure {
public:
  ShenandoahMarkResolveRefsDedupClosure(ShenandoahObjToScanQueue* q, ShenandoahStrDedupQueue* dq, ReferenceProcessor* rp) :
    ShenandoahMarkRefsSuperClosure(q, dq, rp) {};

  template <class T>
  inline void do_oop_nv(T* p)       { work<T, RESOLVE, true /* string dedup */>(p); }
  virtual void do_oop(narrowOop* p) { do_oop_nv(p); }
  virtual void do_oop(oop* p)       { do_oop_nv(p); }
  inline bool do_metadata_nv()      { return false; }
  virtual bool do_metadata()        { return false; }
};

class ShenandoahMarkRefsMetadataClosure : public ShenandoahMarkRefsSuperClosure {
public:
  ShenandoahMarkRefsMetadataClosure(ShenandoahObjToScanQueue* q, ReferenceProcessor* rp) :
    ShenandoahMarkRefsSuperClosure(q, rp) {};

  template <class T>
  inline void do_oop_nv(T* p)       { work<T, NONE, false /* string dedup */>(p); }
  virtual void do_oop(narrowOop* p) { do_oop_nv(p); }
  virtual void do_oop(oop* p)       { do_oop_nv(p); }
  inline bool do_metadata_nv()      { return true; }
  virtual bool do_metadata()        { return true; }
};

class ShenandoahMarkRefsMetadataDedupClosure : public ShenandoahMarkRefsSuperClosure {
public:
  ShenandoahMarkRefsMetadataDedupClosure(ShenandoahObjToScanQueue* q, ShenandoahStrDedupQueue* dq, ReferenceProcessor* rp) :
    ShenandoahMarkRefsSuperClosure(q, dq, rp) {};

  template <class T>
  inline void do_oop_nv(T* p)       { work<T, NONE, true /* string dedup */>(p); }
  virtual void do_oop(narrowOop* p) { do_oop_nv(p); }
  virtual void do_oop(oop* p)       { do_oop_nv(p); }
  inline bool do_metadata_nv()      { return true; }
  virtual bool do_metadata()        { return true; }
};

class ShenandoahUpdateHeapRefsClosure : public ExtendedOopClosure {
private:
  ShenandoahHeap* _heap;
public:
  ShenandoahUpdateHeapRefsClosure();

  template <class T>
  void do_oop_nv(T* p);

  virtual void do_oop(narrowOop* p) { do_oop_nv(p); }
  virtual void do_oop(oop* p)       { do_oop_nv(p); }
};


#endif // SHARE_VM_GC_SHENANDOAH_SHENANDOAHOOPCLOSURES_HPP
