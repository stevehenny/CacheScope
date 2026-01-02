#pragma once

#include <vector>

#include "common/Types.hpp"

struct PerfSample;

class FalseSharingAnalysis {
public:
  static constexpr size_t CACHE_LINE_SIZE = 64;

  static std::vector<CacheLine> find_hot_cache_lines(
    const std::vector<PerfSample>& samples);

  static void print(const std::vector<CacheLine>& hot_lines,
                    size_t max_lines = 10);
};
