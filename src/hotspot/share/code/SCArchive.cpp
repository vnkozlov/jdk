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
#include "code/SCArchive.hpp"
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
  chmod(_full_path, _S_IREAD | _S_IWRITE);
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

bool SCArchive::load_stub(StubCodeGenerator* cgen, vmIntrinsicID id, address start) {
  if (_archive != nullptr) {
    return _archive->load_stub(cgen, id, start);
  }
  return false;
}

bool SCArchive::store_stub(StubCodeGenerator* cgen, vmIntrinsicID id, address start) {
  if (_archive != nullptr) {
    return _archive->store_stub(cgen, id, start);
  }
  return false;
}

SCAFile::SCAFile(const char* archive_path, int fd, size_t file_size, bool for_read) {
  _archive_path = archive_path;
  _file_size = file_size;
  _file_offset = 0;
  _fd = fd;
  _for_read = for_read;
  _table = nullptr;
  _ga_table = nullptr;

  // Read header st the begining of archive
  size_t header_size = sizeof(SCAHeader);
  if (for_read) {
    // Read header from archive
    seek_to_position(0);
    size_t n = read_bytes((void*)&_header, header_size);
    if (n != header_size) {
      close();
      return;
    }
    assert(_header.version() == VM_Version::jvm_version(), "sanity");
    assert(_header.archive_size() <= file_size, "recorded %d vs actual %d", (int)_header.archive_size(), (int)file_size);
    log_info(sca)("Read header from shared code archive '%s'", archive_path);
  } else {
    // Write initial version of header
    _header.init(VM_Version::jvm_version(), 0, 0, 0, 0, 0);
    size_t header_size = sizeof(SCAHeader);
    size_t n = write_bytes((const void*)&_header, header_size);
    if (n != header_size) {
      return;
    }
    log_info(sca)("Wrote initial header to shared code archive '%s'", archive_path);
  }
}

void SCAFile::close() {
  if (_fd >= 0) {
    if (open_for_write()) { // Finalize archive
      finish_write();
    }
    if (::close(_fd) < 0) {
      log_warning(sca)("Failed to close shared code archive file '%s'", _archive_path);
    }
    _fd = -1;
  }
  log_info(sca)("Closed shared code archive '%s'", _archive_path);
  FREE_C_HEAP_ARRAY(char, _archive_path);
  if (_table != nullptr) {
    FREE_C_HEAP_ARRAY(SCAEntry, _table);
  }
  if (_ga_table != nullptr) {
    delete _ga_table;
  }
}

bool SCAFile::open_for_read() const {
  return (_fd >= 0) && _for_read;
}

bool SCAFile::open_for_write() const {
  return (_fd >= 0) && !_for_read;
}

void SCAFile::seek_to_position(size_t pos) {
  if (os::lseek(_fd, (long)pos, SEEK_SET) < 0) {
    log_warning(sca)("Failed to seek to position " SIZE_FORMAT " in shared code archive file '%s'", pos, _archive_path);
  }
}

size_t SCAFile::read_bytes(void* buffer, size_t nbytes) {
  assert(open_for_read(), "Archive file is not open");
  size_t n = ::read(_fd, buffer, (unsigned int)nbytes);
  if (n != nbytes) {
    // Close the file if there's a problem reading it.
    close();
    log_warning(sca)("Failed to read from shared code archive file '%s'", _archive_path);
    return 0;
  }
  log_info(sca)("Read %d bytes from shared code archive '%s'", (int)nbytes, _archive_path);
  return nbytes;
}


size_t SCAFile::write_bytes(const void* buffer, size_t nbytes) {
  assert(open_for_write(), "Archive file is not created");
  ssize_t n = os::write(_fd, buffer, (unsigned int)nbytes);
  if (n < 0 || (size_t)n != nbytes) {
    tty->print_cr("Failed to write %d bytes to shared code archive file '%s'", (int)nbytes, _archive_path);
    // If the shared archive is corrupted, close it and remove it.
    close();
    remove(_archive_path);
    log_warning(sca)("Failed to write to shared code archive file '%s'", _archive_path);
  }
  log_info(sca)("Wrote %d bytes at offset %d to shared code archive '%s'", (int)nbytes, (int)_file_offset, _archive_path);
  _file_offset += n;
  return (size_t)n;
}

void SCAFile::add_entry(SCAEntry entry) {
  if (_ga_table == nullptr) {
    _ga_table = new(mtCode) GrowableArray<SCAEntry>(4, mtCode); // C heap
    assert(_ga_table != nullptr, "Sanity");
  }
  _ga_table->append(entry);
}

SCAEntry* SCAFile::find_entry(vmIntrinsicID id) {
  size_t count = _header.entries_count();
  if (_table == nullptr) {
    // Read it
    seek_to_position(_header.table_offset());
    size_t size  = sizeof(SCAEntry);
    size_t table_size = count * size; // In bytes
    _table = NEW_C_HEAP_ARRAY(SCAEntry, count, mtCode);
    size_t n = read_bytes((void*)&(_table[0]), table_size);
    if (n != table_size) {
      close();
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
  size_t strings_offset = 0;
  size_t strings_size = 0;
  int count   = _ga_table->length();
  if (count > 0) {
    SCAEntry* table_address = _ga_table->adr_at(0);
    // Write SCAEntry table
    table_offset = _file_offset;
    size_t table_size = count * sizeof(SCAEntry); // In bytes
    size_t n = write_bytes((const void*)table_address, table_size);
    if (n != table_size) {
      return false;
    }
    log_info(sca)("Wrote SCAEntry table to shared code archive '%s'", _archive_path);

    /*
    // Write strings
    int strings_count = _strings_table.length();
    _strings_table = NEW_C_HEAP_ARRAY(size_t, strings_count, mtCode);
    */
  }

  // Finalize and write header
  _header.init(version, count, _file_offset, table_offset, strings_offset, strings_size);
  seek_to_position(0);
  _file_offset = 0;
  size_t header_size = sizeof(SCAHeader);
  size_t n = write_bytes((const void*)&_header, header_size);
  if (n != header_size) {
    return false;
  }
  log_info(sca)("Wrote header to shared code archive '%s'", _archive_path);
  return true;
}

bool SCAFile::load_stub(StubCodeGenerator* cgen, vmIntrinsicID id, address start) {
  if (open_for_read()) {
    assert(start == cgen->assembler()->pc(), "wrong buffer");
    SCAEntry* entry = find_entry(id);
    if (entry == nullptr) {
      return false;
    }
    size_t code_position = entry->offset();
    size_t nbytes = entry->size();
    seek_to_position(code_position);
    size_t n = read_bytes(start, nbytes);
    if (n == nbytes) {
      cgen->assembler()->code_section()->set_end(start + n);
      log_info(sca)("Read stub id:%d from shared code archive '%s'", (int)id, _archive_path);
      return true;
    }
  }
  return false;
}

bool SCAFile::store_stub(StubCodeGenerator* cgen, vmIntrinsicID id, address start) {
  if (open_for_write()) {
    address end = cgen->assembler()->pc();
    size_t header_size = sizeof(SCAEntry);
    // size_t code_position = align_up(_file_offset, (size_t)CodeEntryAlignment);
    size_t code_position = _file_offset;
    size_t nbytes = end - start;
    SCAEntry entry(code_position, nbytes, (uint32_t)id, _header.next_idx(), 0 /* strings_count */);

    // Write code
    size_t n = write_bytes(start, nbytes);
    if (n != nbytes) {
      return false;
    }
    add_entry(entry);
    log_info(sca)("Wrote stub id:%d to shared code archive '%s'", (int)id, _archive_path);
    return true;
  }
  return false;
}

