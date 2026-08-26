#pragma once

#include <cstddef>
#include <vector>

#include "common/Types.hpp"

struct PerfSample;

struct FalseSharingOptions {
  size_t cache_line_size = 64;
  size_t min_samples = 1000;
  double min_bounce_score = 0.10;
  double min_private_offset_fraction = 0.50;
  size_t min_unique_top_offsets = 2;
  double max_write_read_ratio = 5.0;
};

class FalseSharingAnalysis {
public:
  static constexpr size_t CACHE_LINE_SIZE = 64;

  static std::vector<CacheLine> find_hot_cache_lines(
    const std::vector<PerfSample>& samples,
    const FalseSharingOptions& options = {});

  static void print(const std::vector<CacheLine>& hot_lines,
                    size_t max_lines = 10);
};
