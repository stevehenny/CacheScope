#include "runtime/FalseSharingAnalysis.hpp"

#include <algorithm>
#include <csignal>
#include <format>
#include <iostream>
#include <ranges>
#include <unordered_map>

#include "common/Types.hpp"

static constexpr double WRITE_READ_HOT_RATIO{5.0};

std::vector<CacheLine> FalseSharingAnalysis::find_hot_cache_lines(
  const std::vector<PerfSample>& samples) {
  std::unordered_map<uint64_t, CacheLine> cache_lines;

  // count cache line access
  for (const auto& s : samples) {
    if (s.addr == 0) continue;

    uint64_t base = (s.addr / CACHE_LINE_SIZE) * CACHE_LINE_SIZE;
    auto& line    = cache_lines[base];

    line.base_addr = base;
    line.tids.push_back(s.tid);
    line.addrs.push_back(s.addr);
    line.sample_count++;
    switch (s.event_type) {
      case SampleType::CACHE_LOAD:
        ++line.sample_reads;
        break;
      case SampleType::CACHE_STORE:
        ++line.sample_writes;
        break;
    }
  }

  std::vector<CacheLine> result;
  result.reserve(cache_lines.size());

  // find hot cache lines
  // lines that have a high write/read ratio
  for (auto& [_, line] : cache_lines) {
    std::sort(line.tids.begin(), line.tids.end());
    auto it = std::unique(line.tids.begin(), line.tids.end());

    if (std::distance(line.tids.begin(), it) > 1 &&
        static_cast<double>(line.sample_writes) /
            static_cast<double>(line.sample_reads) >
          WRITE_READ_HOT_RATIO) {
      result.push_back(std::move(line));
    }
  }

  std::ranges::sort(result, [](const auto& a, const auto& b) {
    return a.sample_count > b.sample_count;
  });

  return result;
}

void FalseSharingAnalysis::print(const std::vector<CacheLine>& hot_lines,
                                 size_t max_lines) {
  std::cout << "\n=== False Sharing Analysis ===\n\n";

  for (size_t i = 0; i < std::min(hot_lines.size(), max_lines); ++i) {
    const auto& line = hot_lines[i];

    std::vector<uint32_t> unique_tids = line.tids;
    if (line.tids.size() <= 1) break;
    std::sort(unique_tids.begin(), unique_tids.end());
    unique_tids.erase(std::unique(unique_tids.begin(), unique_tids.end()),
                      unique_tids.end());

    auto [min_addr, max_addr] = std::ranges::minmax(line.addrs);

    std::cout << std::format(
      "Cache Line #{}: 0x{:x}\n"
      "  Samples: {}\n"
      "  Threads: {}\n"
      "  Address range: 0x{:x} - 0x{:x} ({} bytes)\n\n",
      i + 1, line.base_addr, line.sample_count, unique_tids.size(), min_addr,
      max_addr, max_addr - min_addr);
  }
}
