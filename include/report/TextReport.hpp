#pragma once

#include <algorithm>
#include <format>
#include <ostream>
#include <string>
#include <vector>

#include "report/Report.hpp"

struct TextReport {
  static void write_markdown(std::ostream& os, const Report& report);
};

inline void TextReport::write_markdown(std::ostream& os,
                                       const Report& report) {
  os << "# CacheScope Report\n\n";

  os << "## Metadata\n";
  os << std::format("- Binary: {}\n", report.metadata.binary);
  os << std::format("- Event: {}\n", report.metadata.event);
  os << std::format("- Sample period: {}\n", report.metadata.sample_rate);

  os << "\n## Sample Statistics\n";
  os << std::format("- Total samples: {}\n", report.stats.total_samples);
  os << std::format("- Samples with address: {}\n",
                    report.stats.samples_with_addr);
  os << std::format("- Samples with IP: {}\n", report.stats.samples_with_ip);
  os << std::format("- Samples with SP: {}\n", report.stats.samples_with_sp);
  os << std::format("- Samples with BP: {}\n", report.stats.samples_with_bp);
  os << std::format("- Unique threads: {}\n", report.stats.unique_threads);
  os << std::format("- Unique CPUs: {}\n", report.stats.unique_cpus);

  os << "\n## False Sharing Analysis\n";
  if (report.false_sharing.empty()) {
    os << "No hot cache lines detected.\n";
    return;
  }

  auto write_table = [&](const std::vector<std::string>& headers,
                         const std::vector<std::vector<std::string>>& rows) {
    if (headers.empty()) return;

    std::vector<size_t> widths(headers.size(), 0);
    for (size_t i = 0; i < headers.size(); ++i) {
      widths[i] = headers[i].size();
    }
    for (const auto& row : rows) {
      for (size_t i = 0; i < headers.size() && i < row.size(); ++i) {
        widths[i] = std::max(widths[i], row[i].size());
      }
    }

    auto write_row = [&](const std::vector<std::string>& cells) {
      os << "|";
      for (size_t i = 0; i < headers.size(); ++i) {
        const std::string cell = (i < cells.size()) ? cells[i] : "";
        os << " " << cell << std::string(widths[i] - cell.size(), ' ') << " |";
      }
      os << "\n";
    };

    write_row(headers);
    os << "|";
    for (size_t i = 0; i < headers.size(); ++i) {
      os << std::string(widths[i] + 2, '-') << "|";
    }
    os << "\n";
    for (const auto& row : rows) write_row(row);
  };

  std::vector<std::vector<std::string>> summary_rows;
  std::vector<std::vector<std::string>> offset_rows;
  std::vector<std::vector<std::string>> bounce_rows;
  std::vector<std::vector<std::string>> range_rows;
  for (const auto& entry : report.false_sharing) {
    summary_rows.push_back(
      {std::to_string(entry.index), format_hex_addr(entry.base_addr),
       std::to_string(entry.sample_count), std::to_string(entry.sample_reads),
       std::to_string(entry.sample_writes),
       std::to_string(entry.unique_threads)});

    offset_rows.push_back(
      {std::to_string(entry.index), std::to_string(entry.distinct_offsets),
       std::to_string(entry.shared_offsets),
       std::format("{:.2f}", entry.private_offset_fraction),
       std::to_string(entry.unique_top_offsets)});

    bounce_rows.push_back(
      {std::to_string(entry.index), std::to_string(entry.thread_switches),
       std::format("{:.3f}", entry.bounce_score)});

    range_rows.push_back(
      {std::to_string(entry.index), format_hex_addr(entry.min_addr),
       format_hex_addr(entry.max_addr), std::to_string(entry.range_bytes)});
  }

  os << "\n### Summary\n";
  write_table({"#", "Base Address", "Samples", "Reads", "Writes", "Threads"},
              summary_rows);

  os << "\n### Offsets\n";
  write_table(
    {"#", "Distinct Offsets", "Shared Offsets", "Private Fraction",
     "Top Offsets"},
    offset_rows);

  os << "\n### Bounce\n";
  write_table({"#", "Thread Switches", "Bounce Score"}, bounce_rows);

  os << "\n### Address Range\n";
  write_table({"#", "Min Address", "Max Address", "Range Bytes"}, range_rows);
}
