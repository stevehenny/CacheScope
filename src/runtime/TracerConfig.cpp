#include "runtime/TracerConfig.hpp"

#include <fstream>
#include <stdexcept>

static uint64_t encode_cache_event(CacheEvent ev, CpuVendor cpu) {
  if (cpu == CpuVendor::Intel) {
    // Intel encoding:
    // PERF_TYPE_HW_CACHE
    // config = cache_id | (op << 8) | (result << 16)

    switch (ev) {
      case CacheEvent::L1D_LOAD:
        return PERF_COUNT_HW_CACHE_L1D | (PERF_COUNT_HW_CACHE_OP_READ << 8) |
               (PERF_COUNT_HW_CACHE_RESULT_ACCESS << 16);

      case CacheEvent::L1D_STORE:
        return PERF_COUNT_HW_CACHE_L1D | (PERF_COUNT_HW_CACHE_OP_WRITE << 8) |
               (PERF_COUNT_HW_CACHE_RESULT_ACCESS << 16);

      case CacheEvent::LLC_LOAD:
        return PERF_COUNT_HW_CACHE_LL | (PERF_COUNT_HW_CACHE_OP_READ << 8) |
               (PERF_COUNT_HW_CACHE_RESULT_ACCESS << 16);

      case CacheEvent::LLC_STORE:
        return PERF_COUNT_HW_CACHE_LL | (PERF_COUNT_HW_CACHE_OP_WRITE << 8) |
               (PERF_COUNT_HW_CACHE_RESULT_ACCESS << 16);

      case CacheEvent::CACHE_MISS:
        return PERF_COUNT_HW_CACHE_LL | (PERF_COUNT_HW_CACHE_OP_READ << 8) |
               (PERF_COUNT_HW_CACHE_RESULT_MISS << 16);
    }
  }

  if (cpu == CpuVendor::AMD) {
    // AMD uses same generic HW cache interface,
    // but fewer events are precise / supported
    switch (ev) {
      case CacheEvent::L1D_LOAD:
        return PERF_COUNT_HW_CACHE_L1D | (PERF_COUNT_HW_CACHE_OP_READ << 8) |
               (PERF_COUNT_HW_CACHE_RESULT_ACCESS << 16);

      case CacheEvent::CACHE_MISS:
        return PERF_COUNT_HW_CACHE_LL | (PERF_COUNT_HW_CACHE_OP_READ << 8) |
               (PERF_COUNT_HW_CACHE_RESULT_MISS << 16);

      default:
        throw std::runtime_error("Requested cache event not supported on AMD");
    }
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

  attr.type   = PERF_TYPE_HW_CACHE;
  attr.config = encode_cache_event(event, cpu);

  attr.sample_type =
    PERF_SAMPLE_IP | PERF_SAMPLE_ADDR | PERF_SAMPLE_TID | PERF_SAMPLE_CPU;

  attr.sample_period  = sample_period;
  attr.disabled       = 1;
  attr.exclude_kernel = exclude_kernel;
  attr.exclude_hv     = exclude_hv;
  attr.precise_ip     = precise_ip ? 2 : 0;

  return attr;
}
