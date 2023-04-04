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
  size_t _strings_offset;    // offset of char strings (names, values) buffer
  size_t _strings_size;      // size in bytes of strings buffer

public:
  void init(uint version, int count, size_t archive_size, size_t table_offset, size_t strings_offset, size_t strings_size) {
    _version        = version;
    _entries_count  = count;
    _archive_size   = archive_size;
    _table_offset   = table_offset;
    _strings_offset = strings_offset;
    _strings_size   = strings_size;
  }

  uint   version()        const { return _version; }
  int    entries_count()  const { return _entries_count; }
  int    next_idx()             { return _entries_count++; }

  size_t archive_size()   const { return _archive_size; }
  size_t table_offset()   const { return _table_offset; }
  size_t strings_offset() const { return _strings_offset; }
  size_t strings_size()   const { return _strings_size; }
};

// Archive's entry
class SCAEntry {
private:
  size_t   _offset;
  size_t   _size;
  uint32_t _id;     // vmIntrinsic::ID for stub or 0 for nmethod
  int      _idx;    // Sequential index in archive (< SCAHeader::_entries_count)
  int      _strings_count; // Number of strings in archive for this entry
                           // String is sequence of bytes terminated by 0

public:
  SCAEntry(size_t offset, size_t size, uint32_t id, int idx, int strings_count) {
    _offset = offset;
    _size   = size;
    _id     = id;
    _idx    = idx;
    _strings_count = strings_count;
  }

  SCAEntry() {
    _offset = 0;
    _size   = 0;
    _id     = 0;
    _idx    = 0;
    _strings_count = 0;
  }

  size_t   offset() const { return _offset; }
  size_t   size()   const { return _size; }
  uint32_t id()     const { return _id; }
  int      idx()    const { return _idx; }
  int      strings_count() const { return _strings_count; }
};

class SCAFile : public CHeapObj<mtCode> {
private:
  SCAHeader   _header;
  const char* _archive_path;
  int64_t     _file_size;    // Used when reading archive
  size_t      _file_offset;  // Used when writing archive
  int  _fd;
  bool _for_read;

  SCAEntry* _table;                   // Used when reading archive
  GrowableArray<SCAEntry>* _ga_table; // Used when writing archive
/*
  const char* _strings;                    // Used when reading archive
  size_t* _strings_table; // Strings offset table per entry                  
  GrowableArray<const char*>* _ga_strings; // Used when writing archive
*/
  bool open_for_read() const;
  bool open_for_write() const;

  void seek_to_position(size_t pos);
  size_t read_bytes(void* buffer, size_t nbytes);
  size_t write_bytes(const void* buffer, size_t nbytes);

public:
  SCAFile(const char* archive_path, int fd, size_t file_size, bool for_read);

  void close();
  int fd() const { return _fd; }

  void add_entry(SCAEntry entry);
  SCAEntry* find_entry(vmIntrinsicID id);

  bool finish_write();

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
