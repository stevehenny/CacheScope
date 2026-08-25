#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "common/Types.hpp"
#include "runtime/CacheTopology.hpp"

struct ThrashingOptions {
  size_t min_samples      = 64;
  size_t min_reloads      = 8;
  double min_reload_ratio = 0.20;
  int64_t max_gap_ns      = 10'000'000;
};

enum class AddressBasis {
  Physical,
  VirtualPageOffset,
  VirtualEstimated,
};

std::string_view address_basis_name(AddressBasis basis);

struct ThrashingEvent {
  int cache_level{};
  int cache_id{};
  std::string cache_type;
  std::string shared_cpu_list;
  AddressBasis address_basis{AddressBasis::VirtualEstimated};
  size_t cache_set{};
  int64_t start_time_ns{};
  int64_t end_time_ns{};
  size_t sample_count{};
  size_t unique_lines{};
  size_t evictions{};
  size_t eviction_reloads{};
  size_t unique_threads{};
  size_t unique_cpus{};
  double reload_ratio{};
  double oversubscription{};
  double score{};
};

class ThrashingAnalysis {
public:
  static std::vector<ThrashingEvent> detect(
    const std::vector<PerfSample>& samples,
    const std::vector<CacheInfo>& caches,
    const ThrashingOptions& options = {});

  static void print(const std::vector<ThrashingEvent>& events,
                    const std::vector<CacheInfo>& caches,
                    size_t max_events = 10);
};
