#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <sstream>
#include <string_view>
#include <vector>

#include "report/JsonReport.hpp"
#include "report/TextReport.hpp"
#include "runtime/CacheTopology.hpp"
#include "runtime/Thrashing.hpp"

namespace {

PerfSample sample_for_line(uint64_t line, uint32_t cpu, uint32_t tid,
                           int64_t time_ns,
                           SampleType type = SampleType::CACHE_LOAD,
                           bool with_physical_address = false) {
  PerfSample sample{};
  sample.addr       = static_cast<int64_t>(line * 64);
  sample.phys_addr  =
    with_physical_address ? sample.addr : 0;
  sample.cpu        = cpu;
  sample.tid        = tid;
  sample.time_stamp = time_ns;
  sample.event_type = type;
  return sample;
}

ThrashingOptions test_options() {
  ThrashingOptions options;
  options.min_samples      = 12;
  options.min_reloads      = 4;
  options.min_reload_ratio = 0.5;
  options.max_gap_ns       = 100;
  return options;
}

CacheInfo test_cache(int level = 1, size_t sets = 4,
                     size_t associativity = 2) {
  CacheInfo cache;
  cache.level               = level;
  cache.id                  = 0;
  cache.type                = level == 1 ? "Data" : "Unified";
  cache.size_bytes          = sets * associativity * 64;
  cache.line_size           = 64;
  cache.sets                = sets;
  cache.associativity       = associativity;
  cache.shared_cpu_list     = "0-3";
  cache.shared_cpus         = {0, 1, 2, 3};
  cache.detected_from_sysfs = true;
  return cache;
}

void add_cycle(std::vector<PerfSample>& samples, int64_t& time_ns,
               uint32_t cpu, size_t repetitions) {
  // Lines 4, 8, and 12 all map to set 0 in a four-set cache.
  for (size_t repetition = 0; repetition < repetitions; ++repetition) {
    for (uint64_t line : {4ULL, 8ULL, 12ULL}) {
      samples.push_back(
        sample_for_line(line, cpu, 100 + repetition % 2, time_ns++));
    }
  }
}

bool expect(bool condition, std::string_view message) {
  if (condition) return true;
  std::cerr << "FAILED: " << message << '\n';
  return false;
}

}  // namespace

int main() {
  const auto options = test_options();
  const std::vector<CacheInfo> caches{test_cache()};
  bool ok = true;

  {
    std::vector<PerfSample> samples;
    int64_t time_ns = 1;
    add_cycle(samples, time_ns, 3, 10);
    auto events = ThrashingAnalysis::detect(samples, caches, options);
    ok &= expect(events.size() == 1,
                 "cyclic over-capacity reuse should produce one event");
    if (!events.empty()) {
      ok &= expect(events[0].cache_level == 1 &&
                     events[0].shared_cpu_list == "0-3" &&
                     events[0].cache_set == 0,
                   "event should retain cache instance and set identity");
      ok &= expect(events[0].unique_lines == 3,
                   "event should report all competing lines");
      ok &= expect(events[0].eviction_reloads >= 20,
                   "event should count recurring post-eviction reloads");
      ok &= expect(events[0].reload_ratio > 0.9,
                   "cyclic reuse should have a high reload ratio");
    }
  }

  {
    std::vector<PerfSample> samples;
    for (int64_t i = 1; i <= 30; ++i) {
      const uint64_t line = i % 2 == 0 ? 4 : 8;
      samples.push_back(sample_for_line(line, 0, 1, i));
    }
    ok &= expect(ThrashingAnalysis::detect(samples, caches, options).empty(),
                 "a working set that fits associativity must not trigger");
  }

  {
    std::vector<PerfSample> samples;
    for (uint64_t i = 0; i < 20; ++i) {
      samples.push_back(
        sample_for_line((i + 1) * 4, 0, 1, static_cast<int64_t>(i + 1)));
    }
    ok &= expect(ThrashingAnalysis::detect(samples, caches, options).empty(),
                 "one-pass streaming through a set must not trigger");
  }

  {
    std::vector<PerfSample> samples;
    int64_t time_ns = 1;
    add_cycle(samples, time_ns, 0, 5);
    time_ns += 1'000;
    add_cycle(samples, time_ns, 0, 5);
    auto events = ThrashingAnalysis::detect(samples, caches, options);
    ok &= expect(events.size() == 2,
                 "a long inactivity gap should split two episodes");
  }

  {
    std::vector<PerfSample> samples;
    for (int64_t i = 1; i <= 30; ++i) {
      samples.push_back(
        sample_for_line(static_cast<uint64_t>(i * 4), 0, 1, i,
                        SampleType::PAGE_FAULT));
    }
    ok &= expect(ThrashingAnalysis::detect(samples, caches, options).empty(),
                 "page faults are not CPU cache accesses");
  }

  {
    std::vector<PerfSample> samples;
    int64_t time_ns = 1;
    for (size_t repetition = 0; repetition < 10; ++repetition) {
      for (uint64_t line : {128ULL, 256ULL, 384ULL}) {
        samples.push_back(
          sample_for_line(line, repetition % 2, 10 + repetition % 2,
                          time_ns++, SampleType::CACHE_LOAD, true));
      }
    }
    const std::vector<CacheInfo> multi_level{
      test_cache(1, 4, 2), test_cache(2, 128, 2)};
    const auto events =
      ThrashingAnalysis::detect(samples, multi_level, options);
    ok &= expect(events.size() == 2,
                 "the same trace should be replayed at every cache level");
    ok &= expect(
      std::ranges::any_of(events, [](const auto& event) {
        return event.cache_level == 1 &&
               event.address_basis == AddressBasis::VirtualPageOffset;
      }) &&
        std::ranges::any_of(events, [](const auto& event) {
          return event.cache_level == 2 &&
                 event.address_basis == AddressBasis::Physical;
        }) &&
        std::ranges::all_of(events, [](const auto& event) {
          return event.unique_cpus == 2;
        }),
      "L1 should use exact page offsets while L2 uses physical addresses");
  }

  {
    const auto cpus = CacheTopology::parse_cpu_list("0-2,5,8-9");
    ok &= expect(cpus == std::vector<uint32_t>({0, 1, 2, 5, 8, 9}),
                 "Linux shared_cpu_list ranges should be parsed");
    ok &= expect(CacheTopology::parse_size_bytes("32K") == 32 * 1024,
                 "Linux cache sizes should be parsed");
    ok &= expect(CacheTopology::parse_size_bytes("2M") == 2 * 1024 * 1024,
                 "megabyte cache sizes should be parsed");
  }

  {
    std::vector<PerfSample> samples;
    int64_t time_ns = 1;
    add_cycle(samples, time_ns, 2, 10);
    const auto events = ThrashingAnalysis::detect(samples, caches, options);
    const auto stats  = SampleStats::compute(samples);
    const auto report = Report::from_analysis(
      "synthetic", "mem-loads", 1, stats, {}, events, caches);

    std::ostringstream json;
    JsonReport::write(json, report);
    ok &= expect(json.str().find("\"cache_thrashing\"") != std::string::npos,
                 "JSON report should contain cache-thrashing results");
    ok &= expect(json.str().find("\"cache_topology\"") != std::string::npos,
                 "JSON report should contain detected cache topology");

    std::ostringstream markdown;
    TextReport::write_markdown(markdown, report);
    ok &= expect(
      markdown.str().find("## Cache Thrashing Analysis") != std::string::npos,
      "Markdown report should contain cache-thrashing results");
  }

  return ok ? 0 : 1;
}
