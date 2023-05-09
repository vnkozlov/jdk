/*
 * Copyright (c) 2023, Oracle and/or its affiliates. All rights reserved.
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
#include "asm/macroAssembler.hpp"
#include "compiler/abstractCompiler.hpp"
#include "classfile/stringTable.hpp"
#include "classfile/symbolTable.hpp"
#include "classfile/systemDictionary.hpp"
#include "classfile/vmIntrinsics.hpp"
#include "ci/ciConstant.hpp"
#include "ci/ciEnv.hpp"
#include "ci/ciField.hpp"
#include "ci/ciMethod.hpp"
#include "ci/ciMethodData.hpp"
#include "ci/ciObject.hpp"
#include "ci/ciUtilities.inline.hpp"
#include "code/codeBlob.hpp"
#include "code/codeCache.hpp"
#include "code/oopRecorder.inline.hpp"
#include "code/SCArchive.hpp"
#include "gc/g1/g1BarrierSetRuntime.hpp"
#include "logging/log.hpp"
#include "memory/universe.hpp"
#include "oops/klass.inline.hpp"
#include "oops/method.inline.hpp"
#include "runtime/atomic.hpp"
#include "runtime/flags/flagSetting.hpp"
#include "runtime/globals_extension.hpp"
#include "runtime/handles.inline.hpp"
#include "runtime/jniHandles.inline.hpp"
#include "runtime/os.inline.hpp"
#include "runtime/sharedRuntime.hpp"
#include "runtime/stubCodeGenerator.hpp"
#include "runtime/stubRoutines.hpp"
#include "runtime/threadIdentifier.hpp"
#ifdef COMPILER2
#include "opto/runtime.hpp"
#endif

#include <sys/stat.h>
#include <errno.h>

#ifndef O_BINARY       // if defined (Win32) use binary files.
#define O_BINARY 0     // otherwise do nothing.
#endif

#ifdef _WINDOWS
    const char pathSep = ';';
#else
    const char pathSep = ':';
#endif

SCAFile* SCArchive::_archive = nullptr;

void SCArchive::initialize() {
  if ((LoadSharedCode || StoreSharedCode) && SharedCodeArchive != nullptr) {
    const int len = (int)strlen(SharedCodeArchive);
    char* cp  = NEW_C_HEAP_ARRAY(char, len+1, mtCode);
    memcpy(cp, SharedCodeArchive, len);
    cp[len] = '\0';
    const int file_separator = *os::file_separator();
    const char* start = strrchr(cp, file_separator);
    const char* path = (start == nullptr) ? cp : (start + 1);

    bool success = false;
    if (StoreSharedCode) {
      success = open_for_write(path);
    } else if (LoadSharedCode) {
      success = open_for_read(path);
    }
    if (!success) {
      FREE_C_HEAP_ARRAY(char, cp);
    }
    FLAG_SET_DEFAULT(FoldStableValues, false);
    FLAG_SET_DEFAULT(ForceUnreachable, true);
    FLAG_SET_DEFAULT(DelayCompilerStubsGeneration, false);
  }
}

void SCArchive::close() {
  if (_archive != nullptr) {
    delete _archive; // Free memory
    _archive = nullptr;
  }
}

void SCArchive::invalidate(SCAEntry* entry) {
  // This could be concurent execution
  if (entry != nullptr && _archive != nullptr) { // Request could come after archive is closed.
    _archive->invalidate(entry);
  }
}

void SCArchive::add_C_string(const char* str) {
  if (_archive != nullptr && _archive->for_write()) {
    _archive->add_C_string(str);
  }
}

bool SCArchive::allow_const_field(ciConstant& value) {
  return !is_on() || !StoreSharedCode // Restrict only when we generate archive
        // Can not trust primitive too   || !is_reference_type(value.basic_type())
        // May disable this too for now  || is_reference_type(value.basic_type()) && value.as_object()->should_be_constant()
        ;
}

bool SCArchive::open_for_read(const char* archive_path) {
  log_info(sca)("Trying to load shared code archive '%s'", archive_path);
  struct stat st;
  if (os::stat(archive_path, &st) != 0) {
    log_info(sca, init)("Specified shared code archive not found '%s'", archive_path);
    return false;
  } else if ((st.st_mode & S_IFMT) != S_IFREG) {
    log_info(sca, init)("Specified shared code archive is not file '%s'", archive_path);
    return false;
  }
  int fd = os::open(archive_path, O_RDONLY | O_BINARY, 0);
  if (fd < 0) {
    if (errno == ENOENT) {
      log_info(sca, init)("Specified shared code archive not found '%s'", archive_path);
    } else {
      log_warning(sca, init)("Failed to open shared code archive file '%s': (%s)", archive_path, os::strerror(errno));
    }
    return false;
  } else {
    log_info(sca, init)("Opened for read shared code archive '%s'", archive_path);
  }
  SCAFile* archive = new SCAFile(archive_path, fd, (uint)st.st_size, true /* for_read */);
  if (archive->failed()) {
    delete archive;
    _archive = nullptr;
    return false;
  }
  _archive = archive;
  return true;
}

bool SCArchive::open_for_write(const char* archive_path) {
#ifdef _WINDOWS  // On Windows, need WRITE permission to remove the file.
  chmod(archive_path, _S_IREAD | _S_IWRITE);
#endif
  // Use remove() to delete the existing file because, on Unix, this will
  // allow processes that have it open continued access to the file.
  remove(archive_path);
  int fd = os::open(archive_path, O_RDWR | O_CREAT | O_TRUNC | O_BINARY, 0444);
  if (fd < 0) {
    log_warning(sca, init)("Unable to create shared code archive file '%s': (%s)", archive_path, os::strerror(errno));
    return false;
  } else {
    log_info(sca, init)("Opened for write shared code archive '%s'", archive_path);
  }
  SCAFile* archive = new SCAFile(archive_path, fd, 0, false /* for_read */);
  if (archive->failed()) {
    delete archive;
    _archive = nullptr;
    return false;
  }
  _archive = archive;
  return true;
}

#define DATA_ALIGNMENT HeapWordSize

SCAFile::SCAFile(const char* archive_path, int fd, uint load_size, bool for_read) {
  _archive_path = archive_path;
  _load_size = load_size;
  _store_size = 0;
  _write_position = 0;
  _fd = fd;
  _for_read = for_read;
  _for_write = !for_read;
  _closing  = false;
  _failed = false;
  _lookup_failed = false;
  _table = nullptr;
  _entries = nullptr;
  _write_entries  = nullptr;
  _C_strings_buf  = nullptr;
  _load_buffer = nullptr;
  _store_buffer = nullptr;
  _C_load_buffer = nullptr;
  _C_store_buffer = nullptr;

  // Read header at the begining of archive
  uint header_size = sizeof(SCAHeader);
  if (for_read) {
    // Read archive
    _C_load_buffer = NEW_C_HEAP_ARRAY(char, load_size + DATA_ALIGNMENT, mtCode);
    _load_buffer = align_up(_C_load_buffer, DATA_ALIGNMENT);
    uint n = (uint)::read(_fd, _load_buffer, load_size);
    if (n != load_size) {
      log_warning(sca, init)("Failed to read %d bytes at offset %d from shared code archive file '%s'", load_size, 0, _archive_path);
      set_failed();
      return;
    }
    log_debug(sca, init)("Read %d bytes at offset %d from shared code archive '%s'", load_size, 0, _archive_path);

    _header = (SCAHeader*)addr(0);
    assert(_header->version() == VM_Version::jvm_version(), "sanity");
    assert(_header->archive_size() <= load_size, "recorded %d vs actual %d", _header->archive_size(), load_size);
    log_info(sca, init)("Read header from shared code archive '%s'", archive_path);

    // Read strings
    if (!load_strings()) {
      return;
    }
  } else {
    _C_store_buffer = NEW_C_HEAP_ARRAY(char, ReservedSharedCodeSize + DATA_ALIGNMENT, mtCode);
    _store_buffer = align_up(_C_store_buffer, DATA_ALIGNMENT);
    _header = new SCAHeader();
    // Write initial version of header
    _header->init(VM_Version::jvm_version(), 0 /* entry_count */, 0 /* archive_size */, 0 /* entries_offset */, 0, 0);
    uint header_size = sizeof(SCAHeader);
    uint n = write_bytes((const void*)_header, header_size);
    if (n != header_size) {
      return;
    }
    log_info(sca, init)("Wrote initial header to shared code archive '%s'", archive_path);
  }
  _table = new SCAddressTable();
}

void SCAFile::init_table() {
  SCAFile* archive = SCArchive::archive();
  if (archive != nullptr && archive->_table != nullptr) {
    archive->_table->init();
  }
}

void SCAFile::init_opto_table() {
  SCAFile* archive = SCArchive::archive();
  if (archive != nullptr && archive->_table != nullptr) {
    archive->_table->init_opto();
  }
}

static volatile int _reading_nmethod = 0;

class ReadingMark {
public:
  ReadingMark() {
    Atomic::inc(&_reading_nmethod);
  }
  ~ReadingMark() {
    Atomic::dec(&_reading_nmethod);
  }
};

SCAFile::~SCAFile() {
  if (_fd < 0) {
    return; // Already closed
  }
  // Stop any further access to archive.
  // Checked on entry to load_nmethod() and store_nmethod().
  _closing = true;
  if (_for_read && _reading_nmethod > 0) {
    // Wait for all load_nmethod() finish.
    // TODO: may be have new separate locker for SCA.
    MonitorLocker locker(Compilation_lock, Mutex::_no_safepoint_check_flag);
    while (_reading_nmethod > 0) {
      locker.wait(10); // Wait 10 ms
    }
  }
  // Prevent writing code into archive while we are closing it.
  // This lock held by ciEnv::register_method() which calls store_nmethod().
  MutexLocker ml(Compile_lock);
  if (for_write()) { // Finalize archive
    finish_write();
  }
  if (::close(_fd) < 0) {
    log_warning(sca)("Failed to close shared code archive file '%s'", _archive_path);
  }
  _fd = -1;

  log_info(sca, exit)("Closed shared code archive '%s'", _archive_path);
  FREE_C_HEAP_ARRAY(char, _archive_path);
  if (_header != nullptr && !_for_read) {
    delete _header;
    _header = nullptr;
  }
  if (_C_load_buffer != nullptr) {
    FREE_C_HEAP_ARRAY(char, _C_load_buffer);
    _C_load_buffer = nullptr;
    _load_buffer = nullptr;
  }
  if (_C_store_buffer != nullptr) {
    FREE_C_HEAP_ARRAY(char, _C_store_buffer);
    _C_store_buffer = nullptr;
    _store_buffer = nullptr;
  }
  if (_C_strings_buf != nullptr && !_for_read) {
    FREE_C_HEAP_ARRAY(char, _C_strings_buf);
    _C_strings_buf = nullptr;
  }
  if (_entries != nullptr && !_for_read) {
    FREE_C_HEAP_ARRAY(SCAEntry, _entries);
    _entries = nullptr;
  }
  if (_write_entries != nullptr) {
    delete _write_entries;
    _write_entries = nullptr;
  }
  if (_table != nullptr) {
    delete _table;
    _table = nullptr;
  }
}

bool SCAFile::for_read()  const { return (_fd >= 0) && _for_read  && !_failed; }

bool SCAFile::for_write() const { return (_fd >= 0) && _for_write && !_failed; }

SCAFile* SCAFile::open_for_read() {
  SCAFile* archive = SCArchive::archive();
  if (archive != nullptr && archive->for_read() && !archive->closing()) {
    return archive;
  }
  return nullptr;
}

SCAFile* SCAFile::open_for_write() {
  SCAFile* archive = SCArchive::archive();
  if (archive != nullptr && archive->for_write() && !archive->closing()) {
    archive->clear_lookup_failed(); // Reset bit
    return archive;
  }
  return nullptr;
}

void copy_bytes(const char* from, address to, uint size) {
  assert(size > 0, "sanity");
  bool by_words = true;
  if ((size > 2 * HeapWordSize) && (((intptr_t)from | (intptr_t)to) & (HeapWordSize - 1)) == 0) {
    // Use wordwise copies if possible:
    Copy::disjoint_words((HeapWord*)from,
                         (HeapWord*)to,
                         ((size_t)size + HeapWordSize-1) / HeapWordSize);
  } else {
    by_words = false;
    Copy::conjoint_jbytes(from, to, (size_t)size);
  }
  log_debug(sca)("Copied %d bytes as %s from " INTPTR_FORMAT " to " INTPTR_FORMAT, size, (by_words ? "HeapWord" : "bytes"), p2i(from), p2i(to));
}

void SCAReader::set_read_position(uint pos) {
  if (pos == _read_position) {
    return;
  }
  assert(pos < _archive->load_size(), "offset:%d >= file size:%d", pos, _archive->load_size());
  _read_position = pos;
}

bool SCAFile::set_write_position(uint pos) {
  if (pos == _write_position) {
    return true;
  }
  if (_store_size < _write_position) {
    _store_size = _write_position; // Adjust during write
  }
  assert(pos < _store_size, "offset:%d >= file size:%d", pos, _store_size);
  _write_position = pos;
  return true;
}

static char align_buffer[256] = { 0 };

bool SCAFile::align_write() {
  // We are not executing code from archive - we copy it by bytes first.
  // No need for big alignment (or at all).
  uint padding = DATA_ALIGNMENT - (_write_position & (DATA_ALIGNMENT - 1));
  if (padding == DATA_ALIGNMENT) {
    return true;
  }
  uint n = write_bytes((const void*)&align_buffer, padding);
  if (n != padding) {
    return false;
  }
  log_debug(sca)("Adjust write alignment in shared code archive '%s'", _archive_path);
  return true;
}

uint SCAFile::write_bytes(const void* buffer, uint nbytes) {
  assert(for_write(), "Archive file is not created");
  if (nbytes == 0) {
    return 0;
  }
  if ((_write_position + nbytes) >= ReservedSharedCodeSize) {
    log_warning(sca)("Failed to write %d bytes at offset %d to shared code archive file '%s'. Increase ReservedSharedCodeSize.",
                     nbytes, _write_position, _archive_path);
    set_failed();
    return 0;
  }
  copy_bytes((const char* )buffer, (address)(_store_buffer + _write_position), nbytes);
  log_debug(sca)("Wrote %d bytes at offset %d to shared code archive '%s'", nbytes, _write_position, _archive_path);
  _write_position += nbytes;
  if (_store_size < _write_position) {
    _store_size = _write_position;
  }
  return nbytes;
}

SCAEntry* SCAFile::add_entry(SCAEntry entry) {
  if (_write_entries == nullptr) {
    _write_entries = new(mtCode) GrowableArray<SCAEntry>(4, mtCode); // C heap
    assert(_write_entries != nullptr, "Sanity");
  }
  _write_entries->append(entry);
  // Code was sucessfully added
  return _write_entries->adr_at(_write_entries->length() - 1); // Last
}

SCAEntry* SCAFile::find_entry(SCAEntry::Kind kind, uint id, uint decomp) {
  uint count = _header->entries_count();
  if (_entries == nullptr) {
    // Read it
    _entries = (SCAEntry*)addr(_header->entries_offset());
    log_info(sca)("Read %d SCAEntry entries at offset %d from shared code archive '%s'", count, _header->entries_offset(), _archive_path);
  }
  for(uint i = 0; i < count; i++) {
    SCAEntry* entry = &(_entries[i]);
    if (entry->kind() == kind && entry->id() == id) {
      if (kind == SCAEntry::Code && (entry->not_entrant()/* || entry->decompile() != decomp */)) {
        continue;
      }
      assert(_entries[i].idx() == i, "sanity");
      return entry;
    }
  }
  return nullptr;
}

void SCAFile::invalidate(SCAEntry* entry) {
  assert( entry!= nullptr, "all entries should be read already");
  if (entry->not_entrant()) {
    return; // Someone invalidated it already
  }
#ifdef ASSERT
  SCAEntry* entries_address = (_for_read) ? &(_entries[0]) : _write_entries->adr_at(0);
  uint count = (_for_read) ? _header->entries_count() : _write_entries->length();
  uint i = 0;
  for(; i < count; i++) {
    SCAEntry* entry = &(entries_address[i]);
    if (entry == &(entries_address[i])) {
      break;
    }
  }
  assert(i < count, "entry should exist");
#endif
  entry->set_not_entrant();
}

bool SCAFile::finish_write() {
  uint version = _header->version();
  uint entries_offset = 0; // 0 is deafult if empty
  if (!align_write()) {
    return false;
  }
  uint strings_offset = _write_position;
  int strings_count = store_strings();
  if (strings_count < 0) {
    return false;
  }
  if (!align_write()) {
    return false;
  }
  int count = _write_entries->length();
  if (count > 0) {
    SCAEntry* entries_address = _write_entries->adr_at(0);
    // Write SCAEntry entries
    entries_offset = _write_position;
    uint entries_size = count * sizeof(SCAEntry); // In bytes
    uint n = write_bytes(entries_address, entries_size);
    if (n != entries_size) {
      return false;
    }
    int not_entrant_nb = 0;
    uint max_size = 0;
    for(int i = 0; i < count; i++) {
      if (entries_address[i].not_entrant()) {
        log_info(sca, exit)("Not entrant id: %d, hash: " UINT32_FORMAT_X_0, i, entries_address[i].id());
        not_entrant_nb++;
      }
      uint size = entries_address[i].size();
      if (size > max_size) {
        max_size = size;
      }
    }
    log_info(sca, exit)("Wrote %d SCAEntry entries (%d not entrant, %d max size) to shared code archive '%s'", count, not_entrant_nb, max_size, _archive_path);
  }

  // Finalize and write header
  _header->init(version, count, _write_position, entries_offset, (uint)strings_count, strings_offset);
  if (!set_write_position(0)) {
    return false;
  }
  _write_position = 0;
  uint header_size = sizeof(SCAHeader);
  uint n = write_bytes(_header, header_size);
  if (n != header_size) {
    return false;
  }
  log_info(sca, exit)("Wrote header to shared code archive '%s'", _archive_path);

  // Dump to file
  n = (uint)os::write(_fd, _store_buffer, _store_size);
  if (n != _store_size) {
    log_warning(sca, exit)("Failed to write %d bytes to shared code archive file '%s'", _store_size, _archive_path);
    return false;
  }
  log_info(sca, exit)("Wrote %d bytes to shared code archive '%s'", _store_size, _archive_path);

  return true;
}

bool SCAFile::load_stub(StubCodeGenerator* cgen, vmIntrinsicID id, const char* name, address start) {
  assert(start == cgen->assembler()->pc(), "wrong buffer");
  SCAFile* archive = open_for_read();
  if (archive == nullptr) {
    return false;
  }
  SCAEntry* entry = archive->find_entry(SCAEntry::Stub, (uint)id);
  if (entry == nullptr) {
    return false;
  }
  uint entry_position = entry->offset();
  // Read name
  uint name_offset = entry->name_offset();
  uint name_size   = entry->name_size(); // Includes '/0'
  const char* saved_name = archive->addr(name_offset);
  if (strncmp(name, saved_name, (name_size - 1)) != 0) {
    log_warning(sca)("Saved stub's name '%s' is different from '%s' for id:%d", saved_name, name, (int)id);
    archive->set_failed();
    return false;
  }
  log_info(sca,stubs)("Reading stub '%s' id:%d from shared code archive '%s'", name, (int)id, archive->_archive_path);
  // Read code
  uint code_offset = entry->code_offset();
  uint code_size   = entry->code_size();
  copy_bytes(archive->addr(code_offset), start, code_size);
  cgen->assembler()->code_section()->set_end(start + code_size);
  log_info(sca,stubs)("Read stub '%s' id:%d from shared code archive '%s'", name, (int)id, archive->_archive_path);
  return true;
}

bool SCAFile::store_stub(StubCodeGenerator* cgen, vmIntrinsicID id, const char* name, address start) {
  SCAFile* archive = open_for_write();
  if (archive == nullptr) {
    return false;
  }
  log_info(sca, stubs)("Writing stub '%s' id:%d to shared code archive '%s'", name, (int)id, archive->_archive_path);
  if (!archive->align_write()) {
    return false;
  }
  uint entry_position = archive->_write_position;

  // Write code
  uint code_offset = entry_position;
  uint code_size = cgen->assembler()->pc() - start;
  uint n = archive->write_bytes(start, code_size);
  if (n != code_size) {
    return false;
  }
  // Write name
  uint name_offset = archive->_write_position;
  uint name_size = (uint)strlen(name) + 1; // Includes '/0'
  n = archive->write_bytes(name, name_size);
  if (n != name_size) {
    return false;
  }
  uint entry_size = archive->_write_position - entry_position;
  SCAEntry entry(entry_position, entry_size, name_offset, name_size, code_offset, code_size, 0, 0,
                 SCAEntry::Stub, (uint32_t)id, archive->_header->next_idx());
  archive->add_entry(entry);
  log_info(sca, stubs)("Wrote stub '%s' id:%d to shared code archive '%s'", name, (int)id, archive->_archive_path);
  return true;
}

Klass* SCAReader::read_klass(const methodHandle& comp_method) {
  uint code_offset = read_position();
  int name_length = *(int*)addr(code_offset);
  code_offset += sizeof(int);
  const char* dest = addr(code_offset);
  code_offset += name_length + 1;
  set_read_position(code_offset);
  TempNewSymbol klass_sym = SymbolTable::probe(&(dest[0]), name_length);
  if (klass_sym == nullptr) {
    set_lookup_failed();
    log_info(sca)("Probe failed for class %s", &(dest[0]));
    return nullptr;
  }
  // Use class loader of compiled method.
  Thread* thread = Thread::current();
  Handle loader(thread, comp_method->method_holder()->class_loader());
  Handle protection_domain(thread, comp_method->method_holder()->protection_domain());
  Klass* k = SystemDictionary::find_instance_or_array_klass(thread, klass_sym, loader, protection_domain);
  assert(!thread->has_pending_exception(), "should not throw");
  if (k == nullptr && !loader.is_null()) {
    // Try default loader and domain
    k = SystemDictionary::find_instance_or_array_klass(thread, klass_sym, Handle(), Handle());
    assert(!thread->has_pending_exception(), "should not throw");
  }
  if (k != nullptr) {
    log_info(sca)("Klass lookup %s", k->external_name());
  } else {
    set_lookup_failed();
    log_info(sca)("Lookup failed for class %s", &(dest[0]));
    return nullptr;
  }
  return k;
}

Method* SCAReader::read_method(const methodHandle& comp_method) {
  uint code_offset = read_position();
  int holder_length = *(int*)addr(code_offset);
  code_offset += sizeof(int);
  int name_length = *(int*)addr(code_offset);
  code_offset += sizeof(int);
  int signat_length = *(int*)addr(code_offset);
  code_offset += sizeof(int);

  const char* dest = addr(code_offset);
  code_offset += holder_length + 1 + name_length + 1 + signat_length + 1;
  set_read_position(code_offset);
  TempNewSymbol klass_sym = SymbolTable::probe(&(dest[0]), holder_length);
  if (klass_sym == nullptr) {
    set_lookup_failed();
    log_info(sca)("Probe failed for class %s", &(dest[0]));
    return nullptr;
  }
  // Use class loader of compiled method.
  Thread* thread = Thread::current();
  Handle loader(thread, comp_method->method_holder()->class_loader());
  Handle protection_domain(thread, comp_method->method_holder()->protection_domain());
  Klass* k = SystemDictionary::find_instance_or_array_klass(thread, klass_sym, loader, protection_domain);
  assert(!thread->has_pending_exception(), "should not throw");
  if (k == nullptr && !loader.is_null()) {
    // Try default loader and domain
    k = SystemDictionary::find_instance_or_array_klass(thread, klass_sym, Handle(), Handle());
    assert(!thread->has_pending_exception(), "should not throw");
  }
  if (k != nullptr) {
    log_info(sca)("Holder lookup: %s", k->external_name());
  } else {
    set_lookup_failed();
    log_info(sca)("Lookup failed for holder %s", &(dest[0]));
    return nullptr;
  }
  TempNewSymbol name_sym = SymbolTable::probe(&(dest[holder_length + 1]), name_length);
  int pos = holder_length + 1 + name_length + 1;
  TempNewSymbol sign_sym = SymbolTable::probe(&(dest[pos]), signat_length);
  if (name_sym == nullptr) {
    set_lookup_failed();
    log_info(sca)("Probe failed for method name %s", &(dest[holder_length + 1]));
    return nullptr;
  }
  if (sign_sym == nullptr) {
    set_lookup_failed();
    log_info(sca)("Probe failed for method signature %s", &(dest[pos]));
    return nullptr;
  }
  Method* m = InstanceKlass::cast(k)->find_method(name_sym, sign_sym);
  if (m != nullptr) {
    log_info(sca)("Method lookup: %s", m->name_and_sig_as_C_string());
  } else {
    set_lookup_failed();
    log_info(sca)("Lookup failed for method %s%s", &(dest[holder_length + 1]), &(dest[pos]));
    return nullptr;
  }
  return m;
}

bool SCAFile::write_klass(Klass* klass) {
  ResourceMark rm;
  Symbol* name = klass->name();
  int name_length = name->utf8_length();
  int total_length = name_length + 1;
  char* dest = NEW_RESOURCE_ARRAY(char, total_length);
  name->as_C_string(dest, total_length);
  dest[total_length - 1] = '\0';
  DataKind kind = DataKind::Klass;
  uint n = write_bytes(&kind, sizeof(int));
  if (n != sizeof(int)) {
    return false;
  }
if (UseNewCode) {
  oop loader = klass->class_loader();
  oop domain = klass->protection_domain();
  tty->print("Class %s loader: ", dest);
  if (loader == nullptr) {
    tty->print("nullptr");
  } else {
    loader->print_value_on(tty);
  }
  tty->print(" domain: ");
  if (domain == nullptr) {
    tty->print("nullptr");
  } else {
    domain->print_value_on(tty);
  }
  tty->cr();
}
  n = write_bytes(&name_length, sizeof(int));
  if (n != sizeof(int)) {
    return false;
  }
  n = write_bytes(dest, total_length);
  if (n != (uint)total_length) {
    return false;
  }
  log_info(sca)("Write klass: %s", dest);
  return true;
}

bool SCAFile::write_method(Method* method) {
  ResourceMark rm;
  Symbol* name   = method->name();
  Symbol* holder = method->klass_name();
  Symbol* signat = method->signature();
  int name_length   = name->utf8_length();
  int holder_length = holder->utf8_length();
  int signat_length = signat->utf8_length();

  // Write sizes and strings
  int total_length = holder_length + 1 + name_length + 1 + signat_length + 1;
  char* dest = NEW_RESOURCE_ARRAY(char, total_length);
  holder->as_C_string(dest, total_length);
  dest[holder_length] = '\0';
  int pos = holder_length + 1;
  name->as_C_string(&(dest[pos]), (total_length - pos));
  pos += name_length;
  dest[pos++] = '\0';
  signat->as_C_string(&(dest[pos]), (total_length - pos));
  dest[total_length - 1] = '\0';

if (UseNewCode) {
  Klass* klass = method->method_holder();
  oop loader = klass->class_loader();
  oop domain = klass->protection_domain();
  tty->print("Holder %s loader: ", dest);
  if (loader == nullptr) {
    tty->print("nullptr");
  } else {
    loader->print_value_on(tty);
  }
  tty->print(" domain: ");
  if (domain == nullptr) {
    tty->print("nullptr");
  } else {
    domain->print_value_on(tty);
  }
  tty->cr();
}

  DataKind kind = DataKind::Method;
  uint n = write_bytes(&kind, sizeof(int));
  if (n != sizeof(int)) {
    return false;
  }
  n = write_bytes(&holder_length, sizeof(int));
  if (n != sizeof(int)) {
    return false;
  }
  n = write_bytes(&name_length, sizeof(int));
  if (n != sizeof(int)) {
    return false;
  }
  n = write_bytes(&signat_length, sizeof(int));
  if (n != sizeof(int)) {
    return false;
  }
  n = write_bytes(dest, total_length);
  if (n != (uint)total_length) {
    return false;
  }
  dest[holder_length] = ' ';
  dest[holder_length + 1 + name_length] = ' ';
  log_info(sca)("Wrote method: %s", dest);
  return true;
}

// Repair the pc relative information in the code after load
bool SCAReader::read_relocations(CodeBuffer* buffer, CodeBuffer* orig_buffer,
                               uint max_reloc_size, OopRecorder* oop_recorder,
                               ciMethod* target) {
  // Read max count
  uint max_reloc_count = max_reloc_size / sizeof(relocInfo);
  if (UseNewCode) {
    tty->print_cr("======== read relocations [%d]:", max_reloc_count);
  }
  bool success = true;
  //uint* reloc_data = NEW_C_HEAP_ARRAY(uint, max_reloc_count, mtCode);
  for (int i = 0; i < (int)CodeBuffer::SECT_LIMIT; i++) {
    uint code_offset = read_position();
    int reloc_count = *(int*)addr(code_offset);
    code_offset += sizeof(int);
    if (reloc_count == 0) {
      set_read_position(code_offset);
      continue;
    }
    // Read _locs_point (as offset from start)
    int locs_point_off = *(int*)addr(code_offset);
    code_offset += sizeof(int);
    uint reloc_size = reloc_count * sizeof(relocInfo);
    CodeSection* cs  = buffer->code_section(i);
    if (cs->locs_capacity() < reloc_count) {
      cs->expand_locs(reloc_count);
    }
    relocInfo* reloc_start = cs->locs_start();
    copy_bytes(addr(code_offset), (address)reloc_start, reloc_size);
    code_offset += reloc_size;
    cs->set_locs_end(reloc_start + reloc_count);
    cs->set_locs_point(cs->start() + locs_point_off);

    // Read additional relocation data: uint per relocation
    uint  data_size  = reloc_count * sizeof(uint);
    uint* reloc_data = (uint*)addr(code_offset);
    code_offset += data_size;
    set_read_position(code_offset);
    if (UseNewCode) {
      tty->print_cr("======== read code section %d relocations [%d]:", i, reloc_count);
    }
    RelocIterator iter(cs);
    int j = 0;
    while (iter.next()) {
      switch (iter.type()) {
        case relocInfo::none:
          break;
        case relocInfo::oop_type: {
          VM_ENTRY_MARK;
          oop_Relocation* r = (oop_Relocation*)iter.reloc();
          if (r->oop_is_immediate()) {
            assert(reloc_data[j] == (uint)j, "should be");
            methodHandle comp_method(THREAD, target->get_Method());
            jobject jo = read_oop(THREAD, comp_method);
            if (lookup_failed()) {
              success = false;
              break;
            }
            r->set_value((address)jo);
          } else if (false) {
            // Get already updated value from OopRecorder.
            assert(oop_recorder != nullptr, "sanity");
            int index = r->oop_index();
            jobject jo = oop_recorder->oop_at(index);
            oop obj = JNIHandles::resolve(jo);
            r->set_value(*reinterpret_cast<address*>(&obj));
          }
          break;
        }
        case relocInfo::metadata_type: {
          VM_ENTRY_MARK;
          metadata_Relocation* r = (metadata_Relocation*)iter.reloc();
          Metadata* m;
          if (r->metadata_is_immediate()) {
            assert(reloc_data[j] == (uint)j, "should be");
            methodHandle comp_method(THREAD, target->get_Method());
            m = read_metadata(comp_method);
            if (lookup_failed()) {
              success = false;
              break;
            }
          } else {
            // Get already updated value from OopRecorder.
            assert(oop_recorder != nullptr, "sanity");
            int index = r->metadata_index();
            m = oop_recorder->metadata_at(index);
          }
          r->set_value((address)m);
          break;
        }
        case relocInfo::virtual_call_type:   // Fall through. They all call resolve_*_call blobs.
        case relocInfo::opt_virtual_call_type:
        case relocInfo::static_call_type: {
          address dest = _archive->address_for_id(reloc_data[j]);
          if (dest != (address)-1) {
            ((CallRelocation*)iter.reloc())->set_destination(dest);
          }
          break;
        }
        case relocInfo::static_stub_type:
          iter.reloc()->fix_relocation_after_move(orig_buffer, buffer);
          break;
        case relocInfo::runtime_call_type: {
          address dest = _archive->address_for_id(reloc_data[j]);
          if (dest != (address)-1) {
            ((CallRelocation*)iter.reloc())->set_destination(dest);
          }
          break;
        }
        case relocInfo::runtime_call_w_cp_type:
          fatal("runtime_call_w_cp_type unimplemented");
          //address destination = iter.reloc()->value();
          break;
        case relocInfo::external_word_type: {
          iter.reloc()->fix_relocation_after_move(orig_buffer, buffer);
          address target = _archive->address_for_id(reloc_data[j]);
          int data_len = iter.datalen();
          if (data_len > 0) {
            // Overwrite RelocInfo embedded address
            RelocationHolder rh = external_word_Relocation::spec(target);
            external_word_Relocation* new_reloc = (external_word_Relocation*)rh.reloc();
            short buf[4] = {0}; // 8 bytes
            short* p = new_reloc->pack_data_to(buf);
            if ((p - buf) != data_len) {
              return false; // New address does not fit into old relocInfo
            }
            short* data = iter.data();
            for (int i = 0; i < data_len; i++) {
              data[i] = buf[i];
            }
          }
          external_word_Relocation* reloc = (external_word_Relocation*)iter.reloc();
          reloc->set_value(target); // Patch address in the code
          break;
        }
        case relocInfo::internal_word_type:
          iter.reloc()->fix_relocation_after_move(orig_buffer, buffer);
          break;
        case relocInfo::section_word_type:
          iter.reloc()->fix_relocation_after_move(orig_buffer, buffer);
          break;
        case relocInfo::poll_type:
          break;
        case relocInfo::poll_return_type:
          break;
        case relocInfo::post_call_nop_type:
          break;
        default:
          fatal("relocation %d unimplemented", (int)iter.type());
          break;
      }
#ifdef ASSERT
      if (UseNewCode) {
        iter.print_current();
      }
#endif
      j++;
    }
    assert(j <= (int)reloc_count, "sanity");
  }
  return success;
}

bool SCAReader::read_code(CodeBuffer* buffer, CodeBuffer* orig_buffer, uint code_offset) {
  assert(code_offset == align_up(code_offset, DATA_ALIGNMENT), "%d not aligned to %d", code_offset, DATA_ALIGNMENT);
  assert(buffer->blob() != nullptr, "sanity");
  SCACodeSection* sca_cs = (SCACodeSection*)addr(code_offset);
  for (int i = 0; i < (int)CodeBuffer::SECT_LIMIT; i++) {
    CodeSection* cs = buffer->code_section(i);
    // Read original section size and address.
    uint orig_size = sca_cs[i]._size;
    if (UseNewCode) {
      tty->print_cr("======== read code section %d [%d]:", i, orig_size);
    }
    uint orig_size_align = align_up(orig_size, DATA_ALIGNMENT);
    if (i != (int)CodeBuffer::SECT_INSTS) {
      buffer->initialize_section_size(cs, orig_size_align);
    }
    if (orig_size_align > (uint)cs->capacity()) { // Will not fit
      log_warning(sca)("original code section %d size %d > current capacity %d", i, orig_size, cs->capacity());
      return false;
    }
    if (orig_size == 0) {
      assert(cs->size() == 0, "should match");
      continue;  // skip trivial section
    }
    address orig_start = sca_cs[i]._origin_address;

    // Populate fake original buffer (no code allocation in CodeCache).
    // It is used for relocations to calculate sections addesses delta.
    CodeSection* orig_cs = orig_buffer->code_section(i);
    assert(!orig_cs->is_allocated(), "This %d section should not be set", i);
    orig_cs->initialize(orig_start, orig_size);

    // Load code to new buffer.
    address code_start = cs->start();
    copy_bytes(addr(sca_cs[i]._offset + code_offset), code_start, orig_size_align);
    cs->set_end(code_start + orig_size);
  }

  return true;
}

bool SCAFile::load_exception_blob(CodeBuffer* buffer, int* pc_offset) {
#ifdef ASSERT
if (UseNewCode3) {
  FlagSetting fs(PrintRelocations, true);
  buffer->print();
}
#endif
  SCAFile* archive = open_for_read();
  if (archive == nullptr) {
    return false;
  }
  SCAEntry* entry = archive->find_entry(SCAEntry::Blob, 999);
  if (entry == nullptr) {
    return false;
  }
  SCAReader reader(archive, entry);
  return reader.compile_blob(buffer, pc_offset);
}

bool SCAReader::compile_blob(CodeBuffer* buffer, int* pc_offset) {
  uint entry_position = _entry->offset();

  // Read pc_offset
  *pc_offset = *(int*)addr(entry_position);

  // Read name
  uint name_offset = entry_position + _entry->name_offset();
  uint name_size = _entry->name_size(); // Includes '/0'
  const char* name = addr(name_offset);

  log_info(sca, stubs)("Reading blob '%s' with pc_offset %d from shared code archive '%s'", name, *pc_offset, _archive->archive_path());

  if (strncmp(buffer->name(), name, (name_size - 1)) != 0) {
    log_warning(sca)("Saved blob's name '%s' is different from '%s'", name, buffer->name());
    ((SCAFile*)_archive)->set_failed();
    return false;
  }

  // Create fake original CodeBuffer
  CodeBuffer orig_buffer(name);

  // Read code
  if (!read_code(buffer, &orig_buffer, _entry->code_offset())) {
    return false;
  }

  // Read relocations
  uint reloc_offset = entry_position + _entry->reloc_offset();
  set_read_position(reloc_offset);
  if (!read_relocations(buffer, &orig_buffer, _entry->reloc_size(), nullptr, nullptr)) {
    return false;
  }

  log_info(sca, stubs)("Read blob '%s' from shared code archive '%s'", name, _archive->archive_path());
#ifdef ASSERT
if (UseNewCode3) {
  FlagSetting fs(PrintRelocations, true);
  buffer->print();
  buffer->decode();
}
#endif
  return true;
}

bool SCAFile::write_relocations(CodeBuffer* buffer, uint& max_reloc_size) {
  uint max_reloc_count = 0;
  for (int i = 0; i < (int)CodeBuffer::SECT_LIMIT; i++) {
    CodeSection* cs = buffer->code_section(i);
    uint reloc_count = cs->has_locs() ? cs->locs_count() : 0;
    if (reloc_count > max_reloc_count) {
      max_reloc_count = reloc_count;
    }
  }
  max_reloc_size = max_reloc_count * sizeof(relocInfo);
  bool success = true;
  uint* reloc_data = NEW_C_HEAP_ARRAY(uint, max_reloc_count, mtCode);
  if (UseNewCode) {
    tty->print_cr("======== write relocations [%d]:", max_reloc_count);
  }
  for (int i = 0; i < (int)CodeBuffer::SECT_LIMIT; i++) {
    CodeSection* cs = buffer->code_section(i);
    int reloc_count = cs->has_locs() ? cs->locs_count() : 0;
    uint n = write_bytes(&reloc_count, sizeof(int));
    if (n != sizeof(int)) {
      success = false;
      break;
    }
    if (reloc_count == 0) {
      continue;
    }
    // Write _locs_point (as offset from start)
    int locs_point_off = cs->locs_point_off();
    n = write_bytes(&locs_point_off, sizeof(int));
    if (n != sizeof(int)) {
      success = false;
      break;
    }
    relocInfo* reloc_start = cs->locs_start();
    uint reloc_size      = reloc_count * sizeof(relocInfo);
    n = write_bytes(reloc_start, reloc_size);
    if (n != reloc_size) {
      success = false;
      break;
    }
    if (UseNewCode) {
      tty->print_cr("======== write code section %d relocations [%d]:", i, reloc_count);
    }
    // Collect additional data
    RelocIterator iter(cs);
    bool has_immediate = false;
    int j = 0;
    while (iter.next()) {
      reloc_data[j] = 0; // initialize
      switch (iter.type()) {
        case relocInfo::none:
          break;
        case relocInfo::oop_type: {
          oop_Relocation* r = (oop_Relocation*)iter.reloc();
          if (r->oop_is_immediate()) {
            reloc_data[j] = (uint)j; // Indication that we need to restore immediate
            has_immediate = true;
          }
          break;
        }
        case relocInfo::metadata_type: {
          metadata_Relocation* r = (metadata_Relocation*)iter.reloc();
          if (r->metadata_is_immediate()) {
            reloc_data[j] = (uint)j; // Indication that we need to restore immediate
            has_immediate = true;
          }
          break;
        }
        case relocInfo::virtual_call_type:  // Fall through. They all call resolve_*_call blobs.
        case relocInfo::opt_virtual_call_type:
        case relocInfo::static_call_type: {
          address dest = ((CallRelocation*)iter.reloc())->destination();
          reloc_data[j] = _table->id_for_address(dest);
          break;
        }
        case relocInfo::static_stub_type:
          break;
        case relocInfo::runtime_call_type: {
          // Record offset of runtime destination
          address dest = ((CallRelocation*)iter.reloc())->destination();
          reloc_data[j] = _table->id_for_address(dest);
          break;
        }
        case relocInfo::runtime_call_w_cp_type:
          fatal("runtime_call_w_cp_type unimplemented");
          break;
        case relocInfo::external_word_type: {
          // Record offset of runtime target
          address target = ((external_word_Relocation*)iter.reloc())->target();
          reloc_data[j] = _table->id_for_address(target);
          break;
        }
        case relocInfo::internal_word_type:
          break;
        case relocInfo::section_word_type:
          break;
        case relocInfo::poll_type:
          break;
        case relocInfo::poll_return_type:
          break;
        case relocInfo::post_call_nop_type:
          break;
        default:
          fatal("relocation %d unimplemented", (int)iter.type());
          break;
      }
      j++;
    }
    assert(j <= (int)reloc_count, "sanity");
    // Write additional relocation data: uint per relocation
    uint data_size = reloc_count * sizeof(uint);
    n = write_bytes(reloc_data, data_size);
    if (n != data_size) {
      success = false;
      break;
    }
    if (has_immediate) {
      // Save information about immediates in this Code Section
      RelocIterator iter_imm(cs);
      int j = 0;
      while (iter_imm.next()) {
        switch (iter_imm.type()) {
          case relocInfo::oop_type: {
            oop_Relocation* r = (oop_Relocation*)iter_imm.reloc();
            if (r->oop_is_immediate()) {
              assert(reloc_data[j] == (uint)j, "should be");
              jobject jo = *(jobject*)(r->oop_addr()); // Handle currently
              if (!write_oop(jo)) {
                success = false;
              }
            }
            break;
          }
          case relocInfo::metadata_type: {
            metadata_Relocation* r = (metadata_Relocation*)iter_imm.reloc();
            if (r->metadata_is_immediate()) {
              assert(reloc_data[j] == (uint)j, "should be");
              Metadata* m = r->metadata_value();
              if (!write_metadata(m)) {
                success = false;
              }
            }
            break;
          }
          default:
            break;
        }
        if (!success) {
          break;
        }
        j++;
      } // while (iter_imm.next())
    } // if (has_immediate)
  } // for(i < SECT_LIMIT)
  FREE_C_HEAP_ARRAY(uint, reloc_data);
  return success;
}

bool SCAFile::write_code(CodeBuffer* buffer, uint& code_size) {
  assert(_write_position == align_up(_write_position, DATA_ALIGNMENT), "%d not aligned to %d", _write_position, DATA_ALIGNMENT);
  assert(buffer->blob() != nullptr, "sanity");
  uint code_offset = _write_position;
  uint cb_total_size = (uint)buffer->total_content_size();
  // Write information about Code sections first.
  SCACodeSection sca_cs[CodeBuffer::SECT_LIMIT];
  uint sca_cs_size = (uint)(sizeof(SCACodeSection) * CodeBuffer::SECT_LIMIT);
  uint offset = align_up(sca_cs_size, DATA_ALIGNMENT);
  uint total_size = 0;
  for (int i = 0; i < (int)CodeBuffer::SECT_LIMIT; i++) {
    const CodeSection* cs = buffer->code_section(i);
    assert(cs->mark() == nullptr, "CodeSection::_mark is not implemented");
    uint cs_size = (uint)cs->size();
    sca_cs[i]._size = cs_size;
    sca_cs[i]._origin_address = (cs_size == 0) ? nullptr : cs->start();
    sca_cs[i]._offset = (cs_size == 0) ? 0 : (offset + total_size);
    assert(cs->mark() == nullptr, "CodeSection::_mark is not implemented");
    total_size += align_up(cs_size, DATA_ALIGNMENT);
  }
  uint n = write_bytes(sca_cs, sca_cs_size);
  if (n != sca_cs_size) {
    return false;
  }
  if (!align_write()) {
    return false;
  }
  assert(_write_position == (code_offset + offset), "%d  != (%d + %d)", _write_position, code_offset, offset);
  for (int i = 0; i < (int)CodeBuffer::SECT_LIMIT; i++) {
    const CodeSection* cs = buffer->code_section(i);
    uint cs_size = (uint)cs->size();
    if (cs_size == 0) {
      continue;  // skip trivial section
    }
    assert((_write_position - code_offset) == sca_cs[i]._offset, "%d != %d", _write_position, sca_cs[i]._offset);
    // Write code
    n = write_bytes(cs->start(), cs_size);
    if (n != cs_size) {
      return false;
    }
    if (!align_write()) {
      return false;
    }
  }
  assert((_write_position - code_offset) == (offset + total_size), "(%d - %d) != (%d + %d)", _write_position, code_offset, offset, total_size);
  code_size = total_size;
  return true;
}

bool SCAFile::store_exception_blob(CodeBuffer* buffer, int pc_offset) {
  SCAFile* archive = open_for_write();
  if (archive == nullptr) {
    return false;
  }
  log_info(sca, stubs)("Writing blob '%s' to shared code archive '%s'", buffer->name(), archive->_archive_path);

#ifdef ASSERT
if (UseNewCode3) {
  FlagSetting fs(PrintRelocations, true);
  buffer->print();
  buffer->decode();
}
#endif
  if (!archive->align_write()) {
    return false;
  }
  uint entry_position = archive->_write_position;

  // Write pc_offset
  uint n = archive->write_bytes(&pc_offset, sizeof(int));
  if (n != sizeof(int)) {
    return false;
  }

  // Write name
  const char* name = buffer->name();
  uint name_offset = archive->_write_position  - entry_position;
  uint name_size = (uint)strlen(name) + 1; // Includes '/0'
  n = archive->write_bytes(name, name_size);
  if (n != name_size) {
    return false;
  }

  // Write code section
  if (!archive->align_write()) {
    return false;
  }
  uint code_offset = archive->_write_position;
  uint code_size = 0;
  if (!archive->write_code(buffer, code_size)) {
    return false;
  }
  // Write relocInfo array
  uint reloc_offset = archive->_write_position - entry_position;
  uint reloc_size = 0;
  if (!archive->write_relocations(buffer, reloc_size)) {
    return false;
  }

  uint entry_size = archive->_write_position - entry_position;
  SCAEntry entry(entry_position, entry_size, name_offset, name_size,
                 code_offset, code_size, reloc_offset, reloc_size,
                 SCAEntry::Blob, (uint32_t)999, archive->_header->next_idx());
  archive->add_entry(entry);
  log_info(sca, stubs)("Wrote stub '%s' to shared code archive '%s'", name, archive->_archive_path);
  return true;
}

DebugInformationRecorder* SCAReader::read_debug_info(OopRecorder* oop_recorder) {
  uint code_offset = align_up(read_position(), DATA_ALIGNMENT);
  int data_size  = *(int*)addr(code_offset);
  code_offset   += sizeof(int);
  int pcs_length = *(int*)addr(code_offset);
  code_offset   += sizeof(int);

if (UseNewCode) {
  tty->print_cr("======== read DebugInfo [%d, %d]:", data_size, pcs_length);
}

  // Aligned initial sizes
  int data_size_align  = align_up(data_size, DATA_ALIGNMENT);
  int pcs_length_align = pcs_length + 1;
  assert(sizeof(PcDesc) > DATA_ALIGNMENT, "sanity");
  DebugInformationRecorder* recorder = new DebugInformationRecorder(oop_recorder, data_size_align, pcs_length);

  copy_bytes(addr(code_offset), recorder->stream()->buffer(), data_size_align);
  recorder->stream()->set_position(data_size);
  code_offset += data_size;

  uint pcs_size = pcs_length * sizeof(PcDesc);
  copy_bytes(addr(code_offset), (address)recorder->pcs(), pcs_size);
  code_offset += pcs_size;
  set_read_position(code_offset);
  return recorder;
}

bool SCAFile::write_debug_info(DebugInformationRecorder* recorder) {
  if (!align_write()) {
    return false;
  }
  int data_size = recorder->data_size(); // In bytes
  uint n = write_bytes(&data_size, sizeof(int));
  if (n != sizeof(int)) {
    return false;
  }
  int pcs_length = recorder->pcs_length(); // In bytes
  n = write_bytes(&pcs_length, sizeof(int));
  if (n != sizeof(int)) {
    return false;
  }
  n = write_bytes(recorder->stream()->buffer(), data_size);
  if (n != (uint)data_size) {
    return false;
  }
  uint pcs_size = pcs_length * sizeof(PcDesc);
  n = write_bytes(recorder->pcs(), pcs_size);
  if (n != pcs_size) {
    return false;
  }
  return true;
}

OopMapSet* SCAReader::read_oop_maps() {
  uint code_offset = read_position();
  int om_count = *(int*)addr(code_offset);
  code_offset += sizeof(int);

if (UseNewCode) {
  tty->print_cr("======== read oop maps [%d]:", om_count);
}

  OopMapSet* oop_maps = new OopMapSet(om_count);
  for (int i = 0; i < (int)om_count; i++) {
    int data_size = *(int*)addr(code_offset);
    code_offset += sizeof(int);

    OopMap* oop_map = new OopMap(data_size);
    // Preserve allocated stream
    CompressedWriteStream* stream = oop_map->write_stream();

    // Read data which overwrites default data
    copy_bytes(addr(code_offset), (address)oop_map, sizeof(OopMap));
    code_offset += sizeof(OopMap);
    stream->set_position(data_size);
    oop_map->set_write_stream(stream);
    if (data_size > 0) {
      copy_bytes(addr(code_offset), (address)(oop_map->data()), (uint)data_size);
      code_offset += data_size;
    }
    oop_maps->add(oop_map);
  }
  set_read_position(code_offset);
  return oop_maps;
}

bool SCAFile::write_oop_maps(OopMapSet* oop_maps) {
  uint om_count = oop_maps->size();
  uint n = write_bytes(&om_count, sizeof(int));
  if (n != sizeof(int)) {
    return false;
  }
  for (int i = 0; i < (int)om_count; i++) {
    OopMap* om = oop_maps->at(i);
    int data_size = om->data_size();
    n = write_bytes(&data_size, sizeof(int));
    if (n != sizeof(int)) {
      return false;
    }
    n = write_bytes(om, sizeof(OopMap));
    if (n != sizeof(OopMap)) {
      return false;
    }
    n = write_bytes(om->data(), (uint)data_size);
    if (n != (uint)data_size) {
      return false;
    }
  }
  return true;
}

jobject SCAReader::read_oop(JavaThread* thread, const methodHandle& comp_method) {
  uint code_offset = read_position();
  oop obj = nullptr;
  DataKind kind = *(DataKind*)addr(code_offset);
  code_offset += sizeof(DataKind);
  set_read_position(code_offset);
  if (kind == DataKind::Null) {
    return nullptr;
  } else if (kind == DataKind::No_Data) {
    return (jobject)Universe::non_oop_word();
  } else if (kind == DataKind::Klass) {
    Klass* k = read_klass(comp_method);
    if (k == nullptr) {
      return nullptr;
    }
    obj = k->java_mirror();
  } else if (kind == DataKind::Primitive) {
    code_offset = read_position();
    int t = *(int*)addr(code_offset);
    code_offset += sizeof(int);
    set_read_position(code_offset);
    BasicType bt = (BasicType)t;
    obj = java_lang_Class::primitive_mirror(bt);
    log_info(sca)("Read primitive type klass: %s", type2name(bt));
  } else if (kind == DataKind::String) {
    code_offset = read_position();
    int length = *(int*)addr(code_offset);
    code_offset += sizeof(int);
    set_read_position(code_offset);
    const char* dest = addr(code_offset);
    set_read_position(code_offset + length);
    obj = StringTable::intern(&(dest[0]), thread);
    if (obj == nullptr) {
      set_lookup_failed();
      log_info(sca)("Lookup failed for String %s", &(dest[0]));
      return nullptr;
    }
    assert(java_lang_String::is_instance(obj), "must be string");
    log_info(sca)("Read String: %s", dest);
  } else if (kind == DataKind::SysLoader) {
    obj = SystemDictionary::java_system_loader();
    log_info(sca)("Read java_system_loader");
  } else if (kind == DataKind::PlaLoader) {
    obj = SystemDictionary::java_platform_loader();
    log_info(sca)("Read java_platform_loader");
  } else {
    set_lookup_failed();
    log_info(sca)("Unknown oop's kind: %d", (int)kind);
    return nullptr;
  }
  return JNIHandles::make_local(thread, obj);
}

bool SCAReader::read_oops(OopRecorder* oop_recorder, ciMethod* target) {
  uint code_offset = read_position();
  int oop_count = *(int*)addr(code_offset);
  code_offset += sizeof(int);
  set_read_position(code_offset);
if (UseNewCode) {
  tty->print_cr("======== read oops [%d]:", oop_count);
}
  if (oop_count == 0) {
    return true;
  }
  {
    VM_ENTRY_MARK;
    methodHandle comp_method(THREAD, target->get_Method());
    for (int i = 0; i < oop_count; i++) {
      jobject jo = read_oop(THREAD, comp_method);
      if (lookup_failed()) {
        return false;
      }
      oop_recorder->find_index(jo);
if (UseNewCode) {
      tty->print("%d: " INTPTR_FORMAT " ", i, p2i(jo));
      if (jo == (jobject)Universe::non_oop_word()) {
        tty->print("non-oop word");
      } else if (jo == nullptr) {
        tty->print("nullptr-oop");
      } else {
        JNIHandles::resolve(jo)->print_value_on(tty);
      }
      tty->cr();
}
    }
  }
  return true;
}

Metadata* SCAReader::read_metadata(const methodHandle& comp_method) {
  uint code_offset = read_position();
  Metadata* m = nullptr;
  DataKind kind = *(DataKind*)addr(code_offset);
  code_offset += sizeof(DataKind);
  set_read_position(code_offset);
  if (kind == DataKind::Null) {
    m = (Metadata*)nullptr;
  } else if (kind == DataKind::No_Data) {
    m = (Metadata*)Universe::non_oop_word();
  } else if (kind == DataKind::Klass) {
    m = (Metadata*)read_klass(comp_method);
  } else if (kind == DataKind::Method) {
    m = (Metadata*)read_method(comp_method);
  } else {
    set_lookup_failed();
    log_info(sca)("Unknown metadata's kind: %d", (int)kind);
  }
  return m;
}

bool SCAReader::read_metadata(OopRecorder* oop_recorder, ciMethod* target) {
  uint code_offset = read_position();
  int metadata_count = *(int*)addr(code_offset);
  code_offset += sizeof(int);
  set_read_position(code_offset);
if (UseNewCode) {
  tty->print_cr("======== read metadata [%d]:", metadata_count);
}
  if (metadata_count == 0) {
    return true;
  }
  {
    VM_ENTRY_MARK;
    methodHandle comp_method(THREAD, target->get_Method());

    for (int i = 0; i < metadata_count; i++) {
      Metadata* m = read_metadata(comp_method);
      if (lookup_failed()) {
        return false;
      }
      oop_recorder->find_index(m);
if (UseNewCode) {
     tty->print("%d: " INTPTR_FORMAT " ", i, p2i(m));
      if (m == (Metadata*)Universe::non_oop_word()) {
        tty->print("non-metadata word");
      } else if (m == nullptr) {
        tty->print("nullptr-oop");
      } else {
        Metadata::print_value_on_maybe_null(tty, m);
      }
      tty->cr();
}
    }
  }
  return true;
}

bool SCAFile::write_oop(jobject& jo) {
  DataKind kind;
  uint n = 0;
  oop obj = JNIHandles::resolve(jo);
  if (jo == nullptr) {
    kind = DataKind::Null;
    n = write_bytes(&kind, sizeof(int));
    if (n != sizeof(int)) {
      return false;
    }
  } else if (jo == (jobject)Universe::non_oop_word()) {
    kind = DataKind::No_Data;
    n = write_bytes(&kind, sizeof(int));
    if (n != sizeof(int)) {
      return false;
    }
  } else if (java_lang_Class::is_instance(obj)) {
    if (java_lang_Class::is_primitive(obj)) {
      int bt = (int)java_lang_Class::primitive_type(obj);
      kind = DataKind::Primitive;
      n = write_bytes(&kind, sizeof(int));
      if (n != sizeof(int)) {
        return false;
      }
      n = write_bytes(&bt, sizeof(int));
      if (n != sizeof(int)) {
        return false;
      }
      log_info(sca)("Write primitive type klass: %s", type2name((BasicType)bt));
    } else {
      Klass* klass = java_lang_Class::as_Klass(obj);
      if (!write_klass(klass)) {
        return false;
      }
    }
  } else if (java_lang_String::is_instance(obj)) {
    kind = DataKind::String;
    n = write_bytes(&kind, sizeof(int));
    if (n != sizeof(int)) {
      return false;
    }
    ResourceMark rm;
    int length = 0;
    const char* string = java_lang_String::as_utf8_string(obj, length);
    length++; // write tailing '/0'
    n = write_bytes(&length, sizeof(int));
    if (n != sizeof(int)) {
      return false;
    }
    n = write_bytes(string, (uint)length);
    if (n != (uint)length) {
      return false;
    }
    log_info(sca)("Write String: %s", string);
  } else if (java_lang_Module::is_instance(obj)) {
    fatal("Module object unimplemented");
  } else if (java_lang_ClassLoader::is_instance(obj)) {
    if (obj == SystemDictionary::java_system_loader()) {
      kind = DataKind::SysLoader;
      log_info(sca)("Write ClassLoader: java_system_loader");
    } else if (obj == SystemDictionary::java_platform_loader()) {
      kind = DataKind::PlaLoader;
      log_info(sca)("Write ClassLoader: java_platform_loader");
    } else {
      fatal("ClassLoader object unimplemented");
      return false;
    }
    n = write_bytes(&kind, sizeof(int));
    if (n != sizeof(int)) {
      return false;
    }
  } else {
    // Unhandled oop - bailout
    set_lookup_failed();
    log_info(sca, nmethod)("Unhandled obj: " PTR_FORMAT " : %s", p2i(obj), obj->klass()->external_name());
    return false;
  }
  return true;
}

bool SCAFile::write_oops(OopRecorder* oop_recorder) {
  int oop_count = oop_recorder->oop_count();
  uint n = write_bytes(&oop_count, sizeof(int));
  if (n != sizeof(int)) {
    return false;
  }
if (UseNewCode3) {
  tty->print_cr("======== write oops [%d]:", oop_count);
}
  for (int i = 0; i < oop_count; i++) {
    jobject jo = oop_recorder->oop_at(i);
if (UseNewCode3) {
     tty->print("%d: " INTPTR_FORMAT " ", i, p2i(jo));
      if (jo == (jobject)Universe::non_oop_word()) {
        tty->print("non-oop word");
      } else if (jo == nullptr) {
        tty->print("nullptr-oop");
      } else {
        JNIHandles::resolve(jo)->print_value_on(tty);
      }
      tty->cr();
}
    if (!write_oop(jo)) {
      return false;
    }
  }
  return true;
}

bool SCAFile::write_metadata(Metadata* m) {
  uint n = 0;
  if (m == nullptr) {
    DataKind kind = DataKind::Null;
    n = write_bytes(&kind, sizeof(int));
    if (n != sizeof(int)) {
      return false;
    }
  } else if (m == (Metadata*)Universe::non_oop_word()) {
    DataKind kind = DataKind::No_Data;
    n = write_bytes(&kind, sizeof(int));
    if (n != sizeof(int)) {
      return false;
    }
  } else if (m->is_klass()) {
    if (!write_klass((Klass*)m)) {
      return false;
    }
  } else if (m->is_method()) {
    if (!write_method((Method*)m)) {
      return false;
    }
  } else { // Not supported
    fatal("metadata : " INTPTR_FORMAT " unimplemented", p2i(m));
    return false;
  }
  return true;
}

bool SCAFile::write_metadata(OopRecorder* oop_recorder) {
  int metadata_count = oop_recorder->metadata_count();
  uint n = write_bytes(&metadata_count, sizeof(int));
  if (n != sizeof(int)) {
    return false;
  }

if (UseNewCode3) {
  tty->print_cr("======== write metadata [%d]:", metadata_count);
}
  for (int i = 0; i < metadata_count; i++) {
    Metadata* m = oop_recorder->metadata_at(i);
if (UseNewCode3) {
     tty->print("%d: " INTPTR_FORMAT " ", i, p2i(m));
      if (m == (Metadata*)Universe::non_oop_word()) {
        tty->print("non-metadata word");
      } else if (m == nullptr) {
        tty->print("nillptr-oop");
      } else {
        Metadata::print_value_on_maybe_null(tty, m);
      }
      tty->cr();
}
    if (!write_metadata(m)) {
      return false;
    }
  }
  return true;
}

bool SCAReader::read_dependencies(Dependencies* dependencies) {
  uint code_offset = read_position();
  int dependencies_size = *(int*)addr(code_offset);
if (UseNewCode) {
  tty->print_cr("======== read dependencies [%d]:", dependencies_size);
}
  code_offset += sizeof(int);
  code_offset = align_up(code_offset, DATA_ALIGNMENT);
  if (dependencies_size > 0) {
    dependencies->set_content((u_char*)addr(code_offset), dependencies_size);
  }
  code_offset += dependencies_size;
  set_read_position(code_offset);
  return true;
}

bool SCAFile::load_nmethod(ciEnv* env, ciMethod* target, int entry_bci, AbstractCompiler* compiler) {
  if (entry_bci != InvocationEntryBci) {
    return false; // No OSR
  }
  if (!compiler->is_c2()) {
    return false; // Only C2 now
  }
  SCAFile* archive = open_for_read();
  if (archive == nullptr) {
    return false;
  }
  if (!archive->_table->opto_complete()) {
    return false; // C2 runtime stubs are not ready yet
  }

  ReadingMark rdmk;

  const char* target_name = nullptr;
  uint decomp = (target->method_data() == nullptr) ? 0 : target->method_data()->decompile_count();
  {
    VM_ENTRY_MARK;
    ResourceMark rm;
    methodHandle method(THREAD, target->get_Method());
    target_name = os::strdup(method->name_and_sig_as_C_string());
  }
  uint hash = java_lang_String::hash_code((const jbyte*)target_name, strlen(target_name));
  log_info(sca, nmethod)("Reading nmethod '%s' (decomp: %d) from shared code archive '%s'", target_name, decomp, archive->_archive_path);

  SCAEntry* entry = archive->find_entry(SCAEntry::Code, hash, decomp);
  if (entry == nullptr) {
    log_info(sca)("Missing entry for '%s' hash: " UINT32_FORMAT_X_0 ", decomp: %d", target_name, hash, decomp);
    return false;
  }

  SCAReader reader(archive, entry);
  return reader.compile(env, target, entry_bci, compiler, target_name);
}

SCAReader::SCAReader(SCAFile* archive, SCAEntry* entry) {
  _archive = archive;
  _entry   = entry;
  _load_buffer = archive->archive_buffer();
  _read_position = 0;
  _lookup_failed = false;
}

bool SCAReader::compile(ciEnv* env, ciMethod* target, int entry_bci, AbstractCompiler* compiler, const char* target_name) {
if (UseNewCode) tty->print_cr("=== load_nmethod: 1");

  uint entry_position = _entry->offset();
  set_read_position(entry_position);

  // Read name
  uint name_offset = entry_position + _entry->name_offset();
  uint name_size = _entry->name_size(); // Includes '/0'
  const char* name     = addr(name_offset);

  if (strncmp(target_name, name, (name_size - 1)) != 0) {
    log_warning(sca)("Saved nmethod's name '%s' is different from '%s'", name, target_name);
    set_lookup_failed();
    return false;
  }
  os::free((void*)target_name);

  uint code_offset = entry_position + _entry->code_offset();
  set_read_position(code_offset);

if (UseNewCode) tty->print_cr("=== load_nmethod: 2");

  // Read flags
  int flags = *(int*)addr(code_offset);
  code_offset += sizeof(int);
  bool has_monitors      = (flags & 0xFF) > 0;
  bool has_wide_vectors  = ((flags >>  8) & 0xFF) > 0;
  bool has_unsafe_access = ((flags >> 16) & 0xFF) > 0;

  int orig_pc_offset = *(int*)addr(code_offset);
  code_offset += sizeof(int);
  int frame_size = *(int*)addr(code_offset);
  code_offset += sizeof(int);

  // Read offsets
  CodeOffsets* offsets = (CodeOffsets*)addr(code_offset);
  code_offset += sizeof(CodeOffsets);

if (UseNewCode) tty->print_cr("=== load_nmethod: 3; %d, %d, %d, %d", flags, orig_pc_offset, frame_size, code_offset);

  // Create Debug Information Recorder to record scopes, oopmaps, etc.
  OopRecorder* oop_recorder = new OopRecorder(env->arena());
  env->set_oop_recorder(oop_recorder);

  set_read_position(code_offset);

  // Write OopRecorder data
  if (!read_oops(oop_recorder, target)) {
    return false;
  }
  if (!read_metadata(oop_recorder, target)) {
    return false;
  }

  // Read Debug info
  DebugInformationRecorder* recorder = read_debug_info(oop_recorder);
  if (recorder == nullptr) {
    return false;
  }
  env->set_debug_info(recorder);

if (UseNewCode) tty->print_cr("=== load_nmethod: 4");

  // Read Dependencies (compressed already)
  Dependencies* dependencies = new Dependencies(env);
  if (!read_dependencies(dependencies)) {
    return false;
  }
  env->set_dependencies(dependencies);

  // Read oop maps
  OopMapSet* oop_maps = read_oop_maps();
  if (oop_maps == nullptr) {
    return false;
  }

  // Read exception handles
  code_offset = read_position();
  int exc_table_length = *(int*)addr(code_offset);
  code_offset += sizeof(int);
  ExceptionHandlerTable handler_table(MAX2(exc_table_length, 4));
  if (exc_table_length > 0) {
    handler_table.set_length(exc_table_length);
    uint exc_table_size = handler_table.size_in_bytes();
    copy_bytes(addr(code_offset), (address)handler_table.table(), exc_table_size);
    code_offset += exc_table_size;
  }

  // Read null check table
  int nul_chk_length = *(int*)addr(code_offset);
  code_offset += sizeof(int);
  ImplicitExceptionTable nul_chk_table;
  if (nul_chk_length > 0) {
    nul_chk_table.set_size(nul_chk_length);
    nul_chk_table.set_len(nul_chk_length);
    uint nul_chk_size = nul_chk_table.size_in_bytes();
    copy_bytes(addr(code_offset), (address)nul_chk_table.data(), nul_chk_size);
    code_offset += nul_chk_size;
  }

if (UseNewCode) tty->print_cr("=== load_nmethod: 5");

  uint reloc_max_size = _entry->reloc_size();
  CodeBuffer buffer("Compile::Fill_buffer", _entry->code_size(), reloc_max_size);
  buffer.initialize_oop_recorder(oop_recorder);

  // Create fake original CodeBuffer
  CodeBuffer orig_buffer(name);

  // Read code
  if (!read_code(&buffer, &orig_buffer, align_up(code_offset, DATA_ALIGNMENT))) {
    return false;
  }

  // Read relocations
  uint reloc_offset = entry_position + _entry->reloc_offset();
  set_read_position(reloc_offset);
  if (!read_relocations(&buffer, &orig_buffer, reloc_max_size, oop_recorder, target)) {
    return false;
  }

  log_info(sca, nmethod)("Read nmethod '%s' from shared code archive '%s'", name, _archive->archive_path());
#ifdef ASSERT
if (UseNewCode3) {
  FlagSetting fs(PrintRelocations, true);
  buffer.print();
  buffer.decode();
}
#endif

  if (VerifySharedCode) {
    return false;
  }
  // Register nmethod
  env->register_method(target, entry_bci,
                       offsets, orig_pc_offset,
                       &buffer, frame_size,
                       oop_maps, &handler_table,
                       &nul_chk_table, compiler,
                       has_unsafe_access,
                       has_wide_vectors,
                       has_monitors,
                       0, NoRTM,
                       (SCAEntry *)_entry);
  env->task()->set_num_inlined_bytecodes(_entry->num_inlined_bytecodes());
  return true;
}

// No concurency for writing to archive file because this method is called from
// ciEnv::register_method() under MethodCompileQueue_lock and Compile_lock locks.
SCAEntry* SCAFile::store_nmethod(const methodHandle& method,
                     int compile_id,
                     int entry_bci,
                     CodeOffsets* offsets,
                     int orig_pc_offset,
                     DebugInformationRecorder* recorder,
                     Dependencies* dependencies,
                     CodeBuffer* buffer,
                     int frame_size,
                     OopMapSet* oop_maps,
                     ExceptionHandlerTable* handler_table,
                     ImplicitExceptionTable* nul_chk_table,
                     AbstractCompiler* compiler,
                     bool has_unsafe_access,
                     bool has_wide_vectors,
                     bool has_monitors) {
  if (entry_bci != InvocationEntryBci) {
    return nullptr; // No OSR
  }
  if (!compiler->is_c2()) {
    return nullptr; // Only C2 now
  }
  SCAFile* archive = open_for_write();
  if (archive == nullptr) {
    return nullptr; // Archive is closed
  }
#ifdef ASSERT
if (UseNewCode3) {
  FlagSetting fs(PrintRelocations, true);
  buffer->print();
  buffer->decode();
}
#endif
  if (!archive->align_write()) {
    return nullptr;
  }
  uint entry_position = archive->_write_position;

  assert(entry_bci == InvocationEntryBci, "No OSR");

  // Finalize OopRecorder data here because after we query
  // the size of its data it can't be updated any more.
  buffer->finalize_oop_references(method);

  // Write name
  uint name_offset = 0;
  uint name_size   = 0;
  uint hash = 0;
  uint n;
  {
    ResourceMark rm;
    const char* name   = method->name_and_sig_as_C_string();
    log_info(sca, nmethod)("Writing nmethod '%s' to shared code archive '%s'", name, archive->_archive_path);

    name_offset = archive->_write_position  - entry_position;
    name_size   = (uint)strlen(name) + 1; // Includes '/0'
    n = archive->write_bytes(name, name_size);
    if (n != name_size) {
      return nullptr;
    }
    hash = java_lang_String::hash_code((const jbyte*)name, strlen(name));
  }

  if (!archive->align_write()) {
    return nullptr;
  }

  uint code_offset = archive->_write_position - entry_position;

  int flags = ((has_unsafe_access ? 1 : 0) << 16) | ((has_wide_vectors ? 1 : 0) << 8) | (has_monitors ? 1 : 0);
  n = archive->write_bytes(&flags, sizeof(int));
  if (n != sizeof(int)) {
    return nullptr;
  }

  n = archive->write_bytes(&orig_pc_offset, sizeof(int));
  if (n != sizeof(int)) {
    return nullptr;
  }

  n = archive->write_bytes(&frame_size, sizeof(int));
  if (n != sizeof(int)) {
    return nullptr;
  }

  // Write offsets
  n = archive->write_bytes(offsets, sizeof(CodeOffsets));
  if (n != sizeof(CodeOffsets)) {
    return nullptr;
  }

  // Write OopRecorder data
  if (!archive->write_oops(buffer->oop_recorder())) {
    if (archive->lookup_failed() && !archive->failed()) {
      // Skip this method and reposition file
      archive->set_write_position(entry_position);
    }
    return nullptr;
  }
  if (!archive->write_metadata(buffer->oop_recorder())) {
    return nullptr;
  }

  // Write Debug info
  if (!archive->write_debug_info(recorder)) {
    return nullptr;
  }
  // Write Dependencies
  int dependencies_size = (int)dependencies->size_in_bytes();
  n = archive->write_bytes(&dependencies_size, sizeof(int));
  if (n != sizeof(int)) {
    return nullptr;
  }
  if (!archive->align_write()) {
    return nullptr;
  }
  n = archive->write_bytes(dependencies->content_bytes(), dependencies_size);
  if (n != (uint)dependencies_size) {
    return nullptr;
  }

  // Write oop maps
  if (!archive->write_oop_maps(oop_maps)) {
    return nullptr;
  }

  // Write exception handles
  int exc_table_length = handler_table->length();
  n = archive->write_bytes(&exc_table_length, sizeof(int));
  if (n != sizeof(int)) {
    return nullptr;
  }
  uint exc_table_size = handler_table->size_in_bytes();
  n = archive->write_bytes(handler_table->table(), exc_table_size);
  if (n != exc_table_size) {
    return nullptr;
  }

  // Write null check table
  int nul_chk_length = nul_chk_table->len();
  n = archive->write_bytes(&nul_chk_length, sizeof(int));
  if (n != sizeof(int)) {
    return nullptr;
  }
  uint nul_chk_size = nul_chk_table->size_in_bytes();
  n = archive->write_bytes(nul_chk_table->data(), nul_chk_size);
  if (n != nul_chk_size) {
    return nullptr;
  }

  // Write code section
  if (!archive->align_write()) {
    return nullptr;
  }
  uint code_size = 0;
  if (!archive->write_code(buffer, code_size)) {
    return nullptr;
  }
  // Write relocInfo array
  uint reloc_offset = archive->_write_position - entry_position;
  uint reloc_size = 0;
  if (!archive->write_relocations(buffer, reloc_size)) {
    if (archive->lookup_failed() && !archive->failed()) {
      // Skip this method and reposition file
      archive->set_write_position(entry_position);
    }
    return nullptr;
  }
  uint decomp = (method->method_data() == nullptr) ? 0 : method->method_data()->decompile_count();
  uint entry_size = archive->_write_position - entry_position;
  SCAEntry entry(entry_position, entry_size, name_offset, name_size,
                 code_offset, code_size, reloc_offset, reloc_size,
                 SCAEntry::Code, hash, archive->_header->next_idx(), decomp);
  {
    ResourceMark rm;
    const char* name   = method->name_and_sig_as_C_string();
    log_info(sca, nmethod)("Wrote nmethod '%s' to shared code archive '%s'", name, archive->_archive_path);
  }
  return archive->add_entry(entry);
}

#define _extrs_max 20
#define _stubs_max 110
#define _blobs_max 40
#define _all_max 170

#define SET_ADDRESS(type, addr)                          \
  {                                                      \
    type##_addr[type##_length++] = (address) (addr);     \
    assert(type##_length < type##_max, "increase size"); \
  }

void SCAddressTable::init() {
  assert(!_complete, "init only once");
  _extrs_addr = NEW_C_HEAP_ARRAY(address, _extrs_max, mtCode);
  _stubs_addr = NEW_C_HEAP_ARRAY(address, _stubs_max, mtCode);
  _blobs_addr = NEW_C_HEAP_ARRAY(address, _blobs_max, mtCode);

  _extrs_length = 0;
  _stubs_length = 0;
  _blobs_length = 0;

  // Runtime methods
#ifdef COMPILER2
  SET_ADDRESS(_extrs, OptoRuntime::handle_exception_C);
#endif
  SET_ADDRESS(_extrs, CompressedOops::ptrs_base_addr());
  SET_ADDRESS(_extrs, G1BarrierSetRuntime::write_ref_field_post_entry);
  SET_ADDRESS(_extrs, G1BarrierSetRuntime::write_ref_field_pre_entry);

  SET_ADDRESS(_extrs, SharedRuntime::complete_monitor_unlocking_C);
  SET_ADDRESS(_extrs, SharedRuntime::enable_stack_reserved_zone);
  SET_ADDRESS(_extrs, ci_card_table_address_as<address>());
  SET_ADDRESS(_extrs, ThreadIdentifier::unsafe_offset());

  SET_ADDRESS(_extrs, os::javaTimeMillis);
  SET_ADDRESS(_extrs, os::javaTimeNanos);

#ifndef PRODUCT
  SET_ADDRESS(_extrs, &SharedRuntime::_partial_subtype_ctr);
  SET_ADDRESS(_extrs, JavaThread::verify_cross_modify_fence_failure);
#endif

#if defined(AMD64) || defined(AARCH64) || defined(RISCV64)
  SET_ADDRESS(_extrs, MacroAssembler::debug64);
#endif
#if defined(AMD64)
  SET_ADDRESS(_extrs, StubRoutines::x86::arrays_hashcode_powers_of_31());
#endif

  // Stubs
  SET_ADDRESS(_stubs, StubRoutines::method_entry_barrier());
  SET_ADDRESS(_stubs, StubRoutines::forward_exception_entry());
/*
  SET_ADDRESS(_stubs, StubRoutines::throw_AbstractMethodError_entry());
  SET_ADDRESS(_stubs, StubRoutines::throw_IncompatibleClassChangeError_entry());
  SET_ADDRESS(_stubs, StubRoutines::throw_NullPointerException_at_call_entry());
  SET_ADDRESS(_stubs, StubRoutines::throw_StackOverflowError_entry());
  SET_ADDRESS(_stubs, StubRoutines::throw_delayed_StackOverflowError_entry());
*/
  SET_ADDRESS(_stubs, StubRoutines::atomic_xchg_entry());
  SET_ADDRESS(_stubs, StubRoutines::atomic_cmpxchg_entry());
  SET_ADDRESS(_stubs, StubRoutines::atomic_cmpxchg_long_entry());
  SET_ADDRESS(_stubs, StubRoutines::atomic_add_entry());
  SET_ADDRESS(_stubs, StubRoutines::fence_entry());

  SET_ADDRESS(_stubs, StubRoutines::cont_thaw());
  SET_ADDRESS(_stubs, StubRoutines::cont_returnBarrier());
  SET_ADDRESS(_stubs, StubRoutines::cont_returnBarrierExc());

  JFR_ONLY(SET_ADDRESS(_stubs, StubRoutines::jfr_write_checkpoint());)


  SET_ADDRESS(_stubs, StubRoutines::jbyte_arraycopy());
  SET_ADDRESS(_stubs, StubRoutines::jshort_arraycopy());
  SET_ADDRESS(_stubs, StubRoutines::jint_arraycopy());
  SET_ADDRESS(_stubs, StubRoutines::jlong_arraycopy());
  SET_ADDRESS(_stubs, StubRoutines::_oop_arraycopy);
  SET_ADDRESS(_stubs, StubRoutines::_oop_arraycopy_uninit);

  SET_ADDRESS(_stubs, StubRoutines::jbyte_disjoint_arraycopy());
  SET_ADDRESS(_stubs, StubRoutines::jshort_disjoint_arraycopy());
  SET_ADDRESS(_stubs, StubRoutines::jint_disjoint_arraycopy());
  SET_ADDRESS(_stubs, StubRoutines::jlong_disjoint_arraycopy());
  SET_ADDRESS(_stubs, StubRoutines::_oop_disjoint_arraycopy);
  SET_ADDRESS(_stubs, StubRoutines::_oop_disjoint_arraycopy_uninit);

  SET_ADDRESS(_stubs, StubRoutines::arrayof_jbyte_arraycopy());
  SET_ADDRESS(_stubs, StubRoutines::arrayof_jshort_arraycopy());
  SET_ADDRESS(_stubs, StubRoutines::arrayof_jint_arraycopy());
  SET_ADDRESS(_stubs, StubRoutines::arrayof_jlong_arraycopy());
  SET_ADDRESS(_stubs, StubRoutines::_arrayof_oop_arraycopy);
  SET_ADDRESS(_stubs, StubRoutines::_arrayof_oop_arraycopy_uninit);

  SET_ADDRESS(_stubs, StubRoutines::arrayof_jbyte_disjoint_arraycopy());
  SET_ADDRESS(_stubs, StubRoutines::arrayof_jshort_disjoint_arraycopy());
  SET_ADDRESS(_stubs, StubRoutines::arrayof_jint_disjoint_arraycopy());
  SET_ADDRESS(_stubs, StubRoutines::arrayof_jlong_disjoint_arraycopy());
  SET_ADDRESS(_stubs, StubRoutines::_arrayof_oop_disjoint_arraycopy);
  SET_ADDRESS(_stubs, StubRoutines::_arrayof_oop_disjoint_arraycopy_uninit);

  SET_ADDRESS(_stubs, StubRoutines::_checkcast_arraycopy);
  SET_ADDRESS(_stubs, StubRoutines::_checkcast_arraycopy_uninit);

  SET_ADDRESS(_stubs, StubRoutines::unsafe_arraycopy());
  SET_ADDRESS(_stubs, StubRoutines::generic_arraycopy());

  SET_ADDRESS(_stubs, StubRoutines::jbyte_fill());
  SET_ADDRESS(_stubs, StubRoutines::jshort_fill());
  SET_ADDRESS(_stubs, StubRoutines::jint_fill());
  SET_ADDRESS(_stubs, StubRoutines::arrayof_jbyte_fill());
  SET_ADDRESS(_stubs, StubRoutines::arrayof_jshort_fill());
  SET_ADDRESS(_stubs, StubRoutines::arrayof_jint_fill());

  SET_ADDRESS(_stubs, StubRoutines::data_cache_writeback());
  SET_ADDRESS(_stubs, StubRoutines::data_cache_writeback_sync());

  SET_ADDRESS(_stubs, StubRoutines::aescrypt_encryptBlock());
  SET_ADDRESS(_stubs, StubRoutines::aescrypt_decryptBlock());
  SET_ADDRESS(_stubs, StubRoutines::cipherBlockChaining_encryptAESCrypt());
  SET_ADDRESS(_stubs, StubRoutines::cipherBlockChaining_decryptAESCrypt());
  SET_ADDRESS(_stubs, StubRoutines::electronicCodeBook_encryptAESCrypt());
  SET_ADDRESS(_stubs, StubRoutines::electronicCodeBook_decryptAESCrypt());
  SET_ADDRESS(_stubs, StubRoutines::poly1305_processBlocks());
  SET_ADDRESS(_stubs, StubRoutines::counterMode_AESCrypt());
  SET_ADDRESS(_stubs, StubRoutines::ghash_processBlocks());
  SET_ADDRESS(_stubs, StubRoutines::chacha20Block());
  SET_ADDRESS(_stubs, StubRoutines::base64_encodeBlock());
  SET_ADDRESS(_stubs, StubRoutines::base64_decodeBlock());
  SET_ADDRESS(_stubs, StubRoutines::md5_implCompress());
  SET_ADDRESS(_stubs, StubRoutines::md5_implCompressMB());
  SET_ADDRESS(_stubs, StubRoutines::sha1_implCompress());
  SET_ADDRESS(_stubs, StubRoutines::sha1_implCompressMB());
  SET_ADDRESS(_stubs, StubRoutines::sha256_implCompress());
  SET_ADDRESS(_stubs, StubRoutines::sha256_implCompressMB());
  SET_ADDRESS(_stubs, StubRoutines::sha512_implCompress());
  SET_ADDRESS(_stubs, StubRoutines::sha512_implCompressMB());
  SET_ADDRESS(_stubs, StubRoutines::sha3_implCompress());
  SET_ADDRESS(_stubs, StubRoutines::sha3_implCompressMB());

  SET_ADDRESS(_stubs, StubRoutines::updateBytesCRC32());
  SET_ADDRESS(_stubs, StubRoutines::crc_table_addr());

  SET_ADDRESS(_stubs, StubRoutines::crc32c_table_addr());
  SET_ADDRESS(_stubs, StubRoutines::updateBytesCRC32C());
  SET_ADDRESS(_stubs, StubRoutines::updateBytesAdler32());

  SET_ADDRESS(_stubs, StubRoutines::multiplyToLen());
  SET_ADDRESS(_stubs, StubRoutines::squareToLen());
  SET_ADDRESS(_stubs, StubRoutines::mulAdd());
  SET_ADDRESS(_stubs, StubRoutines::montgomeryMultiply());
  SET_ADDRESS(_stubs, StubRoutines::montgomerySquare());
  SET_ADDRESS(_stubs, StubRoutines::bigIntegerRightShift());
  SET_ADDRESS(_stubs, StubRoutines::bigIntegerLeftShift());
  SET_ADDRESS(_stubs, StubRoutines::galoisCounterMode_AESCrypt());

  SET_ADDRESS(_stubs, StubRoutines::vectorizedMismatch());

  SET_ADDRESS(_stubs, StubRoutines::dexp());
  SET_ADDRESS(_stubs, StubRoutines::dlog());
  SET_ADDRESS(_stubs, StubRoutines::dlog10());
  SET_ADDRESS(_stubs, StubRoutines::dpow());
  SET_ADDRESS(_stubs, StubRoutines::dsin());
  SET_ADDRESS(_stubs, StubRoutines::dcos());
  SET_ADDRESS(_stubs, StubRoutines::dlibm_reduce_pi04l());
  SET_ADDRESS(_stubs, StubRoutines::dlibm_sin_cos_huge());
  SET_ADDRESS(_stubs, StubRoutines::dlibm_tan_cot_huge());
  SET_ADDRESS(_stubs, StubRoutines::dtan());

  SET_ADDRESS(_stubs, StubRoutines::f2hf_adr());
  SET_ADDRESS(_stubs, StubRoutines::hf2f_adr());

#if defined(AMD64)
  SET_ADDRESS(_stubs, StubRoutines::x86::d2i_fixup());
  SET_ADDRESS(_stubs, StubRoutines::x86::f2i_fixup());
  SET_ADDRESS(_stubs, StubRoutines::x86::d2l_fixup());
  SET_ADDRESS(_stubs, StubRoutines::x86::f2l_fixup());
  SET_ADDRESS(_stubs, StubRoutines::x86::float_sign_mask());
  SET_ADDRESS(_stubs, StubRoutines::x86::float_sign_flip());
  SET_ADDRESS(_stubs, StubRoutines::x86::double_sign_mask());
  SET_ADDRESS(_stubs, StubRoutines::x86::double_sign_flip());
#endif
#if defined(AARCH64)
  SET_ADDRESS(_stubs, StubRoutines::aarch64::d2i_fixup());
  SET_ADDRESS(_stubs, StubRoutines::aarch64::f2i_fixup());
  SET_ADDRESS(_stubs, StubRoutines::aarch64::d2l_fixup());
  SET_ADDRESS(_stubs, StubRoutines::aarch64::f2l_fixup());
  SET_ADDRESS(_stubs, StubRoutines::aarch64::float_sign_mask());
  SET_ADDRESS(_stubs, StubRoutines::aarch64::float_sign_flip());
  SET_ADDRESS(_stubs, StubRoutines::aarch64::double_sign_mask());
  SET_ADDRESS(_stubs, StubRoutines::aarch64::double_sign_flip());
#endif

  // Blobs
  SET_ADDRESS(_blobs, SharedRuntime::get_handle_wrong_method_stub());
  SET_ADDRESS(_blobs, SharedRuntime::get_ic_miss_stub());
  SET_ADDRESS(_blobs, SharedRuntime::get_resolve_opt_virtual_call_stub());
  SET_ADDRESS(_blobs, SharedRuntime::get_resolve_virtual_call_stub());
  SET_ADDRESS(_blobs, SharedRuntime::get_resolve_static_call_stub());
  SET_ADDRESS(_blobs, SharedRuntime::deopt_blob()->entry_point());
  SET_ADDRESS(_blobs, SharedRuntime::polling_page_safepoint_handler_blob()->entry_point());
  SET_ADDRESS(_blobs, SharedRuntime::polling_page_return_handler_blob()->entry_point());
#ifdef COMPILER2
  SET_ADDRESS(_blobs, SharedRuntime::polling_page_vectors_safepoint_handler_blob()->entry_point());
  SET_ADDRESS(_blobs, SharedRuntime::uncommon_trap_blob()->entry_point());
#endif
  SET_ADDRESS(_blobs, StubRoutines::throw_AbstractMethodError_entry());
  SET_ADDRESS(_blobs, StubRoutines::throw_IncompatibleClassChangeError_entry());
  SET_ADDRESS(_blobs, StubRoutines::throw_NullPointerException_at_call_entry());
  SET_ADDRESS(_blobs, StubRoutines::throw_StackOverflowError_entry());
  SET_ADDRESS(_blobs, StubRoutines::throw_delayed_StackOverflowError_entry());

  _complete = true;
}

void SCAddressTable::init_opto() {
#ifdef COMPILER2
  // OptoRuntime Blobs
  SET_ADDRESS(_blobs, OptoRuntime::exception_blob()->entry_point());
  SET_ADDRESS(_blobs, OptoRuntime::new_instance_Java());
  SET_ADDRESS(_blobs, OptoRuntime::new_array_Java());
  SET_ADDRESS(_blobs, OptoRuntime::new_array_nozero_Java());
  SET_ADDRESS(_blobs, OptoRuntime::multianewarray2_Java());
  SET_ADDRESS(_blobs, OptoRuntime::multianewarray3_Java());
  SET_ADDRESS(_blobs, OptoRuntime::multianewarray4_Java());
  SET_ADDRESS(_blobs, OptoRuntime::multianewarray5_Java());
  SET_ADDRESS(_blobs, OptoRuntime::multianewarrayN_Java());
  SET_ADDRESS(_blobs, OptoRuntime::vtable_must_compile_stub());
  SET_ADDRESS(_blobs, OptoRuntime::complete_monitor_locking_Java());
  SET_ADDRESS(_blobs, OptoRuntime::monitor_notify_Java());
  SET_ADDRESS(_blobs, OptoRuntime::monitor_notifyAll_Java());
  SET_ADDRESS(_blobs, OptoRuntime::rethrow_stub());
  SET_ADDRESS(_blobs, OptoRuntime::slow_arraycopy_Java());
  SET_ADDRESS(_blobs, OptoRuntime::register_finalizer_Java());
#endif
  _opto_complete = true;
}

#undef SET_ADDRESS
#undef _extrs_max
#undef _stubs_max
#undef _blobs_max

SCAddressTable::~SCAddressTable() {
  if (_extrs_addr != nullptr) {
    FREE_C_HEAP_ARRAY(address, _extrs_addr);
  }
  if (_stubs_addr != nullptr) {
    FREE_C_HEAP_ARRAY(address, _stubs_addr);
  }
  if (_blobs_addr != nullptr) {
    FREE_C_HEAP_ARRAY(address, _blobs_addr);
  }
}

#define MAX_STR_COUNT 200
static const char* _C_strings[MAX_STR_COUNT] = {nullptr};
static int _C_strings_count = 0;

bool SCAFile::load_strings() {
  uint strings_count  = _header->strings_count();
  if (strings_count == 0) {
    return true;
  }
  uint strings_offset = _header->strings_offset();
  uint strings_size   = _header->entries_offset() - strings_offset;
  uint sizes_size = (uint)(strings_count * sizeof(uint));
  uint* sizes = (uint*)addr(strings_offset);
  strings_size -= sizes_size;
  _C_strings_buf = addr(strings_offset + sizes_size);
  const char* p = _C_strings_buf;
  assert(strings_count <= MAX_STR_COUNT, "sanity");
  for (uint i = 0; i < strings_count; i++) {
    _C_strings[i] = p;
    p += sizes[i];
  }
  assert((uint)(p - _C_strings_buf) <= strings_size, "(" INTPTR_FORMAT " - " INTPTR_FORMAT ") = %d > %d ", p2i(p), p2i(_C_strings_buf), (uint)(p - _C_strings_buf), strings_size);
  _C_strings_count = strings_count;
  return true;
}

int SCAFile::store_strings() {
  uint offset = _write_position;
  uint length = 0;
  if (_C_strings_count > 0) {
    // Write sizes first
    for (int i = 0; i < _C_strings_count; i++) {
      uint len = (uint)strlen(_C_strings[i]) + 1; // Include 0
      length += len;
      assert(len < 1000, "big string: %s", _C_strings[i]);
      uint n = write_bytes(&len, sizeof(uint));
      if (n != sizeof(uint)) {
        return -1;
      }
    }
    for (int i = 0; i < _C_strings_count; i++) {
      uint len = (uint)strlen(_C_strings[i]) + 1; // Include 0
      uint n = write_bytes(_C_strings[i], len);
      if (n != len) {
        return -1;
      }
    }
    log_info(sca, exit)("Wrote %d C strings of total length %d at offset %d to shared code archive '%s'", _C_strings_count, length, offset, _archive_path);
  }
  return _C_strings_count;
}

void SCAFile::add_C_string(const char* str) {
  assert(for_write(), "only when storing code");
  _table->add_C_string(str);
}

void SCAddressTable::add_C_string(const char* str) {
  if (str != nullptr && _complete && _opto_complete) {
    // Check previous strings address
    for (int i = 0; i < _C_strings_count; i++) {
      if (_C_strings[i] == str) {
        return; // Found existing one
      }
    }
    // Add new one
    if (_C_strings_count < MAX_STR_COUNT) {
if (UseNewCode3) tty->print_cr("add_C_string: [%d] " INTPTR_FORMAT " %s", _C_strings_count, p2i(str), str);
      _C_strings[_C_strings_count++] = str;
    } else {
      log_warning(sca)("Number of C strings > max %d %s", MAX_STR_COUNT, str);
    }
  }
}

int SCAddressTable::id_for_C_string(address str) {
  for (int i = 0; i < _C_strings_count; i++) {
    if (_C_strings[i] == (const char*)str) {
      return i; // Found existing one
    }
  }
  return -1;
}

address SCAddressTable::address_for_C_string(int idx) {
  assert(idx < _C_strings_count, "sanity");
  return (address)_C_strings[idx];
}

int search_address(address addr, address* table, uint length) {
  for (int i = 0; i < (int)length; i++) {
    if (table[i] == addr) {
      return i;
    }
  }
  return -1;
}

address SCAddressTable::address_for_id(int idx) {
  if (!_complete) {
    fatal("SCA table is not complete");
  }
  if (idx == -1) {
    return (address)-1;
  }
  uint id = (uint)idx;
  if (id >= _all_max && idx < (_all_max + _C_strings_count)) {
    return address_for_C_string(idx - _all_max);
  }
  if (idx < 0 || id == (_extrs_length + _stubs_length + _blobs_length)) {
    fatal("Incorrect id %d for SCA table", id);
  }
  if (idx > (_all_max + _C_strings_count)) {
    return (address)os::init + idx;
  }
  if (id < _extrs_length) {
    return _extrs_addr[id];
  }
  id -= _extrs_length;
  if (id < _stubs_length) {
    return _stubs_addr[id];
  }
  id -= _stubs_length;
  if (id < _blobs_length) {
    return _blobs_addr[id];
  }
  return nullptr;
}

int SCAddressTable::id_for_address(address addr) {
  int id = -1;
  if (addr == (address)-1) { // Static call stub has jump to itself
    return id;
  }
  if (!_complete) {
    fatal("SCA table is not complete");
  }
  // Seach for C string
  id = id_for_C_string(addr);
  if (id >=0) {
    return id + _all_max;
  }
  if (StubRoutines::contains(addr)) {
    // Search in stubs
    id = search_address(addr, _stubs_addr, _stubs_length);
    if (id < 0) {
      StubCodeDesc* desc = StubCodeDesc::desc_for(addr);
      if (desc == nullptr) {
        desc = StubCodeDesc::desc_for(addr + frame::pc_return_offset);
      }
      const char* sub_name = (desc != nullptr) ? desc->name() : "<unknown>";
      fatal("Address " INTPTR_FORMAT " for Stub:%s is missing in SCA table", p2i(addr), sub_name);
    } else {
      id += _extrs_length;
    }
  } else {
    CodeBlob* cb = CodeCache::find_blob(addr);
    if (cb != nullptr) {
      // Search in code blobs
      id = search_address(addr, _blobs_addr, _blobs_length);
      if (id < 0) {
        fatal("Address " INTPTR_FORMAT " for Blob:%s is missing in SCA table", p2i(addr), cb->name());
      } else {
        id += _extrs_length + _stubs_length;
      }
    } else {
      // Search in runtime functions
      id = search_address(addr, _extrs_addr, _extrs_length);
      if (id < 0) {
        ResourceMark rm;
        const int buflen = 1024;
        char* func_name = NEW_RESOURCE_ARRAY(char, buflen);
        int offset = 0;
        if (os::dll_address_to_function_name(addr, func_name, buflen, &offset)) {
          if (offset > 0) {
            // Could be address of C string
            uint dist = (uint)pointer_delta(addr, (address)os::init, 1);
            log_info(sca)("Address " INTPTR_FORMAT " (offset %d) for runtime target '%s' is missing in SCA table", p2i(addr), dist, (const char*)addr);
            assert(dist > (uint)(_all_max + MAX_STR_COUNT), "change encoding of distance");
            return dist;
          }
          fatal("Address " INTPTR_FORMAT " for runtime target '%s+%d' is missing in SCA table", p2i(addr), func_name, offset);
        } else {
          fatal("Address " INTPTR_FORMAT " for <unknown> is missing in SCA table", p2i(addr));
        }
      }
    }
  }
  return id;
}
