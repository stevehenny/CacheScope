#pragma once
#include <any>
#include <cstdint>

struct MemAccess {
  uint64_t ip;
  uint64_t addr;
  uint32_t size;
  uint32_t cpu;
  uint32_t tid;
  bool is_write;
};

struct MemAlloc {
  uint64_t base;
  uint64_t size;
  std::any type;
};
// TODO: Need some way to map static structs to memory accesses
