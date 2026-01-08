#include "runtime/FalseSharingAnalysis.hpp"

#include <algorithm>
#include <array>
#include <csignal>
#include <format>
#include <iostream>
#include <ranges>
#include <unordered_map>
#include <unordered_set>

#include "common/Types.hpp"

static constexpr double WRITE_READ_HOT_RATIO{5.0};
static constexpr size_t MIN_HOT_SAMPLES{1000};
static constexpr double MIN_BOUNCE_SCORE{0.10};
static constexpr double MIN_PRIVATE_OFFSET_FRACTION{0.50};
static constexpr size_t MIN_UNIQUE_TOP_OFFSETS{2};

std::vector<CacheLine> FalseSharingAnalysis::find_hot_cache_lines(
  const std::vector<PerfSample>& samples) {
  std::unordered_map<uint64_t, CacheLine> cache_lines;

  // Pass 1: aggregate per cache line (counts, tids, offsets)
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

  // Pass 2: compute time-ordered switching + per-thread offset overlap for
  // candidate lines. This avoids inflated switching due to cross-CPU
  // interleaving in perf script output.
  struct Touch {
    uint64_t t;
    uint32_t tid;
    uint8_t off;
  };
  std::unordered_map<uint64_t, std::vector<Touch>> seq;
  seq.reserve(cache_lines.size());

  for (auto& [base, line] : cache_lines) {
    if (line.sample_count < MIN_HOT_SAMPLES) continue;

    std::sort(line.tids.begin(), line.tids.end());
    auto tid_it = std::unique(line.tids.begin(), line.tids.end());
    const auto unique_tid_count =
      static_cast<size_t>(std::distance(line.tids.begin(), tid_it));

    std::vector<uint64_t> offsets;
    offsets.reserve(line.addrs.size());
    for (auto a : line.addrs) offsets.push_back(a - line.base_addr);
    std::sort(offsets.begin(), offsets.end());
    offsets.erase(std::unique(offsets.begin(), offsets.end()), offsets.end());
    const auto unique_offset_count = offsets.size();

    if (unique_tid_count <= 1 || unique_offset_count <= 1) continue;

    seq[base].reserve(line.sample_count);
  }

  if (!seq.empty()) {
    for (const auto& s : samples) {
      if (s.addr == 0) continue;
      uint64_t base = (s.addr / CACHE_LINE_SIZE) * CACHE_LINE_SIZE;
      auto it       = seq.find(base);
      if (it == seq.end()) continue;
      const uint8_t off = static_cast<uint8_t>(s.addr - base);
      it->second.push_back(Touch{s.time_stamp, s.tid, off});
    }

    for (auto& [base, v] : seq) {
      auto& line = cache_lines[base];

      bool any_time = false;
      for (const auto& e : v) {
        if (e.t != 0) {
          any_time = true;
          break;
        }
      }
      if (any_time) {
        std::sort(v.begin(), v.end(),
                  [](const auto& a, const auto& b) { return a.t < b.t; });
      }

      // Switches
      uint32_t last  = 0;
      bool have_last = false;
      for (const auto& e : v) {
        if (have_last && e.tid != last) ++line.thread_switches;
        last      = e.tid;
        have_last = true;
      }
      if (v.size() > 1) {
        line.bounce_score = static_cast<double>(line.thread_switches) /
                            static_cast<double>(v.size() - 1);
      }

      // Per-thread offset histograms (0..63)
      std::unordered_map<uint32_t, std::array<uint32_t, CACHE_LINE_SIZE>>
        counts;
      counts.reserve(8);
      for (const auto& e : v) {
        counts[e.tid][e.off]++;
      }

      // shared offsets: touched by >=2 tids
      std::array<uint16_t, CACHE_LINE_SIZE> touched_by{};
      for (const auto& [tid, arr] : counts) {
        (void)tid;
        for (size_t i = 0; i < CACHE_LINE_SIZE; ++i) {
          if (arr[i] != 0) touched_by[i]++;
        }
      }

      size_t total_off  = 0;
      size_t shared_off = 0;
      for (size_t i = 0; i < CACHE_LINE_SIZE; ++i) {
        if (touched_by[i] > 0) {
          ++total_off;
          if (touched_by[i] >= 2) ++shared_off;
        }
      }
      line.total_offset_count  = total_off;
      line.shared_offset_count = shared_off;
      line.private_offset_fraction =
        (total_off == 0) ? 0.0
                         : static_cast<double>(total_off - shared_off) /
                             static_cast<double>(total_off);

      // unique top offsets per thread
      std::unordered_set<uint8_t> tops;
      for (const auto& [tid, arr] : counts) {
        (void)tid;
        uint32_t best  = 0;
        uint8_t best_i = 0;
        for (uint8_t i = 0; i < CACHE_LINE_SIZE; ++i) {
          if (arr[i] > best) {
            best   = arr[i];
            best_i = i;
          }
        }
        if (best != 0) tops.insert(best_i);
      }
      line.unique_top_offsets = tops.size();
    }
  }

  std::vector<CacheLine> result;
  result.reserve(cache_lines.size());

  // Filter: hot + multi-thread + multi-offset + interleaving + low offset
  // overlap
  for (auto& [_, line] : cache_lines) {
    if (line.sample_count < MIN_HOT_SAMPLES) continue;

    std::vector<uint32_t> unique_tids = line.tids;
    std::sort(unique_tids.begin(), unique_tids.end());
    unique_tids.erase(std::unique(unique_tids.begin(), unique_tids.end()),
                      unique_tids.end());

    std::vector<uint64_t> offsets;
    offsets.reserve(line.addrs.size());
    for (auto a : line.addrs) offsets.push_back(a - line.base_addr);
    std::sort(offsets.begin(), offsets.end());
    offsets.erase(std::unique(offsets.begin(), offsets.end()), offsets.end());

    if (unique_tids.size() <= 1 || offsets.size() <= 1) continue;

    // Heuristic to separate "true sharing" (threads hammer same word/offset)
    // from "false sharing" (threads mostly touch different words in same line).
    if (line.private_offset_fraction < MIN_PRIVATE_OFFSET_FRACTION ||
        line.unique_top_offsets < MIN_UNIQUE_TOP_OFFSETS) {
      continue;
    }

    // If we have store info (Intel PEBS), keep the original strong signal.
    if (line.sample_writes > 0) {
      const double reads =
        static_cast<double>(std::max<size_t>(1, line.sample_reads));
      const double ratio = static_cast<double>(line.sample_writes) / reads;
      if (ratio > WRITE_READ_HOT_RATIO ||
          line.bounce_score >= MIN_BOUNCE_SCORE) {
        result.push_back(std::move(line));
      }
    } else {
      // For event sources without reliable load/store split (e.g., AMD IBS):
      if (line.bounce_score >= MIN_BOUNCE_SCORE) {
        result.push_back(std::move(line));
      }
    }
  }

  std::ranges::sort(result, [](const auto& a, const auto& b) {
    const double as = a.bounce_score * a.private_offset_fraction;
    const double bs = b.bounce_score * b.private_offset_fraction;
    if (as != bs) return as > bs;
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
    std::sort(unique_tids.begin(), unique_tids.end());
    unique_tids.erase(std::unique(unique_tids.begin(), unique_tids.end()),
                      unique_tids.end());
    if (unique_tids.size() <= 1) continue;

    std::vector<uint64_t> offsets;
    offsets.reserve(line.addrs.size());
    for (auto a : line.addrs) offsets.push_back(a - line.base_addr);
    std::sort(offsets.begin(), offsets.end());
    offsets.erase(std::unique(offsets.begin(), offsets.end()), offsets.end());

    auto [min_addr, max_addr] = std::ranges::minmax(line.addrs);

    std::cout << std::format(
      "Cache Line #{}: 0x{:x}\n"
      "  Samples: {} (reads={}, writes={})\n"
      "  Threads: {}\n"
      "  Distinct offsets: {} (shared={}, private_frac={:.2f}, "
      "top_offsets={})\n"
      "  Thread switches: {} (bounce={:.3f})\n"
      "  Address range: 0x{:x} - 0x{:x} ({} bytes)\n\n",
      i + 1, line.base_addr, line.sample_count, line.sample_reads,
      line.sample_writes, unique_tids.size(), offsets.size(),
      line.shared_offset_count, line.private_offset_fraction,
      line.unique_top_offsets, line.thread_switches, line.bounce_score,
      min_addr, max_addr, max_addr - min_addr);
  }
}
