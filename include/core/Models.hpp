#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace cachescope {

enum class TargetKind { Launch, Attach };

struct RecordingRequest {
  TargetKind target_kind{TargetKind::Launch};
  std::filesystem::path executable;
  std::vector<std::string> arguments;
  std::map<std::string, std::string> environment;
  std::optional<std::filesystem::path> working_directory;
  std::optional<int> pid;
  std::vector<std::string> events;
  std::uint64_t sample_period{10000};
  std::size_t max_samples{1'000'000};
  std::chrono::seconds rolling_window{60};
  std::filesystem::path output;
  bool allow_root_target{false};
};

struct PmuCapabilities {
  std::string cpu_vendor;
  bool perf_events{false};
  bool intel_pebs{false};
  bool amd_ibs{false};
  bool physical_addresses{false};
  bool user_registers{false};
  std::vector<std::string> unavailable;
};

struct TraceMetadata {
  std::string tool_version;
  std::string schema_version{"1.0"};
  std::vector<std::string> command;
  std::string target_path;
  std::string target_build_id;
  std::string kernel_release;
  std::string cpu_model;
  PmuCapabilities capabilities;
  std::vector<std::string> event_encodings;
  std::string clock_source{"CLOCK_MONOTONIC"};
  std::uint64_t start_time_unix_ns{};
};

struct AnalysisThresholds {
  std::size_t false_sharing_min_samples{1000};
  double false_sharing_min_bounce{0.10};
  double false_sharing_min_private_fraction{0.50};
  std::size_t thrashing_min_samples{64};
  std::size_t thrashing_min_reloads{8};
  double thrashing_min_reload_ratio{0.20};
  std::uint64_t thrashing_max_gap_ns{10'000'000};
};

struct AnalysisRequest {
  std::filesystem::path trace_source;
  AnalysisThresholds thresholds;
  std::filesystem::path markdown_output{"cache_scope.md"};
  std::filesystem::path json_output{"cache_scope.json"};
  std::size_t max_findings{100};
};

struct SampleQuality {
  std::uint64_t samples{};
  std::uint64_t lost{};
  std::uint64_t throttled{};
  std::uint64_t malformed_records{};
  std::uint64_t unknown_records{};
  std::uint64_t evicted_samples{};
  bool truncated{false};
  bool completed{false};
};

struct FindingAttribution {
  std::string variable;
  std::string type;
  std::string field_path;
  std::string scope;
  std::vector<std::uint32_t> tids;
  std::uint64_t sample_count{};
  std::vector<std::string> evidence;
  double confidence{};
};

}  // namespace cachescope
