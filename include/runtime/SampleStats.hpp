#pragma once

#include <cstddef>
#include <iosfwd>
#include <vector>

struct PerfSample;

class SampleStats {
public:
  size_t total_samples     = 0;
  size_t samples_with_addr = 0;
  size_t samples_with_ip   = 0;
  size_t unique_threads    = 0;
  size_t unique_cpus       = 0;

  static SampleStats compute(const std::vector<PerfSample>& samples);

  friend std::ostream& operator<<(std::ostream& os, const SampleStats& stats);
};
