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

  os << "  \"sample_stats\": {\n";
  os << std::format("    \"total_samples\": {},\n",
                    report.stats.total_samples);
  os << std::format("    \"samples_with_address\": {},\n",
                    report.stats.samples_with_addr);
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
  os << "  ]\n";
  os << "}\n";
}
