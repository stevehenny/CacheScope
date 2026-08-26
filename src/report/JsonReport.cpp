#include "report/JsonReport.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <fstream>
#include <nlohmann/json.hpp>
#include <system_error>

namespace {

using json = nlohmann::ordered_json;
using cachescope::AnalysisThresholds;
using cachescope::Error;
using cachescope::ErrorCategory;
using cachescope::FindingAttribution;
using cachescope::PmuCapabilities;
using cachescope::Result;
using cachescope::SampleQuality;
using cachescope::TraceMetadata;

std::uint64_t parse_address(const nlohmann::json& value) {
  if (value.is_number_unsigned()) return value.get<std::uint64_t>();
  if (value.is_number_integer()) {
    return static_cast<std::uint64_t>(value.get<std::int64_t>());
  }
  const auto text = value.get<std::string>();
  std::size_t consumed = 0;
  const auto parsed = std::stoull(text, &consumed, 0);
  if (consumed != text.size()) throw std::invalid_argument("invalid address");
  return parsed;
}

json attribution_json(const FindingAttribution& value) {
  return {
    {"variable", value.variable},
    {"type", value.type},
    {"field_path", value.field_path},
    {"scope", value.scope},
    {"tids", value.tids},
    {"sample_count", value.sample_count},
    {"evidence", value.evidence},
    {"confidence", value.confidence},
  };
}

FindingAttribution parse_attribution(const nlohmann::json& value) {
  FindingAttribution result;
  result.variable = value.value("variable", "");
  result.type = value.value("type", "");
  result.field_path = value.value("field_path", "");
  result.scope = value.value("scope", "");
  result.tids = value.value("tids", std::vector<std::uint32_t>{});
  result.sample_count = value.value("sample_count", std::uint64_t{});
  result.evidence = value.value("evidence", std::vector<std::string>{});
  result.confidence = value.value("confidence", 0.0);
  return result;
}

json capabilities_json(const PmuCapabilities& value) {
  return {
    {"cpu_vendor", value.cpu_vendor},
    {"perf_events", value.perf_events},
    {"intel_pebs", value.intel_pebs},
    {"amd_ibs", value.amd_ibs},
    {"physical_addresses", value.physical_addresses},
    {"user_registers", value.user_registers},
    {"unavailable", value.unavailable},
  };
}

PmuCapabilities parse_capabilities(const nlohmann::json& value) {
  PmuCapabilities result;
  result.cpu_vendor = value.value("cpu_vendor", "");
  result.perf_events = value.value("perf_events", false);
  result.intel_pebs = value.value("intel_pebs", false);
  result.amd_ibs = value.value("amd_ibs", false);
  result.physical_addresses = value.value("physical_addresses", false);
  result.user_registers = value.value("user_registers", false);
  result.unavailable =
    value.value("unavailable", std::vector<std::string>{});
  return result;
}

json result_json(const AnalysisResult& report) {
  json topology = json::array();
  for (const auto& cache : report.cache_topology) {
    topology.push_back({
      {"level", cache.level},
      {"id", cache.id},
      {"type", cache.type},
      {"size_bytes", cache.size_bytes},
      {"line_size", cache.line_size},
      {"sets", cache.sets},
      {"ways", cache.associativity},
      {"shared_cpu_list", cache.shared_cpu_list},
      {"detected", cache.detected_from_sysfs},
    });
  }

  json false_sharing = json::array();
  for (const auto& entry : report.false_sharing) {
    json attribution = json::array();
    for (const auto& item : entry.attribution) {
      attribution.push_back(attribution_json(item));
    }
    false_sharing.push_back({
      {"index", entry.index},
      {"base_addr", format_hex_addr(entry.base_addr)},
      {"samples", entry.sample_count},
      {"reads", entry.sample_reads},
      {"writes", entry.sample_writes},
      {"threads", entry.unique_threads},
      {"distinct_offsets", entry.distinct_offsets},
      {"shared_offsets", entry.shared_offsets},
      {"private_offset_fraction", entry.private_offset_fraction},
      {"top_offsets", entry.unique_top_offsets},
      {"thread_switches", entry.thread_switches},
      {"bounce_score", entry.bounce_score},
      {"address_range",
       {{"min", format_hex_addr(entry.min_addr)},
        {"max", format_hex_addr(entry.max_addr)},
        {"bytes", entry.range_bytes}}},
      {"suspected_cause", entry.suspected_cause},
      {"confidence", entry.confidence},
      {"attribution", std::move(attribution)},
    });
  }

  json thrashing = json::array();
  for (const auto& entry : report.cache_thrashing) {
    json attribution = json::array();
    for (const auto& item : entry.attribution) {
      attribution.push_back(attribution_json(item));
    }
    thrashing.push_back({
      {"index", entry.index},
      {"cache_level", entry.cache_level},
      {"cache_id", entry.cache_id},
      {"cache_type", entry.cache_type},
      {"shared_cpu_list", entry.shared_cpu_list},
      {"address_basis", entry.address_basis},
      {"cache_set", entry.cache_set},
      {"start_time_ns", entry.start_time_ns},
      {"end_time_ns", entry.end_time_ns},
      {"duration_ns", entry.duration_ns},
      {"samples", entry.sample_count},
      {"unique_lines", entry.unique_lines},
      {"evictions", entry.evictions},
      {"eviction_reloads", entry.eviction_reloads},
      {"threads", entry.unique_threads},
      {"cpus", entry.unique_cpus},
      {"reload_ratio", entry.reload_ratio},
      {"oversubscription", entry.oversubscription},
      {"score", entry.score},
      {"suspected_cause", entry.suspected_cause},
      {"confidence", entry.confidence},
      {"attribution", std::move(attribution)},
    });
  }

  json diagnostics = json::array();
  for (const auto& diagnostic : report.diagnostics) {
    diagnostics.push_back({
      {"severity", diagnostic.severity},
      {"code", diagnostic.code},
      {"message", diagnostic.message},
      {"remediation", diagnostic.remediation},
    });
  }

  return {
    {"schema_version", report.schema_version},
    {"metadata",
     {{"binary", report.metadata.binary},
      {"event", report.metadata.event},
      {"sample_period", report.metadata.sample_rate}}},
    {"capture",
     {{"tool_version", report.capture.tool_version},
      {"trace_schema_version", report.capture.schema_version},
      {"command", report.capture.command},
      {"target_path", report.capture.target_path},
      {"target_build_id", report.capture.target_build_id},
      {"kernel_release", report.capture.kernel_release},
      {"cpu_model", report.capture.cpu_model},
      {"event_encodings", report.capture.event_encodings},
      {"clock_source", report.capture.clock_source},
      {"start_time_unix_ns", report.capture.start_time_unix_ns}}},
    {"capabilities", capabilities_json(report.capture.capabilities)},
    {"sample_quality",
     {{"samples", report.quality.samples},
      {"lost", report.quality.lost},
      {"throttled", report.quality.throttled},
      {"malformed_records", report.quality.malformed_records},
      {"unknown_records", report.quality.unknown_records},
      {"evicted_samples", report.quality.evicted_samples},
      {"truncated", report.quality.truncated},
      {"completed", report.quality.completed}}},
    {"analysis_thresholds",
     {{"false_sharing_min_samples",
       report.thresholds.false_sharing_min_samples},
      {"false_sharing_min_bounce",
       report.thresholds.false_sharing_min_bounce},
      {"false_sharing_min_private_fraction",
       report.thresholds.false_sharing_min_private_fraction},
      {"thrashing_min_samples", report.thresholds.thrashing_min_samples},
      {"thrashing_min_reloads", report.thresholds.thrashing_min_reloads},
      {"thrashing_min_reload_ratio",
       report.thresholds.thrashing_min_reload_ratio},
      {"thrashing_max_gap_ns", report.thresholds.thrashing_max_gap_ns}}},
    {"cache_topology", std::move(topology)},
    {"sample_stats",
     {{"total_samples", report.stats.total_samples},
      {"samples_with_address", report.stats.samples_with_addr},
      {"samples_with_physical_address",
       report.stats.samples_with_phys_addr},
      {"samples_with_ip", report.stats.samples_with_ip},
      {"samples_with_sp", report.stats.samples_with_sp},
      {"samples_with_bp", report.stats.samples_with_bp},
      {"unique_threads", report.stats.unique_threads},
      {"unique_cpus", report.stats.unique_cpus}}},
    {"false_sharing", std::move(false_sharing)},
    {"cache_thrashing", std::move(thrashing)},
    {"diagnostics", std::move(diagnostics)},
  };
}

AnalysisResult parse_result(const nlohmann::json& root) {
  AnalysisResult report;
  report.schema_version = root.value("schema_version", "legacy");
  if (report.schema_version != "legacy" && report.schema_version != "1.0") {
    throw std::invalid_argument("unsupported report schema version " +
                                report.schema_version);
  }

  if (const auto it = root.find("metadata"); it != root.end()) {
    report.metadata.binary = it->value("binary", "");
    report.metadata.event = it->value("event", "");
    report.metadata.sample_rate = it->value("sample_period", 0);
  }
  if (const auto it = root.find("capture"); it != root.end()) {
    report.capture.tool_version = it->value("tool_version", "");
    report.capture.schema_version =
      it->value("trace_schema_version", "1.0");
    report.capture.command =
      it->value("command", std::vector<std::string>{});
    report.capture.target_path = it->value("target_path", "");
    report.capture.target_build_id = it->value("target_build_id", "");
    report.capture.kernel_release = it->value("kernel_release", "");
    report.capture.cpu_model = it->value("cpu_model", "");
    report.capture.event_encodings =
      it->value("event_encodings", std::vector<std::string>{});
    report.capture.clock_source =
      it->value("clock_source", "CLOCK_MONOTONIC");
    report.capture.start_time_unix_ns =
      it->value("start_time_unix_ns", std::uint64_t{});
  }
  if (const auto it = root.find("capabilities"); it != root.end()) {
    report.capture.capabilities = parse_capabilities(*it);
  }
  if (const auto it = root.find("sample_quality"); it != root.end()) {
    report.quality.samples = it->value("samples", std::uint64_t{});
    report.quality.lost = it->value("lost", std::uint64_t{});
    report.quality.throttled = it->value("throttled", std::uint64_t{});
    report.quality.malformed_records =
      it->value("malformed_records", std::uint64_t{});
    report.quality.unknown_records =
      it->value("unknown_records", std::uint64_t{});
    report.quality.evicted_samples =
      it->value("evicted_samples", std::uint64_t{});
    report.quality.truncated = it->value("truncated", false);
    report.quality.completed = it->value("completed", false);
  }
  if (const auto it = root.find("analysis_thresholds"); it != root.end()) {
    report.thresholds.false_sharing_min_samples =
      it->value("false_sharing_min_samples", std::size_t{1000});
    report.thresholds.false_sharing_min_bounce =
      it->value("false_sharing_min_bounce", 0.10);
    report.thresholds.false_sharing_min_private_fraction =
      it->value("false_sharing_min_private_fraction", 0.50);
    report.thresholds.thrashing_min_samples =
      it->value("thrashing_min_samples", std::size_t{64});
    report.thresholds.thrashing_min_reloads =
      it->value("thrashing_min_reloads", std::size_t{8});
    report.thresholds.thrashing_min_reload_ratio =
      it->value("thrashing_min_reload_ratio", 0.20);
    report.thresholds.thrashing_max_gap_ns =
      it->value("thrashing_max_gap_ns", std::uint64_t{10'000'000});
  }

  if (const auto it = root.find("sample_stats"); it != root.end()) {
    report.stats.total_samples = it->value("total_samples", std::size_t{});
    report.stats.samples_with_addr =
      it->value("samples_with_address", std::size_t{});
    report.stats.samples_with_phys_addr =
      it->value("samples_with_physical_address", std::size_t{});
    report.stats.samples_with_ip =
      it->value("samples_with_ip", std::size_t{});
    report.stats.samples_with_sp =
      it->value("samples_with_sp", std::size_t{});
    report.stats.samples_with_bp =
      it->value("samples_with_bp", std::size_t{});
    report.stats.unique_threads =
      it->value("unique_threads", std::size_t{});
    report.stats.unique_cpus =
      it->value("unique_cpus", std::size_t{});
  }

  for (const auto& value : root.value("cache_topology", nlohmann::json::array())) {
    CacheInfo cache;
    cache.level = value.value("level", 0);
    cache.id = value.value("id", 0);
    cache.type = value.value("type", "");
    cache.size_bytes = value.value("size_bytes", std::size_t{});
    cache.line_size = value.value("line_size", std::size_t{});
    cache.sets = value.value("sets", std::size_t{});
    cache.associativity = value.value("ways", std::size_t{});
    cache.shared_cpu_list = value.value("shared_cpu_list", "");
    cache.detected_from_sysfs = value.value("detected", false);
    report.cache_topology.push_back(std::move(cache));
  }

  for (const auto& value : root.value("false_sharing", nlohmann::json::array())) {
    FalseSharingEntry entry;
    entry.index = value.value("index", std::size_t{});
    if (const auto address = value.find("base_addr"); address != value.end()) {
      entry.base_addr = static_cast<std::int64_t>(parse_address(*address));
    }
    entry.sample_count = value.value("samples", std::size_t{});
    entry.sample_reads = value.value("reads", std::size_t{});
    entry.sample_writes = value.value("writes", std::size_t{});
    entry.unique_threads = value.value("threads", std::size_t{});
    entry.distinct_offsets =
      value.value("distinct_offsets", std::size_t{});
    entry.shared_offsets = value.value("shared_offsets", std::size_t{});
    entry.private_offset_fraction =
      value.value("private_offset_fraction", 0.0);
    entry.unique_top_offsets = value.value("top_offsets", std::size_t{});
    entry.thread_switches =
      value.value("thread_switches", std::size_t{});
    entry.bounce_score = value.value("bounce_score", 0.0);
    if (const auto range = value.find("address_range"); range != value.end()) {
      if (const auto minimum = range->find("min"); minimum != range->end()) {
        entry.min_addr = static_cast<std::int64_t>(parse_address(*minimum));
      }
      if (const auto maximum = range->find("max"); maximum != range->end()) {
        entry.max_addr = static_cast<std::int64_t>(parse_address(*maximum));
      }
      entry.range_bytes = range->value("bytes", std::int64_t{});
    }
    entry.suspected_cause =
      value.value("suspected_cause", "false sharing");
    entry.confidence = value.value(
      "confidence", entry.bounce_score * entry.private_offset_fraction);
    for (const auto& item :
         value.value("attribution", nlohmann::json::array())) {
      entry.attribution.push_back(parse_attribution(item));
    }
    report.false_sharing.push_back(std::move(entry));
  }

  for (const auto& value :
       root.value("cache_thrashing", nlohmann::json::array())) {
    ThrashingEntry entry;
    entry.index = value.value("index", std::size_t{});
    entry.cache_level = value.value("cache_level", 0);
    entry.cache_id = value.value("cache_id", 0);
    entry.cache_type = value.value("cache_type", "");
    entry.shared_cpu_list = value.value("shared_cpu_list", "");
    entry.address_basis = value.value("address_basis", "");
    entry.cache_set = value.value("cache_set", std::size_t{});
    entry.start_time_ns = value.value("start_time_ns", std::int64_t{});
    entry.end_time_ns = value.value("end_time_ns", std::int64_t{});
    entry.duration_ns = value.value("duration_ns", std::int64_t{});
    entry.sample_count = value.value("samples", std::size_t{});
    entry.unique_lines = value.value("unique_lines", std::size_t{});
    entry.evictions = value.value("evictions", std::size_t{});
    entry.eviction_reloads =
      value.value("eviction_reloads", std::size_t{});
    entry.unique_threads = value.value("threads", std::size_t{});
    entry.unique_cpus = value.value("cpus", std::size_t{});
    entry.reload_ratio = value.value("reload_ratio", 0.0);
    entry.oversubscription = value.value("oversubscription", 0.0);
    entry.score = value.value("score", 0.0);
    entry.suspected_cause =
      value.value("suspected_cause", "cache-set thrashing");
    entry.confidence = value.value("confidence", entry.score);
    for (const auto& item :
         value.value("attribution", nlohmann::json::array())) {
      entry.attribution.push_back(parse_attribution(item));
    }
    report.cache_thrashing.push_back(std::move(entry));
  }

  for (const auto& value : root.value("diagnostics", nlohmann::json::array())) {
    report.diagnostics.push_back(
      {value.value("severity", ""), value.value("code", ""),
       value.value("message", ""), value.value("remediation", "")});
  }
  if (report.schema_version == "legacy") report.schema_version = "1.0";
  return report;
}

Error report_error(ErrorCategory category, std::string code,
                   std::string operation, std::string message,
                   std::error_code system_error = {}) {
  return Error{category, std::move(code), std::move(operation),
               std::move(message), system_error, {}};
}

}  // namespace

void JsonReport::write(std::ostream& output, const AnalysisResult& result) {
  output << result_json(result).dump(2) << '\n';
}

Result<AnalysisResult, Error> JsonReport::read(std::istream& input) {
  try {
    nlohmann::json root;
    input >> root;
    if (!input && !input.eof()) {
      return Result<AnalysisResult, Error>::failure(report_error(
        ErrorCategory::Io, "report.read_failed", "read report",
        "Failed while reading JSON report"));
    }
    return Result<AnalysisResult, Error>::success(parse_result(root));
  } catch (const std::exception& exception) {
    return Result<AnalysisResult, Error>::failure(report_error(
      ErrorCategory::Schema, "report.invalid_schema", "parse report",
      exception.what()));
  }
}

Result<AnalysisResult, Error> JsonReport::read_file(
  const std::filesystem::path& path) {
  std::ifstream input(path);
  if (!input) {
    return Result<AnalysisResult, Error>::failure(report_error(
      ErrorCategory::Io, "report.open_failed", "open report", path.string(),
      std::error_code(errno, std::generic_category())));
  }
  return read(input);
}

Result<void, Error> JsonReport::write_atomic(
  const std::filesystem::path& path, const AnalysisResult& result) {
  const auto temporary = path.string() + ".tmp";
  {
    const int fd = ::open(temporary.c_str(),
                          O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) {
      return Result<void, Error>::failure(report_error(
        ErrorCategory::Io, "report.open_failed", "open temporary report",
        temporary, std::error_code(errno, std::generic_category())));
    }
    if (close(fd) != 0) {
      return Result<void, Error>::failure(report_error(
        ErrorCategory::Io, "report.close_failed", "close temporary report",
        temporary, std::error_code(errno, std::generic_category())));
    }
    std::ofstream output(temporary, std::ios::trunc);
    if (!output) {
      return Result<void, Error>::failure(report_error(
        ErrorCategory::Io, "report.open_failed", "write temporary report",
        temporary));
    }
    write(output, result);
    output.flush();
    if (!output) {
      return Result<void, Error>::failure(report_error(
        ErrorCategory::Io, "report.write_failed", "write temporary report",
        temporary));
    }
  }
  std::error_code error;
  std::filesystem::rename(temporary, path, error);
  if (error) {
    std::filesystem::remove(temporary);
    return Result<void, Error>::failure(report_error(
      ErrorCategory::Io, "report.rename_failed", "commit report",
      path.string(), error));
  }
  return Result<void, Error>::success();
}
