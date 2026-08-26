#include <iostream>
#include <sstream>
#include <string_view>

#include "report/JsonReport.hpp"

namespace {

bool expect(bool condition, std::string_view message) {
  if (condition) return true;
  std::cerr << "FAILED: " << message << '\n';
  return false;
}

}  // namespace

int main() {
  bool ok = true;

  AnalysisResult result;
  result.metadata.binary = "/tmp/a\"b";
  result.metadata.event = "mem-loads";
  result.metadata.sample_rate = 1000;
  result.capture.tool_version = "0.1.0";
  result.capture.capabilities.perf_events = true;
  result.quality.samples = 9;
  result.quality.lost = 1;
  result.quality.completed = true;
  result.stats.total_samples = 9;
  result.diagnostics.push_back(
    {"warning", "test.warning", "sample warning", "test remediation"});

  std::ostringstream output;
  JsonReport::write(output, result);
  ok &= expect(output.str().find("\"schema_version\": \"1.0\"") !=
                 std::string::npos,
               "new reports should declare schema 1.0");
  ok &= expect(output.str().find("\"sample_quality\"") != std::string::npos,
               "new reports should include quality metadata");

  std::istringstream input(output.str());
  const auto parsed = JsonReport::read(input);
  ok &= expect(parsed.has_value(), "schema 1.0 should parse");
  if (parsed) {
    ok &= expect(parsed.value().metadata.binary == result.metadata.binary,
                 "escaped metadata should round-trip");
    ok &= expect(parsed.value().quality.lost == 1,
                 "quality should round-trip");
  }

  std::istringstream legacy(R"({
    "metadata":{"binary":"legacy","event":"old","sample_period":7},
    "cache_topology":[],
    "sample_stats":{"total_samples":3},
    "false_sharing":[],
    "cache_thrashing":[]
  })");
  const auto adapted = JsonReport::read(legacy);
  ok &= expect(adapted.has_value() &&
                 adapted.value().schema_version == "1.0" &&
                 adapted.value().metadata.binary == "legacy",
               "unversioned legacy reports should be adapted");

  std::istringstream future(R"({"schema_version":"2.0"})");
  const auto rejected = JsonReport::read(future);
  ok &= expect(!rejected.has_value() &&
                 rejected.error().category ==
                   cachescope::ErrorCategory::Schema,
               "unsupported report major versions should be rejected");

  return ok ? 0 : 1;
}
