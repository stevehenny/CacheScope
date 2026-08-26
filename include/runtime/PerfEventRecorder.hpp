#pragma once

#include <sys/types.h>

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/Types.hpp"

struct ProcMapRange {
  int64_t start;
  int64_t end;
  int64_t offset;
};

struct PerfRecordResult {
  std::vector<PerfSample> samples;
  std::optional<int64_t> load_bias;
  std::vector<ProcMapRange> binary_maps;
  std::optional<int> target_exit_status;
  std::optional<int> target_signal;
  std::uint64_t lost_records{};
  std::uint64_t throttled_records{};
  std::uint64_t malformed_records{};
  std::string error;

  bool ok() const { return error.empty(); }
};

class PerfEventRecorder {
public:
  using MonitorUpdateCallback = std::function<void(
    const std::vector<PerfSample>& samples, size_t new_samples, bool done)>;

  PerfRecordResult record_binary(
    const std::string& binary, const std::string& event_spec,
    int sample_period, bool verbose,
    const std::vector<std::string>& arguments = {},
    const std::optional<std::filesystem::path>& working_directory =
      std::nullopt);

  PerfRecordResult record_pid(pid_t pid, const std::string& binary,
                              const std::string& event_spec, int sample_period,
                              bool verbose,
                              MonitorUpdateCallback on_update = {});

  static std::vector<std::string> split_event_spec(std::string_view spec);
};
