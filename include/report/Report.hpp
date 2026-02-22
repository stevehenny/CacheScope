#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <vector>

#include "common/Types.hpp"
#include "runtime/SampleStats.hpp"

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

struct Report {
  ReportMetadata metadata;
  SampleStats stats;
  std::vector<FalseSharingEntry> false_sharing;

  static Report from_false_sharing(const std::string& binary,
                                   const std::string& event,
                                   int sample_rate,
                                   const SampleStats& stats,
                                   const std::vector<CacheLine>& hot_lines);
};

inline std::string format_hex_addr(int64_t addr) {
  return std::format("0x{:x}", static_cast<uint64_t>(addr));
}

inline Report Report::from_false_sharing(const std::string& binary,
                                         const std::string& event,
                                         int sample_rate,
                                         const SampleStats& stats,
                                         const std::vector<CacheLine>& hot_lines) {
  Report report;
  report.metadata.binary      = binary;
  report.metadata.event       = event;
  report.metadata.sample_rate = sample_rate;
  report.stats                = stats;

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

  return report;
}
