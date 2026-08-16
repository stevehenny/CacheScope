#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <vector>

#include "common/Types.hpp"
#include "runtime/SampleStats.hpp"
#include "runtime/Thrashing.hpp"

struct ReportMetadata {
  std::string binary;
  std::string event;
  int sample_rate = 0;
};

struct FalseSharingEntry {
  size_t index{};
  int64_t base_addr{};
  size_t sample_count{};
  size_t sample_reads{};
  size_t sample_writes{};
  size_t unique_threads{};
  size_t distinct_offsets{};
  size_t shared_offsets{};
  size_t unique_top_offsets{};
  double private_offset_fraction{};
  size_t thread_switches{};
  double bounce_score{};
  int64_t min_addr{};
  int64_t max_addr{};
  int64_t range_bytes{};
};

struct ThrashingEntry {
  size_t index{};
  int cache_level{};
  int cache_id{};
  std::string cache_type;
  std::string shared_cpu_list;
  std::string address_basis;
  size_t cache_set{};
  int64_t start_time_ns{};
  int64_t end_time_ns{};
  int64_t duration_ns{};
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

struct Report {
  ReportMetadata metadata;
  SampleStats stats;
  std::vector<CacheInfo> cache_topology;
  std::vector<FalseSharingEntry> false_sharing;
  std::vector<ThrashingEntry> cache_thrashing;

  static Report from_false_sharing(const std::string& binary,
                                   const std::string& event,
                                   int sample_rate,
                                   const SampleStats& stats,
                                   const std::vector<CacheLine>& hot_lines);

  static Report from_analysis(
    const std::string& binary, const std::string& event, int sample_rate,
    const SampleStats& stats, const std::vector<CacheLine>& hot_lines,
    const std::vector<ThrashingEvent>& thrashing,
    const std::vector<CacheInfo>& cache_topology);
};

inline std::string format_hex_addr(int64_t addr) {
  return std::format("0x{:x}", static_cast<uint64_t>(addr));
}

inline Report Report::from_false_sharing(const std::string& binary,
                                         const std::string& event,
                                         int sample_rate,
                                         const SampleStats& stats,
                                         const std::vector<CacheLine>& hot_lines) {
  return from_analysis(binary, event, sample_rate, stats, hot_lines, {}, {});
}

inline Report Report::from_analysis(
  const std::string& binary, const std::string& event, int sample_rate,
  const SampleStats& stats, const std::vector<CacheLine>& hot_lines,
  const std::vector<ThrashingEvent>& thrashing,
  const std::vector<CacheInfo>& cache_topology) {
  Report report;
  report.metadata.binary      = binary;
  report.metadata.event       = event;
  report.metadata.sample_rate = sample_rate;
  report.stats                = stats;
  report.cache_topology       = cache_topology;

  size_t index = 0;
  for (const auto& line : hot_lines) {
    std::vector<uint32_t> unique_tids = line.tids;
    std::sort(unique_tids.begin(), unique_tids.end());
    unique_tids.erase(std::unique(unique_tids.begin(), unique_tids.end()),
                      unique_tids.end());
    if (unique_tids.size() <= 1) continue;

    std::vector<int64_t> offsets;
    offsets.reserve(line.addrs.size());
    for (auto a : line.addrs) offsets.push_back(a - line.base_addr);
    std::sort(offsets.begin(), offsets.end());
    offsets.erase(std::unique(offsets.begin(), offsets.end()), offsets.end());

    int64_t min_addr = 0;
    int64_t max_addr = 0;
    if (!line.addrs.empty()) {
      auto [min_it, max_it] =
        std::minmax_element(line.addrs.begin(), line.addrs.end());
      min_addr = *min_it;
      max_addr = *max_it;
    }

    FalseSharingEntry entry;
    entry.index                   = ++index;
    entry.base_addr               = line.base_addr;
    entry.sample_count            = line.sample_count;
    entry.sample_reads            = line.sample_reads;
    entry.sample_writes           = line.sample_writes;
    entry.unique_threads          = unique_tids.size();
    entry.distinct_offsets        = offsets.size();
    entry.shared_offsets          = line.shared_offset_count;
    entry.unique_top_offsets      = line.unique_top_offsets;
    entry.private_offset_fraction = line.private_offset_fraction;
    entry.thread_switches         = line.thread_switches;
    entry.bounce_score            = line.bounce_score;
    entry.min_addr                = min_addr;
    entry.max_addr                = max_addr;
    entry.range_bytes             = (max_addr >= min_addr)
                                      ? (max_addr - min_addr)
                                      : 0;

    report.false_sharing.push_back(entry);
  }

  for (size_t i = 0; i < thrashing.size(); ++i) {
    const auto& event = thrashing[i];
    ThrashingEntry entry;
    entry.index             = i + 1;
    entry.cache_level       = event.cache_level;
    entry.cache_id          = event.cache_id;
    entry.cache_type        = event.cache_type;
    entry.shared_cpu_list   = event.shared_cpu_list;
    entry.address_basis     = address_basis_name(event.address_basis);
    entry.cache_set         = event.cache_set;
    entry.start_time_ns     = event.start_time_ns;
    entry.end_time_ns       = event.end_time_ns;
    entry.duration_ns       = event.end_time_ns > event.start_time_ns
                                ? event.end_time_ns - event.start_time_ns
                                : 0;
    entry.sample_count      = event.sample_count;
    entry.unique_lines      = event.unique_lines;
    entry.evictions         = event.evictions;
    entry.eviction_reloads  = event.eviction_reloads;
    entry.unique_threads    = event.unique_threads;
    entry.unique_cpus       = event.unique_cpus;
    entry.reload_ratio      = event.reload_ratio;
    entry.oversubscription  = event.oversubscription;
    entry.score             = event.score;
    report.cache_thrashing.push_back(entry);
  }

  return report;
}
