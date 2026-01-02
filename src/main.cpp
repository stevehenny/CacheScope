#include <sys/wait.h>
#include <unistd.h>

#include <CLI/CLI.hpp>
#include <algorithm>
#include <format>
#include <iostream>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

#include "dwarf/Extractor.hpp"
#include "runtime/FalseSharingAnalysis.hpp"
#include "runtime/PipeStream.hpp"
#include "runtime/SampleStats.hpp"

static inline std::string_view trim(std::string_view sv) {
  auto b = sv.find_first_not_of(" \t\n");
  auto e = sv.find_last_not_of(" \t\n");
  if (b == std::string_view::npos) return {};
  return sv.substr(b, e - b + 1);
}
// Parse a single perf script line
static std::optional<PerfSample> parse_perf_line(std::string_view line) {
  line = trim(line);
  if (line.empty() || line[0] == '#') return std::nullopt;

  PerfSample s{};

  // Tokenize by whitespace
  std::vector<std::string_view> toks;
  size_t pos = 0;
  while (pos < line.size()) {
    size_t start = line.find_first_not_of(" \t", pos);
    if (start == std::string_view::npos) break;

    size_t end = line.find_first_of(" \t", start);
    toks.push_back(line.substr(start, end - start));
    pos = end;
  }

  // Minimum expected:
  // pid/tid [cpu] ip addr sym...
  if (toks.size() < 5) return std::nullopt;

  size_t idx = 0;

  // Handle optional comm name (non pid/tid token)
  if (toks[0].find('/') == std::string_view::npos) {
    idx++;
    if (toks.size() - idx < 5) return std::nullopt;
  }

  // pid/tid
  auto slash = toks[idx].find('/');
  if (slash == std::string_view::npos) return std::nullopt;

  s.pid = std::stoul(std::string(toks[idx].substr(0, slash)));
  s.tid = std::stoul(std::string(toks[idx].substr(slash + 1)));
  idx++;

  // [cpu]
  if (toks[idx].front() != '[' || toks[idx].back() != ']') return std::nullopt;

  s.cpu = std::stoul(std::string(toks[idx].substr(1, toks[idx].size() - 2)));
  idx++;

  // ip
  s.ip = std::stoull(std::string(toks[idx]), nullptr, 16);
  idx++;

  // addr
  s.addr = std::stoull(std::string(toks[idx]), nullptr, 16);
  idx++;

  // symbol = rest of line
  size_t sym_pos = line.find(toks[idx]);
  if (sym_pos != std::string_view::npos) {
    s.symbol = std::string(trim(line.substr(sym_pos)));
  }

  return s;
}

static bool run_perf_record(const std::string& binary,
                            const std::string& output_file,
                            const std::string& event, int sample_rate) {
  pid_t perf_pid = fork();

  if (perf_pid == 0) {
    // Child: exec perf record
    auto count_str = std::to_string(sample_rate);

    execlp("perf", "perf", "record", "-e", event.c_str(),
           "-d",                     // Record addresses
           "--sample-cpu",           // Record CPU
           "-c", count_str.c_str(),  // Sample period
           "-o", output_file.c_str(), "--", binary.c_str(), nullptr);

    // If exec fails
    perror("execlp perf");
    _exit(127);
  }

  // Parent: wait for perf to finish
  int status;
  waitpid(perf_pid, &status, 0);

  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

// Parse perf script output
std::vector<PerfSample> parse_perf_data(const std::string& perf_data_file) {
  std::string cmd = std::format(
    "perf script -i {} -F tid,pid,cpu,ip,addr,sym 2>/dev/null", perf_data_file);

  PipeStream pipe(cmd);
  auto lines = pipe.read_lines();

  std::vector<PerfSample> samples;
  samples.reserve(lines.size());  // Optimize allocation

  for (const auto& line : lines) {
    if (auto sample = parse_perf_line(line)) {
      samples.push_back(std::move(*sample));
    }
  }

  return samples;
}

auto parse_perf_data_ranges(const std::string& perf_data_file) {
  std::string cmd = std::format(
    "perf script -i {} -F tid,pid,cpu,ip,addr,sym 2>/dev/null", perf_data_file);

  PipeStream pipe(cmd);
  auto lines = pipe.read_lines();

  // Use ranges to transform and filter
  return lines | std::views::transform([](const auto& line) {
           return parse_perf_line(line);
         }) |
         std::views::filter([](const auto& opt) { return opt.has_value(); }) |
         std::views::transform([](auto&& opt) { return std::move(*opt); });
}

// Statistics helper

int main(int argc, char* argv[]) {
  CLI::App app("CacheScope: Analyze and visualize CPU cache behavior");
  app.require_subcommand(1);

  bool verbose = false;
  app.add_flag("-v,--verbose", verbose, "Enable verbose debugging output");

  std::string binary;
  std::string output_file = "perf.data";
  std::string event       = "mem-loads:pp";
  int sample_rate         = 10000;

  auto* analyze = app.add_subcommand("analyze", "Analyze cache behavior");
  analyze->add_option("binary", binary)->required()->check(CLI::ExistingFile);
  analyze->add_option("-o,--output", output_file, "Output perf data file");
  analyze->add_option("-e,--event", event, "Perf event to record");
  analyze->add_option("-c,--count", sample_rate, "Sample period");

  analyze->callback([&]() {
    // Phase 1: DWARF extraction
    std::cout << "=== Phase 1: DWARF Analysis ===\n";
    Extractor ext{binary};
    ext.create_registry();

    if (verbose) {
      for (const auto& [k, v] : ext.get_registry().get_map()) {
        std::cout << std::format("{}: {} bytes\n", k, v.size);
      }
    }

    const auto& stack_objects = ext.get_stack_objects();
    std::cout << std::format("Found {} stack objects\n\n",
                             stack_objects.size());

    // Phase 2: Run perf record
    std::cout << "=== Phase 2: Performance Recording ===\n";
    std::cout << std::format("Recording {} with event '{}' (period={})\n",
                             binary, event, sample_rate);

    if (!run_perf_record(binary, output_file, event, sample_rate)) {
      std::cerr << "Perf recording failed\n";
      return;
    }

    std::cout << std::format("Recording completed: {}\n\n", output_file);

    // Phase 3: Parse samples
    std::cout << "=== Phase 3: Sample Parsing ===\n";

    auto samples = parse_perf_data(output_file);

    if (samples.empty()) {
      std::cerr << "No samples collected. Try:\n"
                << "  - Lower sample rate (-c)\n"
                << "  - Different event (-e mem-loads)\n"
                << "  - Check: perf list | grep mem\n";
      return;
    }

    // Compute statistics
    auto stats = SampleStats::compute(samples);
    std::cout << stats;

    // Show sample preview
    if (verbose || samples.size() <= 20) {
      std::cout << "\n=== Sample Preview ===\n";
      for (size_t i = 0; i < std::min(samples.size(), size_t{10}); ++i) {
        std::cout << std::format("Sample #{}:\n{}\n", i + 1, samples[i].symbol);
      }
    }

    // Phase 4: False sharing analysis
    auto hot_lines = FalseSharingAnalysis::find_hot_cache_lines(samples);
    FalseSharingAnalysis::print(hot_lines);

    // Phase 5: Correlate with DWARF (future)
    std::cout << "=== Phase 5: Symbol Correlation ===\n";
    std::cout << "(TODO: Match addresses to stack variables)\n";
  });

  CLI11_PARSE(app, argc, argv);
  return 0;
}
