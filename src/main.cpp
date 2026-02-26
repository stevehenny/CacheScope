#include <sys/wait.h>
#include <unistd.h>

#include <CLI/CLI.hpp>
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "analysis/AnalyzeCommand.hpp"
#include "common/Types.hpp"
#include "dwarf/Extractor.hpp"
#include "report/JsonReport.hpp"
#include "report/TextReport.hpp"
#include "runtime/FalseSharingAnalysis.hpp"
#include "runtime/Parser.hpp"
#include "runtime/PerfEventRecorder.hpp"
#include "runtime/SampleStats.hpp"
#ifdef CACHESCOPE_BUILD_GUI
#include "gui/ReportGUI.hpp"
#endif


// Statistics helper

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
  int sample_rate              = 10000;
  std::string report_md_path   = "cache_scope.md";
  std::string report_json_path = "cache_scope.json";
  std::string report_gui_path  = "cache_scope.json";

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
  analyze->add_option("-e,--event", default_events, "Perf event to record");
  analyze->add_option("-c,--count", sample_rate, "Sample period");
  analyze->add_option("--report-md", report_md_path,
                      "Write false sharing report to Markdown file");
  analyze->add_option("--report-json", report_json_path,
                      "Write false sharing report to JSON file");

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

  CLI11_PARSE(app, argc, argv);
  return 0;
}
