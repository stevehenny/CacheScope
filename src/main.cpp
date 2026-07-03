#include <sys/wait.h>
#include <unistd.h>

#include <CLI/CLI.hpp>
#include <cctype>
#include <cstdio>
#include <iostream>
#include <string>

#include "analysis/AnalyzeCommand.hpp"
#include "runtime/Parser.hpp"
#include "runtime/PerfEventRecorder.hpp"
#ifdef CACHESCOPE_BUILD_GUI
#include "gui/ReportGUI.hpp"
#endif

int main(int argc, char* argv[]) {
  CLI::App app("CacheScope: Analyze and visualize CPU cache behavior");
  app.require_subcommand(1);

  bool verbose = false;
  app.add_flag("-v,--verbose", verbose, "Enable verbose debugging output");

  Parser parser;
  PerfEventRecorder recorder;
  AnalyzeCommand analyze_cmd(parser, recorder);
  std::string binary;
  std::string default_events   = parser.get_default_mem_events();
  int sample_rate              = 1000;
  std::string report_md_path   = "cache_scope.md";
  std::string report_json_path = "cache_scope.json";
  std::string report_gui_path  = "cache_scope.json";

  auto add_recording_options = [&](CLI::App* subcommand) {
    subcommand->add_option("-e,--event", default_events,
                           "Perf event to record");
    subcommand->add_option("-c,--count", sample_rate, "Sample period");
    subcommand->add_option("--report-md", report_md_path,
                           "Write false sharing report to Markdown file");
    subcommand->add_option("--report-json", report_json_path,
                           "Write false sharing report to JSON file");
  };

  auto* report = app.add_subcommand(
    "report",
    "Generate gui report from json file. Default is cache_scope.json");
#ifdef CACHESCOPE_BUILD_GUI
  report->add_option("report_json", report_gui_path,
                     "JSON report to display (default: cache_scope.json)");
  report->callback([&]() {
    ReportGUI gui;
    std::string error;
    if (!gui.init(error)) {
      std::cerr << error << "\n";
      return;
    }
    gui.render(report_gui_path);
  });
#else
  report->callback([&]() {
    std::cerr
      << "GUI support disabled. Rebuild with -DCACHESCOPE_BUILD_GUI=ON\n";
  });
#endif

  auto* analyze = app.add_subcommand("analyze", "Analyze cache behavior");
  analyze->add_option("binary", binary)->required()->check(CLI::ExistingFile);
  add_recording_options(analyze);

  analyze->callback([&]() {
    AnalyzeOptions options;
    options.binary           = binary;
    options.events           = default_events;
    options.sample_rate      = sample_rate;
    options.report_md_path   = report_md_path;
    options.report_json_path = report_json_path;
    options.verbose          = verbose;
    analyze_cmd.run(options);
  });

  int monitor_pid = 0;
  auto* monitor = app.add_subcommand("monitor",
                                     "Monitor a running process by pid");
  monitor->add_option("pid", monitor_pid)->required();
  add_recording_options(monitor);

  monitor->callback([&]() {
    if (monitor_pid <= 0) {
      std::cerr << "pid must be positive\n";
      return;
    }

    AnalyzeOptions options;
    options.pid              = static_cast<pid_t>(monitor_pid);
    options.events           = default_events;
    options.sample_rate      = sample_rate;
    options.report_md_path   = report_md_path;
    options.report_json_path = report_json_path;
    options.verbose          = verbose;
    analyze_cmd.run(options);
  });

  CLI11_PARSE(app, argc, argv);
  return 0;
}
