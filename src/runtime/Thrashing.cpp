#include "runtime/Thrashing.hpp"

#include <unistd.h>

#include <algorithm>
#include "common/Format.hpp"
#include <iostream>
#include <list>
#include <ranges>
#include <unordered_map>
#include <unordered_set>

namespace {

struct Access {
  uint64_t line{};
  int64_t time_ns{};
  uint32_t tid{};
  uint32_t cpu{};
  size_t order{};
};

using AccessGroups = std::unordered_map<size_t, std::vector<Access>>;

void analyze_segment(const CacheInfo& cache, AddressBasis basis,
                     size_t cache_set, const std::vector<Access>& accesses,
                     size_t begin, size_t end,
                     const ThrashingOptions& options,
                     std::vector<ThrashingEvent>& events) {
  const size_t sample_count = end - begin;
  if (sample_count < options.min_samples) return;

  std::list<uint64_t> lru;
  std::unordered_map<uint64_t, std::list<uint64_t>::iterator> resident;
  std::unordered_set<uint64_t> seen;
  std::unordered_set<uint32_t> tids;
  std::unordered_set<uint32_t> cpus;
  resident.reserve(cache.associativity * 2);
  seen.reserve(sample_count);

  size_t evictions = 0;
  size_t reloads   = 0;
  for (size_t i = begin; i < end; ++i) {
    const auto& access = accesses[i];
    tids.insert(access.tid);
    cpus.insert(access.cpu);

    if (auto it = resident.find(access.line); it != resident.end()) {
      lru.splice(lru.begin(), lru, it->second);
      it->second = lru.begin();
      continue;
    }

    const bool previously_seen = seen.contains(access.line);
    if (resident.size() >= cache.associativity) {
      resident.erase(lru.back());
      lru.pop_back();
      ++evictions;
      if (previously_seen) ++reloads;
    }

    lru.push_front(access.line);
    resident[access.line] = lru.begin();
    seen.insert(access.line);
  }

  if (seen.size() <= cache.associativity ||
      reloads < options.min_reloads || evictions == 0) {
    return;
  }

  const double reload_ratio =
    static_cast<double>(reloads) / static_cast<double>(evictions);
  if (reload_ratio < options.min_reload_ratio) return;

  const double oversubscription =
    static_cast<double>(seen.size()) /
    static_cast<double>(cache.associativity);
  const double excess_pressure = std::min(1.0, oversubscription - 1.0);

  ThrashingEvent event;
  event.cache_level        = cache.level;
  event.cache_id           = cache.id;
  event.cache_type         = cache.type;
  event.shared_cpu_list    = cache.shared_cpu_list;
  event.address_basis      = basis;
  event.cache_set          = cache_set;
  event.start_time_ns      = accesses[begin].time_ns;
  event.end_time_ns        = accesses[end - 1].time_ns;
  event.sample_count       = sample_count;
  event.unique_lines       = seen.size();
  event.evictions          = evictions;
  event.eviction_reloads   = reloads;
  event.unique_threads     = tids.size();
  event.unique_cpus        = cpus.size();
  event.reload_ratio       = reload_ratio;
  event.oversubscription   = oversubscription;
  event.score = reload_ratio * (0.5 + 0.5 * excess_pressure);
  events.push_back(std::move(event));
}

AddressBasis choose_address_basis(const std::vector<PerfSample>& samples,
                                  const CacheInfo& cache,
                                  const ThrashingOptions& options) {
  const long page_size_value = sysconf(_SC_PAGESIZE);
  const size_t page_size =
    page_size_value > 0 ? static_cast<size_t>(page_size_value) : 4096;
  const size_t page_offset_sets =
    cache.line_size <= page_size ? page_size / cache.line_size : 0;
  const bool page_offset_safe =
    page_offset_sets > 0 && cache.sets <= page_offset_sets;
  if (page_offset_safe) return AddressBasis::VirtualPageOffset;

  size_t physical_samples = 0;
  size_t consistent_page_offsets = 0;
  for (const auto& sample : samples) {
    if (sample.addr > 0 && sample.phys_addr > 0 &&
        sample.event_type != SampleType::PAGE_FAULT &&
        cache.contains_cpu(sample.cpu)) {
      ++physical_samples;
      if (page_offset_sets > 0) {
        const auto virtual_line =
          static_cast<uint64_t>(sample.addr) / cache.line_size;
        const auto physical_line =
          static_cast<uint64_t>(sample.phys_addr) / cache.line_size;
        if (virtual_line % page_offset_sets ==
            physical_line % page_offset_sets) {
          ++consistent_page_offsets;
        }
      }
    }
  }
  const bool physical_addresses_valid =
    physical_samples >= options.min_samples && page_offset_sets > 0 &&
    consistent_page_offsets * 100 >= physical_samples * 95;
  if (physical_addresses_valid) {
    return AddressBasis::Physical;
  }

  return AddressBasis::VirtualEstimated;
}

}  // namespace

std::string_view address_basis_name(AddressBasis basis) {
  switch (basis) {
    case AddressBasis::Physical:
      return "physical";
    case AddressBasis::VirtualPageOffset:
      return "virtual-page-offset";
    case AddressBasis::VirtualEstimated:
      return "virtual-estimate";
  }
  return "unknown";
}

std::vector<ThrashingEvent> ThrashingAnalysis::detect(
  const std::vector<PerfSample>& samples,
  const std::vector<CacheInfo>& caches,
  const ThrashingOptions& options) {
  if (options.min_samples == 0 || options.min_reload_ratio < 0.0 ||
      options.min_reload_ratio > 1.0 || options.max_gap_ns < 0) {
    return {};
  }

  std::vector<ThrashingEvent> events;
  for (const auto& cache : caches) {
    if (cache.line_size == 0 || cache.sets == 0 ||
        cache.associativity == 0) {
      continue;
    }

    const AddressBasis basis =
      choose_address_basis(samples, cache, options);
    AccessGroups groups;
    size_t order = 0;
    for (const auto& sample : samples) {
      if (sample.addr <= 0 || sample.event_type == SampleType::PAGE_FAULT ||
          !cache.contains_cpu(sample.cpu)) {
        ++order;
        continue;
      }

      uint64_t address = static_cast<uint64_t>(sample.addr);
      if (basis == AddressBasis::Physical) {
        if (sample.phys_addr <= 0) {
          ++order;
          continue;
        }
        address = static_cast<uint64_t>(sample.phys_addr);
      }

      const uint64_t line = address / cache.line_size;
      const size_t set    = static_cast<size_t>(line % cache.sets);
      groups[set].push_back(
        Access{line, sample.time_stamp, sample.tid, sample.cpu, order++});
    }

    for (auto& [cache_set, accesses] : groups) {
      const bool all_timed = std::ranges::all_of(
        accesses, [](const Access& access) { return access.time_ns != 0; });
      if (all_timed) {
        std::ranges::stable_sort(
          accesses, [](const Access& a, const Access& b) {
            if (a.time_ns != b.time_ns) return a.time_ns < b.time_ns;
            return a.order < b.order;
          });
      }

      size_t segment_begin = 0;
      for (size_t i = 1; i <= accesses.size(); ++i) {
        bool split = i == accesses.size();
        if (!split && options.max_gap_ns > 0 &&
            accesses[i - 1].time_ns > 0 &&
            accesses[i].time_ns > accesses[i - 1].time_ns) {
          split = accesses[i].time_ns - accesses[i - 1].time_ns >
                  options.max_gap_ns;
        }
        if (!split) continue;

        analyze_segment(cache, basis, cache_set, accesses, segment_begin, i,
                        options, events);
        segment_begin = i;
      }
    }
  }

  std::ranges::sort(events, [](const auto& a, const auto& b) {
    if (a.score != b.score) return a.score > b.score;
    if (a.eviction_reloads != b.eviction_reloads)
      return a.eviction_reloads > b.eviction_reloads;
    if (a.cache_level != b.cache_level)
      return a.cache_level < b.cache_level;
    if (a.cache_id != b.cache_id) return a.cache_id < b.cache_id;
    return a.cache_set < b.cache_set;
  });
  return events;
}

void ThrashingAnalysis::print(const std::vector<ThrashingEvent>& events,
                              const std::vector<CacheInfo>& caches,
                              size_t max_events) {
  std::cout << "\n=== Cache Thrashing Analysis ===\n";
  std::cout << cachescope::format("Detected {} data/unified cache instances:\n",
                           caches.size());
  for (const auto& cache : caches) {
    std::cout << cachescope::format(
      "  L{} {} id={} CPUs={}  {} KiB, {} sets x {} ways x {} bytes{}\n",
      cache.level, cache.type, cache.id, cache.shared_cpu_list,
      cache.size_bytes / 1024, cache.sets, cache.associativity,
      cache.line_size,
      cache.detected_from_sysfs ? "" : " (fallback geometry)");
  }
  std::cout << "\n";

  if (events.empty()) {
    std::cout << "No cache-thrashing episodes detected.\n";
    return;
  }

  for (size_t i = 0; i < std::min(events.size(), max_events); ++i) {
    const auto& event = events[i];
    const auto duration = event.end_time_ns > event.start_time_ns
                            ? event.end_time_ns - event.start_time_ns
                            : 0;
    std::cout << cachescope::format(
      "Event #{}: L{} {} id={} / set {} / CPUs {}\n"
      "  Samples: {} across {} threads, {} CPUs, and {} unique lines\n"
      "  Evictions: {} (reloaded={}, reload ratio={:.3f})\n"
      "  Oversubscription: {:.2f}x  Score: {:.3f}  Address basis: {}\n"
      "  Time: {} - {} ns (duration={} ns)\n\n",
      i + 1, event.cache_level, event.cache_type, event.cache_id,
      event.cache_set, event.shared_cpu_list, event.sample_count,
      event.unique_threads, event.unique_cpus, event.unique_lines,
      event.evictions, event.eviction_reloads, event.reload_ratio,
      event.oversubscription, event.score,
      address_basis_name(event.address_basis), event.start_time_ns,
      event.end_time_ns, duration);
  }
}
