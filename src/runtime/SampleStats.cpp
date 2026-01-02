#include "runtime/SampleStats.hpp"

#include <format>
#include <ostream>
#include <unordered_set>

#include "common/Types.hpp"

SampleStats SampleStats::compute(const std::vector<PerfSample>& samples) {
  SampleStats s;
  s.total_samples = samples.size();

  std::unordered_set<uint32_t> tids;
  std::unordered_set<uint32_t> cpus;

  for (const auto& sample : samples) {
    if (sample.addr != 0) s.samples_with_addr++;
    if (sample.ip != 0) s.samples_with_ip++;

    tids.insert(sample.tid);
    cpus.insert(sample.cpu);
  }

  s.unique_threads = tids.size();
  s.unique_cpus    = cpus.size();

  return s;
}

std::ostream& operator<<(std::ostream& os, const SampleStats& s) {
  if (s.total_samples == 0) {
    return os << "\n=== Sample Statistics ===\nNo samples collected\n";
  }

  return os << std::format(
           "\n=== Sample Statistics ===\n"
           "Total samples: {}\n"
           "Samples with address: {} ({:.1f}%)\n"
           "Samples with IP: {} ({:.1f}%)\n"
           "Unique threads: {}\n"
           "Unique CPUs: {}\n",
           s.total_samples, s.samples_with_addr,
           100.0 * s.samples_with_addr / s.total_samples, s.samples_with_ip,
           100.0 * s.samples_with_ip / s.total_samples, s.unique_threads,
           s.unique_cpus);
}
