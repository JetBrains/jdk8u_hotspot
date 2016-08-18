/*
 * Copyright (c) 2003, 2011, Oracle and/or its affiliates. All rights reserved.
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
#include "classfile/systemDictionary.hpp"
#include "classfile/verifier.hpp"
#include "code/codeCache.hpp"
#include "interpreter/oopMapCache.hpp"
#include "interpreter/rewriter.hpp"
#include "memory/gcLocker.hpp"
#include "memory/universe.inline.hpp"
#include "memory/metaspaceShared.hpp"
#include "oops/fieldStreams.hpp"
#include "oops/klassVtable.hpp"
#include "prims/jvmtiImpl.hpp"
#include "prims/jvmtiRedefineClasses2.hpp"
#include "prims/methodComparator.hpp"
#include "prims/jvmtiClassFileReconstituter.hpp"
#include "runtime/deoptimization.hpp"
#include "runtime/relocator.hpp"
#include "utilities/bitMap.inline.hpp"
#include "compiler/compileBroker.hpp"
#include "oops/instanceMirrorKlass.hpp"
#include "utilities/pair.hpp"


Array<Method*>* VM_EnhancedRedefineClasses::_old_methods = NULL;
Array<Method*>* VM_EnhancedRedefineClasses::_new_methods = NULL;
int*        VM_EnhancedRedefineClasses::_matching_old_methods = NULL;
int*        VM_EnhancedRedefineClasses::_matching_new_methods = NULL;
int*        VM_EnhancedRedefineClasses::_deleted_methods      = NULL;
int*        VM_EnhancedRedefineClasses::_added_methods        = NULL;
int         VM_EnhancedRedefineClasses::_matching_methods_length = 0;
int         VM_EnhancedRedefineClasses::_deleted_methods_length  = 0;
int         VM_EnhancedRedefineClasses::_added_methods_length    = 0;
GrowableArray<instanceKlassHandle>* VM_EnhancedRedefineClasses::_affected_klasses = NULL;


// Holds the revision number of the current class redefinition
int    VM_EnhancedRedefineClasses::_revision_number = -1;

VM_EnhancedRedefineClasses::VM_EnhancedRedefineClasses(jint class_count, const jvmtiClassDefinition *class_defs, JvmtiClassLoadKind class_load_kind)
   : VM_GC_Operation(Universe::heap()->total_full_collections(), GCCause::_heap_inspection) {
  RC_TIMER_START(_timer_total);
  _class_count = class_count;
  _class_defs = class_defs;
  _class_load_kind = class_load_kind;
  _result = JVMTI_ERROR_NONE;
}

VM_EnhancedRedefineClasses::~VM_EnhancedRedefineClasses() {
  RC_TIMER_STOP(_timer_total);
}

void VM_EnhancedRedefineClasses::add_affected_klasses( Klass* klass )
{
  assert(!_affected_klasses->contains(klass), "must not occur more than once!");
  assert(klass->new_version() == NULL, "Only last version is valid entry in system dictionary");

  if (klass->check_redefinition_flag(Klass::MarkedAsAffected)) {
    _affected_klasses->append(klass);
    return;
  }

  for (juint i = 0; i < klass->super_depth(); i++) {
    Klass* primary = klass->primary_super_of_depth(i);
    // super_depth returns "8" for interfaces, but they don't have primaries other than Object.
    if (primary == NULL) {
      break;
    }
    if (primary->check_redefinition_flag(Klass::MarkedAsAffected)) {
      RC_TRACE(0x00000001, ("Found affected class: %s", klass->name()->as_C_string()));
      klass->set_redefinition_flag(Klass::MarkedAsAffected);
      _affected_klasses->append(klass);
      return;
    }
  }

  // Check secondary supers
  int cnt = klass->secondary_supers()->length();
  for (int i = 0; i < cnt; i++) {
    Klass* secondary = klass->secondary_supers()->at(i);
    if (secondary->check_redefinition_flag(Klass::MarkedAsAffected)) {
      RC_TRACE(0x00000001, ("Found affected class: %s", klass->name()->as_C_string()));
      klass->set_redefinition_flag(Klass::MarkedAsAffected);
      _affected_klasses->append(klass);
      return;
    }
  }
}


// Searches for all affected classes and performs a sorting such that a supertype is always before a subtype.
jvmtiError VM_EnhancedRedefineClasses::find_sorted_affected_classes() {

  assert(_affected_klasses, "");
  for (int i = 0; i < _class_count; i++) {
    oop mirror = JNIHandles::resolve_non_null(_class_defs[i].klass);
    instanceKlassHandle klass_handle(Thread::current(), java_lang_Class::as_Klass(mirror));
    klass_handle->set_redefinition_flag(Klass::MarkedAsAffected);
    assert(klass_handle->new_version() == NULL, "Must be new class");
    RC_TRACE(0x00000001, ("Class being reloaded: %s", klass_handle->name()->as_C_string()));
  }

  // Find classes not directly redefined, but affected by a redefinition (because one of its supertypes is redefined)
  SystemDictionary::classes_do(VM_EnhancedRedefineClasses::add_affected_klasses);
  RC_TRACE(0x00000001, ("%d classes affected", _affected_klasses->length()));

  // Sort the affected klasses such that a supertype is always on a smaller array index than its subtype.
  jvmtiError result = do_topological_class_sorting(_class_defs, _class_count, Thread::current());
  if (RC_TRACE_ENABLED(0x00000001)) {
    RC_TRACE(0x00000001, ("Redefine order: "));
    for (int i = 0; i < _affected_klasses->length(); i++) {
      RC_TRACE(0x00000001, ("%s", _affected_klasses->at(i)->name()->as_C_string()));
    }
  }

  return result;
}

// Searches for the class bytes of the given class and returns them as a byte array.
jvmtiError VM_EnhancedRedefineClasses::find_class_bytes(instanceKlassHandle the_class, const unsigned char **class_bytes, jint *class_byte_count, jboolean *not_changed) {

  *not_changed = false;

  // Search for the index in the redefinition array that corresponds to the current class
  int j;
  for (j=0; j<_class_count; j++) {
    oop mirror = JNIHandles::resolve_non_null(_class_defs[j].klass);
    Klass* the_class_oop = java_lang_Class::as_Klass(mirror);
    if (the_class_oop == the_class()) {
      break;
    }
  }

  if (j == _class_count) {

    *not_changed = true;

    // Redefine with same bytecodes. This is a class that is only indirectly affected by redefinition,
    // so the user did not specify a different bytecode for that class.

    if (the_class->get_cached_class_file_bytes() == NULL) {
      // not cached, we need to reconstitute the class file from VM representation
      constantPoolHandle  constants(Thread::current(), the_class->constants());
      MonitorLockerEx ml(constants->lock());            // lock constant pool while we query it
      //ObjectLocker ol(constants, Thread::current());    // lock constant pool while we query it

      JvmtiClassFileReconstituter reconstituter(the_class);
      if (reconstituter.get_error() != JVMTI_ERROR_NONE) {
        return reconstituter.get_error();
      }

      *class_byte_count = (jint)reconstituter.class_file_size();
      *class_bytes      = (unsigned char*)reconstituter.class_file_bytes();
    } else {

      // it is cached, get it from the cache
      *class_byte_count = the_class->get_cached_class_file_len();
      *class_bytes      = the_class->get_cached_class_file_bytes();
    }

  } else {

    // Redefine with bytecodes at index j
    *class_bytes = _class_defs[j].class_bytes;
    *class_byte_count = _class_defs[j].class_byte_count;
  }

  return JVMTI_ERROR_NONE;
}

// Prologue of the VM operation, called on the Java thread in parallel to normal program execution
bool VM_EnhancedRedefineClasses::doit_prologue() {

  _revision_number++;
  RC_TRACE(0x00000001,
        ("Redefinition with revision number %d started!", _revision_number));

  assert(Thread::current()->is_Java_thread(), "must be Java thread");
  RC_TIMER_START(_timer_prologue);

  if (!check_arguments()) {
    RC_TIMER_STOP(_timer_prologue);
    return false;
  }

  // We first load new class versions in the prologue, because somewhere down the
  // call chain it is required that the current thread is a Java thread.
  _new_classes = new (ResourceObj::C_HEAP, mtInternal) GrowableArray<instanceKlassHandle>(5, true);

  assert(_affected_klasses == NULL, "");
  _affected_klasses = new (ResourceObj::C_HEAP, mtInternal) GrowableArray<instanceKlassHandle>(_class_count, true);

  _result = load_new_class_versions(Thread::current());

  RC_TRACE(0x00000001,
        ("Loaded new class versions!"));
  if (_result != JVMTI_ERROR_NONE) {
    RC_TRACE(0x00000001,
          ("error occured: %d!", _result));
    delete _new_classes;
    _new_classes = NULL;
    delete _affected_klasses;
    _affected_klasses = NULL;
    RC_TIMER_STOP(_timer_prologue);
    return false;
  }

  VM_GC_Operation::doit_prologue();
  RC_TIMER_STOP(_timer_prologue);

  RC_TRACE(0x00000001, ("doit_prologue finished!"));
  return true;
}

// Checks basic properties of the arguments of the redefinition command.
jvmtiError VM_EnhancedRedefineClasses::check_arguments_error() {
  if (_class_defs == NULL) return JVMTI_ERROR_NULL_POINTER;
  for (int i = 0; i < _class_count; i++) {
    if (_class_defs[i].klass == NULL) return JVMTI_ERROR_INVALID_CLASS;
    if (_class_defs[i].class_byte_count == 0) return JVMTI_ERROR_INVALID_CLASS_FORMAT;
    if (_class_defs[i].class_bytes == NULL) return JVMTI_ERROR_NULL_POINTER;
  }
  return JVMTI_ERROR_NONE;
  }

// Returns false and sets an result error code if the redefinition should be aborted.
bool VM_EnhancedRedefineClasses::check_arguments() {
  jvmtiError error = check_arguments_error();
  if (error != JVMTI_ERROR_NONE || _class_count == 0) {
    _result = error;
    return false;
  }
  return true;
}

jvmtiError VM_EnhancedRedefineClasses::check_exception() const {
  Thread* THREAD = Thread::current();
  if (HAS_PENDING_EXCEPTION) {

    Symbol* ex_name = PENDING_EXCEPTION->klass()->name();
    RC_TRACE_WITH_THREAD(0x00000002, THREAD, ("parse_stream exception: '%s'", ex_name->as_C_string()));
    CLEAR_PENDING_EXCEPTION;

    if (ex_name == vmSymbols::java_lang_UnsupportedClassVersionError()) {
      return JVMTI_ERROR_UNSUPPORTED_VERSION;
    } else if (ex_name == vmSymbols::java_lang_ClassFormatError()) {
      return JVMTI_ERROR_INVALID_CLASS_FORMAT;
    } else if (ex_name == vmSymbols::java_lang_ClassCircularityError()) {
      return JVMTI_ERROR_CIRCULAR_CLASS_DEFINITION;
    } else if (ex_name == vmSymbols::java_lang_NoClassDefFoundError()) {
      // The message will be "XXX (wrong name: YYY)"
      return JVMTI_ERROR_NAMES_DONT_MATCH;
    } else if (ex_name == vmSymbols::java_lang_OutOfMemoryError()) {
      return JVMTI_ERROR_OUT_OF_MEMORY;
    } else {
      // Just in case more exceptions can be thrown..
      return JVMTI_ERROR_FAILS_VERIFICATION;
    }
  }

  return JVMTI_ERROR_NONE;
}

// Loads all new class versions and stores the InstanceKlass handles in an array.
jvmtiError VM_EnhancedRedefineClasses::load_new_class_versions(TRAPS) {

  ResourceMark rm(THREAD);

  RC_TRACE(0x00000001,
        ("loading new class versions (%d)", _class_count));

  // Retrieve an array of all classes that need to be redefined
  jvmtiError err = find_sorted_affected_classes();
  if (err != JVMTI_ERROR_NONE) {
    RC_TRACE(0x00000001,
          ("Error finding sorted affected classes: %d", (int)err));
    return err;
  }


  JvmtiThreadState *state = JvmtiThreadState::state_for(JavaThread::current());

  _max_redefinition_flags = Klass::NoRedefinition;
  jvmtiError result = JVMTI_ERROR_NONE;

  for (int i = 0; i < _affected_klasses->length(); i++) {
    instanceKlassHandle the_class = _affected_klasses->at(i);

    RC_TRACE(0x00000001,
            ("Processing affected class %s (%d of %d)",
                the_class->name()->as_C_string(),
                i + 1,
                _affected_klasses->length()));

    the_class->link_class(THREAD);
    result = check_exception();
    if (result != JVMTI_ERROR_NONE) break;

    // Find new class bytes
    const unsigned char* class_bytes;
    jint class_byte_count;
    jvmtiError error;
    jboolean not_changed;
    if ((error = find_class_bytes(the_class, &class_bytes, &class_byte_count, &not_changed)) != JVMTI_ERROR_NONE) {
      RC_TRACE_WITH_THREAD(0x00000002, THREAD,
            ("Error finding class bytes: %d", (int)error));
      result = error;
      break;
    }
    assert(class_bytes != NULL && class_byte_count != 0, "Class bytes defined at this point!");


    // Set redefined class handle in JvmtiThreadState class.
    // This redefined class is sent to agent event handler for class file
    // load hook event.
    state->set_class_being_redefined(&the_class, _class_load_kind);

    RC_TIMER_STOP(_timer_prologue);
    RC_TIMER_START(_timer_class_loading);

    // Parse the stream.
    Handle the_class_loader(THREAD, the_class->class_loader());
    Handle protection_domain(THREAD, the_class->protection_domain());
    ClassFileStream st((u1*) class_bytes, class_byte_count, (char *)"__VM_EhnancedRedefineClasses__");

    Klass* klass =
        SystemDictionary::resolve_from_stream(
            the_class->name(),
            the_class_loader,
            protection_domain,
            &st,
            true,
            the_class,
            THREAD);
    instanceKlassHandle new_class(THREAD, klass);

    RC_TIMER_STOP(_timer_class_loading);
    RC_TIMER_START(_timer_prologue);

    // Clear class_being_redefined just to be sure.
    state->clear_class_being_redefined();

    result = check_exception();
    if (result != JVMTI_ERROR_NONE) break;

    not_changed = false;

#ifdef ASSERT

    assert(new_class() != NULL, "Class could not be loaded!");
    assert(new_class() != the_class(), "must be different");
    assert(new_class->new_version() == NULL && new_class->old_version() != NULL, "");


    Array<Klass*>* k_interfaces = new_class->local_interfaces();
    for (int j = 0; j < k_interfaces->length(); j++) {
      assert(k_interfaces->at(j)->is_newest_version(), "just checking");
    }

    if (!THREAD->is_Compiler_thread()) {
      RC_TRACE(0x00000001, ("name=%s loader="INTPTR_FORMAT" protection_domain="INTPTR_FORMAT,
                                               the_class->name()->as_C_string(),
                                               (intptr_t) (oopDesc*) the_class->class_loader(),
                                               (intptr_t) (oopDesc*) the_class->protection_domain()));
      // If we are on the compiler thread, we must not try to resolve a class.
      Klass* systemLookup = SystemDictionary::resolve_or_null(the_class->name(), the_class->class_loader(), the_class->protection_domain(), THREAD);

      if (systemLookup != NULL) {
        assert(systemLookup == new_class->old_version(), "Old class must be in system dictionary!");
        Klass *subklass = new_class()->subklass();
        while (subklass != NULL) {
          assert(subklass->new_version() == NULL, "Most recent version of class!");
          subklass = subklass->next_sibling();
        }
      } else {
        // This can happen for reflection generated classes.. ?
        CLEAR_PENDING_EXCEPTION;
      }
    }

#endif

    if (RC_TRACE_ENABLED(0x00000001)) {
      if (new_class->layout_helper() != the_class->layout_helper()) {
        RC_TRACE(0x00000001,
              ("Instance size change for class %s: new=%d old=%d",
                  new_class->name()->as_C_string(),
                  new_class->layout_helper(),
                  the_class->layout_helper()));
      }
    }

    // Set the new version of the class
    new_class->set_revision_number(_revision_number);
    new_class->set_redefinition_index(i);
    the_class->set_new_version(new_class());
    _new_classes->append(new_class);

    assert(new_class->new_version() == NULL, "");

    int redefinition_flags = Klass::NoRedefinition;

    if (not_changed) {
      redefinition_flags = Klass::NoRedefinition;
    } else {
      redefinition_flags = calculate_redefinition_flags(new_class);
      if (redefinition_flags >= Klass::RemoveSuperType) {
        result = JVMTI_ERROR_UNSUPPORTED_REDEFINITION_HIERARCHY_CHANGED;
        break;
      }
    }

    if (new_class->super() != NULL) {
      redefinition_flags = redefinition_flags | new_class->super()->redefinition_flags();
    }

    for (int j = 0; j<new_class->local_interfaces()->length(); j++) {
      redefinition_flags = redefinition_flags | (new_class->local_interfaces()->at(j))->redefinition_flags();
    }

    new_class->set_redefinition_flags(redefinition_flags);

    new_class->set_deoptimization_incl(true);

    _max_redefinition_flags = _max_redefinition_flags | redefinition_flags;

    if ((redefinition_flags & Klass::ModifyInstances) != 0) {
      // TODO: Check if watch access flags of static fields are updated correctly.
      calculate_instance_update_information(_new_classes->at(i)());
    } else {
      // Fields were not changed, transfer special flags only
      assert(new_class->layout_helper() >> 1 == new_class->old_version()->layout_helper() >> 1, "must be equal");
      assert(new_class->fields()->length() == InstanceKlass::cast(new_class->old_version())->fields()->length(), "must be equal");
      
      JavaFieldStream old_fs(the_class);
      JavaFieldStream new_fs(new_class);
      for (; !old_fs.done() && !new_fs.done(); old_fs.next(), new_fs.next()) {
        AccessFlags flags = new_fs.access_flags();
        flags.set_is_field_modification_watched(old_fs.access_flags().is_field_modification_watched());
        flags.set_is_field_access_watched(old_fs.access_flags().is_field_access_watched());
        new_fs.set_access_flags(flags);
      }
    }

    if (RC_TRACE_ENABLED(0x00000001)) {
      RC_TRACE(0x00000001,
            ("Super class is %s", new_class->super()->name()->as_C_string()));
    }

#ifdef ASSERT
    assert(new_class->super() == NULL || new_class->super()->new_version() == NULL, "Super klass must be newest version!");

    the_class->vtable()->verify(tty);
    new_class->vtable()->verify(tty);
#endif

    if (i == _affected_klasses->length() - 1) {
      // This was the last class processed => check if additional classes have been loaded in the meantime
      for (int j = 0; j<_affected_klasses->length(); j++) {

        Klass* initial_klass = _affected_klasses->at(j)();
        Klass *initial_subklass = initial_klass->subklass();
        Klass *cur_klass = initial_subklass;
        while(cur_klass != NULL) {

          if(cur_klass->oop_is_instance() && cur_klass->is_newest_version() && !cur_klass->is_redefining()) {
            instanceKlassHandle handle(THREAD, cur_klass);
            if (!_affected_klasses->contains(handle)) {

              int k = i + 1;
              for (; k<_affected_klasses->length(); k++) {
                if (_affected_klasses->at(k)->is_subtype_of(cur_klass)) {
                  break;
                }
              }
              _affected_klasses->insert_before(k, handle);
              RC_TRACE(0x00000001,
                    ("Adding newly loaded class to affected classes: %s", cur_klass->name()->as_C_string()));
            }
      }

          cur_klass = cur_klass->next_sibling();
        }
      }

      int new_count = _affected_klasses->length() - 1 - i;
      if (new_count != 0) {
        RC_TRACE(0x00000001,
              ("Found new number of affected classes: %d", new_count));
      }
    }
  }

  if (result != JVMTI_ERROR_NONE) {
    rollback();
    return result;
  }

  RC_TIMER_STOP(_timer_prologue);
  RC_TIMER_START(_timer_class_linking);
  // Link and verify new classes _after_ all classes have been updated in the system dictionary!
  for (int i=0; i<_affected_klasses->length(); i++) {
    instanceKlassHandle the_class = _affected_klasses->at(i);
    instanceKlassHandle new_class(the_class->new_version());

    RC_TRACE(0x00000001,
          ("Linking class %d/%d %s", i, _affected_klasses->length(), the_class->name()->as_C_string()));
    new_class->link_class(THREAD);

    result = check_exception();
    if (result != JVMTI_ERROR_NONE) break;
  }
  RC_TIMER_STOP(_timer_class_linking);
  RC_TIMER_START(_timer_prologue);

  if (result != JVMTI_ERROR_NONE) {
    rollback();
    return result;
  }

  RC_TRACE(0x00000001, ("All classes loaded!"));

#ifdef ASSERT
  for (int i=0; i<_affected_klasses->length(); i++) {
    instanceKlassHandle the_class = _affected_klasses->at(i);
    assert(the_class->new_version() != NULL, "Must have been redefined");
    instanceKlassHandle new_version = instanceKlassHandle(THREAD, the_class->new_version());
    assert(new_version->new_version() == NULL, "Must be newest version");

    if (!(new_version->super() == NULL || new_version->super()->new_version() == NULL)) {
      new_version()->print();
      new_version->super()->print();
    }
    assert(new_version->super() == NULL || new_version->super()->new_version() == NULL, "Super class must be newest version");
  }

  SystemDictionary::classes_do(check_class, THREAD);

#endif

  RC_TRACE(0x00000001, ("Finished verification!"));
  return JVMTI_ERROR_NONE;
}

int VM_EnhancedRedefineClasses::calculate_redefinition_flags(instanceKlassHandle new_class) {

  int result = Klass::NoRedefinition;
  RC_TRACE(0x00000001,
        ("Comparing different class versions of class %s", new_class->name()->as_C_string()));

  assert(new_class->old_version() != NULL, "must have old version");
  instanceKlassHandle the_class(new_class->old_version());

  // Check whether class is in the error init state.
  if (the_class->is_in_error_state()) {
    // TBD #5057930: special error code is needed in 1.6
    //result = Klass::union_redefinition_level(result, Klass::Invalid);
  }

  int i;

  //////////////////////////////////////////////////////////////////////////////////////////////////////////
  // Check superclasses
  assert(new_class->super() == NULL || new_class->super()->is_newest_version(), "");
  if (the_class->super() != new_class->super()) {
    // Super class changed
    Klass* cur_klass = the_class->super();
    while (cur_klass != NULL) {
      if (!new_class->is_subclass_of(cur_klass->newest_version())) {
        RC_TRACE(0x00000001,
              ("Removed super class %s", cur_klass->name()->as_C_string()));
        result = result | Klass::RemoveSuperType | Klass::ModifyInstances | Klass::ModifyClass;

        if (!cur_klass->has_subtype_changed()) {
          RC_TRACE(0x00000001,
                ("Subtype changed of class %s", cur_klass->name()->as_C_string()));
          cur_klass->set_subtype_changed(true);
        }
  }

      cur_klass = cur_klass->super();
  }

    cur_klass = new_class->super();
    while (cur_klass != NULL) {
      if (!the_class->is_subclass_of(cur_klass->old_version())) {
        RC_TRACE(0x00000001,
              ("Added super class %s", cur_klass->name()->as_C_string()));
        result = result | Klass::ModifyClass | Klass::ModifyInstances;
      }
      cur_klass = cur_klass->super();
    }
  }

  //////////////////////////////////////////////////////////////////////////////////////////////////////////
  // Check interfaces

  // Interfaces removed?
  Array<Klass*>* old_interfaces = the_class->transitive_interfaces();
  for (i = 0; i<old_interfaces->length(); i++) {
    instanceKlassHandle old_interface(old_interfaces->at(i));
    if (!new_class->implements_interface_any_version(old_interface())) {
      result = result | Klass::RemoveSuperType | Klass::ModifyClass;
      RC_TRACE(0x00000001,
            ("Removed interface %s", old_interface->name()->as_C_string()));

      if (!old_interface->has_subtype_changed()) {
        RC_TRACE(0x00000001,
              ("Subtype changed of interface %s", old_interface->name()->as_C_string()));
        old_interface->set_subtype_changed(true);
      }
    }
  }

  // Interfaces added?
  Array<Klass*>* new_interfaces = new_class->transitive_interfaces();
  for (i = 0; i<new_interfaces->length(); i++) {
    if (!the_class->implements_interface_any_version(new_interfaces->at(i))) {
      result = result | Klass::ModifyClass;
      RC_TRACE(0x00000001,
            ("Added interface %s", new_interfaces->at(i)->name()->as_C_string()));
    }
  }


  // Check whether class modifiers are the same.
  jushort old_flags = (jushort) the_class->access_flags().get_flags();
  jushort new_flags = (jushort) new_class->access_flags().get_flags();
  if (old_flags != new_flags) {
    // TODO Can this have any effects?
  }

  // Check if the number, names, types and order of fields declared in these classes
  // are the same.
  JavaFieldStream old_fs(the_class);
  JavaFieldStream new_fs(new_class);
  for (; !old_fs.done() && !new_fs.done(); old_fs.next(), new_fs.next()) {
    // access
    old_flags = old_fs.access_flags().as_short();
    new_flags = new_fs.access_flags().as_short();
    if ((old_flags ^ new_flags) & JVM_RECOGNIZED_FIELD_MODIFIERS) {
      // TODO can this have any effect?
    }
    // offset
    if (old_fs.offset() != new_fs.offset()) {
      result = result | Klass::ModifyInstances;
    }
    // name and signature
    Symbol* name_sym1 = the_class->constants()->symbol_at(old_fs.name_index());
    Symbol* sig_sym1 = the_class->constants()->symbol_at(old_fs.signature_index());
    Symbol* name_sym2 = new_class->constants()->symbol_at(new_fs.name_index());
    Symbol* sig_sym2 = new_class->constants()->symbol_at(new_fs.signature_index());
    if (name_sym1 != name_sym2 || sig_sym1 != sig_sym2) {
      result = result | Klass::ModifyInstances;
    }
  }

  // If both streams aren't done then we have a differing number of
  // fields.
  if (!old_fs.done() || !new_fs.done()) {
      result = result | Klass::ModifyInstances;
    }

  // Do a parallel walk through the old and new methods. Detect
  // cases where they match (exist in both), have been added in
  // the new methods, or have been deleted (exist only in the
  // old methods).  The class file parser places methods in order
  // by method name, but does not order overloaded methods by
  // signature.  In order to determine what fate befell the methods,
  // this code places the overloaded new methods that have matching
  // old methods in the same order as the old methods and places
  // new overloaded methods at the end of overloaded methods of
  // that name. The code for this order normalization is adapted
  // from the algorithm used in InstanceKlass::find_method().
  // Since we are swapping out of order entries as we find them,
  // we only have to search forward through the overloaded methods.
  // Methods which are added and have the same name as an existing
  // method (but different signature) will be put at the end of
  // the methods with that name, and the name mismatch code will
  // handle them.
  Array<Method*>* k_old_methods(the_class->methods()); // FIXME-isd: handles???
  Array<Method*>* k_new_methods(new_class->methods());
  int n_old_methods = k_old_methods->length();
  int n_new_methods = k_new_methods->length();

  int ni = 0;
  int oi = 0;
  while (true) {
    Method* k_old_method;
    Method* k_new_method;
    enum { matched, added, deleted, undetermined } method_was = undetermined;

    if (oi >= n_old_methods) {
      if (ni >= n_new_methods) {
        break; // we've looked at everything, done
      }
      // New method at the end
      k_new_method = k_new_methods->at(ni);
      method_was = added;
    } else if (ni >= n_new_methods) {
      // Old method, at the end, is deleted
      k_old_method = k_old_methods->at(oi);
      method_was = deleted;
    } else {
      // There are more methods in both the old and new lists
      k_old_method = k_old_methods->at(oi);
      k_new_method = k_new_methods->at(ni);
      if (k_old_method->name() != k_new_method->name()) {
        // Methods are sorted by method name, so a mismatch means added
        // or deleted
        if (k_old_method->name()->fast_compare(k_new_method->name()) > 0) {
          method_was = added;
        } else {
          method_was = deleted;
        }
      } else if (k_old_method->signature() == k_new_method->signature()) {
        // Both the name and signature match
        method_was = matched;
        } else {
          // The name matches, but the signature doesn't, which means we have to
          // search forward through the new overloaded methods.
          int nj;  // outside the loop for post-loop check
          for (nj = ni + 1; nj < n_new_methods; nj++) {
            Method* m = k_new_methods->at(nj);
            if (k_old_method->name() != m->name()) {
              // reached another method name so no more overloaded methods
              method_was = deleted;
              break;
          }
          if (k_old_method->signature() == m->signature()) {
            // found a match so swap the methods
            k_new_methods->at_put(ni, m);
            k_new_methods->at_put(nj, k_new_method);
            k_new_method = m;
            method_was = matched;
            break;
          }
        }

        if (nj >= n_new_methods) {
          // reached the end without a match; so method was deleted
          method_was = deleted;
        }
      }
    }

    switch (method_was) {
  case matched:
    // methods match, be sure modifiers do too
    old_flags = (jushort) k_old_method->access_flags().get_flags();
    new_flags = (jushort) k_new_method->access_flags().get_flags();
    if ((old_flags ^ new_flags) & ~(JVM_ACC_NATIVE)) {
      // TODO Can this have any effects? Probably yes on vtables?
      result = result | Klass::ModifyClass;
    }
    {
      u2 new_num = k_new_method->method_idnum();
      u2 old_num = k_old_method->method_idnum();
      if (new_num != old_num) {
        Method* idnum_owner = new_class->method_with_idnum(old_num);
        if (idnum_owner != NULL) {
          // There is already a method assigned this idnum -- switch them
          idnum_owner->set_method_idnum(new_num);
        }
        k_new_method->set_method_idnum(old_num);
        RC_TRACE(0x00008000,
            ("swapping idnum of new and old method %d / %d!", new_num, old_num));
      }
    }
    RC_TRACE(0x00008000, ("Method matched: new: %s [%d] == old: %s [%d]",
                         k_new_method->name_and_sig_as_C_string(), ni,
                         k_old_method->name_and_sig_as_C_string(), oi));
    // advance to next pair of methods
    ++oi;
    ++ni;
    break;
  case added:
    // method added, see if it is OK
    new_flags = (jushort) k_new_method->access_flags().get_flags();
    if ((new_flags & JVM_ACC_PRIVATE) == 0
      // hack: private should be treated as final, but alas
      || (new_flags & (JVM_ACC_FINAL|JVM_ACC_STATIC)) == 0) {
        // new methods must be private
        result = result | Klass::ModifyClass;
    }
    {
      u2 num = new_class->next_method_idnum();
      if (num == ConstMethod::UNSET_IDNUM) {
        // cannot add any more methods
        result = result | Klass::ModifyClass;
      }
      u2 new_num = k_new_method->method_idnum();
      Method* idnum_owner = new_class->method_with_idnum(num);
      if (idnum_owner != NULL) {
        // There is already a method assigned this idnum -- switch them
        idnum_owner->set_method_idnum(new_num);
      }
      k_new_method->set_method_idnum(num);
    }
    RC_TRACE(0x00008000, ("Method added: new: %s [%d], idnum %d",
                         k_new_method->name_and_sig_as_C_string(), ni, k_new_method->method_idnum()));
    ++ni; // advance to next new method
    break;
  case deleted:
    // method deleted, see if it is OK
    old_flags = (jushort) k_old_method->access_flags().get_flags();
    if ((old_flags & JVM_ACC_PRIVATE) == 0
      // hack: private should be treated as final, but alas
      || (old_flags & (JVM_ACC_FINAL|JVM_ACC_STATIC)) == 0
      ) {
        // deleted methods must be private
        result = result | Klass::ModifyClass;
    }
    RC_TRACE(0x00008000, ("Method deleted: old: %s [%d]",
                          k_old_method->name_and_sig_as_C_string(), oi));
    ++oi; // advance to next old method
    break;
  default:
    ShouldNotReachHere();
    }
  }

  if (new_class()->size() != new_class->old_version()->size()) {
    result |= Klass::ModifyClassSize;
  }

  if (new_class->size_helper() != ((InstanceKlass*)(new_class->old_version()))->size_helper()) {
    result |= Klass::ModifyInstanceSize;
  }

  // TODO Check method bodies to be able to return NoChange?
  return result;
}

void VM_EnhancedRedefineClasses::calculate_instance_update_information(Klass* new_version) {

  class CalculateFieldUpdates : public FieldClosure {

  private:
    InstanceKlass* _old_ik;
    GrowableArray<int> _update_info;
    int _position;
    bool _copy_backwards;

  public:

    bool does_copy_backwards() {
      return _copy_backwards;
  }

    CalculateFieldUpdates(InstanceKlass* old_ik) :
        _old_ik(old_ik), _position(instanceOopDesc::base_offset_in_bytes()), _copy_backwards(false) {
      _update_info.append(_position);
      _update_info.append(0);
  }

    GrowableArray<int> &finish() {
      _update_info.append(0);
      return _update_info;
  }

    void do_field(fieldDescriptor* fd) {
      int alignment = fd->offset() - _position;
      if (alignment > 0) {
        // This field was aligned, so we need to make sure that we fill the gap
        fill(alignment);
      }

      assert(_position == fd->offset(), "must be correct offset!");

      fieldDescriptor old_fd;
      if (_old_ik->find_field(fd->name(), fd->signature(), false, &old_fd) != NULL) {
        // Found field in the old class, copy
        copy(old_fd.offset(), type2aelembytes(fd->field_type()));

        if (old_fd.offset() < fd->offset()) {
          _copy_backwards = true;
        }

        // Transfer special flags
        fd->set_is_field_modification_watched(old_fd.is_field_modification_watched());
        fd->set_is_field_access_watched(old_fd.is_field_access_watched());
      } else {
        // New field, fill
        fill(type2aelembytes(fd->field_type()));
      }
  }

  private:

    void fill(int size) {
      if (_update_info.length() > 0 && _update_info.at(_update_info.length() - 1) < 0) {
        (*_update_info.adr_at(_update_info.length() - 1)) -= size;
      } else {
        _update_info.append(-size);
      }
      _position += size;
    }

    void copy(int offset, int size) {
      int prev_end = -1;
      if (_update_info.length() > 0 && _update_info.at(_update_info.length() - 1) > 0) {
        prev_end = _update_info.at(_update_info.length() - 2) + _update_info.at(_update_info.length() - 1);
      }

      if (prev_end == offset) {
        (*_update_info.adr_at(_update_info.length() - 2)) += size;
      } else {
        _update_info.append(size);
        _update_info.append(offset);
      }

      _position += size;
    }
  };

  InstanceKlass* ik = InstanceKlass::cast(new_version);
  InstanceKlass* old_ik = InstanceKlass::cast(new_version->old_version());
  CalculateFieldUpdates cl(old_ik);
  ik->do_nonstatic_fields(&cl);

  GrowableArray<int> result = cl.finish();
  ik->store_update_information(result);
  ik->set_copying_backwards(cl.does_copy_backwards());


  if (RC_TRACE_ENABLED(0x00000001)) {
    RC_TRACE(0x00000001, ("Instance update information for %s:", new_version->name()->as_C_string()));
    if (cl.does_copy_backwards()) {
      RC_TRACE(0x00000001, ("\tDoes copy backwards!"));
    }
    for (int i=0; i<result.length(); i++) {
      int curNum = result.at(i);
      if (curNum < 0) {
        RC_TRACE(0x00000001, ("\t%d CLEAN", curNum));
      } else if (curNum > 0) {
        RC_TRACE(0x00000001, ("\t%d COPY from %d", curNum, result.at(i + 1)));
        i++;
      } else {
        RC_TRACE(0x00000001, ("\tEND"));
      }
    }
  }
}

void VM_EnhancedRedefineClasses::rollback() {
  RC_TRACE(0x00000001, ("Rolling back redefinition!"));
  SystemDictionary::rollback_redefinition();

  for (int i=0; i<_new_classes->length(); i++) {
    SystemDictionary::remove_from_hierarchy(_new_classes->at(i));
  }

  for (int i=0; i<_new_classes->length(); i++) {
    instanceKlassHandle new_class = _new_classes->at(i);
    new_class->set_redefining(false);
    new_class->old_version()->set_new_version(NULL);
    new_class->set_old_version(NULL);
  }

}

void VM_EnhancedRedefineClasses::swap_marks(oop first, oop second) {
  markOop first_mark = first->mark();
  markOop second_mark = second->mark();
  first->set_mark(second_mark);
  second->set_mark(first_mark);
}

class FieldCopier : public FieldClosure {
  public:
  void do_field(fieldDescriptor* fd) {
    InstanceKlass* cur = InstanceKlass::cast(fd->field_holder());
    oop cur_oop = cur->java_mirror();

    InstanceKlass* old = InstanceKlass::cast(cur->old_version());
    oop old_oop = old->java_mirror();

    fieldDescriptor result;
    bool found = old->find_local_field(fd->name(), fd->signature(), &result);
    if (found && result.is_static()) {
      RC_TRACE(0x00000001, ("Copying static field value for field %s old_offset=%d new_offset=%d",
                                               fd->name()->as_C_string(), result.offset(), fd->offset()));
      memcpy(cur_oop->obj_field_addr<HeapWord>(fd->offset()),
             old_oop->obj_field_addr<HeapWord>(result.offset()),
             type2aelembytes(fd->field_type()));

      // Static fields may have references to java.lang.Class
      if (fd->field_type() == T_OBJECT) {
         oop oop = cur_oop->obj_field(fd->offset());
         if (oop != NULL && oop->is_instanceMirror()) {
            Klass* klass = java_lang_Class::as_Klass(oop);
            if (klass != NULL && klass->oop_is_instance()) {
              assert(oop == InstanceKlass::cast(klass)->java_mirror(), "just checking");
              if (klass->new_version() != NULL) {
                oop = InstanceKlass::cast(klass->new_version())->java_mirror();
                cur_oop->obj_field_put(fd->offset(), oop);
              }
            }
         }
        }
      }
    }
};

void VM_EnhancedRedefineClasses::mark_as_scavengable(nmethod* nm) {
  if (!nm->on_scavenge_root_list()) {
    CodeCache::add_scavenge_root_nmethod(nm);
  }
}

struct StoreBarrier {
  template <class T> static void oop_store(T* p, oop v) { ::oop_store(p, v); }
};

struct StoreNoBarrier {
  template <class T> static void oop_store(T* p, oop v) { oopDesc::encode_store_heap_oop_not_null(p, v); }
};

template <class S>
class ChangePointersOopClosure : public ExtendedOopClosure {
  // import java_lang_invoke_MemberName.*
  enum {
    REFERENCE_KIND_SHIFT = java_lang_invoke_MemberName::MN_REFERENCE_KIND_SHIFT,
    REFERENCE_KIND_MASK  = java_lang_invoke_MemberName::MN_REFERENCE_KIND_MASK,
  };


  bool update_member_name(oop obj) {
    int flags    =       java_lang_invoke_MemberName::flags(obj);
    int ref_kind =       (flags >> REFERENCE_KIND_SHIFT) & REFERENCE_KIND_MASK;
    if (MethodHandles::ref_kind_is_method(ref_kind)) {
      Method* m = (Method*) java_lang_invoke_MemberName::vmtarget(obj);
      if (m != NULL && !m->method_holder()->is_newest_version()) {
        // Let's try to re-resolve method
        InstanceKlass* newest = InstanceKlass::cast(m->method_holder()->newest_version());
        Method* new_method = newest->find_method(m->name(), m->signature());

        if (new_method != NULL) {
          // Note: we might set NULL at this point, which should force AbstractMethodError at runtime
          CallInfo info(new_method, newest);
          MethodHandles::init_method_MemberName(obj, info, true);
        } else {
          java_lang_invoke_MemberName::set_vmtarget(obj, NULL);
        }
      }
    } else if (MethodHandles::ref_kind_is_field(ref_kind)) {
      Klass* k = (Klass*) java_lang_invoke_MemberName::vmtarget(obj);
      if (k == NULL) {
        return false; // Was cleared before, this MemberName is invalid.
      }

      if (k != NULL && !k->is_newest_version()) {
        // Let's try to re-resolve field
        fieldDescriptor fd;
        int offset = java_lang_invoke_MemberName::vmindex(obj);
        bool is_static = MethodHandles::ref_kind_is_static(ref_kind);
        InstanceKlass* ik = InstanceKlass::cast(k);
        if (ik->find_local_field_from_offset(offset, is_static, &fd)) {
          InstanceKlass* newest = InstanceKlass::cast(k->newest_version());
          fieldDescriptor fd_new;
          if (newest->find_local_field(fd.name(), fd.signature(), &fd_new)) {
            MethodHandles::init_field_MemberName(obj, fd_new, MethodHandles::ref_kind_is_setter(ref_kind));
          } else {
            // Matching field is not found in new version, not much we can do here.
            // JVM will crash once faulty MH is invoked.
            // However, to avoid that all DMH's using this faulty MH are cleared (set to NULL)
            // Eventually, we probably want to replace them with something more meaningful,
            // like instance throwing NoSuchFieldError or DMH that will resort to dynamic
            // field resolution (with possibility of type conversion)
            java_lang_invoke_MemberName::set_vmtarget(obj, NULL);
            java_lang_invoke_MemberName::set_vmindex(obj, 0);
            return false;
          }
        }
      }
    }
    return true;
  }

  bool update_direct_method_handle(oop obj) {
    // Always update member name first.
    oop mem_name = java_lang_invoke_DirectMethodHandle::member(obj);
    if (!update_member_name(mem_name)) {
      return false;
    }

    // Here we rely on DirectMethodHandle implementation.
    // The current implementation caches field offset in $StaticAccessor/$Accessor
    int flags    =       java_lang_invoke_MemberName::flags(mem_name);
    int ref_kind =       (flags >> REFERENCE_KIND_SHIFT) & REFERENCE_KIND_MASK;
    if (MethodHandles::ref_kind_is_field(ref_kind)) {
      // Note: we don't care about staticBase field (which is java.lang.Class)
      // It should be processed during normal object update.
      // Update offset in StaticAccessor
      int offset = java_lang_invoke_MemberName::vmindex(mem_name);
      if (offset != 0) { // index of 0 means that field no longer exist
        if (java_lang_invoke_DirectMethodHandle_StaticAccessor::is_instance(obj)) {
          java_lang_invoke_DirectMethodHandle_StaticAccessor::set_static_offset(obj, offset);
        } else if (java_lang_invoke_DirectMethodHandle_Accessor::is_instance(obj)) {
          java_lang_invoke_DirectMethodHandle_Accessor::set_field_offset(obj, offset);
        }
      }
    }
    return true;
  }

  // Forward pointers to InstanceKlass and mirror class to new versions
  template <class T>
  inline void do_oop_work(T* p) {
    oop obj = oopDesc::load_decode_heap_oop(p);
    if (obj == NULL) {
      return;
    }
    if (obj->is_instanceMirror()) {
      Klass* klass = java_lang_Class::as_Klass(obj);
      if (klass != NULL && klass->oop_is_instance()) {
        assert(obj == InstanceKlass::cast(klass)->java_mirror(), "just checking");
        if (klass->new_version() != NULL) {
          obj = InstanceKlass::cast(klass->new_version())->java_mirror();
          S::oop_store(p, obj);
        }
      }
    }

    // JSR 292 support, uptade java.lang.invoke.MemberName instances
    if (java_lang_invoke_MemberName::is_instance(obj)) {
      update_member_name(obj);
    } else if (java_lang_invoke_DirectMethodHandle::is_instance(obj)) {
      if (!update_direct_method_handle(obj)) {
        // DMH is no longer valid, replace it with null reference.
        // See note above. We probably want to replace this with something more meaningful.
        S::oop_store(p, NULL);
      }
    }
  }

  virtual void do_oop(oop* o) {
    do_oop_work(o);
  }

  virtual void do_oop(narrowOop* o) {
    do_oop_work(o);
  }
};

class ChangePointersObjectClosure : public ObjectClosure {
  private:

  OopClosure *_closure;
  bool _needs_instance_update;
  oop _tmp_obj;
  int _tmp_obj_size;

public:
  ChangePointersObjectClosure(OopClosure *closure) : _closure(closure), _needs_instance_update(false), _tmp_obj(NULL), _tmp_obj_size(0) {}

  bool needs_instance_update() {
    return _needs_instance_update;
  }

  void copy_to_tmp(oop o) {
    int size = o->size();
    if (_tmp_obj_size < size) {
      _tmp_obj_size = size;
      _tmp_obj = (oop)resource_allocate_bytes(size * HeapWordSize);
    }
    Copy::aligned_disjoint_words((HeapWord*)o, (HeapWord*)_tmp_obj, size);
  }

  virtual void do_object(oop obj) {
    // FIXME: if (obj->is_instanceKlass()) return;
    if (obj->is_instanceMirror()) {
      // static fields may have references to old java.lang.Class instances, update them
      // at the same time, we don't want to update other oops in the java.lang.Class
      // Causes SIGSEGV?
      //instanceMirrorKlass::oop_fields_iterate(obj, _closure);
    } else {
      obj->oop_iterate_no_header(_closure);
    }

    if (obj->klass()->new_version() != NULL) {
      Klass* new_klass = obj->klass()->new_version();
      /* FIXME: if (obj->is_perm()) {
        _needs_instance_update = true;
      } else */if(new_klass->update_information() != NULL) {
        int size_diff = obj->size() - obj->size_given_klass(new_klass);

        // Either new size is bigger or gap is to small to be filled
        if (size_diff < 0 || (size_diff > 0 && (size_t) size_diff < CollectedHeap::min_fill_size())) {
          // We need an instance update => set back to old klass
          _needs_instance_update = true;
        } else {
          oop src = obj;
          if (new_klass->is_copying_backwards()) {
            copy_to_tmp(obj);
            src = _tmp_obj;
          }
          src->set_klass(obj->klass()->new_version());
          MarkSweep::update_fields(obj, src, new_klass->update_information());

          if (size_diff > 0) {
            HeapWord* dead_space = ((HeapWord *)obj) + obj->size();
            CollectedHeap::fill_with_object(dead_space, size_diff);
          }
        }
      } else {
        obj->set_klass(obj->klass()->new_version());
      }
    }
  }
};


void VM_EnhancedRedefineClasses::doit() {

  Thread *thread = Thread::current();

  assert((_max_redefinition_flags & Klass::RemoveSuperType) == 0, "removing super types not allowed");

  if (UseSharedSpaces) {
    // Sharing is enabled so we remap the shared readonly space to
    // shared readwrite, private just in case we need to redefine
    // a shared class. We do the remap during the doit() phase of
    // the safepoint to be safer.
    if (!MetaspaceShared::remap_shared_readonly_as_readwrite()) {
      RC_TRACE(0x00000001,
        ("failed to remap shared readonly space to readwrite, private"));
      _result = JVMTI_ERROR_INTERNAL;
      return;
    }
  }

  RC_TIMER_START(_timer_prepare_redefinition);
  for (int i = 0; i < _new_classes->length(); i++) {
    redefine_single_class(_new_classes->at(i), thread);
  }

  // Deoptimize all compiled code that depends on this class
  flush_dependent_code(instanceKlassHandle(Thread::current(), (Klass*)NULL), Thread::current());

  // Adjust constantpool caches for all classes
  // that reference methods of the evolved class.
  ClearCpoolCacheAndUnpatch clear_cpool_cache(Thread::current());
  ClassLoaderDataGraph::classes_do(&clear_cpool_cache);

  RC_TIMER_STOP(_timer_prepare_redefinition);
  RC_TIMER_START(_timer_heap_iteration);

  ChangePointersOopClosure<StoreNoBarrier> oopClosureNoBarrier;
  ChangePointersOopClosure<StoreBarrier> oopClosure;
  ChangePointersObjectClosure objectClosure(&oopClosure);

  RC_TRACE(0x00000001, ("Before updating instances"));
  {
    // Since we may update oops inside nmethod's code blob to point to java.lang.Class in new generation, we need to
    // make sure such references are properly recognized by GC. For that, If ScavengeRootsInCode is true, we need to
    // mark such nmethod's as "scavengable".
    // For now, mark all nmethod's as scavengable that are not scavengable already
    if (ScavengeRootsInCode) {
      CodeCache::nmethods_do(mark_as_scavengable);
    }

    SharedHeap::heap()->gc_prologue(true);
    Universe::heap()->object_iterate(&objectClosure);
    Universe::root_oops_do(&oopClosureNoBarrier);
    SharedHeap::heap()->gc_epilogue(false);
  }
  RC_TRACE(0x00000001, ("After updating instances"));

  for (int i = 0; i < _new_classes->length(); i++) {
    InstanceKlass* cur = InstanceKlass::cast(_new_classes->at(i)());
    InstanceKlass* old = InstanceKlass::cast(cur->old_version());

    // Swap marks to have same hashcodes
    markOop cur_mark = cur->prototype_header();
    markOop old_mark = old->prototype_header();
    cur->set_prototype_header(old_mark);
    old->set_prototype_header(cur_mark);

    //swap_marks(cur, old);
    swap_marks(cur->java_mirror(), old->java_mirror());

    // Revert pool holder for old version of klass (it was updated by one of ours closure!)
    old->constants()->set_pool_holder(old);

    Klass* array_klasses = old->array_klasses();
    if (array_klasses != NULL) {
      assert(cur->array_klasses() == NULL, "just checking");

      // Transfer the array classes, otherwise we might get cast exceptions when casting array types.
      // Also, set array klasses element klass.
      cur->set_array_klasses(array_klasses);
      ObjArrayKlass::cast(array_klasses)->set_element_klass(cur);
      ArrayKlass::cast(array_klasses)->set_component_mirror(cur->java_mirror());
    }

    // Initialize the new class! Special static initialization that does not execute the
    // static constructor but copies static field values from the old class if name
    // and signature of a static field match.
    FieldCopier copier;
    cur->do_local_static_fields(&copier); // TODO (tw): What about internal static fields??
    //java_lang_Class::set_klass(old->java_mirror(), cur); // FIXME-isd: is that correct?
    //FIXME-isd: do we need this: ??? old->set_java_mirror(cur->java_mirror());

    // Transfer init state
    InstanceKlass::ClassState state = old->init_state();
    if (state > InstanceKlass::linked) {
      cur->set_init_state(state);
    }
  }

  RC_TIMER_STOP(_timer_heap_iteration);
  RC_TIMER_START(_timer_redefinition);
  if (objectClosure.needs_instance_update()) {
    // Do a full garbage collection to update the instance sizes accordingly
    RC_TRACE(0x00000001, ("Before performing full GC!"));
    Universe::set_redefining_gc_run(true);
    notify_gc_begin(true);
    Universe::heap()->collect_as_vm_thread(GCCause::_heap_inspection);
    notify_gc_end();
    Universe::set_redefining_gc_run(false);
    RC_TRACE(0x00000001, ("GC done!"));
  }

  // Unmark Klass*s as "redefining"
  for (int i=0; i<_new_classes->length(); i++) {
    Klass* cur_klass = _new_classes->at(i)();
    InstanceKlass* cur = (InstanceKlass*)cur_klass;
    cur->set_redefining(false);
    cur->clear_update_information();
  }

  // Disable any dependent concurrent compilations
  SystemDictionary::notice_modification();

  // Update klass pointers
  SystemDictionary::update_constraints_after_redefinition();

  // Set flag indicating that some invariants are no longer true.
  // See jvmtiExport.hpp for detailed explanation.
  JvmtiExport::set_has_redefined_a_class();

  // Clean up caches in the compiler interface and compiler threads
  ciObjectFactory::resort_shared_ci_metadata();

#ifdef ASSERT

  // Universe::verify();
  // JNIHandles::verify();

  SystemDictionary::classes_do(check_class, thread);
#endif

  RC_TIMER_STOP(_timer_redefinition);

  if (TraceRedefineClasses > 0) {
    tty->flush();
  }
}

void VM_EnhancedRedefineClasses::doit_epilogue() {

  RC_TIMER_START(_timer_vm_op_epilogue);

  ResourceMark mark;

  VM_GC_Operation::doit_epilogue();
  RC_TRACE(0x00000001, ("GC Operation epilogue finished!"));

  // Free the array of scratch classes
  delete _new_classes;
  _new_classes = NULL;

  // Free the array of affected classes
  delete _affected_klasses;
  _affected_klasses = NULL;

  RC_TRACE(0x00000001, ("Redefinition finished!"));

  RC_TIMER_STOP(_timer_vm_op_epilogue);
}

bool VM_EnhancedRedefineClasses::is_modifiable_class(oop klass_mirror) {
  // classes for primitives cannot be redefined
  if (java_lang_Class::is_primitive(klass_mirror)) {
    return false;
  }
  Klass* klass = java_lang_Class::as_Klass(klass_mirror);
  // classes for arrays cannot be redefined
  if (klass == NULL || !klass->oop_is_instance()) {
    return false;
  }
  return true;
}

#ifdef ASSERT

void VM_EnhancedRedefineClasses::verify_classes(Klass* k_oop_latest, oop initiating_loader, TRAPS) {
  Klass* k_oop = k_oop_latest;
  while (k_oop != NULL) {

    instanceKlassHandle k_handle(THREAD, k_oop);
    Verifier::verify(k_handle, Verifier::ThrowException, true, THREAD);
    k_oop = k_oop->old_version();
  }
}

#endif

// Rewrite faster byte-codes back to their slower equivalent. Undoes rewriting happening in templateTable_xxx.cpp
// The reason is that once we zero cpool caches, we need to re-resolve all entries again. Faster bytecodes do not
// do that, they assume that cache entry is resolved already.
void VM_EnhancedRedefineClasses::unpatch_bytecode(Method* method) {
  RawBytecodeStream bcs(method);
  Bytecodes::Code code;
  Bytecodes::Code java_code;
  while (!bcs.is_last_bytecode()) {
    code = bcs.raw_next();
    address bcp = bcs.bcp();

    if (code == Bytecodes::_breakpoint) {
      int bci = method->bci_from(bcp);
      code = method->orig_bytecode_at(bci);
      java_code = Bytecodes::java_code(code);
      if (code != java_code &&
           (java_code == Bytecodes::_getfield ||
            java_code == Bytecodes::_putfield ||
            java_code == Bytecodes::_aload_0)) {
        // Let breakpoint table handling unpatch bytecode
        method->set_orig_bytecode_at(bci, java_code);
      }
    } else {
      java_code = Bytecodes::java_code(code);
      if (code != java_code &&
           (java_code == Bytecodes::_getfield ||
            java_code == Bytecodes::_putfield ||
            java_code == Bytecodes::_aload_0)) {
        *bcp = java_code;
      }
    }

    // Additionally, we need to unpatch bytecode at bcp+1 for fast_xaccess (which would be fast field access)
    if (code == Bytecodes::_fast_iaccess_0 || code == Bytecodes::_fast_aaccess_0 || code == Bytecodes::_fast_faccess_0) {
      Bytecodes::Code code2 = Bytecodes::code_or_bp_at(bcp + 1);
      assert(code2 == Bytecodes::_fast_igetfield ||
             code2 == Bytecodes::_fast_agetfield ||
             code2 == Bytecodes::_fast_fgetfield, "");
        *(bcp + 1) = Bytecodes::java_code(code2);
      }
    }
  }

// Unevolving classes may point to old methods directly
// from their constant pool caches, itables, and/or vtables. We
// use the SystemDictionary::classes_do() facility and this helper
// to fix up these pointers. Additional field offsets and vtable indices
// in the constant pool cache entries are fixed.
//
// Note: We currently don't support updating the vtable in
// arrayKlassOops. See Open Issues in jvmtiRedefineClasses.hpp.
void VM_EnhancedRedefineClasses::ClearCpoolCacheAndUnpatch::do_klass(Klass* klass) {
  if (!klass->oop_is_instance()) {
    return;
  }
  HandleMark hm(_thread);
  InstanceKlass *ik = InstanceKlass::cast(klass);
  constantPoolHandle other_cp = constantPoolHandle(ik->constants());

  // Update host klass of anonymous classes (for example, produced by lambdas) to newest version.
  if (ik->is_anonymous() && !ik->host_klass()->is_newest_version()) {
    ik->set_host_klass(ik->host_klass()->newest_version());
  }

  for (int i = 0; i < other_cp->length(); i++) {
    if (other_cp->tag_at(i).is_klass()) {
      Klass* klass = other_cp->klass_at(i, _thread);
      if (klass->new_version() != NULL) {
        // (DCEVM) TODO: check why/if this is necessary
        other_cp->klass_at_put(i, klass->new_version());
      }
      klass = other_cp->klass_at(i, _thread);
      assert(klass->new_version() == NULL, "Must be new klass!");
    }
  }

  ConstantPoolCache* cp_cache = other_cp->cache();
  if (cp_cache != NULL) {
    cp_cache->clear_entries();
  }

  // If bytecode rewriting is enabled, we also need to unpatch bytecode to force resolution of zeroed entries
  if (RewriteBytecodes) {
    ik->methods_do(unpatch_bytecode);
  }
}

void VM_EnhancedRedefineClasses::update_jmethod_ids() {
  for (int j = 0; j < _matching_methods_length; ++j) {
    Method* old_method = _old_methods->at(_matching_old_methods[j]);
    jmethodID jmid = old_method->find_jmethod_id_or_null();
    RC_TRACE(0x00008000, ("matching method %s, jmid %d", old_method->name_and_sig_as_C_string(), *((int *)&jmid)));
    if (old_method->new_version() != NULL && jmid == NULL) {
       // (DCEVM) Have to create jmethodID in this case
       jmid = old_method->jmethod_id();
    }

    if (jmid != NULL) {
      // There is a jmethodID, change it to point to the new method
      methodHandle new_method_h(_new_methods->at(_matching_new_methods[j]));
      if (old_method->new_version() == NULL) {
        methodHandle old_method_h(_old_methods->at(_matching_old_methods[j]));
        jmethodID new_jmethod_id = Method::make_jmethod_id(old_method_h->method_holder()->class_loader_data(), old_method_h());
        bool result = InstanceKlass::cast(old_method_h->method_holder())->update_jmethod_id(old_method_h(), new_jmethod_id);
      } else {
        jmethodID mid = new_method_h->jmethod_id();
        bool result = InstanceKlass::cast(new_method_h->method_holder())->update_jmethod_id(new_method_h(), jmid);
      }
      Method::change_method_associated_with_jmethod_id(jmid, new_method_h());
      assert(Method::resolve_jmethod_id(jmid) == _new_methods->at(_matching_new_methods[j]), "should be replaced");
      jmethodID mid = (_new_methods->at(_matching_new_methods[j]))->jmethod_id();
      //assert(JNIHandles::resolve_non_null((jobject)mid) == new_method_h(), "must match!");
    }
  }
}


// Deoptimize all compiled code that depends on this class.
//
// If the can_redefine_classes capability is obtained in the onload
// phase then the compiler has recorded all dependencies from startup.
// In that case we need only deoptimize and throw away all compiled code
// that depends on the class.
//
// If can_redefine_classes is obtained sometime after the onload
// phase then the dependency information may be incomplete. In that case
// the first call to RedefineClasses causes all compiled code to be
// thrown away. As can_redefine_classes has been obtained then
// all future compilations will record dependencies so second and
// subsequent calls to RedefineClasses need only throw away code
// that depends on the class.
//
void VM_EnhancedRedefineClasses::flush_dependent_code(instanceKlassHandle k_h, TRAPS) {
  assert_locked_or_safepoint(Compile_lock);

  // All dependencies have been recorded from startup or this is a second or
  // subsequent use of RedefineClasses

  // For now deopt all
  // (tw) TODO: Improve the dependency system such that we can safely deopt only a subset of the methods
  if (0 && JvmtiExport::all_dependencies_are_recorded()) {
    Universe::flush_evol_dependents_on(k_h);
  } else {
  	if (HotswapDeoptClassPath == NULL)
  		CodeCache::mark_all_nmethods_for_deoptimization();
  	else
    	CodeCache::mark_all_incl_nmethods_for_deoptimization();

    ResourceMark rm(THREAD);
    DeoptimizationMarker dm;

    // Deoptimize all activations depending on marked nmethods
    Deoptimization::deoptimize_dependents();

    // Make the dependent methods not entrant (in VM_Deoptimize they are made zombies)
    CodeCache::make_marked_nmethods_not_entrant();

    // From now on we know that the dependency information is complete
    JvmtiExport::set_all_dependencies_are_recorded(true);
  }
}

void VM_EnhancedRedefineClasses::compute_added_deleted_matching_methods() {
  Method* old_method;
  Method* new_method;

  _matching_old_methods = NEW_RESOURCE_ARRAY(int, _old_methods->length());
  _matching_new_methods = NEW_RESOURCE_ARRAY(int, _old_methods->length());
  _added_methods        = NEW_RESOURCE_ARRAY(int, _new_methods->length());
  _deleted_methods      = NEW_RESOURCE_ARRAY(int, _old_methods->length());

  _matching_methods_length = 0;
  _deleted_methods_length  = 0;
  _added_methods_length    = 0;

  int nj = 0;
  int oj = 0;
  while (true) {
    if (oj >= _old_methods->length()) {
      if (nj >= _new_methods->length()) {
        break; // we've looked at everything, done
    }
      // New method at the end
      new_method = _new_methods->at(nj);
      _added_methods[_added_methods_length++] = nj;
      ++nj;
    } else if (nj >= _new_methods->length()) {
      // Old method, at the end, is deleted
      old_method = _old_methods->at(oj);
      _deleted_methods[_deleted_methods_length++] = oj;
      ++oj;
    } else {
      old_method = _old_methods->at(oj);
      new_method = _new_methods->at(nj);
      if (old_method->name() == new_method->name()) {
        if (old_method->signature() == new_method->signature()) {
          _matching_old_methods[_matching_methods_length] = oj;//old_method;
          _matching_new_methods[_matching_methods_length] = nj;//new_method;
          _matching_methods_length++;
          ++nj;
          ++oj;
        } else {
          // added overloaded have already been moved to the end,
          // so this is a deleted overloaded method
          _deleted_methods[_deleted_methods_length++] = oj;//old_method;
          ++oj;
        }
      } else { // names don't match
        if (old_method->name()->fast_compare(new_method->name()) > 0) {
          // new method
          _added_methods[_added_methods_length++] = nj;//new_method;
          ++nj;
        } else {
          // deleted method
          _deleted_methods[_deleted_methods_length++] = oj;//old_method;
          ++oj;
        }
      }
    }
  }
  assert(_matching_methods_length + _deleted_methods_length == _old_methods->length(), "sanity");
  assert(_matching_methods_length + _added_methods_length == _new_methods->length(), "sanity");
  RC_TRACE(0x00008000, ("Matching methods = %d / deleted methods = %d / added methods = %d",
                       _matching_methods_length, _deleted_methods_length, _added_methods_length));
}



// Install the redefinition of a class:
//    - house keeping (flushing breakpoints and caches, deoptimizing
//      dependent compiled code)
//    - adjusting constant pool caches and vtables in other classes
void VM_EnhancedRedefineClasses::redefine_single_class(instanceKlassHandle the_new_class, TRAPS) {

  ResourceMark rm(THREAD);

  assert(the_new_class->old_version() != NULL, "Must not be null");
  assert(the_new_class->old_version()->new_version() == the_new_class(), "Must equal");

  instanceKlassHandle the_old_class = instanceKlassHandle(THREAD, the_new_class->old_version());

#ifndef JVMTI_KERNEL
  // Remove all breakpoints in methods of this class
  JvmtiBreakpoints& jvmti_breakpoints = JvmtiCurrentBreakpoints::get_jvmti_breakpoints();
  jvmti_breakpoints.clearall_in_class_at_safepoint(the_old_class());
#endif // !JVMTI_KERNEL

  /* FIXME
  if (the_old_class() == Universe::reflect_invoke_cache()->klass()) {
    // We are redefining java.lang.reflect.Method. Method.invoke() is
    // cached and users of the cache care about each active version of
    // the method so we have to track this previous version.
    // Do this before methods get switched
    Universe::reflect_invoke_cache()->add_previous_version(
      the_old_class->method_with_idnum(Universe::reflect_invoke_cache()->method_idnum()));
  }*/

  _old_methods = the_old_class->methods();
  _new_methods = the_new_class->methods();
  compute_added_deleted_matching_methods();

  // track which methods are EMCP for add_previous_version() call below

  // TODO: Check if we need the concept of EMCP?
  BitMap emcp_methods(_old_methods->length());
  int emcp_method_count = 0;
  emcp_methods.clear();  // clears 0..(length() - 1)

  // We need to mark methods as old!!
  check_methods_and_mark_as_obsolete(&emcp_methods, &emcp_method_count);
  update_jmethod_ids();

  // TODO:
  transfer_old_native_function_registrations(the_old_class);

  // JSR-292 support

  // Swap method handles
  MemberNameTable* mnt = the_old_class->member_names();
  assert(the_new_class->member_names() == NULL, "");
  the_new_class->set_member_names(mnt);
  the_old_class->set_member_names(NULL);

  // FIXME: should we add field MemberName's in this list and process all of them here?
//  if (mnt != NULL) {
//    for (int i = 0; i < mnt->length(); i++) {
//      oop mem_name = mnt->get_member_name(i);
//      if (mem_name != NULL) {
//        Method* method = (Method*) java_lang_invoke_MemberName::vmtarget(mem_name);
//
//        // Replace the method with matching one from the new class
//        Method* new_method = the_new_class->find_method(method->name(), method->signature());
//        java_lang_invoke_MemberName::set_vmtarget(mem_name, new_method);
//      }
//    }
//  }

#ifdef ASSERT

//  Klass* systemLookup1 = SystemDictionary::resolve_or_null(the_old_class->name(), the_old_class->class_loader(), the_old_class->protection_domain(), THREAD);
//  assert(systemLookup1 == the_new_class(), "New class must be in system dictionary!");

  //JNIHandles::verify();

//  Klass* systemLookup = SystemDictionary::resolve_or_null(the_old_class->name(), the_old_class->class_loader(), the_old_class->protection_domain(), THREAD);

//  assert(systemLookup == the_new_class(), "New class must be in system dictionary!");
  assert(the_new_class->old_version() != NULL, "Must not be null");
  assert(the_new_class->old_version()->new_version() == the_new_class(), "Must equal");

  for (int i=0; i<the_new_class->methods()->length(); i++) {
    assert((the_new_class->methods()->at(i))->method_holder() == the_new_class(), "method holder must match!");
  }

  // FIXME:
  //_old_methods->verify();
  //_new_methods->verify();

  the_new_class->vtable()->verify(tty);
  the_old_class->vtable()->verify(tty);

#endif

  // increment the classRedefinedCount field in the_class and in any
  // direct and indirect subclasses of the_class
  increment_class_counter((InstanceKlass *)the_old_class(), THREAD);

}


void VM_EnhancedRedefineClasses::check_methods_and_mark_as_obsolete(BitMap *emcp_methods, int * emcp_method_count_p) {
  RC_TRACE(0x00000100, ("Checking matching methods for EMCP"));
  *emcp_method_count_p = 0;
  int obsolete_count = 0;
  int old_index = 0;
  for (int j = 0; j < _matching_methods_length; ++j, ++old_index) {
      Method* old_method = _old_methods->at(_matching_old_methods[j]);
      Method* new_method = _new_methods->at(_matching_new_methods[j]);
      Method* old_array_method;

    // Maintain an old_index into the _old_methods array by skipping
    // deleted methods
    while ((old_array_method = _old_methods->at(old_index)) != old_method) {
      ++old_index;
    }

    if (MethodComparator::methods_EMCP(old_method, new_method)) {
      // The EMCP definition from JSR-163 requires the bytecodes to be
      // the same with the exception of constant pool indices which may
      // differ. However, the constants referred to by those indices
      // must be the same.
      //
      // We use methods_EMCP() for comparison since constant pool
      // merging can remove duplicate constant pool entries that were
      // present in the old method and removed from the rewritten new
      // method. A faster binary comparison function would consider the
      // old and new methods to be different when they are actually
      // EMCP.

      // track which methods are EMCP for add_previous_version() call
      emcp_methods->set_bit(old_index);
      (*emcp_method_count_p)++;

      // An EMCP method is _not_ obsolete. An obsolete method has a
      // different jmethodID than the current method. An EMCP method
      // has the same jmethodID as the current method. Having the
      // same jmethodID for all EMCP versions of a method allows for
      // a consistent view of the EMCP methods regardless of which
      // EMCP method you happen to have in hand. For example, a
      // breakpoint set in one EMCP method will work for all EMCP
      // versions of the method including the current one.

        old_method->set_new_version(new_method);
        new_method->set_old_version(old_method);

        RC_TRACE(0x00000100, ("Found EMCP method %s", old_method->name_and_sig_as_C_string()));

        // Transfer breakpoints
        InstanceKlass *ik = InstanceKlass::cast(old_method->method_holder());
        for (BreakpointInfo* bp = ik->breakpoints(); bp != NULL; bp = bp->next()) {
          RC_TRACE(0x00000100, ("Checking breakpoint: %d / %d",
                               bp->match(old_method), bp->match(new_method)));
          if (bp->match(old_method)) {
            assert(bp->match(new_method), "if old method is method, then new method must match too");
            RC_TRACE(0x00000100, ("Found a breakpoint in an old EMCP method"));
            new_method->set_breakpoint(bp->bci());
          }
        }
    } else {
      // mark obsolete methods as such
      old_method->set_is_obsolete();
      obsolete_count++;

      // With tracing we try not to "yack" too much. The position of
      // this trace assumes there are fewer obsolete methods than
      // EMCP methods.
      RC_TRACE(0x00000100, ("mark %s(%s) as obsolete",
                           old_method->name()->as_C_string(),
                           old_method->signature()->as_C_string()));
    }
    old_method->set_is_old();
  }
  for (int i = 0; i < _deleted_methods_length; ++i) {
      Method* old_method = _old_methods->at(_deleted_methods[i]);

    //assert(old_method->vtable_index() < 0,
    //  "cannot delete methods with vtable entries");;

    // Mark all deleted methods as old and obsolete
    old_method->set_is_old();
    old_method->set_is_obsolete();
    ++obsolete_count;
    // With tracing we try not to "yack" too much. The position of
    // this trace assumes there are fewer obsolete methods than
    // EMCP methods.
      RC_TRACE(0x00000100, ("mark deleted %s(%s) as obsolete",
                           old_method->name()->as_C_string(),
                           old_method->signature()->as_C_string()));
    }
    //assert((*emcp_method_count_p + obsolete_count) == _old_methods->length(), "sanity check");
    RC_TRACE(0x00000100, ("EMCP_cnt=%d, obsolete_cnt=%d !",
                         *emcp_method_count_p, obsolete_count));
}

// Increment the classRedefinedCount field in the specific InstanceKlass
// and in all direct and indirect subclasses.
void VM_EnhancedRedefineClasses::increment_class_counter(Klass* klass, TRAPS) {
  oop class_mirror = klass->java_mirror();
  int new_count = java_lang_Class::classRedefinedCount(class_mirror) + 1;
  java_lang_Class::set_classRedefinedCount(class_mirror, new_count);
  RC_TRACE(0x00000008, ("updated count for class=%s to %d", klass->external_name(), new_count));
}

#ifndef PRODUCT
void VM_EnhancedRedefineClasses::check_class(Klass* k_oop, TRAPS) {
  Klass *k = k_oop;
  if (k->oop_is_instance()) {
    HandleMark hm(THREAD);
    InstanceKlass *ik = (InstanceKlass *) k;
    assert(ik->is_newest_version(), "must be latest version in system dictionary");

    if (ik->vtable_length() > 0) {
      ResourceMark rm(THREAD);
      assert(ik->vtable()->check_no_old_or_obsolete_entries(), "old method found");
      ik->vtable()->verify(tty, true);
    }
  }
}

#endif

static bool match_second(void* value, Pair<Klass*, Klass*> elem) {
  return elem.second == value;
}

jvmtiError VM_EnhancedRedefineClasses::do_topological_class_sorting( const jvmtiClassDefinition *class_defs, int class_count, TRAPS) {
  ResourceMark mark(THREAD);
  GrowableArray<Pair<Klass*, Klass*> > links;

  for (int i=0; i<class_count; i++) {

    oop mirror = JNIHandles::resolve_non_null(class_defs[i].klass);
    instanceKlassHandle the_class(THREAD, java_lang_Class::as_Klass(mirror));
    Handle the_class_loader(THREAD, the_class->class_loader());
    Handle protection_domain(THREAD, the_class->protection_domain());

    ClassFileStream st((u1*) class_defs[i].class_bytes,
      class_defs[i].class_byte_count, (char *)"__VM_EnhancedRedefineClasses__");
    ClassFileParser cfp(&st);



    TempNewSymbol parsed_name;
    GrowableArray<Symbol*>* super_symbols = new (ResourceObj::C_HEAP, mtInternal) GrowableArray<Symbol*>(0, true);
    cfp.parseClassFile(the_class->name(),
                       the_class->class_loader_data(),
                       protection_domain,
                       the_class, KlassHandle(),
                       NULL,
                       super_symbols,
                       parsed_name,
                       false,
                       THREAD);

    for (int j = 0; j < super_symbols->length(); j++) {
      Symbol* sym = super_symbols->at(j);
      Klass* super_klass = SystemDictionary::resolve_or_null(sym, the_class_loader, protection_domain, THREAD);
      if (super_klass != NULL) {
        instanceKlassHandle the_super_class(THREAD, super_klass);
        if (_affected_klasses->contains(the_super_class)) {
          links.append(Pair<Klass*, Klass*>(super_klass, the_class()));
        }
      }
    }
    delete super_symbols;

    assert(the_class->check_redefinition_flag(Klass::MarkedAsAffected), "");
    the_class->clear_redefinition_flag(Klass::MarkedAsAffected);
  }

  for (int i=0; i < _affected_klasses->length(); i++) {
    instanceKlassHandle klass = _affected_klasses->at(i);

    if (klass->check_redefinition_flag(Klass::MarkedAsAffected)) {
      klass->clear_redefinition_flag(Klass::MarkedAsAffected);
      Klass* superKlass = klass->super();
      if (_affected_klasses->contains(superKlass)) {
        links.append(Pair<Klass*, Klass*>(superKlass, klass()));
      }

      Array<Klass*>* superInterfaces = klass->local_interfaces();
      for (int j=0; j<superInterfaces->length(); j++) {
        Klass* interfaceKlass = superInterfaces->at(j);
        if (_affected_klasses->contains(interfaceKlass)) {
          links.append(Pair<Klass*, Klass*>(interfaceKlass, klass()));
        }
      }
    }
  }

  for (int i = 0; i < _affected_klasses->length(); i++) {
    int j;
    for (j = i; j < _affected_klasses->length(); j++) {
      // Search for node with no incoming edges
      Klass* oop = _affected_klasses->at(j)();
      int k = links.find(oop, match_second);
      if (k == -1) break;
    }
    if (j == _affected_klasses->length()) {
      return JVMTI_ERROR_CIRCULAR_CLASS_DEFINITION;
    }

    // Remove all links from this node
    Klass* oop = _affected_klasses->at(j)();
    int k = 0;
    while (k < links.length()) {
      if (links.adr_at(k)->first == oop) {
        links.delete_at(k);
      } else {
        k++;
      }
    }

    // Swap node
    instanceKlassHandle tmp = _affected_klasses->at(j);
    _affected_klasses->at_put(j, _affected_klasses->at(i));
    _affected_klasses->at_put(i, tmp);
  }

  return JVMTI_ERROR_NONE;
}

// This internal class transfers the native function registration from old methods
// to new methods.  It is designed to handle both the simple case of unchanged
// native methods and the complex cases of native method prefixes being added and/or
// removed.
// It expects only to be used during the VM_EnhancedRedefineClasses op (a safepoint).
//
// This class is used after the new methods have been installed in "the_class".
//
// So, for example, the following must be handled.  Where 'm' is a method and
// a number followed by an underscore is a prefix.
//
//                                      Old Name    New Name
// Simple transfer to new method        m       ->  m
// Add prefix                           m       ->  1_m
// Remove prefix                        1_m     ->  m
// Simultaneous add of prefixes         m       ->  3_2_1_m
// Simultaneous removal of prefixes     3_2_1_m ->  m
// Simultaneous add and remove          1_m     ->  2_m
// Same, caused by prefix removal only  3_2_1_m ->  3_2_m
//
class TransferNativeFunctionRegistration {
 private:
  instanceKlassHandle the_class;
  int prefix_count;
  char** prefixes;

  // Recursively search the binary tree of possibly prefixed method names.
  // Iteration could be used if all agents were well behaved. Full tree walk is
  // more resilent to agents not cleaning up intermediate methods.
  // Branch at each depth in the binary tree is:
  //    (1) without the prefix.
  //    (2) with the prefix.
  // where 'prefix' is the prefix at that 'depth' (first prefix, second prefix,...)
  Method* search_prefix_name_space(int depth, char* name_str, size_t name_len,
                                     Symbol* signature) {
      Symbol* name_symbol = SymbolTable::probe(name_str, (int)name_len);
    if (name_symbol != NULL) {
        Method* method = the_class()->new_version()->lookup_method(name_symbol, signature);
      if (method != NULL) {
        // Even if prefixed, intermediate methods must exist.
        if (method->is_native()) {
          // Wahoo, we found a (possibly prefixed) version of the method, return it.
          return method;
        }
        if (depth < prefix_count) {
          // Try applying further prefixes (other than this one).
          method = search_prefix_name_space(depth+1, name_str, name_len, signature);
          if (method != NULL) {
            return method; // found
          }

          // Try adding this prefix to the method name and see if it matches
          // another method name.
          char* prefix = prefixes[depth];
          size_t prefix_len = strlen(prefix);
          size_t trial_len = name_len + prefix_len;
          char* trial_name_str = NEW_RESOURCE_ARRAY(char, trial_len + 1);
          strcpy(trial_name_str, prefix);
          strcat(trial_name_str, name_str);
          method = search_prefix_name_space(depth+1, trial_name_str, trial_len,
                                            signature);
          if (method != NULL) {
            // If found along this branch, it was prefixed, mark as such
            method->set_is_prefixed_native();
            return method; // found
          }
        }
      }
    }
    return NULL;  // This whole branch bore nothing
  }

  // Return the method name with old prefixes stripped away.
  char* method_name_without_prefixes(Method* method) {
    Symbol* name = method->name();
    char* name_str = name->as_utf8();

    // Old prefixing may be defunct, strip prefixes, if any.
    for (int i = prefix_count-1; i >= 0; i--) {
      char* prefix = prefixes[i];
      size_t prefix_len = strlen(prefix);
      if (strncmp(prefix, name_str, prefix_len) == 0) {
        name_str += prefix_len;
      }
    }
    return name_str;
  }

  // Strip any prefixes off the old native method, then try to find a
  // (possibly prefixed) new native that matches it.
  Method* strip_and_search_for_new_native(Method* method) {
    ResourceMark rm;
    char* name_str = method_name_without_prefixes(method);
    return search_prefix_name_space(0, name_str, strlen(name_str),
                                    method->signature());
  }

 public:

  // Construct a native method transfer processor for this class.
  TransferNativeFunctionRegistration(instanceKlassHandle _the_class) {
    assert(SafepointSynchronize::is_at_safepoint(), "sanity check");

    the_class = _the_class;
    prefixes = JvmtiExport::get_all_native_method_prefixes(&prefix_count);
  }

  // Attempt to transfer any of the old or deleted methods that are native
  void transfer_registrations(instanceKlassHandle old_klass, int* old_methods, int methods_length) {
    for (int j = 0; j < methods_length; j++) {
      Method* old_method = old_klass->methods()->at(old_methods[j]);

      if (old_method->is_native() && old_method->has_native_function()) {
        Method* new_method = strip_and_search_for_new_native(old_method);
        if (new_method != NULL) {
          // Actually set the native function in the new method.
          // Redefine does not send events (except CFLH), certainly not this
          // behind the scenes re-registration.
          new_method->set_native_function(old_method->native_function(),
            !Method::native_bind_event_is_interesting);
        }
      }
    }
  }
};

// Don't lose the association between a native method and its JNI function.
void VM_EnhancedRedefineClasses::transfer_old_native_function_registrations(instanceKlassHandle old_klass) {
  TransferNativeFunctionRegistration transfer(old_klass);
  transfer.transfer_registrations(old_klass, _deleted_methods, _deleted_methods_length);
  transfer.transfer_registrations(old_klass, _matching_old_methods, _matching_methods_length);
}
