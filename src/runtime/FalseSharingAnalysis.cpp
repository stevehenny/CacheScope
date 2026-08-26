#include "runtime/FalseSharingAnalysis.hpp"

#include <algorithm>
#include "common/Format.hpp"
#include <iostream>
#include <ranges>
#include <unordered_map>
#include <unordered_set>

#include "common/Types.hpp"

std::vector<CacheLine> FalseSharingAnalysis::find_hot_cache_lines(
  const std::vector<PerfSample>& samples,
  const FalseSharingOptions& options) {
  if (options.cache_line_size == 0 || options.cache_line_size > 4096 ||
      options.min_samples == 0 || options.min_bounce_score < 0.0 ||
      options.min_bounce_score > 1.0 ||
      options.min_private_offset_fraction < 0.0 ||
      options.min_private_offset_fraction > 1.0) {
    return {};
  }

  std::unordered_map<int64_t, CacheLine> cache_lines;
  for (const auto& sample : samples) {
    if (sample.addr <= 0) continue;
    const auto line_size = static_cast<int64_t>(options.cache_line_size);
    const int64_t base = (sample.addr / line_size) * line_size;
    auto& line = cache_lines[base];
    line.base_addr = base;
    line.tids.push_back(sample.tid);
    line.addrs.push_back(sample.addr);
    ++line.sample_count;
    if (sample.event_type == SampleType::CACHE_LOAD) ++line.sample_reads;
    if (sample.event_type == SampleType::CACHE_STORE) ++line.sample_writes;
  }

  struct Touch {
    int64_t timestamp;
    uint32_t tid;
    size_t offset;
  };
  std::unordered_map<int64_t, std::vector<Touch>> sequences;
  for (auto& [base, line] : cache_lines) {
    if (line.sample_count < options.min_samples) continue;
    std::unordered_set<uint32_t> tids(line.tids.begin(), line.tids.end());
    std::unordered_set<int64_t> addresses(line.addrs.begin(), line.addrs.end());
    if (tids.size() > 1 && addresses.size() > 1) {
      sequences[base].reserve(line.sample_count);
    }
  }

  const auto line_size = static_cast<int64_t>(options.cache_line_size);
  for (const auto& sample : samples) {
    if (sample.addr <= 0) continue;
    const int64_t base = (sample.addr / line_size) * line_size;
    const auto it = sequences.find(base);
    if (it == sequences.end()) continue;
    it->second.push_back(
      {sample.time_stamp, sample.tid,
       static_cast<size_t>(sample.addr - base)});
  }

  for (auto& [base, sequence] : sequences) {
    auto& line = cache_lines[base];
    const bool has_time = std::ranges::any_of(
      sequence, [](const Touch& touch) { return touch.timestamp != 0; });
    if (has_time) {
      std::ranges::stable_sort(
        sequence, [](const Touch& left, const Touch& right) {
          return left.timestamp < right.timestamp;
        });
    }

    for (size_t i = 1; i < sequence.size(); ++i) {
      if (sequence[i - 1].tid != sequence[i].tid) ++line.thread_switches;
    }
    if (sequence.size() > 1) {
      line.bounce_score =
        static_cast<double>(line.thread_switches) /
        static_cast<double>(sequence.size() - 1);
    }

    using OffsetCounts = std::unordered_map<size_t, uint32_t>;
    std::unordered_map<uint32_t, OffsetCounts> per_thread;
    std::unordered_map<size_t, size_t> touched_by;
    for (const auto& touch : sequence) ++per_thread[touch.tid][touch.offset];
    for (const auto& [tid, offsets] : per_thread) {
      (void)tid;
      for (const auto& [offset, count] : offsets) {
        if (count != 0) ++touched_by[offset];
      }
    }

    line.total_offset_count = touched_by.size();
    line.shared_offset_count = static_cast<size_t>(std::ranges::count_if(
      touched_by, [](const auto& item) { return item.second >= 2; }));
    if (line.total_offset_count != 0) {
      line.private_offset_fraction =
        static_cast<double>(line.total_offset_count -
                            line.shared_offset_count) /
        static_cast<double>(line.total_offset_count);
    }

    std::unordered_set<size_t> top_offsets;
    for (const auto& [tid, offsets] : per_thread) {
      (void)tid;
      const auto best = std::ranges::max_element(
        offsets, {}, [](const auto& item) { return item.second; });
      if (best != offsets.end()) top_offsets.insert(best->first);
    }
    line.unique_top_offsets = top_offsets.size();
  }

  std::vector<CacheLine> result;
  for (auto& [base, line] : cache_lines) {
    (void)base;
    if (line.sample_count < options.min_samples) continue;
    std::unordered_set<uint32_t> tids(line.tids.begin(), line.tids.end());
    std::unordered_set<int64_t> addresses(line.addrs.begin(), line.addrs.end());
    if (tids.size() <= 1 || addresses.size() <= 1) continue;
    if (line.private_offset_fraction <
          options.min_private_offset_fraction ||
        line.unique_top_offsets < options.min_unique_top_offsets) {
      continue;
    }

    const double reads =
      static_cast<double>(std::max<size_t>(1, line.sample_reads));
    const double write_read_ratio =
      static_cast<double>(line.sample_writes) / reads;
    const bool bounce_signal =
      line.bounce_score >= options.min_bounce_score;
    if ((line.sample_writes == 0 && bounce_signal) ||
        (line.sample_writes != 0 &&
         (write_read_ratio > options.max_write_read_ratio ||
          bounce_signal))) {
      result.push_back(std::move(line));
    }
  }

  std::ranges::sort(result, [](const auto& left, const auto& right) {
    const double left_score =
      left.bounce_score * left.private_offset_fraction;
    const double right_score =
      right.bounce_score * right.private_offset_fraction;
    if (left_score != right_score) return left_score > right_score;
    if (left.sample_count != right.sample_count) {
      return left.sample_count > right.sample_count;
    }
    return left.base_addr < right.base_addr;
  });
  return result;
}

void FalseSharingAnalysis::print(const std::vector<CacheLine>& hot_lines,
                                 size_t max_lines) {
  std::cout << "\n=== False Sharing Analysis ===\n\n";
  for (size_t i = 0; i < std::min(hot_lines.size(), max_lines); ++i) {
    const auto& line = hot_lines[i];
    const auto [min_addr, max_addr] = std::ranges::minmax(line.addrs);
    std::unordered_set<uint32_t> tids(line.tids.begin(), line.tids.end());
    std::unordered_set<int64_t> addresses(line.addrs.begin(), line.addrs.end());
    std::cout << cachescope::format(
      "Cache Line #{}: 0x{:x}\n"
      "  Samples: {} (reads={}, writes={})\n"
      "  Threads: {}\n"
      "  Distinct offsets: {} (shared={}, private_frac={:.2f}, "
      "top_offsets={})\n"
      "  Thread switches: {} (bounce={:.3f})\n"
      "  Address range: 0x{:x} - 0x{:x} ({} bytes)\n"
      "  Suspected cause: false sharing (confidence={:.3f})\n\n",
      i + 1, line.base_addr, line.sample_count, line.sample_reads,
      line.sample_writes, tids.size(), addresses.size(),
      line.shared_offset_count, line.private_offset_fraction,
      line.unique_top_offsets, line.thread_switches, line.bounce_score,
      min_addr, max_addr, max_addr - min_addr,
      line.bounce_score * line.private_offset_fraction);
  }
}
