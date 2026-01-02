#pragma once

#include <linux/perf_event.h>

#include <cstdint>
#include <optional>

enum class CpuVendor { Intel, AMD, Unknown };

enum class CacheEvent {
  MEM_LOAD_RETIRED_L1_MISS,
  MEM_LOAD_RETIRED_L1_HIT,
  MEM_LOAD_RETIRED_L3_MISS,
  MEM_LOAD_RETIRED_L3_HIT,
  MEM_INST_RETIRED_ALL_LOADS,
};

struct TracerConfig {
  CacheEvent event;
  uint64_t sample_period{1000};

  bool precise_ip{true};
  bool exclude_kernel{true};
  bool exclude_hv{true};

  CpuVendor cpu{CpuVendor::Unknown};

  perf_event_attr build_attr() const;

  static CpuVendor detect_cpu_vendor();
};
