#pragma once

#include "common/Format.hpp"
#include <ostream>

#include "report/Report.hpp"

struct TextReport {
  static void write_markdown(std::ostream& output,
                             const AnalysisResult& report) {
    output << "# CacheScope Report\n\n";
    output << "Schema: " << report.schema_version << "\n\n";
    output << "## Metadata\n\n";
    output << cachescope::format("- Binary: {}\n- Event: {}\n- Sample period: {}\n",
                          report.metadata.binary, report.metadata.event,
                          report.metadata.sample_rate);
    output << cachescope::format("- Tool: {}\n- Kernel: {}\n- CPU: {}\n",
                          report.capture.tool_version,
                          report.capture.kernel_release,
                          report.capture.cpu_model);

    output << "\n## Capabilities and Sample Quality\n\n";
    output << cachescope::format(
      "- Perf events: {}\n- Intel PEBS: {}\n- AMD IBS: {}\n"
      "- Physical addresses: {}\n- User registers: {}\n",
      report.capture.capabilities.perf_events ? "available" : "unavailable",
      report.capture.capabilities.intel_pebs ? "available" : "unavailable",
      report.capture.capabilities.amd_ibs ? "available" : "unavailable",
      report.capture.capabilities.physical_addresses
        ? "available" : "unavailable",
      report.capture.capabilities.user_registers
        ? "available" : "unavailable");
    output << cachescope::format(
      "- Samples: {}\n- Lost: {}\n- Throttled: {}\n"
      "- Malformed records: {}\n- Evicted by cap: {}\n"
      "- Capture completed: {}\n",
      report.quality.samples, report.quality.lost,
      report.quality.throttled, report.quality.malformed_records,
      report.quality.evicted_samples,
      report.quality.completed ? "yes" : "no");

    output << "\n## Analysis Thresholds\n\n";
    output << cachescope::format(
      "- False-sharing minimum samples: {}\n"
      "- False-sharing minimum bounce: {:.3f}\n"
      "- False-sharing minimum private fraction: {:.3f}\n"
      "- Thrashing minimum samples: {}\n"
      "- Thrashing minimum reloads: {}\n"
      "- Thrashing minimum reload ratio: {:.3f}\n"
      "- Thrashing maximum gap (ns): {}\n",
      report.thresholds.false_sharing_min_samples,
      report.thresholds.false_sharing_min_bounce,
      report.thresholds.false_sharing_min_private_fraction,
      report.thresholds.thrashing_min_samples,
      report.thresholds.thrashing_min_reloads,
      report.thresholds.thrashing_min_reload_ratio,
      report.thresholds.thrashing_max_gap_ns);

    output << "\n## Sample Statistics\n\n";
    output << cachescope::format(
      "- Total samples: {}\n- Samples with address: {}\n"
      "- Samples with physical address: {}\n- Samples with IP: {}\n"
      "- Samples with SP: {}\n- Samples with BP: {}\n"
      "- Unique threads: {}\n- Unique CPUs: {}\n",
      report.stats.total_samples, report.stats.samples_with_addr,
      report.stats.samples_with_phys_addr, report.stats.samples_with_ip,
      report.stats.samples_with_sp, report.stats.samples_with_bp,
      report.stats.unique_threads, report.stats.unique_cpus);

    output << "\n## False Sharing Analysis\n\n";
    if (report.false_sharing.empty()) {
      output << "No suspected false-sharing findings.\n";
    } else {
      output << "| # | Cache line | Samples | Threads | Bounce | Private | "
                "Confidence | Suspected cause |\n";
      output << "|---:|---:|---:|---:|---:|---:|---:|---|\n";
      for (const auto& finding : report.false_sharing) {
        output << cachescope::format(
          "| {} | {} | {} | {} | {:.3f} | {:.3f} | {:.3f} | {} |\n",
          finding.index, format_hex_addr(finding.base_addr),
          finding.sample_count, finding.unique_threads,
          finding.bounce_score, finding.private_offset_fraction,
          finding.confidence, finding.suspected_cause);
        for (const auto& attribution : finding.attribution) {
          output << cachescope::format(
            "\n- Attribution: {} {} {} ({} samples, confidence {:.3f})\n",
            attribution.scope, attribution.variable, attribution.field_path,
            attribution.sample_count, attribution.confidence);
          for (const auto& evidence : attribution.evidence) {
            output << "  - Evidence: " << evidence << '\n';
          }
        }
      }
    }

    output << "\n## Cache Thrashing Analysis\n\n";
    output << "### Detected Cache Topology\n\n";
    output << "| Level | Type | ID | Size KiB | Line | Sets | Ways | "
              "Shared CPUs | Source |\n";
    output << "|---|---|---:|---:|---:|---:|---:|---|---|\n";
    for (const auto& cache : report.cache_topology) {
      output << cachescope::format(
        "| L{} | {} | {} | {} | {} | {} | {} | {} | {} |\n",
        cache.level, cache.type, cache.id, cache.size_bytes / 1024,
        cache.line_size, cache.sets, cache.associativity,
        cache.shared_cpu_list,
        cache.detected_from_sysfs ? "sysfs" : "fallback");
    }

    output << "\n### Detected Episodes\n\n";
    if (report.cache_thrashing.empty()) {
      output << "No suspected cache-thrashing findings.\n";
    } else {
      output << "| # | Cache | Set | Basis | Samples | Lines | Score | "
                "Confidence | Suspected cause |\n";
      output << "|---:|---|---:|---|---:|---:|---:|---:|---|\n";
      for (const auto& finding : report.cache_thrashing) {
        output << cachescope::format(
          "| {} | L{} {} {} | {} | {} | {} | {} | {:.3f} | {:.3f} | {} |\n",
          finding.index, finding.cache_level, finding.cache_type,
          finding.cache_id, finding.cache_set, finding.address_basis,
          finding.sample_count, finding.unique_lines, finding.score,
          finding.confidence, finding.suspected_cause);
      }
    }

    output << "\n## Warnings and Limitations\n\n";
    if (report.diagnostics.empty()) {
      output << "None reported.\n";
    } else {
      for (const auto& diagnostic : report.diagnostics) {
        output << cachescope::format("- **{}** {}: {}", diagnostic.severity,
                              diagnostic.code, diagnostic.message);
        if (!diagnostic.remediation.empty()) {
          output << " " << diagnostic.remediation;
        }
        output << '\n';
      }
    }
  }
};
