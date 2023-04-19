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
#include "classfile/symbolTable.hpp"
#include "classfile/systemDictionary.hpp"
#include "classfile/vmIntrinsics.hpp"
#include "ci/ciEnv.hpp"
#include "ci/ciMethod.hpp"
#include "ci/ciUtilities.inline.hpp"
#include "code/codeBlob.hpp"
#include "code/codeCache.hpp"
#include "code/oopRecorder.inline.hpp"
#include "code/SCArchive.hpp"
#include "logging/log.hpp"
#include "oops/klass.inline.hpp"
#include "oops/method.inline.hpp"
#include "runtime/flags/flagSetting.hpp"
#include "runtime/os.inline.hpp"
#include "runtime/stubCodeGenerator.hpp"
#include "runtime/stubRoutines.hpp"
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
  }
}

void SCArchive::close() {
  if (_archive != nullptr) {
    delete _archive; // Free memory
  }
}

bool SCArchive::open_for_read(const char* archive_path) {
  log_info(sca)("Trying to load shared code archive '%s'", archive_path);
  struct stat st;
  if (os::stat(archive_path, &st) != 0) {
    log_info(sca)("Specified shared code archive not found '%s'", archive_path);
    return false;
  } else if ((st.st_mode & S_IFMT) != S_IFREG) {
    log_info(sca)("Specified shared code archive is not file '%s'", archive_path);
    return false;
  }
  int fd = os::open(archive_path, O_RDONLY | O_BINARY, 0);
  if (fd < 0) {
    if (errno == ENOENT) {
      log_info(sca)("Specified shared code archive not found '%s'", archive_path);
    } else {
      log_warning(sca)("Failed to open shared code archive file '%s': (%s)", archive_path, os::strerror(errno));
    }
    return false;
  } else {
    log_info(sca)("Opened for read shared code archive '%s'", archive_path);
  }
  SCAFile* archive = new SCAFile(archive_path, fd, (size_t)st.st_size, true /* for_read */);
  if (archive->fd() < 0) { // failed
    delete archive;
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
    log_warning(sca)("Unable to create shared code archive file '%s': (%s)", archive_path, os::strerror(errno));
    return false;
  } else {
    log_info(sca)("Opened for write shared code archive '%s'", archive_path);
  }
  SCAFile* archive = new SCAFile(archive_path, fd, 0, false /* for_read */);
  if (archive->fd() < 0) { // failed
    delete archive;
    return false;
  }
  _archive = archive;
  return true;
}

SCAFile::SCAFile(const char* archive_path, int fd, size_t file_size, bool for_read) {
  _archive_path = archive_path;
  _file_size = file_size;
  _file_offset = 0;
  _fd = fd;
  _for_read = for_read;
  _failed = false;
  _table = nullptr;
  _entries = nullptr;
  _write_entries = nullptr;

  // Read header st the begining of archive
  size_t header_size = sizeof(SCAHeader);
  if (for_read) {
    // Read header from archive
    if (!seek_to_position(0)) {
      return;
    }
    size_t n = read_bytes((void*)&_header, header_size);
    if (n != header_size) {
      return;
    }
    assert(_header.version() == VM_Version::jvm_version(), "sanity");
    assert(_header.archive_size() <= file_size, "recorded %d vs actual %d", (int)_header.archive_size(), (int)file_size);
    log_info(sca)("Read header from shared code archive '%s'", archive_path);
  } else {
    // Write initial version of header
    _header.init(VM_Version::jvm_version(), 0 /* entry_count */, 0 /* archive_size */, 0 /* entries_offset */);
    size_t header_size = sizeof(SCAHeader);
    size_t n = write_bytes((const void*)&_header, header_size);
    if (n != header_size) {
      return;
    }
    log_info(sca)("Wrote initial header to shared code archive '%s'", archive_path);
  }
  _table = new SCATable();
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

SCAFile::~SCAFile() {
  if (_fd < 0) {
    return; // Already closed
  }
  if (open_for_write()) { // Finalize archive
    finish_write();
  }
  if (::close(_fd) < 0) {
    log_warning(sca)("Failed to close shared code archive file '%s'", _archive_path);
  }
  _fd = -1;
  log_info(sca)("Closed shared code archive '%s'", _archive_path);
  FREE_C_HEAP_ARRAY(char, _archive_path);
  if (_entries != nullptr) {
    FREE_C_HEAP_ARRAY(SCAEntry, _entries);
  }
  if (_write_entries != nullptr) {
    delete _write_entries;
  }
  if (_table != nullptr) {
    delete _table;
  }
}

bool SCAFile::for_read()  const { return (_fd >= 0) &&  _for_read && !_failed; }

bool SCAFile::for_write() const { return (_fd >= 0) && !_for_read && !_failed; }

SCAFile* SCAFile::open_for_read() {
  SCAFile* archive = SCArchive::archive();
  if (archive != nullptr && archive->for_read()) {
    return archive;
  }
  return nullptr;
}

SCAFile* SCAFile::open_for_write() {
  SCAFile* archive = SCArchive::archive();
  if (archive != nullptr && archive->for_write()) {
    return archive;
  }
  return nullptr;
}

bool SCAFile::seek_to_position(size_t pos) {
  if (pos == _file_offset) {
    return true;
  }
  if (os::lseek(_fd, (long)pos, SEEK_SET) < 0) {
    log_warning(sca)("Failed to seek to position " SIZE_FORMAT " in shared code archive file '%s'", pos, _archive_path);
    // Close the file if there's a problem reading it.
    failed();
    return false;
  }
  _file_offset = pos;
  return true;
}

static char align_buffer[256] = { 0 };

bool SCAFile::align_write() {
  // We are not executing code from archive - we copy it by bytes first.
  // No need for big alignment (or at all).
  size_t padding = sizeof(size_t) - (_file_offset & (sizeof(size_t) - 1));
  if (padding == 0) {
    return true;
  }
  size_t n = write_bytes((const void*)&align_buffer, padding);
  if (n != padding) {
    return false;
  }
  log_info(sca)("Adjust write alignment in shared code archive '%s'", _archive_path);
  return true;
}

size_t SCAFile::read_bytes(void* buffer, size_t nbytes) {
  assert(for_read(), "Archive file is not open");
  if (nbytes == 0) {
    return 0;
  }
  size_t n = ::read(_fd, buffer, (unsigned int)nbytes);
  if (n != nbytes) {
    log_warning(sca)("Failed to read %d bytes at offset %d from shared code archive file '%s'", (int)nbytes, (int)_file_offset, _archive_path);
    failed();
    return 0;
  }
  log_info(sca)("Read %d bytes at offset %d from shared code archive '%s'", (int)nbytes, (int)_file_offset, _archive_path);
  _file_offset += n;
  return nbytes;
}


size_t SCAFile::write_bytes(const void* buffer, size_t nbytes) {
  assert(for_write(), "Archive file is not created");
  if (nbytes == 0) {
    return 0;
  }
  ssize_t n = os::write(_fd, buffer, (unsigned int)nbytes);
  if (n < 0 || (size_t)n != nbytes) {
    log_warning(sca)("Failed to write %d bytes to shared code archive file '%s'", (int)nbytes, _archive_path);
    failed();
    return 0;
  }
  log_info(sca)("Wrote %d bytes at offset %d to shared code archive '%s'", (int)nbytes, (int)_file_offset, _archive_path);
  _file_offset += n;
  return (size_t)n;
}

void SCAFile::add_entry(SCAEntry entry) {
  if (_write_entries == nullptr) {
    _write_entries = new(mtCode) GrowableArray<SCAEntry>(4, mtCode); // C heap
    assert(_write_entries != nullptr, "Sanity");
  }
  _write_entries->append(entry);
}

SCAEntry* SCAFile::find_entry(SCAEntry::Kind kind, uint32_t id) {
  size_t count = _header.entries_count();
  if (_entries == nullptr) {
    // Read it
    if (!seek_to_position(_header.entries_offset())) {
      return nullptr;
    }
    size_t size  = sizeof(SCAEntry);
    size_t entries_size = count * size; // In bytes
    _entries = NEW_C_HEAP_ARRAY(SCAEntry, count, mtCode);
    size_t n = read_bytes((void*)&(_entries[0]), entries_size);
    if (n != entries_size) {
      return nullptr;
    }
    log_info(sca)("Read SCAEntry entries with %d elements at offset %d", (int)count, (int)_header.entries_offset());
  }
  for(int i = 0; i < (int)count; i++) {
    if (_entries[i].kind() == kind && _entries[i].id() == id) {
      assert(_entries[i].idx() == i, "sanity");
      return &(_entries[i]);
    }
  }
  return nullptr;
}

bool SCAFile::finish_write() {
  int version = _header.version();
  size_t entries_offset   = 0; // 0 is deafult if empty
  int count   = _write_entries->length();
  if (count > 0) {
    SCAEntry* entries_address = _write_entries->adr_at(0);
    // Write SCAEntry entries
    entries_offset = _file_offset;
    size_t entries_size = count * sizeof(SCAEntry); // In bytes
    size_t n = write_bytes((const void*)entries_address, entries_size);
    if (n != entries_size) {
      return false;
    }
    log_info(sca)("Wrote SCAEntry entries to shared code archive '%s'", _archive_path);
  }

  // Finalize and write header
  _header.init(version, count, _file_offset, entries_offset);
  if (!seek_to_position(0)) {
    return false;
  }
  _file_offset = 0;
  size_t header_size = sizeof(SCAHeader);
  size_t n = write_bytes((const void*)&_header, header_size);
  if (n != header_size) {
    return false;
  }
  log_info(sca)("Wrote header to shared code archive '%s'", _archive_path);
  return true;
}

bool SCAFile::load_stub(StubCodeGenerator* cgen, vmIntrinsicID id, const char* name, address start) {
  assert(start == cgen->assembler()->pc(), "wrong buffer");
  SCAFile* archive = open_for_read();
  if (archive == nullptr) {
    return false;
  }
  SCAEntry* entry = archive->find_entry(SCAEntry::Stub, (uint32_t)id);
  if (entry == nullptr) {
    return false;
  }
  size_t entry_position = entry->offset();
  // Read name
  size_t name_offset = entry_position + entry->name_offset();
  size_t name_size   = entry->name_size(); // Includes '/0'
  char*  saved_name  = NEW_C_HEAP_ARRAY(char, name_size, mtCode);
  if (!archive->seek_to_position(name_offset)) {
    return false;
  }
  size_t n = archive->read_bytes(saved_name, name_size);
  if (n != name_size) {
    return false;
  }
  if (strncmp(name, saved_name, (name_size - 1)) != 0) {
    log_warning(sca)("Saved stub's name '%s' is different from '%s' for id:%d", saved_name, name, (int)id);
    archive->failed();
    return false;
  }
  // Read code
  size_t code_offset = entry_position + entry->code_offset();
  size_t code_size   = entry->code_size();
  if (!archive->seek_to_position(code_offset)) {
    return false;
  }
  n = archive->read_bytes(start, code_size);
  if (n != code_size) {
    return false;
  }
  cgen->assembler()->code_section()->set_end(start + n);
  log_info(sca)("Read stub '%s' id:%d from shared code archive '%s'", name, (int)id, archive->_archive_path);
  return true;
}

bool SCAFile::store_stub(StubCodeGenerator* cgen, vmIntrinsicID id, const char* name, address start) {
  SCAFile* archive = open_for_write();
  if (archive == nullptr) {
    return false;
  }
  if (!archive->align_write()) {
    return false;
  }
  size_t entry_position = archive->_file_offset;

  // Write name
  size_t name_offset = 0; // Write name first
  size_t name_size = strlen(name) + 1; // Includes '/0'
  size_t n = archive->write_bytes(name, name_size);
  if (n != name_size) {
    return false;
  }
  if (!archive->align_write()) {
    return false;
  }
  // Write code
  size_t code_offset = archive->_file_offset - entry_position;
  size_t code_size = cgen->assembler()->pc() - start;
  n = archive->write_bytes(start, code_size);
  if (n != code_size) {
    return false;
  }
  SCAEntry entry(entry_position, name_offset, name_size, code_offset, code_size, 0, 0,
                 SCAEntry::Stub, (uint32_t)id, archive->_header.next_idx());
  archive->add_entry(entry);
  log_info(sca)("Wrote stub '%s' id:%d to shared code archive '%s'", name, (int)id, archive->_archive_path);
  return true;
}

// Repair the pc relative information in the code after load
bool SCAFile::read_relocations(CodeBuffer* buffer, CodeBuffer* orig_buffer, size_t max_reloc_size) {
  // Read max count
  size_t max_reloc_count = max_reloc_size / sizeof(relocInfo);
  bool success = true;
  size_t* reloc_data = NEW_C_HEAP_ARRAY(size_t, max_reloc_count, mtCode);
  for (int i = 0; i < (int)CodeBuffer::SECT_LIMIT; i++) {
    int reloc_count = 0;
    size_t n = read_bytes(&reloc_count, sizeof(int));
    if (n != sizeof(int)) {
      success = false;
      break;
    }
    if (reloc_count == 0) {
      continue;
    }
    // Read _locs_point (as offset from start)
    int locs_point_off = 0;
    n = read_bytes(&locs_point_off, sizeof(int));
    if (n != sizeof(int)) {
      success = false;
      break;
    }
    size_t reloc_size = reloc_count * sizeof(relocInfo);
    CodeSection* cs  = buffer->code_section(i);
    if (!cs->has_locs()) {
      cs->initialize_locs(reloc_count);
    }
    relocInfo* reloc_start = cs->locs_start();
    n = read_bytes(reloc_start, reloc_size);
    if (n != reloc_size) {
      success = false;
      break;
    }
    cs->set_locs_end(reloc_start + (int)reloc_count);
    cs->set_locs_point(cs->start() + locs_point_off);

    // Read additional relocation data: size_t per relocation
    size_t  data_size  = reloc_count * sizeof(size_t);
    n = read_bytes(reloc_data, data_size);
    if (n != data_size) {
      success = false;
      break;
    }

    RelocIterator iter(cs);
    int j = 0;
    while (iter.next()) {
      switch (iter.type()) {
        case relocInfo::none:
          break;
        case relocInfo::virtual_call_type:
          fatal("virtual_call_type unimplemented");
          break;
        case relocInfo::opt_virtual_call_type:
          fatal("opt_virtual_call_type unimplemented");
          break;
        case relocInfo::static_call_type:
          fatal("static_call_type unimplemented");
          break;
        case relocInfo::runtime_call_type: {
          iter.reloc()->fix_relocation_after_move(orig_buffer, buffer);
          address dest = _table->address_for_id(reloc_data[j]);
          ((CallRelocation*)iter.reloc())->set_destination(dest);
          break;
        }
        case relocInfo::runtime_call_w_cp_type:
          fatal("runtime_call_w_cp_type unimplemented");
          //address destination = iter.reloc()->value();
          break;
        case relocInfo::external_word_type: {
          iter.reloc()->fix_relocation_after_move(orig_buffer, buffer);
          address target = _table->address_for_id(reloc_data[j]);
          ((external_word_Relocation*)iter.reloc())->set_value(target);
          break;
        }
        case relocInfo::internal_word_type:
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
      j++;
    }
    assert(j <= (int)reloc_count, "sanity");
  }
  FREE_C_HEAP_ARRAY(size_t, reloc_data);
  return success;
}

bool SCAFile::read_code(CodeBuffer* buffer, CodeBuffer* orig_buffer) {
  assert(buffer->blob() != nullptr, "sanity");
  for (int i = 0; i < (int)CodeBuffer::SECT_LIMIT; i++) {
    CodeSection* cs = buffer->code_section(i);
    // Read original section size and address.
    size_t orig_size = 0;
    size_t n = read_bytes(&orig_size, sizeof(size_t));
    if (n != sizeof(size_t)) {
      return false;
    }
    if (i != (int)CodeBuffer::SECT_INSTS) {
      buffer->initialize_section_size(cs, orig_size);
    }
    if (orig_size > (size_t)cs->capacity()) { // Will not fit
      return false;
    }
    if (orig_size == 0) {
      assert(cs->size() == 0, "should match");
      continue;  // skip trivial section
    }
    address orig_start;
    n = read_bytes(&orig_start, sizeof(address));
    if (n != sizeof(address)) {
      return false;
    }

    // Populate fake original buffer (no code allocation in CodeCache).
    // It is used for relocations to calculate sections addesses delta.
    CodeSection* orig_cs = orig_buffer->code_section(i);
    assert(!orig_cs->is_allocated(), "This %d section should not be set", i);
    orig_cs->initialize(orig_start, orig_size);

    // Load code to new buffer.
    address code_start = cs->start();
    n = read_bytes(code_start, orig_size);
    if (n != orig_size) {
      return false;
    }
    cs->set_end(code_start + orig_size);
  }

  return true;
}

bool SCAFile::load_exception_blob(CodeBuffer* buffer, int* pc_offset) {
#ifdef ASSERT
if (UseNewCode2) {
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
  size_t entry_position = entry->offset();
  if (!archive->seek_to_position(entry_position)) {
    return false;
  }

  // Read pc_offset
  size_t n = archive->read_bytes(pc_offset, sizeof(int));
  if (n != sizeof(int)) {
    return false;
  }

  // Read name
  size_t name_offset = entry_position + entry->name_offset();
  if (!archive->seek_to_position(name_offset)) {
    return false;
  }
  size_t name_size   = entry->name_size(); // Includes '/0'
  char*  name        = NEW_C_HEAP_ARRAY(char, name_size, mtCode);
  n = archive->read_bytes(name, name_size);
  if (n != name_size) {
    FREE_C_HEAP_ARRAY(char, name);
    return false;
  }
  if (strncmp(buffer->name(), name, (name_size - 1)) != 0) {
    log_warning(sca)("Saved stub's name '%s' is different from '%s'", name, buffer->name());
    archive->failed();
    return false;
  }

  size_t code_offset = entry_position + entry->code_offset();
  if (!archive->seek_to_position(code_offset)) {
    return false;
  }

  // Create fake original CodeBuffer
  CodeBuffer orig_buffer(name);

  // Read code
  if (!archive->read_code(buffer, &orig_buffer)) {
    return false;
  }

  // Read relocations
  size_t reloc_offset = entry_position + entry->reloc_offset();
  if (!archive->seek_to_position(reloc_offset)) {
    return false;
  }
  if (!archive->read_relocations(buffer, &orig_buffer, entry->reloc_size())) {
    return false;
  }

  log_info(sca)("Read blob '%s' from shared code archive '%s'", name, archive->_archive_path);
#ifdef ASSERT
if (UseNewCode2) {
  FlagSetting fs(PrintRelocations, true);
  buffer->print();
  buffer->decode();
}
#endif
  FREE_C_HEAP_ARRAY(char, name);
  return true;
}

bool SCAFile::write_relocations(CodeBuffer* buffer, size_t& max_reloc_size) {
  size_t max_reloc_count = 0;
  for (int i = 0; i < (int)CodeBuffer::SECT_LIMIT; i++) {
    CodeSection* cs = buffer->code_section(i);
    size_t reloc_count = cs->has_locs() ? cs->locs_count() : 0;
    if (reloc_count > max_reloc_count) {
      max_reloc_count = reloc_count;
    }
  }
  max_reloc_size = max_reloc_count * sizeof(relocInfo);
  bool success = true;
  size_t* reloc_data = NEW_C_HEAP_ARRAY(size_t, max_reloc_count, mtCode);
  for (int i = 0; i < (int)CodeBuffer::SECT_LIMIT; i++) {
    CodeSection* cs = buffer->code_section(i);
    int reloc_count = cs->has_locs() ? cs->locs_count() : 0;
    size_t n = write_bytes(&reloc_count, sizeof(int));
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
    size_t reloc_size      = reloc_count * sizeof(relocInfo);
    n = write_bytes(reloc_start, reloc_size);
    if (n != reloc_size) {
      success = false;
      break;
    }
    // Collect additional data
    RelocIterator iter(cs);
    int j = 0;
    while (iter.next()) {
      reloc_data[j] = 0; // initialize
      switch (iter.type()) {
        case relocInfo::none:
          break;
        case relocInfo::virtual_call_type:
          fatal("virtual_call_type unimplemented");
          break;
        case relocInfo::opt_virtual_call_type:
          fatal("opt_virtual_call_type unimplemented");
          break;
        case relocInfo::static_call_type:
          fatal("static_call_type unimplemented");
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
    // Write additional relocation data: size_t per relocation
    size_t data_size = reloc_count * sizeof(size_t);
    n = write_bytes(reloc_data, data_size);
    if (n != data_size) {
      success = false;
      break;
    }
  }
  FREE_C_HEAP_ARRAY(size_t, reloc_data);
  return success;
}

bool SCAFile::write_code(CodeBuffer* buffer, size_t& code_size) {
  assert(buffer->blob() != nullptr, "sanity");
  size_t total_size = 0;
  for (int i = 0; i < (int)CodeBuffer::SECT_LIMIT; i++) {
    const CodeSection* cs = buffer->code_section(i);
    size_t cs_size = cs->size();
    size_t n = write_bytes(&cs_size, sizeof(size_t));
    if (n != sizeof(size_t)) {
      return false;
    }
    if (cs_size == 0) {
      continue;  // skip trivial section
    }
    assert(cs->mark() == nullptr, "CodeSection::_mark is not implemented");

    // Write original address
    address cs_start = cs->start();
    n = write_bytes(&cs_start, sizeof(address));
    if (n != sizeof(address)) {
      return false;
    }

    // Write code
    n = write_bytes(cs_start, cs_size);
    if (n != cs_size) {
      return false;
    }
    total_size += cs_size;
  }
  code_size = total_size;
  return true;
}

bool SCAFile::store_exception_blob(CodeBuffer* buffer, int pc_offset) {
  SCAFile* archive = open_for_write();
  if (archive == nullptr) {
    return false;
  }
#ifdef ASSERT
if (UseNewCode2) {
  FlagSetting fs(PrintRelocations, true);
  buffer->print();
  buffer->decode();
}
#endif
  if (!archive->align_write()) {
    return false;
  }
  size_t entry_position = archive->_file_offset;

  // Write pc_offset
  size_t n = archive->write_bytes(&pc_offset, sizeof(int));
  if (n != sizeof(int)) {
    return false;
  }

  // Write name
  const char* name = buffer->name();
  size_t name_offset = archive->_file_offset  - entry_position;
  size_t name_size = strlen(name) + 1; // Includes '/0'
  n = archive->write_bytes(name, name_size);
  if (n != name_size) {
    return false;
  }
  if (!archive->align_write()) {
    return false;
  }

  // Write code section
  size_t code_offset = archive->_file_offset - entry_position;
  size_t code_size = 0;
  if (!archive->write_code(buffer, code_size)) {
    return false;
  }
  // Write relocInfo array
  size_t reloc_offset = archive->_file_offset - entry_position;
  size_t reloc_size = 0;
  if (!archive->write_relocations(buffer, reloc_size)) {
    return false;
  }

  SCAEntry entry(entry_position, name_offset, name_size,
                 code_offset, code_size, reloc_offset, reloc_size,
                 SCAEntry::Blob, (uint32_t)999, archive->_header.next_idx());
  archive->add_entry(entry);
  log_info(sca)("Wrote stub '%s' to shared code archive '%s'", name, archive->_archive_path);
  return true;
}

DebugInformationRecorder* SCAFile::read_debug_info(OopRecorder* oop_recorder) {
  int data_size = 0;
  size_t n = read_bytes(&data_size, sizeof(int));
  if (n != sizeof(int)) {
    return nullptr;
  }
  int pcs_length = 0;
  n = read_bytes(&pcs_length, sizeof(int));
  if (n != sizeof(int)) {
    return nullptr;
  }

  DebugInformationRecorder* recorder = new DebugInformationRecorder(oop_recorder, data_size, pcs_length);

  n = read_bytes(recorder->stream()->buffer(), data_size);
  if (n != (size_t)data_size) {
    return nullptr;
  }
  recorder->stream()->set_position(data_size);

  size_t pcs_size = pcs_length * sizeof(PcDesc);
  n = read_bytes(recorder->pcs(), pcs_size);
  if (n != pcs_size) {
    return nullptr;
  }
  return recorder;
}

bool SCAFile::write_debug_info(DebugInformationRecorder* recorder) {
  int data_size = recorder->data_size(); // In bytes
  size_t n = write_bytes(&data_size, sizeof(int));
  if (n != sizeof(int)) {
    return false;
  }
  int pcs_length = recorder->pcs_length(); // In bytes
  n = write_bytes(&pcs_length, sizeof(int));
  if (n != sizeof(int)) {
    return false;
  }
  n = write_bytes(recorder->stream()->buffer(), data_size);
  if (n != (size_t)data_size) {
    return false;
  }
  size_t pcs_size = pcs_length * sizeof(PcDesc);
  n = write_bytes(recorder->pcs(), pcs_size);
  if (n != pcs_size) {
    return false;
  }
  return true;
}

bool SCAFile::read_oop_maps(OopMapSet* oop_maps) {
  size_t om_count = 0;
  size_t n = read_bytes(&om_count, sizeof(int));
  if (n != sizeof(int)) {
    return false;
  }
  oop_maps = new OopMapSet(om_count);
  for (int i = 0; i < (int)om_count; i++) {
    int data_size = 0;
    n = read_bytes(&data_size, sizeof(int));
    if (n != sizeof(int)) {
      return false;
    }
    OopMap* oop_map = new OopMap(data_size);
    // Preserve allocated stream
    CompressedWriteStream* stream = oop_map->write_stream();
    OopMap* om = oop_maps->at(i);
    // Read data which overwrites default data
    n = read_bytes(oop_map, sizeof(OopMap));
    if (n != sizeof(OopMap)) {
      return false;
    }
    oop_map->set_write_stream(stream);
    n = read_bytes(oop_map->data(), (size_t)data_size);
    if (n != (size_t)data_size) {
      return false;
    }
    oop_maps->add(oop_map);
  }
  return true;
}

bool SCAFile::write_oop_maps(OopMapSet* oop_maps) {
  size_t om_count = oop_maps->size();
  size_t n = write_bytes(&om_count, sizeof(int));
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
    n = write_bytes(om->data(), (size_t)data_size);
    if (n != (size_t)data_size) {
      return false;
    }
  }
  return true;
}

bool SCAFile::read_metadata(OopRecorder* oop_recorder, ciMethod* target) {
  int metadata_count = 0;
  size_t n = read_bytes(&metadata_count, sizeof(int));
  if (n != sizeof(int)) {
    return false;
  }
  if (metadata_count == 0) {
    return true;
  }

  {
    VM_ENTRY_MARK;
    methodHandle comp_method(THREAD, target->get_Method());
    //const char* target_name = comp_method->name_and_sig_as_C_string();

    for (int i = 0; i < metadata_count; i++) {
      int kind = 0;
      n = read_bytes(&kind, sizeof(int));
      if (n != sizeof(int)) {
        return false;
      }
      if (kind < 0) {
        continue; // Not supported yet
      }
      if (kind == 1) { // Method*
        int holder_length = 0;
        n = read_bytes(&holder_length, sizeof(int));
        if (n != sizeof(int)) {
          return false;
        }
        int name_length = 0;
        n = read_bytes(&name_length, sizeof(int));
        if (n != sizeof(int)) {
          return false;
        }
        int signat_length = 0;
        n = read_bytes(&signat_length, sizeof(int));
        if (n != sizeof(int)) {
          return false;
        }
        int total_length = holder_length + 1 + name_length + 1 + signat_length + 1;
        char* dest = NEW_RESOURCE_ARRAY(char, total_length);
        n = read_bytes(dest, total_length);
        if (n != (size_t)total_length) {
          return false;
        }
        TempNewSymbol klass_sym = SymbolTable::probe(&(dest[0]), holder_length);
        dest[holder_length] = '\0';
        if (klass_sym == NULL) {
          log_info(sca)("Probe failed for class %s", &(dest[0]));
          return false;
        }
        // Use class loader of compiled method.
        Thread* thread = Thread::current();
        Handle loader(thread, comp_method->method_holder()->class_loader());
        Handle protection_domain(thread, comp_method->method_holder()->protection_domain());
        Klass* k = SystemDictionary::find_instance_or_array_klass(thread, klass_sym, loader, protection_domain);
        assert(!thread->has_pending_exception(), "should not throw");

        if (k != NULL) {
          log_info(sca)("%s %s (lookup)", comp_method->method_holder()->external_name(), k->external_name());
        } else {
          log_info(sca)("Lookup failed for class %s", &(dest[0]));
        }

        TempNewSymbol name_sym = SymbolTable::probe(&(dest[holder_length + 1]), name_length);
        int pos = holder_length + 1 + name_length;
        dest[pos++] = '\0';
        TempNewSymbol sign_sym = SymbolTable::probe(&(dest[pos]), signat_length);
        if (name_sym == NULL) {
          log_info(sca)("Probe failed for method name %s", &(dest[holder_length + 1]));
          return false;
        }
        if (sign_sym == NULL) {
          log_info(sca)("Probe failed for method signature %s", &(dest[pos]));
          return false;
        }
        Method* m = InstanceKlass::cast(k)->find_method(name_sym, sign_sym);
        if (k != NULL) {
          ResourceMark rm;
          log_info(sca)("Method lookup: %s", m->name_and_sig_as_C_string());
        } else {
          log_info(sca)("Lookup failed for method %s%s", &(dest[holder_length + 1]), &(dest[pos]));
          return false;
        }
        oop_recorder->find_index(m);
      }
    }
  }
  return true;
}

bool SCAFile::write_metadata(OopRecorder* oop_recorder, const methodHandle& comp_method) {
  int metadata_count = oop_recorder->metadata_count();
  size_t n = write_bytes(&metadata_count, sizeof(int));
  if (n != sizeof(int)) {
    return false;
  }

  int no_data = -1;
  for (int i = 0; i < metadata_count; i++) {
    Metadata* m = oop_recorder->metadata_at(i);
    if (!oop_recorder->is_real(m)) {
      n = write_bytes(&no_data, sizeof(int));
      if (n != sizeof(int)) {
        return false;
      }
    } else {
      if (m->is_method()) {
        Method* method = (Method*)m;
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
        dest[holder_length] = ' ';
        int pos = holder_length + 1;
        name->as_C_string(&(dest[pos]), (total_length - pos));
        pos += name_length;
        dest[pos++] = ' ';
        signat->as_C_string(&(dest[pos]), (total_length - pos));
        dest[total_length - 1] = '\0';
        int kind = 1; // Method*
        n = write_bytes(&kind, sizeof(int));
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
        if (n != (size_t)total_length) {
          return false;
        }
        log_info(sca)("Write metadata [%d]: %s", i, dest);
      } else { // Not supported
        n = write_bytes(&no_data, sizeof(int));
        if (n != sizeof(int)) {
          return false;
        }
      }
    }
  }
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
  int compile_id = env->compile_id();
  SCAEntry* entry = archive->find_entry(SCAEntry::Code, (uint32_t)compile_id);
  if (entry == nullptr) {
    return false;
  }
  size_t entry_position = entry->offset();
  if (!archive->seek_to_position(entry_position)) {
    return false;
  }

  // Read name
  size_t name_offset = entry_position + entry->name_offset();
  if (!archive->seek_to_position(name_offset)) {
    return false;
  }
  size_t name_size   = entry->name_size(); // Includes '/0'
  char*  name        = NEW_C_HEAP_ARRAY(char, name_size, mtCode);
  size_t n = archive->read_bytes(name, name_size);
  if (n != name_size) {
    FREE_C_HEAP_ARRAY(char, name);
    return false;
  }
  {
    VM_ENTRY_MARK;
    ResourceMark rm;
    methodHandle method(THREAD, target->get_Method());
    const char* target_name = method->name_and_sig_as_C_string();

    if (strncmp(target_name, name, (name_size - 1)) != 0) {
      log_warning(sca)("Saved stub's name '%s' is different from '%s'", name, target_name);
      archive->failed();
      return false;
    }
  }

  size_t code_offset = entry_position + entry->code_offset();
  if (!archive->seek_to_position(code_offset)) {
    return false;
  }

  // Read flags
  int flags = 0;
  n = archive->read_bytes(&flags, sizeof(int));
  if (n != sizeof(int)) {
    return false;
  }
  bool has_monitors      = flags & 0xFF;
  bool has_wide_vectors  = (flags >>  8) & 0xFF;
  bool has_unsafe_access = (flags >> 16) & 0xFF;

  int orig_pc_offset = 0;
  n = archive->read_bytes(&orig_pc_offset, sizeof(int));
  if (n != sizeof(int)) {
    return false;
  }

  int frame_size = 0;
  n = archive->read_bytes(&frame_size, sizeof(int));
  if (n != sizeof(int)) {
    return false;
  }

  // Read offsets
  CodeOffsets offsets;
  n = archive->read_bytes(&offsets, sizeof(CodeOffsets));
  if (n != sizeof(CodeOffsets)) {
    return false;
  }

  // Create Debug Information Recorder to record scopes, oopmaps, etc.
  OopRecorder* oop_recorder = new OopRecorder(env->arena());
  env->set_oop_recorder(oop_recorder);

  // Write OopRecorder data
  if (!archive->read_metadata(oop_recorder, target)) {
    return false;
  }

  // Read Debug info
  DebugInformationRecorder* recorder = archive->read_debug_info(oop_recorder);
  if (recorder == nullptr) {
    return false;
  }
  env->set_debug_info(recorder);

  // Read Dependencies (compressed already)
  Dependencies* dependencies = new Dependencies(env);
  int dependencies_size = 0;
  n = archive->read_bytes(&dependencies_size, sizeof(int));
  if (n != sizeof(int)) {
    return false;
  }
  if (dependencies_size) {
    u_char* dependencies_buffer = NEW_RESOURCE_ARRAY(u_char, dependencies_size);
    n = archive->read_bytes(dependencies_buffer, dependencies_size);
    if (n != (size_t)dependencies_size) {
      return false;
    }
    dependencies->set_content(dependencies_buffer, dependencies_size);
  }
  env->set_dependencies(dependencies);

  // Read oop maps
  OopMapSet* oop_maps = nullptr;
  if (!archive->read_oop_maps(oop_maps)) {
    return false;
  }

  // Read exception handles
  int exc_table_length = 0;
  n = archive->read_bytes(&exc_table_length, sizeof(int));
  if (n != sizeof(int)) {
    return false;
  }
  ExceptionHandlerTable handler_table(MAX2(exc_table_length, 4));
  if (exc_table_length > 0) {
    handler_table.set_length(exc_table_length);
    size_t exc_table_size = handler_table.size_in_bytes();
    n = archive->read_bytes(handler_table.table(), exc_table_size);
    if (n != exc_table_size) {
      return false;
    }
  }

  // Read null check table
  int nul_chk_length = 0;
  n = archive->read_bytes(&nul_chk_length, sizeof(int));
  if (n != sizeof(int)) {
    return false;
  }
  ImplicitExceptionTable nul_chk_table;
  if (nul_chk_length > 0) {
    nul_chk_table.set_size(nul_chk_length);
    nul_chk_table.set_len(nul_chk_length);
    size_t nul_chk_size = nul_chk_table.size_in_bytes();
    n = archive->read_bytes(nul_chk_table.data(), nul_chk_size);
    if (n != nul_chk_size) {
      return false;
    }
  }

  CodeBuffer buffer("Compile::Fill_buffer", entry->code_size(), entry->reloc_size());
  buffer.initialize_oop_recorder(oop_recorder);

  // Create fake original CodeBuffer
  CodeBuffer orig_buffer(name);

  // Read code
  if (!archive->read_code(&buffer, &orig_buffer)) {
    return false;
  }

  // Read relocations
  size_t reloc_offset = entry_position + entry->reloc_offset();
  if (!archive->seek_to_position(reloc_offset)) {
    return false;
  }
  if (!archive->read_relocations(&buffer, &orig_buffer, entry->reloc_size())) {
    return false;
  }

  log_info(sca)("Read nmethod '%s' from shared code archive '%s'", name, archive->_archive_path);
#ifdef ASSERT
if (UseNewCode3) {
  FlagSetting fs(PrintRelocations, true);
  buffer.print();
  buffer.decode();
}
#endif
  FREE_C_HEAP_ARRAY(char, name);

  // Register nmethod
  env->register_method(target, entry_bci,
                       &offsets, orig_pc_offset,
                       &buffer, frame_size,
                       oop_maps, &handler_table,
                       &nul_chk_table, compiler,
                       has_unsafe_access,
                       has_wide_vectors,
                       has_monitors,
                       0, NoRTM,
                       true /* is_shared */);

  return true;
}

bool SCAFile::store_nmethod(const methodHandle& method,
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
    return false; // No OSR
  }
  if (!compiler->is_c2()) {
    return false; // Only C2 now
  }
  SCAFile* archive = open_for_write();
  if (archive == nullptr) {
    return false;
  }
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
  size_t entry_position = archive->_file_offset;

  assert(entry_bci == InvocationEntryBci, "No OSR");

  // Write name
  size_t name_offset = 0;
  size_t name_size   = 0;
  size_t n;
  {
    ResourceMark rm;
    const char* name   = method->name_and_sig_as_C_string();
    name_offset = archive->_file_offset  - entry_position;
    name_size   = strlen(name) + 1; // Includes '/0'
    n = archive->write_bytes(name, name_size);
    if (n != name_size) {
      return false;
    }
  }

  if (!archive->align_write()) {
    return false;
  }

  size_t code_offset = archive->_file_offset - entry_position;

  int flags = (has_unsafe_access << 16) | (has_wide_vectors << 8) | has_monitors;
  n = archive->write_bytes(&flags, sizeof(int));
  if (n != sizeof(int)) {
    return false;
  }

  n = archive->write_bytes(&orig_pc_offset, sizeof(int));
  if (n != sizeof(int)) {
    return false;
  }

  n = archive->write_bytes(&frame_size, sizeof(int));
  if (n != sizeof(int)) {
    return false;
  }

  // Write offsets
  n = archive->write_bytes(offsets, sizeof(CodeOffsets));
  if (n != sizeof(CodeOffsets)) {
    return false;
  }

  // Write OopRecorder data
  if (!archive->write_metadata(buffer->oop_recorder(), method)) {
    return false;
  }

  // Write Debug info
  if (!archive->write_debug_info(recorder)) {
    return false;
  }

  // Write Dependencies
  int dependencies_size = dependencies->size_in_bytes();
  n = archive->write_bytes(&dependencies_size, sizeof(int));
  if (n != sizeof(int)) {
    return false;
  }
  n = archive->write_bytes(dependencies->content_bytes(), dependencies_size);
  if (n != (size_t)dependencies_size) {
    return false;
  }

  // Write oop maps
  if (!archive->write_oop_maps(oop_maps)) {
    return false;
  }

  // Write exception handles
  int exc_table_length = handler_table->length();
  n = archive->write_bytes(&exc_table_length, sizeof(int));
  if (n != sizeof(int)) {
    return false;
  }
  size_t exc_table_size = handler_table->size_in_bytes();
  n = archive->write_bytes(handler_table->table(), exc_table_size);
  if (n != exc_table_size) {
    return false;
  }

  // Write null check table
  int nul_chk_length = nul_chk_table->len();
  n = archive->write_bytes(&nul_chk_length, sizeof(int));
  if (n != sizeof(int)) {
    return false;
  }
  size_t nul_chk_size = nul_chk_table->size_in_bytes();
  n = archive->write_bytes(nul_chk_table->data(), nul_chk_size);
  if (n != nul_chk_size) {
    return false;
  }

  // Write code section
  size_t code_size = 0;
  if (!archive->write_code(buffer, code_size)) {
    return false;
  }
  // Write relocInfo array
  size_t reloc_offset = archive->_file_offset - entry_position;
  size_t reloc_size = 0;
  if (!archive->write_relocations(buffer, reloc_size)) {
    return false;
  }

  SCAEntry entry(entry_position, name_offset, name_size,
                 code_offset, code_size, reloc_offset, reloc_size,
                 SCAEntry::Code, compile_id, archive->_header.next_idx());
  archive->add_entry(entry);
  {
    ResourceMark rm;
    const char* name   = method->name_and_sig_as_C_string();
    log_info(sca)("Wrote nmethod '%s' to shared code archive '%s'", name, archive->_archive_path);
  }
  return true;
}

#define SET_ADDRESS(type, addr) \
  {                                       \
    type##_addr[type##_length++] = (address) (addr);    \
  }

void SCATable::init() {
  assert(!_complete, "init only once");
  _extrs_addr = NEW_C_HEAP_ARRAY(address, 10, mtCode);
  _stubs_addr = NEW_C_HEAP_ARRAY(address, 10, mtCode);
  _blobs_addr = NEW_C_HEAP_ARRAY(address, 30, mtCode);

  _extrs_length = 0;
  _stubs_length = 0;
  _blobs_length = 0;

#ifdef COMPILER2
  SET_ADDRESS(_extrs, OptoRuntime::handle_exception_C);
#endif
  SET_ADDRESS(_extrs, CompressedOops::ptrs_base_addr());
  SET_ADDRESS(_extrs, MacroAssembler::debug64);

  // Stubs
  SET_ADDRESS(_stubs, StubRoutines::method_entry_barrier());

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
  _complete = true;
}

void SCATable::init_opto() {
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
}

#undef SET_ADDRESS

SCATable::~SCATable() {
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

int search_address(address addr, address* table, int length) {
  for (int i = 0; i < length; i++) {
    if (table[i] == addr) {
      return i;
    }
  }
  return -1;
}

address SCATable::address_for_id(size_t idx) {
  if (!_complete) {
    fatal("SCA table is not complete");
  }
  int id = (int)idx;
  if (id < 0 || id == (_extrs_length + _stubs_length + _blobs_length)) {
    fatal("Incorrect id %d for SCA table", id);
  }
  if (id > (_extrs_length + _stubs_length + _blobs_length)) {
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

size_t SCATable::id_for_address(address addr) {
  int id = -1;
  if (!_complete) {
    fatal("SCA table is not complete");
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
            log_info(sca)("Address " INTPTR_FORMAT " for runtime target '%s' is missing in SCA table", p2i(addr), (const char*)addr);
            size_t dist = (size_t)pointer_delta(addr, (address)os::init, 1);
            assert(dist > (size_t)(_extrs_length + _stubs_length + _blobs_length), "change encoding of distance");
            return dist;
          }
          fatal("Address " INTPTR_FORMAT " for runtime target '%s+%d' is missing in SCA table", p2i(addr), func_name, offset);
        } else {
          fatal("Address " INTPTR_FORMAT " for <unknown> is missing in SCA table", p2i(addr));
        }
      }
    }
  }
  return (size_t)id;
}
