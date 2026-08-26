#include <sys/stat.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string_view>
#include <vector>

#include "trace/Trace.hpp"

namespace {

bool expect(bool condition, std::string_view message) {
  if (condition) return true;
  std::cerr << "FAILED: " << message << '\n';
  return false;
}

}  // namespace

int main() {
  namespace fs = std::filesystem;
  using namespace cachescope;
  using namespace cachescope::trace;

  const fs::path directory =
    fs::temp_directory_path() / "cachescope-trace-format-test";
  fs::create_directories(directory);
  const auto trace_path = directory / "roundtrip.cst";

  TraceMetadata metadata;
  metadata.tool_version = "test";
  metadata.command = {"fixture", "--argument"};
  metadata.target_path = "/tmp/fixture";
  metadata.kernel_release = "test-kernel";
  metadata.event_encodings = {"mem-loads", "mem-stores"};
  metadata.capabilities.perf_events = true;
  metadata.capabilities.user_registers = true;

  TraceSample sample;
  sample.presence = HasIp | HasVirtualAddress | HasPhysicalAddress |
                    HasStackPointer | HasBasePointer | HasDataSource;
  sample.event_id = 7;
  sample.pid = 100;
  sample.tid = 101;
  sample.cpu = 3;
  sample.timestamp = 123456;
  sample.ip = 0x401000;
  sample.virtual_address = 0x7fff0000;
  sample.physical_address = 0x12340000;
  sample.stack_pointer = 0x7ffff000;
  sample.base_pointer = 0x7ffff100;
  sample.data_source = 42;
  sample.kind = SampleKind::CacheStore;

  bool ok = true;
  const auto written = write(trace_path, metadata, {sample}, 4, 2);
  ok &= expect(written.has_value(), "a valid trace should be written");

  struct stat status {};
  ok &= expect(::stat(trace_path.c_str(), &status) == 0,
               "written trace should exist");
  ok &= expect((status.st_mode & 0777) == 0600,
               "trace permissions should be exactly 0600");

  const auto loaded = read(trace_path);
  ok &= expect(loaded.has_value(), "a valid trace should round-trip");
  if (loaded) {
    ok &= expect(loaded.value().minor_version == kTraceMinorVersion,
                 "minor version should round-trip");
    ok &= expect(loaded.value().metadata.command == metadata.command,
                 "metadata command should round-trip");
    ok &= expect(loaded.value().samples.size() == 1,
                 "one sample should round-trip");
    ok &= expect(loaded.value().quality.lost == 4 &&
                   loaded.value().quality.throttled == 2 &&
                   loaded.value().quality.completed,
                 "quality records and completion should round-trip");
    if (!loaded.value().samples.empty()) {
      const auto& actual = loaded.value().samples.front();
      ok &= expect(actual.virtual_address == sample.virtual_address &&
                     actual.physical_address == sample.physical_address &&
                     actual.kind == SampleKind::CacheStore,
                   "sample fields should round-trip");
    }
  }

  const auto capped = read(trace_path, 0);
  ok &= expect(capped.has_value() && capped.value().samples.empty() &&
                 capped.value().quality.truncated &&
                 capped.value().quality.evicted_samples == 1,
               "sample cap should surface deterministic truncation");

  const auto corrupt_path = directory / "corrupt.cst";
  fs::copy_file(trace_path, corrupt_path,
                fs::copy_options::overwrite_existing);
  {
    std::fstream file(corrupt_path, std::ios::in | std::ios::out |
                                      std::ios::binary);
    file.seekp(-1, std::ios::end);
    char byte = 1;
    file.write(&byte, 1);
  }
  const auto corrupt = read(corrupt_path);
  ok &= expect(!corrupt.has_value() &&
                 corrupt.error().category == ErrorCategory::Schema,
               "corrupt frames should be rejected");

  const auto truncated_path = directory / "truncated.cst";
  {
    std::ofstream file(truncated_path, std::ios::binary);
    file.write("CSTRACE", 7);
  }
  const auto truncated = read(truncated_path);
  ok &= expect(!truncated.has_value() &&
                 truncated.error().code == "trace.truncated_header",
               "truncated headers should be rejected");

  fs::remove_all(directory);
  return ok ? 0 : 1;
}
