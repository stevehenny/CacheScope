#pragma once
#include <libdwarf-2/libdwarf.h>

#include <cstdint>
#include <format>
#include <string>
#include <vector>

using std::string, std::vector;

// Forward declarations (this solves the cycle)
struct FieldInfo;
struct TypeInfo;

enum class TypeKind {
  Primitive,
  Pointer,
  Array,
  Struct,
  Class,
  Union,
  Enum,
  Typedef,
  Function,
  Const,
  Volatile,
  Reference,
  Unknown
};

struct TypeInfo {
  string name;
  TypeKind kind;
  size_t size  = 0;
  size_t align = 0;

  // Relationships
  TypeInfo* pointee = nullptr;
  TypeInfo* element = nullptr;
  size_t array_len  = 0;

  vector<TypeInfo*> bases;
  vector<FieldInfo*> fields;

  // Flags
  bool is_const    = false;
  bool is_volatile = false;
  bool is_signed   = false;

  // DWARF identity
  Dwarf_Off die_offset = 0;
};

struct FieldInfo {
  string name;
  size_t offset;
  size_t size;
  Dwarf_Unsigned bit_size   = 0;
  Dwarf_Unsigned bit_offset = 0;

  TypeInfo* type;
};

struct StructInfo {
  string name;
  size_t size;
  vector<FieldInfo> fields;
  TypeInfo* self_type;
};

struct StackFrameEvent {
  uint64_t function_ip;
  uint64_t cfa;  // Canonical Frame Address
  uint64_t callsite;
  uint32_t pid;
  uint32_t tid;
};
struct CacheLine {
  uint64_t base_addr = 0;
  std::vector<uint32_t> tids;
  std::vector<uint64_t> addrs;
  size_t sample_count = 0;
};

struct DwarfStackObject {
  std::string function;
  std::string name;
  std::string file;
  uint64_t size;
  int64_t frame_offset;
  TypeInfo* type;
};

struct RuntimeStackObject {
  uint64_t function_ip;
  uint64_t cfa;
  uint64_t callsite;
  uint64_t pid;
};

struct PerfSample {
  uint32_t tid;
  uint32_t pid;
  uint32_t cpu;
  uint64_t ip;
  uint64_t addr;
  uint64_t timestamp;
  std::string symbol;

  friend std::ostream& operator<<(std::ostream& os, const PerfSample& s) {
    return os << std::format(
             "TID: {}\nPID: {}\nCPU: {}\nIP: 0x{:x}\nADDR: 0x{:x}\n"
             "TIME: {}\nSYM: {}\n",
             s.tid, s.pid, s.cpu, s.ip, s.addr, s.timestamp,
             s.symbol.empty() ? "<unknown>" : s.symbol);
  }
};
