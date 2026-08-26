#include <sys/stat.h>
#include <unistd.h>

#include <CLI/CLI.hpp>

#include <chrono>
#include <filesystem>
#include <format>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "analysis/AnalyzeCommand.hpp"
#include "core/Error.hpp"
#include "core/Result.hpp"
#include "pipeline/AnalysisPipeline.hpp"
#include "platform/linux/Doctor.hpp"
#include "report/JsonReport.hpp"
#include "runtime/Parser.hpp"
#include "runtime/PerfEventRecorder.hpp"
#include "trace/Trace.hpp"
#ifdef CACHESCOPE_BUILD_GUI
#include "gui/ReportGUI.hpp"
#endif

namespace {

using cachescope::Error;
using cachescope::ErrorCategory;
using cachescope::Result;

class ConsoleProgress final : public cachescope::ProgressSink {
public:
  explicit ConsoleProgress(bool verbose) : verbose_(verbose) {}
  void update(std::string_view phase, double fraction) override {
    if (verbose_) {
      std::cerr << std::format("[{:3.0f}%] {}\n", fraction * 100.0, phase);
    }
  }

private:
  bool verbose_;
};

void print_error(const Error& error) {
  std::cerr << error.code << ": " << error.message;
  if (error.system_error) {
    std::cerr << ": " << error.system_error.message();
  }
  std::cerr << '\n';
  if (!error.remediation.empty()) {
    std::cerr << "Remediation: " << error.remediation << '\n';
  }
}

Error collection_error(std::string message) {
  const bool permission =
    message.find("Permission denied") != std::string::npos ||
    message.find("Operation not permitted") != std::string::npos;
  return Error{
    permission ? ErrorCategory::Permission : ErrorCategory::Collection,
    permission ? "collection.permission_denied" : "collection.failed",
    "record perf events", std::move(message), {},
    permission
      ? "Run cachescope doctor and grant CAP_PERFMON or adjust "
        "perf_event_paranoid."
      : "Run cachescope doctor and verify the selected PMU event."};
}

std::vector<cachescope::trace::TraceSample> trace_samples(
  const std::vector<PerfSample>& samples) {
  std::vector<cachescope::trace::TraceSample> output;
  output.reserve(samples.size());
  for (const auto& sample : samples) {
    if (sample.ip < 0 || sample.addr < 0 || sample.phys_addr < 0 ||
        sample.sp < 0 || sample.bp < 0 || sample.time_stamp < 0) {
      continue;
    }
    cachescope::trace::TraceSample converted;
    converted.pid = sample.pid;
    converted.tid = sample.tid;
    converted.cpu = sample.cpu;
    converted.timestamp = static_cast<std::uint64_t>(sample.time_stamp);
    converted.ip = static_cast<std::uint64_t>(sample.ip);
    converted.virtual_address = static_cast<std::uint64_t>(sample.addr);
    converted.physical_address =
      static_cast<std::uint64_t>(sample.phys_addr);
    converted.stack_pointer = static_cast<std::uint64_t>(sample.sp);
    converted.base_pointer = static_cast<std::uint64_t>(sample.bp);
    converted.data_source = sample.data_source;
    if (converted.data_source != 0) converted.presence |= cachescope::trace::HasDataSource;
    if (converted.ip != 0) converted.presence |= cachescope::trace::HasIp;
    if (converted.virtual_address != 0) {
      converted.presence |= cachescope::trace::HasVirtualAddress;
    }
    if (converted.physical_address != 0) {
      converted.presence |= cachescope::trace::HasPhysicalAddress;
    }
    if (converted.stack_pointer != 0) {
      converted.presence |= cachescope::trace::HasStackPointer;
    }
    if (converted.base_pointer != 0) {
      converted.presence |= cachescope::trace::HasBasePointer;
    }
    switch (sample.event_type) {
      case SampleType::CACHE_LOAD:
        converted.kind = cachescope::trace::SampleKind::CacheLoad;
        break;
      case SampleType::CACHE_STORE:
        converted.kind = cachescope::trace::SampleKind::CacheStore;
        break;
      case SampleType::PAGE_FAULT:
        converted.kind = cachescope::trace::SampleKind::PageFault;
        break;
    }
    output.push_back(converted);
  }
  return output;
}

cachescope::TraceMetadata capture_metadata(
  const std::vector<std::string>& command,
  const std::vector<std::string>& events,
  const PerfRecordResult& recording) {
  const auto doctor = cachescope::linux_platform::inspect_system();
  cachescope::TraceMetadata metadata;
  metadata.tool_version = CACHESCOPE_VERSION;
  metadata.command = command;
  if (!command.empty()) metadata.target_path = command.front();
  metadata.kernel_release = doctor.kernel_release;
  metadata.cpu_model = doctor.cpu_model;
  metadata.capabilities = doctor.capabilities;
  metadata.event_encodings = events;
  metadata.start_time_unix_ns =
    static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch())
        .count());
  metadata.capabilities.physical_addresses =
    std::ranges::any_of(recording.samples, [](const PerfSample& sample) {
      return sample.phys_addr != 0;
    });
  metadata.capabilities.user_registers =
    std::ranges::any_of(recording.samples, [](const PerfSample& sample) {
      return sample.sp != 0 || sample.bp != 0;
    });
  return metadata;
}

Result<PerfRecordResult, Error> record_launch(
  PerfEventRecorder& recorder, const std::vector<std::string>& program,
  const std::string& event_spec, int sample_period,
  const std::filesystem::path& output, bool verbose, bool allow_root_target) {
  if (program.empty()) {
    return Result<PerfRecordResult, Error>::failure(
      {ErrorCategory::Usage, "cli.missing_program", "record target",
       "A target program is required after --.", {}, {}});
  }
  if (geteuid() == 0 && !allow_root_target) {
    return Result<PerfRecordResult, Error>::failure(
      {ErrorCategory::Permission, "target.root_refused", "launch target",
       "Refusing to launch a target while CacheScope has effective UID 0.",
       {}, "Run unprivileged with CAP_PERFMON, or pass --allow-root-target "
           "after reviewing the target."});
  }

  std::error_code filesystem_error;
  const auto executable = std::filesystem::canonical(program.front(),
                                                      filesystem_error);
  if (filesystem_error || access(program.front().c_str(), X_OK) != 0) {
    return Result<PerfRecordResult, Error>::failure(
      {ErrorCategory::Usage, "target.not_executable", "validate target",
       program.front() + " is not an executable file.", {},
       "Provide an executable path after --."});
  }

  std::vector<std::string> arguments(program.begin() + 1, program.end());
  auto recording = recorder.record_binary(
    executable.string(), event_spec, sample_period, verbose, arguments);
  if (!recording.ok()) {
    return Result<PerfRecordResult, Error>::failure(
      collection_error(recording.error));
  }

  std::vector<std::string> command{executable.string()};
  command.insert(command.end(), arguments.begin(), arguments.end());
  const auto events = PerfEventRecorder::split_event_spec(event_spec);
  auto metadata = capture_metadata(command, events, recording);
  const auto written = cachescope::trace::write(
    output, metadata, trace_samples(recording.samples),
    recording.lost_records, recording.throttled_records);
  if (!written) {
    return Result<PerfRecordResult, Error>::failure(written.error());
  }
  return Result<PerfRecordResult, Error>::success(std::move(recording));
}

int target_status(const PerfRecordResult& recording) {
  if (recording.target_signal) return 128 + *recording.target_signal;
  return recording.target_exit_status.value_or(0);
}

std::optional<std::filesystem::path> temporary_trace(Error& error) {
  std::string pattern =
    (std::filesystem::temp_directory_path() /
     "cachescope-XXXXXX.cst").string();
  std::vector<char> writable(pattern.begin(), pattern.end());
  writable.push_back('\0');
  const int fd = mkstemps(writable.data(), 4);
  if (fd < 0) {
    error = Error::from_errno(ErrorCategory::Io, "trace.temp_failed",
                              "create temporary trace", pattern);
    return std::nullopt;
  }
  if (fchmod(fd, S_IRUSR | S_IWUSR) != 0) {
    error = Error::from_errno(ErrorCategory::Io, "trace.chmod_failed",
                              "secure temporary trace", writable.data());
    close(fd);
    return std::nullopt;
  }
  close(fd);
  return std::filesystem::path(writable.data());
}

int analyze_trace(const std::filesystem::path& trace_path,
                  const std::string& markdown,
                  const std::string& json,
                  size_t max_findings, bool verbose) {
  cachescope::AnalysisRequest request;
  request.trace_source = trace_path;
  request.markdown_output = markdown;
  request.json_output = json;
  request.max_findings = max_findings;

  ConsoleProgress progress(verbose);
  cachescope::AnalysisPipeline pipeline;
  auto result = pipeline.run(request, progress, {});
  if (!result) {
    print_error(result.error());
    return cachescope::exit_status(result.error());
  }

  std::cout << std::format(
    "Analyzed {} samples: {} suspected false-sharing findings, "
    "{} suspected cache-thrashing findings\n",
    result.value().stats.total_samples,
    result.value().false_sharing.size(),
    result.value().cache_thrashing.size());
  for (const auto& diagnostic : result.value().diagnostics) {
    std::cerr << diagnostic.severity << " [" << diagnostic.code
              << "]: " << diagnostic.message << '\n';
  }
  return 0;
}

std::string resolve_binary(pid_t pid) {
  std::error_code error;
  const auto path =
    std::filesystem::read_symlink(std::format("/proc/{}/exe", pid), error);
  return error ? std::string{} : path.string();
}

}  // namespace

int main(int argc, char* argv[]) {
  CLI::App app("CacheScope: reproducible CPU cache analysis");
  app.require_subcommand(1);
  app.set_version_flag("--version", CACHESCOPE_VERSION);

  bool verbose = false;
  app.add_flag("-v,--verbose", verbose, "Enable diagnostic progress output");

  Parser parser;
  PerfEventRecorder recorder;
  AnalyzeCommand legacy_analyze(parser, recorder);
  const std::string detected_events = parser.get_default_mem_events();
  int command_status = 0;

  std::vector<std::string> record_program;
  std::string record_output;
  std::string record_events = detected_events;
  int record_period = 10000;
  bool record_allow_root = false;
  auto* record = app.add_subcommand(
    "record", "Record a target into a reproducible .cst trace");
  record->positionals_at_end();
  record->add_option("-o,--output", record_output, "Output .cst trace")
    ->required();
  record->add_option("-e,--event", record_events, "Perf event list");
  record->add_option("-c,--count", record_period, "Sample period")
    ->check(CLI::PositiveNumber);
  record->add_flag("--allow-root-target", record_allow_root,
                   "Allow launching the target with effective UID 0");
  record->add_option("program", record_program,
                     "Target and arguments after --")->required();
  record->callback([&]() {
    auto result = record_launch(recorder, record_program, record_events,
                                record_period, record_output, verbose,
                                record_allow_root);
    if (!result) {
      print_error(result.error());
      command_status = cachescope::exit_status(result.error());
      return;
    }
    std::cout << "Wrote trace: " << record_output << '\n';
    command_status = target_status(result.value());
  });

  std::string analyze_trace_path;
  std::string compatibility_binary;
  std::string analyze_markdown = "cache_scope.md";
  std::string analyze_json = "cache_scope.json";
  size_t analyze_max_findings = 100;
  std::string compatibility_events = detected_events;
  int compatibility_period = 10000;
  auto* analyze = app.add_subcommand(
    "analyze", "Analyze a .cst trace (or deprecated binary alias)");
  analyze->add_option("--trace", analyze_trace_path, "Input .cst trace")
    ->check(CLI::ExistingFile);
  analyze->add_option("binary", compatibility_binary,
                      "Deprecated: binary to record and analyze");
  analyze->add_option("-e,--event", compatibility_events, "Perf event list");
  analyze->add_option("-c,--count", compatibility_period, "Sample period")
    ->check(CLI::PositiveNumber);
  analyze->add_option("--report-md", analyze_markdown,
                      "Markdown report destination");
  analyze->add_option("--report-json", analyze_json,
                      "JSON report destination");
  analyze->add_option("--max-findings", analyze_max_findings,
                      "Maximum findings per detector")
    ->check(CLI::PositiveNumber);
  analyze->callback([&]() {
    if (!analyze_trace_path.empty()) {
      command_status = analyze_trace(analyze_trace_path, analyze_markdown,
                                     analyze_json, analyze_max_findings,
                                     verbose);
      return;
    }
    if (compatibility_binary.empty()) {
      Error error{ErrorCategory::Usage, "cli.missing_trace",
                  "select analysis input",
                  "Provide --trace recording.cst or a compatibility binary.",
                  {}, {}};
      print_error(error);
      command_status = cachescope::exit_status(error);
      return;
    }
    std::cerr << "warning: 'analyze <binary>' is deprecated; use "
                 "'run -- <binary>'.\n";
    AnalyzeOptions options;
    options.binary = compatibility_binary;
    options.events = compatibility_events;
    options.sample_rate = compatibility_period;
    options.report_md_path = analyze_markdown;
    options.report_json_path = analyze_json;
    options.verbose = verbose;
    legacy_analyze.run(options);
  });

  std::vector<std::string> run_program;
  std::string run_events = detected_events;
  int run_period = 10000;
  std::string run_markdown = "cache_scope.md";
  std::string run_json = "cache_scope.json";
  size_t run_max_findings = 100;
  bool keep_trace = false;
  bool run_allow_root = false;
  auto* run = app.add_subcommand(
    "run", "Record a target, then analyze the trace offline");
  run->positionals_at_end();
  run->add_option("-e,--event", run_events, "Perf event list");
  run->add_option("-c,--count", run_period, "Sample period")
    ->check(CLI::PositiveNumber);
  run->add_option("--report-md", run_markdown,
                  "Markdown report destination");
  run->add_option("--report-json", run_json, "JSON report destination");
  run->add_option("--max-findings", run_max_findings,
                  "Maximum findings per detector")
    ->check(CLI::PositiveNumber);
  run->add_flag("--keep-trace", keep_trace,
                "Retain the temporary trace after success");
  run->add_flag("--allow-root-target", run_allow_root,
                "Allow launching the target with effective UID 0");
  run->add_option("program", run_program,
                  "Target and arguments after --")->required();
  run->callback([&]() {
    Error temporary_error;
    const auto trace_path = temporary_trace(temporary_error);
    if (!trace_path) {
      print_error(temporary_error);
      command_status = cachescope::exit_status(temporary_error);
      return;
    }

    auto recording = record_launch(recorder, run_program, run_events,
                                   run_period, *trace_path, verbose,
                                   run_allow_root);
    if (!recording) {
      print_error(recording.error());
      std::error_code ignored;
      std::filesystem::remove(*trace_path, ignored);
      command_status = cachescope::exit_status(recording.error());
      return;
    }

    const int analysis_status =
      analyze_trace(*trace_path, run_markdown, run_json, run_max_findings,
                    verbose);
    if (analysis_status != 0) {
      std::cerr << "Preserved trace after analysis/report failure: "
                << trace_path->string() << '\n';
      command_status = analysis_status;
      return;
    }

    if (keep_trace) {
      std::cout << "Retained trace: " << trace_path->string() << '\n';
    } else {
      std::error_code remove_error;
      std::filesystem::remove(*trace_path, remove_error);
      if (remove_error) {
        Error error{ErrorCategory::Io, "trace.cleanup_failed",
                    "remove temporary trace", remove_error.message(),
                    remove_error, {}};
        print_error(error);
        std::cerr << "Preserved trace: " << trace_path->string() << '\n';
        command_status = cachescope::exit_status(error);
        return;
      }
    }
    command_status = target_status(recording.value());
  });

  int monitor_pid = 0;
  std::string monitor_output;
  std::string monitor_events = detected_events;
  int monitor_period = 10000;
  std::string monitor_markdown = "cache_scope.md";
  std::string monitor_json = "cache_scope.json";
  auto* monitor = app.add_subcommand("monitor", "Monitor a running process");
  monitor->add_option("pid", monitor_pid, "Process ID")->required();
  monitor->add_option("-o,--output", monitor_output,
                      "Optional .cst trace destination");
  monitor->add_option("-e,--event", monitor_events, "Perf event list");
  monitor->add_option("-c,--count", monitor_period, "Sample period")
    ->check(CLI::PositiveNumber);
  monitor->add_option("--report-md", monitor_markdown,
                      "Markdown report destination");
  monitor->add_option("--report-json", monitor_json,
                      "JSON report destination");
  monitor->callback([&]() {
    if (monitor_pid <= 0) {
      Error error{ErrorCategory::Usage, "cli.invalid_pid", "monitor process",
                  "PID must be positive.", {}, {}};
      print_error(error);
      command_status = cachescope::exit_status(error);
      return;
    }
    if (monitor_output.empty()) {
      AnalyzeOptions options;
      options.pid = static_cast<pid_t>(monitor_pid);
      options.events = monitor_events;
      options.sample_rate = monitor_period;
      options.report_md_path = monitor_markdown;
      options.report_json_path = monitor_json;
      options.verbose = verbose;
      legacy_analyze.run(options);
      return;
    }

    const auto binary = resolve_binary(static_cast<pid_t>(monitor_pid));
    if (binary.empty()) {
      Error error{ErrorCategory::Io, "monitor.resolve_failed",
                  "resolve monitored executable",
                  "Could not resolve /proc/<pid>/exe.", {}, {}};
      print_error(error);
      command_status = cachescope::exit_status(error);
      return;
    }
    auto recording = recorder.record_pid(
      static_cast<pid_t>(monitor_pid), binary, monitor_events,
      monitor_period, verbose);
    if (!recording.ok()) {
      const auto error = collection_error(recording.error);
      print_error(error);
      command_status = cachescope::exit_status(error);
      return;
    }
    auto events = PerfEventRecorder::split_event_spec(monitor_events);
    auto metadata = capture_metadata(
      {"monitor", std::to_string(monitor_pid)}, events, recording);
    metadata.target_path = binary;
    const auto written = cachescope::trace::write(
      monitor_output, metadata, trace_samples(recording.samples),
      recording.lost_records, recording.throttled_records);
    if (!written) {
      print_error(written.error());
      command_status = cachescope::exit_status(written.error());
      return;
    }
    std::cout << "Wrote trace: " << monitor_output << '\n';
  });

  auto* doctor = app.add_subcommand(
    "doctor", "Check platform, PMU, permissions, and cache topology");
  doctor->callback([&]() {
    const auto report = cachescope::linux_platform::inspect_system();
    std::cout << "CacheScope doctor\n"
              << "Kernel: " << report.kernel_release << '\n'
              << "CPU: " << report.cpu_model << '\n';
    for (const auto& check : report.checks) {
      const char* status =
        check.status == cachescope::linux_platform::CheckStatus::Pass
          ? "PASS"
          : check.status ==
                cachescope::linux_platform::CheckStatus::Warning
              ? "WARN"
              : "FAIL";
      std::cout << std::format("[{}] {}: {}\n", status, check.name,
                               check.detail);
      if (!check.remediation.empty()) {
        std::cout << "       " << check.remediation << '\n';
      }
    }
    if (!report.supported()) command_status = 3;
    else if (!report.permission_ready()) command_status = 4;
  });

  std::string report_path = "cache_scope.json";
  auto* report = app.add_subcommand(
    "report", "Open a legacy or schema 1.0 JSON report");
  report->add_option("report_json", report_path, "JSON report to display");
  report->callback([&]() {
    const auto parsed = JsonReport::read_file(report_path);
    if (!parsed) {
      print_error(parsed.error());
      command_status = cachescope::exit_status(parsed.error());
      return;
    }
#ifdef CACHESCOPE_BUILD_GUI
    ReportGUI gui;
    std::string error;
    if (!gui.init(error)) {
      std::cerr << error << '\n';
      command_status = 3;
      return;
    }
    gui.render(report_path);
#else
    std::cerr << "GUI support disabled. Rebuild with "
                 "-DCACHESCOPE_BUILD_GUI=ON\n";
    command_status = 3;
#endif
  });

  CLI11_PARSE(app, argc, argv);
  return command_status;
}
