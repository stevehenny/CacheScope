#pragma once

#include <optional>
#include <functional>
#include <string>
#include <string_view>
#include <vector>
#include <sys/types.h>

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
  std::string error;

  bool ok() const { return error.empty(); }
};

class PerfEventRecorder {
public:
  using MonitorUpdateCallback =
    std::function<void(const std::vector<PerfSample>& samples,
                       size_t new_samples, bool done)>;

  PerfRecordResult record_binary(const std::string& binary,
                                 const std::string& event_spec,
                                 int sample_period, bool verbose);

  PerfRecordResult record_pid(pid_t pid, const std::string& binary,
                              const std::string& event_spec,
                              int sample_period, bool verbose,
                              MonitorUpdateCallback on_update = {});

  static std::vector<std::string> split_event_spec(std::string_view spec);
};
