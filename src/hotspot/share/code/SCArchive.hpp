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

#ifndef SHARE_CODE_SCARCHIVE_HPP
#define SHARE_CODE_SCARCHIVE_HPP

class SCAHeader {
private:
  size_t _code_offset;
  size_t _code_size;

public:
  void init(size_t code_offset, size_t code_size) {
    _code_offset = code_offset;
    _code_size = code_size;
  }

  size_t code_offset() const { return _code_offset; }
  size_t code_size()   const { return _code_size; }
};

class StubCodeGenerator;
enum class vmIntrinsicID : int;

class SCAFile : public CHeapObj<mtCode> {
private:
  SCAHeader _header;
  const char* _archive_path;
  int _fd;
  bool _for_read;

  bool open_for_read() const;
  bool open_for_write() const;

  void seek_to_position(size_t pos);
  size_t read_bytes(void* buffer, size_t nbytes);
  size_t write_bytes(const void* buffer, size_t nbytes);

public:
  SCAFile(const char* archive_path, int fd, bool for_read);
  void close();
  int fd() const { return _fd; }

  bool load_stub(StubCodeGenerator* cgen, vmIntrinsicID id, address start);
  bool store_stub(StubCodeGenerator* cgen, vmIntrinsicID id, address start);
};

class SCArchive {
private:
  static SCAFile* _archive;

  static bool open_for_read(const char* archive_path);
  static bool open_for_write(const char* archive_path);

public:
  static void initialize();
  static void close();

  static bool load_stub(StubCodeGenerator* cgen, vmIntrinsicID id, address start);
  static bool store_stub(StubCodeGenerator* cgen, vmIntrinsicID id, address start);
};

#endif // SHARE_CODE_SCARCHIVE_HPP
