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
#include "classfile/vmIntrinsics.hpp"
#include "code/codeBlob.hpp"
#include "code/SCArchive.hpp"
#include "logging/log.hpp"
#include "opto/runtime.hpp"
#include "runtime/os.inline.hpp"
#include "runtime/stubCodeGenerator.hpp"


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
  if (SharedCodeArchive != nullptr) {
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
    _archive->close();
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
  _write_table = nullptr;

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
    _header.init(VM_Version::jvm_version(), 0 /* entry_count */, 0 /* archive_size */, 0 /* table_offset */);
    size_t header_size = sizeof(SCAHeader);
    size_t n = write_bytes((const void*)&_header, header_size);
    if (n != header_size) {
      return;
    }
    log_info(sca)("Wrote initial header to shared code archive '%s'", archive_path);
  }
}

void SCAFile::close() {
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
  if (_table != nullptr) {
    FREE_C_HEAP_ARRAY(SCAEntry, _table);
  }
  if (_write_table != nullptr) {
    delete _write_table;
  }
}

bool SCAFile::open_for_read() const {
  return (_fd >= 0) && _for_read && !_failed;
}

bool SCAFile::open_for_write() const {
  return (_fd >= 0) && !_for_read && !_failed;
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
  assert(open_for_read(), "Archive file is not open");
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
  assert(open_for_write(), "Archive file is not created");
  ssize_t n = os::write(_fd, buffer, (unsigned int)nbytes);
  if (n < 0 || (size_t)n != nbytes) {
    tty->print_cr("Failed to write %d bytes to shared code archive file '%s'", (int)nbytes, _archive_path);
    log_warning(sca)("Failed to write to shared code archive file '%s'", _archive_path);
    failed();
    return 0;
  }
  log_info(sca)("Wrote %d bytes at offset %d to shared code archive '%s'", (int)nbytes, (int)_file_offset, _archive_path);
  _file_offset += n;
  return (size_t)n;
}

void SCAFile::add_entry(SCAEntry entry) {
  if (_write_table == nullptr) {
    _write_table = new(mtCode) GrowableArray<SCAEntry>(4, mtCode); // C heap
    assert(_write_table != nullptr, "Sanity");
  }
  _write_table->append(entry);
}

SCAEntry* SCAFile::find_entry(uint32_t id) {
  size_t count = _header.entries_count();
  if (_table == nullptr) {
    // Read it
    if (!seek_to_position(_header.table_offset())) {
      return nullptr;
    }
    size_t size  = sizeof(SCAEntry);
    size_t table_size = count * size; // In bytes
    _table = NEW_C_HEAP_ARRAY(SCAEntry, count, mtCode);
    size_t n = read_bytes((void*)&(_table[0]), table_size);
    if (n != table_size) {
      return nullptr;
    }
    log_info(sca)("Read SCAEntry table with %d elements at offset %d", (int)count, (int)_header.table_offset());
  }
  for(int i = 0; i < (int)count; i++) {
    if (_table[i].id() == (uint32_t)id) {
      assert(_table[i].idx() == i, "sanity");
      return &(_table[i]);
    }
  }
  return nullptr; 
}

bool SCAFile::finish_write() {
  int version = _header.version();
  size_t table_offset   = 0; // 0 is deafult if empty
  int count   = _write_table->length();
  if (count > 0) {
    SCAEntry* table_address = _write_table->adr_at(0);
    // Write SCAEntry table
    table_offset = _file_offset;
    size_t table_size = count * sizeof(SCAEntry); // In bytes
    size_t n = write_bytes((const void*)table_address, table_size);
    if (n != table_size) {
      return false;
    }
    log_info(sca)("Wrote SCAEntry table to shared code archive '%s'", _archive_path);
  }

  // Finalize and write header
  _header.init(version, count, _file_offset, table_offset);
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
  if (open_for_read()) {
    assert(start == cgen->assembler()->pc(), "wrong buffer");
    SCAEntry* entry = find_entry((uint32_t)id);
    if (entry == nullptr) {
      return false;
    }
    size_t entry_position = entry->offset();
    // Read name
    size_t name_offset    = entry_position + entry->name_offset();
    size_t name_size      = entry->name_size(); // Includes '/0'
    char*  saved_name     = NEW_C_HEAP_ARRAY(char, name_size, mtCode);
    if (!seek_to_position(name_offset)) {
      return false;
    }
    size_t n = read_bytes(saved_name, name_size);
    if (n != name_size) {
      return false;
    }
    if (strncmp(name, saved_name, (name_size - 1)) != 0) {
      log_warning(sca)("Saved stub's name '%s' is different from '%s' for id:%d", saved_name, name, (int)id);
      failed();
      return false;
    }
    // Read code
    size_t code_offset    = entry_position + entry->code_offset();
    size_t code_size      = entry->code_size();
    if (!seek_to_position(code_offset)) {
      return false;
    }
    n = read_bytes(start, code_size);
    if (n == code_size) {
      cgen->assembler()->code_section()->set_end(start + n);
      log_info(sca)("Read stub '%s' id:%d from shared code archive '%s'", name, (int)id, _archive_path);
      return true;
    }
  }
  return false;
}

bool SCAFile::store_stub(StubCodeGenerator* cgen, vmIntrinsicID id, const char* name, address start) {
  if (open_for_write()) {
    if (!align_write()) {
      return false;
    }
    size_t entry_position = _file_offset;

    // Write name
    size_t name_offset = 0; // Write name first
    size_t name_size = strlen(name) + 1; // Includes '/0'
    size_t n = write_bytes(name, name_size);
    if (n != name_size) {
      return false;
    }
    if (!align_write()) {
      return false;
    }
    // Write code
    size_t code_offset = _file_offset - entry_position;
    size_t code_size = cgen->assembler()->pc() - start;
    n = write_bytes(start, code_size);
    if (n != code_size) {
      return false;
    }
    SCAEntry entry(entry_position, name_offset, name_size, code_offset, code_size, 0, 0, (uint32_t)id, _header.next_idx());
    add_entry(entry);
    log_info(sca)("Wrote stub '%s' id:%d to shared code archive '%s'", name, (int)id, _archive_path);
    return true;
  }
  return false;
}

// Repair the pc relative information in the code after load
bool SCAFile::read_relocations(CodeBuffer* buffer, size_t reloc_count, address orig_start, size_t orig_size) {
  if (reloc_count > 0) {
    size_t reloc_size      = reloc_count * sizeof(relocInfo);
    CodeSection* code      = buffer->insts();
    relocInfo* reloc_start = code->locs_start();
    size_t n = read_bytes(reloc_start, reloc_size);
    if (n != reloc_size) {
      return false;
    }
    code->set_locs_end(reloc_start + (int)reloc_count);

    // Read additional relocation data: size_t per relocation
    size_t* reloc_data = NEW_C_HEAP_ARRAY(size_t, reloc_count, mtCode);
    size_t  data_size  = reloc_count * sizeof(size_t);
    n = read_bytes(reloc_data, data_size);
    if (n != data_size) {
      return false;
    }
 
    // Create fake original CodeBuffer
    CodeBuffer orig_cb(orig_start, orig_size);
    RelocIterator iter(code);
    int id = 0;
    while (iter.next()) {
      switch (iter.type()) {
        case relocInfo::virtual_call_type:
          break;
        case relocInfo::opt_virtual_call_type:
          break;
        case relocInfo::static_call_type:
          break;
        case relocInfo::runtime_call_type: {
          iter.reloc()->fix_relocation_after_move(&orig_cb, buffer);
          address dest = (address)os::init + reloc_data[id];
          ((CallRelocation*)iter.reloc())->set_destination(dest);
          break;
        }
        case relocInfo::runtime_call_w_cp_type:
          //address destination = iter.reloc()->value();
          break;
        case relocInfo::external_word_type:
          break;
        case relocInfo::internal_word_type:
          iter.reloc()->fix_relocation_after_move(&orig_cb, buffer);
          break;
        default:
          break;
      }
      id++;
    }
  }
  return true;
}

bool SCAFile::load_exception_blob(CodeBuffer* buffer, int* pc_offset) {
if (UseNewCode2) {
  FlagSetting fs(PrintRelocations, true);
  buffer->print();
}
  if (open_for_read()) {
    SCAEntry* entry = find_entry(999);
    if (entry == nullptr) {
      return false;
    }
    size_t entry_position = entry->offset();
    if (!seek_to_position(entry_position)) {
      return false;
    }

    // Read pc_offset
    size_t n = read_bytes(pc_offset, sizeof(int));
    if (n != sizeof(int)) {
      return false;
    }

    // Read name
    size_t name_offset = entry_position + entry->name_offset();
    size_t name_size   = entry->name_size(); // Includes '/0'
    char*  name        = NEW_C_HEAP_ARRAY(char, name_size, mtCode);
    if (!seek_to_position(name_offset)) {
      return false;
    }
    n = read_bytes(name, name_size);
    if (n != name_size) {
      return false;
    }
    if (strncmp(buffer->name(), name, (name_size - 1)) != 0) {
      log_warning(sca)("Saved stub's name '%s' is different from '%s'", name, buffer->name());
      failed();
      return false;
    }

    // Read code
    size_t code_offset    = entry_position + entry->code_offset();
    size_t code_size      = entry->code_size();
    if (!seek_to_position(code_offset)) {
      return false;
    }
    // Read instraction section info first: [original addess, size]
    address orig_start = 0;
    size_t  orig_size  = 0;
    n = read_bytes(&orig_start, sizeof(address));
    if (n != sizeof(address)) {
      return false;
    }
    n = read_bytes(&orig_size, sizeof(size_t));
    if (n != sizeof(size_t)) {
      return false;
    }

    address code_start = buffer->insts_begin();
    n = read_bytes(code_start, code_size);
    if (n != code_size) {
      return false;
    }
    CodeSection* code = buffer->insts();
    code->set_end(code_start + n);

    // Read relocations
    size_t reloc_offset = entry_position + entry->reloc_offset();
    if (!seek_to_position(reloc_offset)) {
      return false;
    }

    size_t reloc_count = entry->reloc_count();
    if (!read_relocations(buffer, reloc_count, orig_start, orig_size)) {
      return false;
    }
 
    log_info(sca)("Read stub '%s' from shared code archive '%s'", name, _archive_path);
if (UseNewCode2) {
  FlagSetting fs(PrintRelocations, true);
  buffer->print();
  buffer->decode();
}
    return true;
  }
  return false;
}

int SCAFile::write_relocations(CodeBuffer* buffer) {
  CodeSection* code  = buffer->insts();
  size_t reloc_count = code->locs_count();
  if (reloc_count != 0) {
    relocInfo* reloc_start = code->locs_start();
    size_t reloc_size      = reloc_count * sizeof(relocInfo);
    size_t n = write_bytes(reloc_start, reloc_size);
    if (n != reloc_size) {
      return -1;
    }
    // Write additional relocation data: size_t per relocation
    size_t* reloc_data = NEW_C_HEAP_ARRAY(size_t, reloc_count, mtCode);
    size_t  data_size  = reloc_count * sizeof(size_t);

    // Collect data
    RelocIterator iter(code);
    int id = 0;
    while (iter.next()) {
      reloc_data[id] = 0; // initialize
      switch (iter.type()) {
        case relocInfo::virtual_call_type:
          break;
        case relocInfo::opt_virtual_call_type:
          break;
        case relocInfo::static_call_type:
          break;
        case relocInfo::runtime_call_type: {
          // Record offset of runtime destination
          address dest = ((CallRelocation*)iter.reloc())->destination();
          reloc_data[id] = (size_t)pointer_delta(dest, (address)os::init, 1);
          break;
        }
        case relocInfo::runtime_call_w_cp_type:
          break;
        case relocInfo::external_word_type:
          break;
        case relocInfo::internal_word_type:
          break;
        default:
          break;
      }
      id++;
    }
    n = write_bytes(reloc_data, data_size);
    if (n != data_size) {
      return -1;
    }
  }
  return reloc_count;
}

bool SCAFile::store_exception_blob(CodeBuffer* buffer, int pc_offset) {
  if (open_for_write()) {
if (UseNewCode2) {
  FlagSetting fs(PrintRelocations, true);
  buffer->print();
  buffer->decode();
}
    if (!align_write()) {
      return false;
    }
    size_t entry_position = _file_offset;

    // Write pc_offset
    size_t n = write_bytes(&pc_offset, sizeof(int));
    if (n != sizeof(int)) {
      return false;
    }

    // Write name
    const char* name = buffer->name();
    size_t name_offset = _file_offset  - entry_position;
    size_t name_size = strlen(name) + 1; // Includes '/0'
    n = write_bytes(name, name_size);
    if (n != name_size) {
      return false;
    }
    if (!align_write()) {
      return false;
    }

    // Write _inst section
    assert(buffer->blob() != nullptr, "sanity");
    address code_start = buffer->insts_begin();
    size_t code_size = buffer->insts_size();
    size_t code_offset = _file_offset - entry_position;

    // Write instraction section info first: [original addess, size]
    n = write_bytes(&code_start, sizeof(address));
    if (n != sizeof(address)) {
      return false;
    }
    n = write_bytes(&code_size, sizeof(size_t));
    if (n != sizeof(size_t)) {
      return false;
    }

    n = write_bytes(code_start, code_size);
    if (n != code_size) {
      return false;
    }
    if (!align_write()) {
      return false;
    }
    size_t reloc_offset = _file_offset - entry_position;

    // Write relocInfo array
    size_t reloc_count = write_relocations(buffer);
    if (reloc_count < 0) {
      return false;
    }

    SCAEntry entry(entry_position, name_offset, name_size, code_offset, code_size, reloc_offset, reloc_count, (uint32_t)999, _header.next_idx());
    add_entry(entry);
    log_info(sca)("Wrote stub '%s' to shared code archive '%s'", name, _archive_path);
    return true;
  }
  return false;
}
