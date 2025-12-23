#pragma once

#include <cstdint>

struct MemAccess {
  uint64_t ip;
  uint64_t addr;
  uint32_t size;
  uint32_t tid;
  uint32_t cpu;
  bool is_write;
};
