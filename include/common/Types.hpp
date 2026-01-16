#pragma once
#include <libdwarf/libdwarf.h>

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
  uint64_t base_addr{};
  std::vector<uint32_t> tids;
  std::vector<uint64_t> addrs;
  size_t sample_count{};
  size_t sample_reads{};
  size_t sample_writes{};

  // "Bouncing" heuristic: how often consecutive touches of this line come from
  // different threads (higher suggests cache-line ping-pong / false sharing).
  size_t thread_switches{};
  double bounce_score{};

  // Offset overlap heuristic: false sharing often looks like different threads
  // repeatedly touching different offsets within the same cache line.
  size_t shared_offset_count{};  // offsets touched by >=2 threads
  size_t total_offset_count{};   // distinct offsets touched by any thread
  size_t unique_top_offsets{};   // distinct "most frequent" offsets per thread
  double private_offset_fraction{};  // 1 - shared/total
};

struct DwarfStackObject {
  std::string function;
  std::string name;
  std::string file;
  uint64_t size;
  int64_t frame_offset;
  TypeInfo* type;
};

struct DwarfGlobalObject {
  std::string name;
  std::string file;
  uint64_t size;
  uint64_t addr;  // link-time VMA (DW_OP_addr)
  TypeInfo* type;
};

struct RuntimeStackObject {
  uint64_t function_ip;
  uint64_t cfa;
  uint64_t callsite;
  uint64_t pid;
};

enum class SampleType {
  CACHE_LOAD,
  CACHE_STORE,
};
struct PerfSample {
  uint32_t tid;
  uint32_t pid;
  uint32_t cpu;
  uint64_t ip;
  uint64_t addr;
  uint64_t sp{};  // sampled user stack pointer (perf --user-regs=sp)
  uint64_t bp{};  // sampled user frame pointer (perf --user-regs=bp)
  uint64_t time_stamp{};
  SampleType event_type;
  std::string symbol;
  std::string dso;

  friend std::ostream& operator<<(std::ostream& os, const PerfSample& s) {
    return os << std::format(
             "TID: {}\nPID: {}\nCPU: {}\nIP: 0x{:x}\nADDR: 0x{:x}\nSP: 0x{:x}\nBP: 0x{:x}\n"
             "TIME: {}\nSYM: {}\nDSO: {}\n",
             s.tid, s.pid, s.cpu, s.ip, s.addr, s.sp, s.bp, s.time_stamp,
             s.symbol.empty() ? "<unknown>" : s.symbol,
             s.dso.empty() ? "<unknown>" : s.dso);
  }
};

struct ResolvedVariable {
  std::string name;
  std::string type_name;
  uint64_t address;
  size_t size;
  int64_t offset;
  enum class Kind { Global, Stack, TLS } kind;
};

struct StaticRange {
  uint64_t start;
  uint64_t end;
  DwarfGlobalObject* obj;
};
