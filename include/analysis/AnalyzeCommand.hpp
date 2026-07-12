#pragma once

#include <optional>
#include <string>
#include <sys/types.h>

#include "runtime/Parser.hpp"
#include "runtime/PerfEventRecorder.hpp"

struct AnalyzeOptions {
  std::string binary;
  std::optional<pid_t> pid;
  std::string events;
  int sample_rate = 0;
  std::string report_md_path;
  std::string report_json_path;
  bool verbose = false;
};

class AnalyzeCommand {
public:
  AnalyzeCommand(Parser& parser, PerfEventRecorder& recorder);

  void run(const AnalyzeOptions& options);

private:
  Parser& parser_;
  PerfEventRecorder& recorder_;
};
