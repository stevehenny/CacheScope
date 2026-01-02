#include "runtime/TracerConfig.hpp"

#include <fstream>
#include <stdexcept>

struct RawEvent {
  uint32_t type;
  uint64_t config;
};

static RawEvent encode_cache_event(CacheEvent ev, CpuVendor cpu) {
  if (cpu == CpuVendor::Intel) {
    // Intel PEBS events that support PERF_SAMPLE_ADDR
    // These are the "MEM_*_RETIRED" events from Intel SDM

    switch (ev) {
      case CacheEvent::MEM_LOAD_RETIRED_L1_MISS:
        // event=0xD1, umask=0x08 - L1 data cache misses
        return {PERF_TYPE_RAW, 0x08D1};

      case CacheEvent::MEM_LOAD_RETIRED_L1_HIT:
        // event=0xD1, umask=0x01 - L1 data cache hits
        return {PERF_TYPE_RAW, 0x01D1};

      case CacheEvent::MEM_LOAD_RETIRED_L3_MISS:
        // event=0xD1, umask=0x20 - L3 cache misses
        return {PERF_TYPE_RAW, 0x20D1};

      case CacheEvent::MEM_LOAD_RETIRED_L3_HIT:
        // event=0xD1, umask=0x04 - L3 cache hits
        return {PERF_TYPE_RAW, 0x04D1};

      case CacheEvent::MEM_INST_RETIRED_ALL_LOADS:
        // event=0xD0, umask=0x81 - All loads (retired)
        return {PERF_TYPE_RAW, 0x81D0};
    }
  }

  if (cpu == CpuVendor::AMD) {
    // AMD doesn't have direct equivalents with address sampling
    // You'd need to use IBS (Instruction-Based Sampling) which is more complex
    throw std::runtime_error(
      "AMD address sampling requires IBS, not yet implemented");
  }

  throw std::runtime_error("Unknown CPU vendor");
}

CpuVendor TracerConfig::detect_cpu_vendor() {
  std::ifstream cpuinfo("/proc/cpuinfo");
  std::string line;
  while (std::getline(cpuinfo, line)) {
    if (line.find("GenuineIntel") != std::string::npos) return CpuVendor::Intel;
    if (line.find("AuthenticAMD") != std::string::npos) return CpuVendor::AMD;
  }
  return CpuVendor::Unknown;
}

perf_event_attr TracerConfig::build_attr() const {
  perf_event_attr attr{};
  attr.size = sizeof(attr);

  auto raw_event = encode_cache_event(event, cpu);
  attr.type      = raw_event.type;
  attr.config    = raw_event.config;

  // CRITICAL: Must include PERF_SAMPLE_ADDR and set precise_ip for addresses
  attr.sample_type = PERF_SAMPLE_IP | PERF_SAMPLE_ADDR |  // Virtual address
                     PERF_SAMPLE_TID | PERF_SAMPLE_CPU | PERF_SAMPLE_TIME |
                     PERF_SAMPLE_DATA_SRC |  // Memory hierarchy info
                     PERF_SAMPLE_WEIGHT;     // Latency (optional but useful)

  attr.sample_period  = sample_period;
  attr.disabled       = 1;
  attr.exclude_kernel = exclude_kernel;
  attr.exclude_hv     = exclude_hv;

  // MUST have precise_ip for PERF_SAMPLE_ADDR to work
  // precise_ip=2 means "request 0 skid" which enables PEBS
  attr.precise_ip = precise_ip ? 2 : 0;

  // Enable inheritance for child threads
  attr.inherit = 1;

  return attr;
}
