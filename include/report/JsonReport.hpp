#pragma once

#include <format>
#include <ostream>
#include <string>
#include <string_view>

#include "report/Report.hpp"

struct JsonReport {
  static void write(std::ostream& os, const Report& report);
};

inline std::string escape_json(std::string_view value) {
  std::string out;
  out.reserve(value.size());
  for (unsigned char c : value) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\b':
        out += "\\b";
        break;
      case '\f':
        out += "\\f";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (c < 0x20) {
          out += std::format("\\u{:04x}", c);
        } else {
          out += static_cast<char>(c);
        }
        break;
    }
  }
  return out;
}

inline void JsonReport::write(std::ostream& os, const Report& report) {
  os << "{\n";
  os << "  \"metadata\": {\n";
  os << std::format("    \"binary\": \"{}\",\n",
                    escape_json(report.metadata.binary));
  os << std::format("    \"event\": \"{}\",\n",
                    escape_json(report.metadata.event));
  os << std::format("    \"sample_period\": {}\n", report.metadata.sample_rate);
  os << "  },\n";

  os << "  \"cache_topology\": [\n";
  for (size_t i = 0; i < report.cache_topology.size(); ++i) {
    const auto& cache = report.cache_topology[i];
    os << "    {\n";
    os << std::format("      \"level\": {},\n", cache.level);
    os << std::format("      \"id\": {},\n", cache.id);
    os << std::format("      \"type\": \"{}\",\n",
                      escape_json(cache.type));
    os << std::format("      \"size_bytes\": {},\n", cache.size_bytes);
    os << std::format("      \"line_size\": {},\n", cache.line_size);
    os << std::format("      \"sets\": {},\n", cache.sets);
    os << std::format("      \"ways\": {},\n", cache.associativity);
    os << std::format("      \"shared_cpu_list\": \"{}\",\n",
                      escape_json(cache.shared_cpu_list));
    os << std::format("      \"detected\": {}\n",
                      cache.detected_from_sysfs ? "true" : "false");
    os << "    }";
    if (i + 1 < report.cache_topology.size()) os << ",";
    os << "\n";
  }
  os << "  ],\n";

  os << "  \"sample_stats\": {\n";
  os << std::format("    \"total_samples\": {},\n",
                    report.stats.total_samples);
  os << std::format("    \"samples_with_address\": {},\n",
                    report.stats.samples_with_addr);
  os << std::format("    \"samples_with_physical_address\": {},\n",
                    report.stats.samples_with_phys_addr);
  os << std::format("    \"samples_with_ip\": {},\n",
                    report.stats.samples_with_ip);
  os << std::format("    \"samples_with_sp\": {},\n",
                    report.stats.samples_with_sp);
  os << std::format("    \"samples_with_bp\": {},\n",
                    report.stats.samples_with_bp);
  os << std::format("    \"unique_threads\": {},\n",
                    report.stats.unique_threads);
  os << std::format("    \"unique_cpus\": {}\n", report.stats.unique_cpus);
  os << "  },\n";

  os << "  \"false_sharing\": [\n";
  for (size_t i = 0; i < report.false_sharing.size(); ++i) {
    const auto& entry = report.false_sharing[i];
    os << "    {\n";
    os << std::format("      \"index\": {},\n", entry.index);
    os << std::format("      \"base_addr\": \"{}\",\n",
                      format_hex_addr(entry.base_addr));
    os << std::format("      \"samples\": {},\n", entry.sample_count);
    os << std::format("      \"reads\": {},\n", entry.sample_reads);
    os << std::format("      \"writes\": {},\n", entry.sample_writes);
    os << std::format("      \"threads\": {},\n", entry.unique_threads);
    os << std::format("      \"distinct_offsets\": {},\n",
                      entry.distinct_offsets);
    os << std::format("      \"shared_offsets\": {},\n",
                      entry.shared_offsets);
    os << std::format("      \"private_offset_fraction\": {:.4f},\n",
                      entry.private_offset_fraction);
    os << std::format("      \"top_offsets\": {},\n",
                      entry.unique_top_offsets);
    os << std::format("      \"thread_switches\": {},\n",
                      entry.thread_switches);
    os << std::format("      \"bounce_score\": {:.4f},\n", entry.bounce_score);
    os << "      \"address_range\": {\n";
    os << std::format("        \"min\": \"{}\",\n",
                      format_hex_addr(entry.min_addr));
    os << std::format("        \"max\": \"{}\",\n",
                      format_hex_addr(entry.max_addr));
    os << std::format("        \"bytes\": {}\n", entry.range_bytes);
    os << "      }\n";
    os << "    }";
    if (i + 1 < report.false_sharing.size()) os << ",";
    os << "\n";
  }
  os << "  ],\n";

  os << "  \"cache_thrashing\": [\n";
  for (size_t i = 0; i < report.cache_thrashing.size(); ++i) {
    const auto& entry = report.cache_thrashing[i];
    os << "    {\n";
    os << std::format("      \"index\": {},\n", entry.index);
    os << std::format("      \"cache_level\": {},\n",
                      entry.cache_level);
    os << std::format("      \"cache_id\": {},\n", entry.cache_id);
    os << std::format("      \"cache_type\": \"{}\",\n",
                      escape_json(entry.cache_type));
    os << std::format("      \"shared_cpu_list\": \"{}\",\n",
                      escape_json(entry.shared_cpu_list));
    os << std::format("      \"address_basis\": \"{}\",\n",
                      escape_json(entry.address_basis));
    os << std::format("      \"cache_set\": {},\n", entry.cache_set);
    os << std::format("      \"start_time_ns\": {},\n",
                      entry.start_time_ns);
    os << std::format("      \"end_time_ns\": {},\n", entry.end_time_ns);
    os << std::format("      \"duration_ns\": {},\n", entry.duration_ns);
    os << std::format("      \"samples\": {},\n", entry.sample_count);
    os << std::format("      \"unique_lines\": {},\n",
                      entry.unique_lines);
    os << std::format("      \"evictions\": {},\n", entry.evictions);
    os << std::format("      \"eviction_reloads\": {},\n",
                      entry.eviction_reloads);
    os << std::format("      \"threads\": {},\n", entry.unique_threads);
    os << std::format("      \"cpus\": {},\n", entry.unique_cpus);
    os << std::format("      \"reload_ratio\": {:.4f},\n",
                      entry.reload_ratio);
    os << std::format("      \"oversubscription\": {:.4f},\n",
                      entry.oversubscription);
    os << std::format("      \"score\": {:.4f}\n", entry.score);
    os << "    }";
    if (i + 1 < report.cache_thrashing.size()) os << ",";
    os << "\n";
  }
  os << "  ]\n";
  os << "}\n";
}
