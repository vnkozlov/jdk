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

class AbstractCompiler;
class ciConstant;
class ciEnv;
class ciMethod;
class CodeBuffer;
class CodeOffsets;
class DebugInformationRecorder;
class Dependencies;
class ExceptionTable;
class ExceptionHandlerTable;
class ImplicitExceptionTable;
class OopMapSet;
class OopRecorder;
class StubCodeGenerator;

template <typename T> class GrowableArray;

enum class vmIntrinsicID : int;

// Archive file header
class SCAHeader {
private:
  // Here should be version and other verification fields
  uint _version;           // JDK version (should match when reading archive)
  uint _entries_count;     // number of recorded entries in archive
  uint _archive_size;      // archive size in bytes
  uint _entries_offset;    // offset of SCAEntry array describing entries

public:
  void init(uint version, int count, uint archive_size, uint entries_offset) {
    _version        = version;
    _entries_count  = count;
    _archive_size   = archive_size;
    _entries_offset = entries_offset;
  }

  uint version()        const { return _version; }
  uint entries_count()  const { return _entries_count; }
  uint next_idx()             { return _entries_count++; }

  uint archive_size()   const { return _archive_size; }
  uint entries_offset() const { return _entries_offset; }
};

// Archive's entry contain information from CodeBuffer
class SCAEntry {
public:
  enum Kind {
    None = 0,
    Stub = 1,
    Blob = 2,
    Code = 3
  };

private:
  uint   _offset; // Offset to entry
  uint   _name_offset; // Method's or intrinsic name
  uint   _name_size;
  uint   _code_offset; // Start of code in archive
  uint   _code_size;   // Total size of all code sections
  uint   _reloc_offset;// Relocations
  uint   _reloc_size;  // Max size of relocations per code section
  Kind   _kind;        // 1:stub, 2:blob, 3:nmethod
  uint   _id;          // vmIntrinsic::ID for stub or name's hash for nmethod
  uint   _idx;         // Sequential index in archive (< SCAHeader::_entries_count)
  uint   _decompile;   // Decompile count for this nmethod
  bool   _not_entrant; // Deoptimized

public:
  SCAEntry(uint offset, uint name_offset, uint name_size,
           uint code_offset, uint code_size,
           uint reloc_offset, uint reloc_size,
           Kind kind, uint id, uint idx, uint decomp = 0) {
    _offset       = offset;
    _name_offset  = name_offset;
    _name_size    = name_size;
    _code_offset  = code_offset;
    _code_size    = code_size;
    _reloc_offset = reloc_offset;
    _reloc_size   = reloc_size;
    _kind         = kind;
    _id           = id;
    _idx          = idx;
    _decompile    = decomp;
    _not_entrant  = false;
  }

  SCAEntry() {
    _offset = 0;
    _name_offset  = 0;
    _name_size    = 0;
    _code_offset  = 0;
    _code_size    = 0;
    _reloc_offset = 0;
    _reloc_size   = 0;
    _kind         = None;
    _id           = 0;
    _idx          = 0;
    _decompile    = 0;
    _not_entrant  = false;
  }

  uint offset()       const { return _offset; }
  uint name_offset()  const { return _name_offset; }
  uint name_size()    const { return _name_size; }
  uint code_offset()  const { return _code_offset; }
  uint code_size()    const { return _code_size; }
  uint reloc_offset() const { return _reloc_offset; }
  uint reloc_size()   const { return _reloc_size; }
  Kind kind()         const { return _kind; }
  uint id()           const { return _id; }
  uint idx()          const { return _idx; }
  uint decompile()    const { return _decompile; }
  bool not_entrant()  const { return _not_entrant; }
  void set_not_entrant()    { _not_entrant = true; }
};

// Addresses of stubs, blobs and runtime finctions called from compiled code.
class SCATable : public CHeapObj<mtCode> {
private:
  address* _extrs_addr;
  address* _stubs_addr;
  address* _blobs_addr;
  uint     _extrs_length;
  uint     _stubs_length;
  uint     _blobs_length;

  bool _complete;
public:
  SCATable() { _complete = false; }
  ~SCATable();
  void init();
  void init_opto();
  int  id_for_address(address addr);
  address address_for_id(int id);
};

class SCAFile : public CHeapObj<mtCode> {
private:
  SCAHeader   _header;
  const char* _archive_path;
  uint        _file_size;    // Used when reading archive
  uint        _file_offset;
  int  _fd;                  // _fd == -1 - file is closed
  bool _for_read;
  bool _failed;              // Failed read/write to/from archive (archive is broken?)
  bool _lookup_failed;       // Failed to lookup for info (skip only this code load)

  SCATable* _table;

  SCAEntry* _entries;                      // Used when reading archive
  GrowableArray<SCAEntry>* _write_entries; // Used when writing archive

  ciMethod* _target; // Current compiled method

  void set_target(ciMethod* target) { _target = target; }
  ciMethod* target()          const { return _target; }   

  bool for_read() const;
  bool for_write() const;

  static SCAFile* open_for_read();
  static SCAFile* open_for_write();

  bool seek_to_position(uint pos);
  bool align_write();
  uint read_bytes(void* buffer, uint nbytes);
  uint write_bytes(const void* buffer, uint nbytes);

  void set_failed()   { _failed = true; }
  bool failed() const { return _failed; }

  void set_lookup_failed()     { _lookup_failed = true; }
  void clear_lookup_failed()   { _lookup_failed = false; }
  bool lookup_failed()   const { return _lookup_failed; }

  enum class DataKind: int {
    No_Data   = -1,
    Null      = 0,
    Klass     = 1,
    Method    = 2,
    String    = 3,
    Array     = 4, // primitive array
    SysLoader = 5, // java_system_loader
    PlaLoader = 6  // java_platform_loader
  };

public:
  SCAFile(const char* archive_path, int fd, uint file_size, bool for_read);
  ~SCAFile();
  static void init_table();
  static void init_opto_table();
  int fd() const { return _fd; }

  void add_entry(SCAEntry entry);
  SCAEntry* find_entry(SCAEntry::Kind kind, uint id, uint decomp = 0);
  void invalidate(SCAEntry* entry);

  bool finish_write();

  static bool load_stub(StubCodeGenerator* cgen, vmIntrinsicID id, const char* name, address start);
  static bool store_stub(StubCodeGenerator* cgen, vmIntrinsicID id, const char* name, address start);

  Klass* read_klass(const methodHandle& comp_method);
  Method* read_method(const methodHandle& comp_method);

  bool write_klass(Klass* klass);
  bool write_method(Method* method);

  bool read_code(CodeBuffer* buffer, CodeBuffer* orig_buffer);
  bool read_relocations(CodeBuffer* buffer, CodeBuffer* orig_buffer, uint reloc_size);
  bool write_code(CodeBuffer* buffer, uint& code_size);
  bool write_relocations(CodeBuffer* buffer, uint& reloc_size);
  DebugInformationRecorder* read_debug_info(OopRecorder* oop_recorder);
  bool write_debug_info(DebugInformationRecorder* recorder);
  OopMapSet* read_oop_maps();
  bool write_oop_maps(OopMapSet* oop_maps);

  jobject read_oop(JavaThread* thread, const methodHandle& comp_method);
  Metadata* read_metadata(const methodHandle& comp_method);
  bool read_oops(OopRecorder* oop_recorder, ciMethod* target);
  bool read_metadata(OopRecorder* oop_recorder, ciMethod* target);

  bool write_oop(jobject& jo);
  bool write_oops(OopRecorder* oop_recorder);
  bool write_metadata(Metadata* m);
  bool write_metadata(OopRecorder* oop_recorder);

  static bool load_exception_blob(CodeBuffer* buffer, int* pc_offset);
  static bool store_exception_blob(CodeBuffer* buffer, int pc_offset);

  static bool load_nmethod(ciEnv* env, ciMethod* target, int entry_bci, AbstractCompiler* compiler);

  static bool store_nmethod(const methodHandle& method,
                     int compile_id,
                     int entry_bci,
                     CodeOffsets* offsets,
                     int orig_pc_offset,
                     DebugInformationRecorder* recorder,
                     Dependencies* dependencies,
                     CodeBuffer *code_buffer,
                     int frame_size,
                     OopMapSet* oop_maps,
                     ExceptionHandlerTable* handler_table,
                     ImplicitExceptionTable* nul_chk_table,
                     AbstractCompiler* compiler,
                     bool has_unsafe_access,
                     bool has_wide_vectors,
                     bool has_monitors);
};

class SCArchive {
private:
  static SCAFile*  _archive;

  static bool open_for_read(const char* archive_path);
  static bool open_for_write(const char* archive_path);

public:
  static SCAFile* archive() { return _archive; }
  static void initialize();
  static void close();
  static bool is_on() { return _archive != nullptr; }
  static bool allow_const_field(ciConstant& value);
  static void invalidate(SCAEntry* entry);
};

#endif // SHARE_CODE_SCARCHIVE_HPP
