#pragma once

#include <cstdint>
#include <string>
struct MemAccess {
  uint64_t ip;
  uint64_t addr;
  uint32_t tid;
  uint32_t pid;
  uint32_t cpu;
  uint64_t data_src;  // Add this
  bool is_write;

  // Helper to decode data_src
  std::string decode_mem_level() const {
    uint64_t lvl = (data_src >> 5) & 0x1F;  // PERF_MEM_LVL_SHIFT = 5
    if (lvl & 0x01) return "L1";
    if (lvl & 0x02) return "LFB";
    if (lvl & 0x04) return "L2";
    if (lvl & 0x08) return "L3";
    if (lvl & 0x10) return "Local RAM";
    if (lvl & 0x20) return "Remote RAM";
    return "Unknown";
  }
};
