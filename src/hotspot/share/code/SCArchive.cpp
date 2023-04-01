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
  if (SharedCodeFile != nullptr) {
    const int len = (int)strlen(SharedCodeFile);
    char* cp  = NEW_C_HEAP_ARRAY(char, len+1, mtCode);
    memcpy(cp, SharedCodeFile, len);
    cp[len] = '\0';
    const int file_separator = *os::file_separator();
    const char* start = strrchr(cp, file_separator);
    const char* path = (start == NULL) ? cp : (start + 1);

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
    delete _archive;
  }
}

bool SCArchive::open_for_read(const char* archive_path) {
  log_info(sca)("Trying to load shared code archive '%s'", archive_path);
  int fd = os::open(archive_path, O_RDONLY | O_BINARY, 0);
  if (fd < 0) {
    if (errno == ENOENT) {
      log_info(sca)("Specified shared code archive not found '%s'", archive_path);
    } else {
      log_warning(sca)("Failed to open shared code archive file '%s': (%s)", archive_path,
                       os::strerror(errno));
    }
    return false;
  } else {
    log_info(sca)("Opened for read shared code archive '%s'", archive_path);
  }
  SCAFile* archive = new SCAFile(archive_path, fd, true /* for_read */);
  if (archive->fd() < 0) { // failed
    delete archive;
    return false;
  }
  _archive = archive;
  return true;
}

bool SCArchive::open_for_write(const char* archive_path) {
  log_info(sca)("Trying to store shared code archive '%s'", archive_path);
#ifdef _WINDOWS  // On Windows, need WRITE permission to remove the file.
  chmod(_full_path, _S_IREAD | _S_IWRITE);
#endif
  // Use remove() to delete the existing file because, on Unix, this will
  // allow processes that have it open continued access to the file.
  remove(archive_path);
  int fd = os::open(archive_path, O_RDWR | O_CREAT | O_TRUNC | O_BINARY, 0444);
  if (fd < 0) {
    log_warning(sca)("Unable to create shared code archive file '%s': (%s)", archive_path,
                os::strerror(errno));
    return false;
  } else {
    log_info(sca)("Opened for write shared code archive '%s'", archive_path);
  }
  SCAFile* archive = new SCAFile(archive_path, fd, false /* for_read */);
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

SCAFile::SCAFile(const char* archive_path, int fd, bool for_read) {
  _archive_path = archive_path;
  _fd = fd;
  _for_read = for_read;
  // Read header st the begining of archive
  size_t size = sizeof(SCAHeader);
  if (for_read) {
    // Read header from archive
    seek_to_position(0);
    size_t n = read_bytes((void*)&_header, size);
    if (n != size) {
      close();
      return;
    }
  } else {
    _header.init(0, 0);
  }
}


void SCAFile::close() {
  if (_fd >= 0) {
    if (::close(_fd) < 0) {
      log_warning(sca)("Failed to close shared code archive file '%s'", _archive_path);
    }
    _fd = -1;
  }
  FREE_C_HEAP_ARRAY(char, _archive_path);
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
  return (size_t)n;
}

bool SCAFile::load_stub(StubCodeGenerator* cgen, vmIntrinsicID id, address start) {
  if (open_for_read()) {
    assert(start == cgen->assembler()->pc(), "wrong buffer");
    if (id == vmIntrinsics::_mulAdd) {
      size_t code_position = _header.code_offset();
      size_t nbytes = _header.code_size();
      seek_to_position(code_position);
      size_t n = read_bytes(start, nbytes);
      if (n == nbytes) {
        cgen->assembler()->code_section()->set_end(start + n);
tty->print_cr("load_stub success");
        return true;
      }
    }
  }
  return false;
}

bool SCAFile::store_stub(StubCodeGenerator* cgen, vmIntrinsicID id, address start) {
  if (open_for_write()) {
    if (id == vmIntrinsics::_mulAdd) {
tty->print_cr("cgen->assembler()->pc()");
      address end = cgen->assembler()->pc();
      size_t header_size = sizeof(SCAHeader);
      // size_t code_position = align_up(header_size, (size_t)CodeEntryAlignment);
      size_t code_position = header_size;
      size_t nbytes = end - start;
tty->print_cr("code_position: %d nbytes: %d", (int)code_position, (int)nbytes);
      _header.init(code_position, nbytes);

      // Write header to archive
tty->print_cr("seek_to_position(0)");
      seek_to_position(0);
tty->print_cr("write_bytes((const void*)_header");
      size_t n = write_bytes((const void*)&_header, header_size);
      if (n != header_size) {
        return false;
      }

      // Write code
      // seek_to_position(code_position);
tty->print_cr("write_bytes(start");
      n = write_bytes(start, nbytes);
      if (n != nbytes) {
        return false;
      }
      return true;
    }
  }
  return false;
}

