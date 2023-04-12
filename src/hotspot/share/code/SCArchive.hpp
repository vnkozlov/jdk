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

class StubCodeGenerator;
enum class vmIntrinsicID : int;

// Archive file header
class SCAHeader {
private:
  // Here should be version and other verification fields
  uint   _version;           // JDK version (should match when reading archive)
  int    _entries_count;     // number of recorded entries in archive
  size_t _archive_size;      // archive size in bytes
  size_t _table_offset;      // offset of SCAEntry array describing entries

public:
  void init(uint version, int count, size_t archive_size, size_t table_offset) {
    _version        = version;
    _entries_count  = count;
    _archive_size   = archive_size;
    _table_offset   = table_offset;
  }

  uint   version()        const { return _version; }
  int    entries_count()  const { return _entries_count; }
  int    next_idx()             { return _entries_count++; }

  size_t archive_size()   const { return _archive_size; }
  size_t table_offset()   const { return _table_offset; }
};

// Archive's entry
class SCAEntry {
private:
  size_t   _offset; // Offset to entry [first constans then code]
  size_t   _name_offset; // Method's or intrinsic name
  size_t   _name_size;
  size_t   _code_offset; // Start of code
  size_t   _code_size;
  size_t   _reloc_offset;// Relocations
  size_t   _reloc_count;
  uint32_t _id;     // vmIntrinsic::ID for stub or 0 for nmethod
  int      _idx;    // Sequential index in archive (< SCAHeader::_entries_count)

public:
  SCAEntry(size_t offset, size_t name_offset, size_t name_size,
          size_t code_offset, size_t code_size,
          size_t reloc_offset, size_t reloc_count, uint32_t id, int idx) {
    _offset       = offset;
    _name_offset  = name_offset;
    _name_size    = name_size;
    _code_offset  = code_offset;
    _code_size    = code_size;
    _reloc_offset = reloc_offset;
    _reloc_count  = reloc_count;
    _id     = id;
    _idx    = idx;
  }

  SCAEntry() {
    _offset = 0;
    _name_offset  = 0;
    _name_size    = 0;
    _code_offset  = 0;
    _code_size    = 0;
    _reloc_offset = 0;
    _reloc_count  = 0;
    _id   = 0;
    _idx  = 0;
  }

  size_t   offset()       const { return _offset; }
  size_t   code_offset()  const { return _code_offset; }
  size_t   code_size()    const { return _code_size; }
  size_t   reloc_offset() const { return _reloc_offset; }
  size_t   reloc_count()  const { return _reloc_count; }
  size_t   name_offset()  const { return _name_offset; }
  size_t   name_size()    const { return _name_size; }
  uint32_t id()           const { return _id; }
  int      idx()          const { return _idx; }
};

class SCAFile : public CHeapObj<mtCode> {
private:
  SCAHeader   _header;
  const char* _archive_path;
  int64_t     _file_size;    // Used when reading archive
  size_t      _file_offset;  // Used when writing archive
  int  _fd;                  // _fd == -1 - file is closed
  bool _for_read;
  bool _failed;

  SCAEntry* _table;                      // Used when reading archive
  GrowableArray<SCAEntry>* _write_table; // Used when writing archive
  bool open_for_read() const;
  bool open_for_write() const;

  bool   seek_to_position(size_t pos);
  bool   align_write();
  size_t read_bytes(void* buffer, size_t nbytes);
  size_t write_bytes(const void* buffer, size_t nbytes);

  void failed() { _failed = true; }

public:
  SCAFile(const char* archive_path, int fd, size_t file_size, bool for_read);

  void close();
  int fd() const { return _fd; }

  void add_entry(SCAEntry entry);
  SCAEntry* find_entry(uint32_t id);

  bool finish_write();

  bool load_stub(StubCodeGenerator* cgen, vmIntrinsicID id, const char* name, address start);
  bool store_stub(StubCodeGenerator* cgen, vmIntrinsicID id, const char* name, address start);

  bool read_relocations(CodeBuffer* buffer, size_t reloc_count, address orig_start, size_t orig_size);
  int  write_relocations(CodeBuffer* buffer);

  bool load_exception_blob(CodeBuffer* buffer, int* pc_offset);
  bool store_exception_blob(CodeBuffer* buffer, int pc_offset);
};

class SCArchive {
private:
  static SCAFile* _archive;

  static bool open_for_read(const char* archive_path);
  static bool open_for_write(const char* archive_path);

public:
  static void initialize();
  static void close();

  static bool load_stub(StubCodeGenerator* cgen, vmIntrinsicID id, const char* name, address start) {
    if (_archive != nullptr) {
      return _archive->load_stub(cgen, id, name, start);
    }
    return false;
  }

  static bool store_stub(StubCodeGenerator* cgen, vmIntrinsicID id, const char* name, address start) {
    if (_archive != nullptr) {
      return _archive->store_stub(cgen, id, name, start);
    }
    return false;
  }

  static bool load_exception_blob(CodeBuffer* buffer, int* pc_offset) {
    if (_archive != nullptr) {
      return _archive->load_exception_blob(buffer, pc_offset);
    }
    return false;
  }

  static bool store_exception_blob(CodeBuffer* buffer, int pc_offset) {
    if (_archive != nullptr) {
      return _archive->store_exception_blob(buffer, pc_offset);
    }
    return false;
  }

  // Unimplemented
  static bool load_opto_stub(StubCodeGenerator* cgen, vmIntrinsicID id, const char* name, address start) { return false; }
  static bool store_opto_stub(StubCodeGenerator* cgen, vmIntrinsicID id, const char* name, address start) { return false; }
};

#endif // SHARE_CODE_SCARCHIVE_HPP
