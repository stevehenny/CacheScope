#include "pipeline/AnalysisPipeline.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <limits>
#include <set>
#include <string>

#include "report/JsonReport.hpp"
#include "report/TextReport.hpp"
#include "runtime/CacheTopology.hpp"
#include "runtime/FalseSharingAnalysis.hpp"
#include "runtime/Thrashing.hpp"
#include "trace/Trace.hpp"

namespace cachescope {
namespace {

Error interrupted_error() {
  return Error{ErrorCategory::Interrupted, "pipeline.interrupted",
               "analyze trace", "Analysis was interrupted", {},
               "Run the command again to restart analysis."};
}

Result<void, Error> write_markdown_atomic(
  const std::filesystem::path& path, const AnalysisResult& result) {
  const auto temporary = path.string() + ".tmp";
  const int fd = ::open(temporary.c_str(),
                        O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (fd < 0) {
    return Result<void, Error>::failure(Error::from_errno(
      ErrorCategory::Io, "report.open_failed", "open temporary Markdown",
      temporary));
  }
  if (close(fd) != 0) {
    return Result<void, Error>::failure(Error::from_errno(
      ErrorCategory::Io, "report.close_failed", "close temporary Markdown",
      temporary));
  }

  {
    std::ofstream output(temporary, std::ios::trunc);
    if (!output) {
      return Result<void, Error>::failure(Error{
        ErrorCategory::Io, "report.open_failed", "write Markdown", temporary,
        {}, {}});
    }
    TextReport::write_markdown(output, result);
    output.flush();
    if (!output) {
      return Result<void, Error>::failure(Error{
        ErrorCategory::Io, "report.write_failed", "write Markdown", temporary,
        {}, {}});
    }
  }

  std::error_code error;
  std::filesystem::rename(temporary, path, error);
  if (error) {
    std::filesystem::remove(temporary);
    return Result<void, Error>::failure(Error{
      ErrorCategory::Io, "report.rename_failed", "commit Markdown",
      path.string(), error, {}});
  }
  return Result<void, Error>::success();
}

SampleType sample_kind(trace::SampleKind kind) {
  switch (kind) {
    case trace::SampleKind::CacheLoad: return SampleType::CACHE_LOAD;
    case trace::SampleKind::CacheStore: return SampleType::CACHE_STORE;
    case trace::SampleKind::PageFault: return SampleType::PAGE_FAULT;
  }
  return SampleType::CACHE_LOAD;
}

std::string join_events(const std::vector<std::string>& events) {
  std::string joined;
  for (const auto& event : events) {
    if (!joined.empty()) joined += ",";
    joined += event;
  }
  return joined;
}

}  // namespace

Result<AnalysisResult, Error> AnalysisPipeline::run(
  const AnalysisRequest& request, ProgressSink& progress,
  std::stop_token stop) {
  if (stop.stop_requested()) {
    return Result<AnalysisResult, Error>::failure(interrupted_error());
  }

  progress.update("read-trace", 0.05);
  auto trace_result = trace::read(request.trace_source);
  if (!trace_result) {
    return Result<AnalysisResult, Error>::failure(trace_result.error());
  }
  auto trace_data = std::move(trace_result.value());

  std::vector<PerfSample> samples;
  samples.reserve(trace_data.samples.size());
  for (const auto& source : trace_data.samples) {
    if (source.ip > static_cast<std::uint64_t>(
                      std::numeric_limits<std::int64_t>::max()) ||
        source.virtual_address > static_cast<std::uint64_t>(
                                   std::numeric_limits<std::int64_t>::max()) ||
        source.physical_address > static_cast<std::uint64_t>(
                                    std::numeric_limits<std::int64_t>::max())) {
      ++trace_data.quality.malformed_records;
      continue;
    }
    PerfSample sample{};
    sample.pid = source.pid;
    sample.tid = source.tid;
    sample.cpu = source.cpu;
    sample.time_stamp = static_cast<std::int64_t>(source.timestamp);
    sample.ip = static_cast<std::int64_t>(source.ip);
    sample.addr = static_cast<std::int64_t>(source.virtual_address);
    sample.phys_addr =
      static_cast<std::int64_t>(source.physical_address);
    sample.sp = static_cast<std::int64_t>(source.stack_pointer);
    sample.bp = static_cast<std::int64_t>(source.base_pointer);
    sample.event_type = sample_kind(source.kind);
    samples.push_back(std::move(sample));
  }

  if (stop.stop_requested()) {
    return Result<AnalysisResult, Error>::failure(interrupted_error());
  }

  progress.update("detect", 0.35);
  const auto topology = CacheTopology::discover();
  std::set<size_t> line_sizes;
  for (const auto& cache : topology) {
    if (cache.line_size != 0) line_sizes.insert(cache.line_size);
  }
  if (line_sizes.size() > 1) {
    return Result<AnalysisResult, Error>::failure(Error{
      ErrorCategory::Unsupported, "cache.mixed_line_geometry",
      "select cache-line geometry",
      "Mixed cache-line sizes are not supported by false-sharing analysis",
      {}, "Analyze on a system with uniform data-cache line geometry."});
  }

  FalseSharingOptions false_sharing_options;
  if (!line_sizes.empty()) {
    false_sharing_options.cache_line_size = *line_sizes.begin();
  }
  false_sharing_options.min_samples =
    request.thresholds.false_sharing_min_samples;
  false_sharing_options.min_bounce_score =
    request.thresholds.false_sharing_min_bounce;
  false_sharing_options.min_private_offset_fraction =
    request.thresholds.false_sharing_min_private_fraction;

  ThrashingOptions thrashing_options;
  thrashing_options.min_samples = request.thresholds.thrashing_min_samples;
  thrashing_options.min_reloads = request.thresholds.thrashing_min_reloads;
  thrashing_options.min_reload_ratio =
    request.thresholds.thrashing_min_reload_ratio;
  if (request.thresholds.thrashing_max_gap_ns >
      static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    return Result<AnalysisResult, Error>::failure(Error{
      ErrorCategory::Usage, "threshold.out_of_range", "validate thresholds",
      "Thrashing maximum gap is out of range", {}, {}});
  }
  thrashing_options.max_gap_ns = static_cast<std::int64_t>(
    request.thresholds.thrashing_max_gap_ns);

  const auto hot_lines =
    FalseSharingAnalysis::find_hot_cache_lines(samples, false_sharing_options);
  const auto thrashing =
    ThrashingAnalysis::detect(samples, topology, thrashing_options);
  const auto stats = SampleStats::compute(samples);

  if (stop.stop_requested()) {
    return Result<AnalysisResult, Error>::failure(interrupted_error());
  }

  progress.update("attribute", 0.70);
  auto result = AnalysisResult::from_analysis(
    trace_data.metadata.target_path,
    join_events(trace_data.metadata.event_encodings), 0, stats, hot_lines,
    thrashing, topology, trace_data.metadata, trace_data.quality,
    request.thresholds);
  if (result.false_sharing.size() > request.max_findings) {
    result.false_sharing.resize(request.max_findings);
    result.diagnostics.push_back(
      {"warning", "analysis.finding_limit",
       "False-sharing findings were limited by configuration.",
       "Increase --max-findings to retain more findings."});
  }
  if (result.cache_thrashing.size() > request.max_findings) {
    result.cache_thrashing.resize(request.max_findings);
    result.diagnostics.push_back(
      {"warning", "analysis.finding_limit",
       "Cache-thrashing findings were limited by configuration.",
       "Increase --max-findings to retain more findings."});
  }

  progress.update("write-reports", 0.85);
  if (!request.json_output.empty()) {
    auto written = JsonReport::write_atomic(request.json_output, result);
    if (!written) {
      return Result<AnalysisResult, Error>::failure(written.error());
    }
  }
  if (!request.markdown_output.empty()) {
    auto written = write_markdown_atomic(request.markdown_output, result);
    if (!written) {
      return Result<AnalysisResult, Error>::failure(written.error());
    }
  }
  progress.update("complete", 1.0);
  return Result<AnalysisResult, Error>::success(std::move(result));
}

}  // namespace cachescope
