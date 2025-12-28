#pragma once

#include <linux/perf_event.h>

#include <cstdint>
#include <optional>

enum class CpuVendor { Intel, AMD, Unknown };

enum class CacheEvent {
  L1D_LOAD,
  L1D_STORE,
  LLC_LOAD,
  LLC_STORE,
  CACHE_MISS,
};

struct TracerConfig {
  // ------------------------------------------------------------
  // Target behavior
  // ------------------------------------------------------------
  CacheEvent event;
  uint64_t sample_period{1000};

  bool precise_ip{true};
  bool exclude_kernel{true};
  bool exclude_hv{true};

  // ------------------------------------------------------------
  // Derived fields
  // ------------------------------------------------------------
  CpuVendor cpu{CpuVendor::Unknown};

  // ------------------------------------------------------------
  // API
  // ------------------------------------------------------------
  perf_event_attr build_attr() const;

  static CpuVendor detect_cpu_vendor();
};
