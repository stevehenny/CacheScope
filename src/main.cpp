#include <sys/wait.h>
#include <unistd.h>

#include <CLI/CLI.hpp>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <format>
#include <iostream>
#include <numbers>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

#include "common/Types.hpp"
#include "dwarf/Extractor.hpp"
#include "runtime/FalseSharingAnalysis.hpp"
#include "runtime/PipeStream.hpp"
#include "runtime/SampleStats.hpp"

// Detect CPU vendor from /proc/cpuinfo
static std::string detect_cpu_vendor() {
  std::ifstream cpuinfo("/proc/cpuinfo");
  std::string line;
  while (std::getline(cpuinfo, line)) {
    if (line.find("vendor_id") != std::string::npos) {
      if (line.find("GenuineIntel") != std::string::npos)
        return "intel";
      if (line.find("AuthenticAMD") != std::string::npos)
        return "amd";
    }
  }
  return "unknown";
}

// Get appropriate memory sampling events for the CPU
static std::string get_default_mem_events() {
  std::string vendor = detect_cpu_vendor();
  if (vendor == "intel") {
    // Intel PEBS events
    return "mem-loads:pp,mem-stores:pp";
  } else if (vendor == "amd") {
    // AMD IBS op sampling captures memory accesses
    return "ibs_op//";
  }
  // Fallback to generic events that should work on most x86
  return "cpu-cycles";
}

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

  // Optional time token (when perf script -F includes time)
  // Usually formatted like "12345.678901" or "12345.678901:".
  if (idx < toks.size()) {
    auto tt = toks[idx];
    if (!tt.empty() && tt.back() == ':') tt = tt.substr(0, tt.size() - 1);

    if (tt.find_first_not_of("0123456789.") == std::string_view::npos &&
        tt.find('.') != std::string_view::npos) {
      auto dot = tt.find('.');
      uint64_t secs = 0, nsecs = 0;
      try {
        secs = std::stoull(std::string(tt.substr(0, dot)));
        auto frac = std::string(tt.substr(dot + 1));
        if (frac.size() > 9) frac.resize(9);
        while (frac.size() < 9) frac.push_back('0');
        nsecs = std::stoull(frac);
      } catch (...) {
        secs = 0;
        nsecs = 0;
      }
      s.time_stamp = secs * 1000000000ULL + nsecs;
      idx++;
    }
  }

  // event types (often ends with ':')
  std::string event_str = std::string(toks[idx]);
  if (!event_str.empty() && event_str.back() == ':') event_str.pop_back();
  idx++;

  if (event_str == "mem-stores:pp" || event_str.find("store") != std::string::npos)
    s.event_type = SampleType::CACHE_STORE;
  else if (event_str == "mem-loads:pp" || event_str.find("load") != std::string::npos)
    s.event_type = SampleType::CACHE_LOAD;
  else
    s.event_type = SampleType::CACHE_LOAD;  // Generic / IBS: treat as access

  // ip
  s.ip = std::stoull(std::string(toks[idx]), nullptr, 16);
  idx++;

  // addr
  s.addr = std::stoull(std::string(toks[idx]), nullptr, 16);
  idx++;

  // sym
  if (idx < toks.size()) {
    s.symbol = std::string(toks[idx]);
    idx++;
  }

  // dso
  if (idx < toks.size()) {
    s.dso = std::string(toks[idx]);
    idx++;
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
    "perf script -i {} -F tid,pid,cpu,time,event,ip,addr,sym,dso 2>/dev/null",
    perf_data_file);

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
    "perf script -i {} -F tid,pid,cpu,time,ip,addr,sym,dso 2>/dev/null", perf_data_file);

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
  std::string output_file    = "perf.data";
  std::string default_events = get_default_mem_events();
  int sample_rate            = 10000;

  auto* analyze = app.add_subcommand("analyze", "Analyze cache behavior");
  analyze->add_option("binary", binary)->required()->check(CLI::ExistingFile);
  analyze->add_option("-o,--output", output_file, "Output perf data file");
  analyze->add_option("-e,--event", default_events, "Perf event to record");
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
                             binary, default_events, sample_rate);

    if (!run_perf_record(binary, output_file, default_events, sample_rate)) {
      std::cerr << "Perf recording failed\n";
      return;
    }

    std::cout << std::format("Recording completed: {}\n\n", output_file);

    // Phase 3: Parse samples
    std::cout << "=== Phase 3: Sample Parsing ===\n";

    auto samples = parse_perf_data(output_file);

    // Filter to samples attributed to the target binary (reduces libc/pthread noise).
    const auto bin_name = std::filesystem::path(binary).filename().string();
    size_t before = samples.size();
    std::erase_if(samples, [&](const PerfSample& s) {
      if (s.dso.empty()) return false;  // keep unknown
      if (s.dso.find(bin_name) != std::string::npos) return false;
      if (s.dso.find(binary) != std::string::npos) return false;
      return true;
    });
    if (verbose) {
      std::cout << std::format("Filtered samples by DSO: {} -> {}\n", before,
                               samples.size());
    }

    if (samples.empty()) {
      std::cerr << "No samples collected. Try:\n"
                << "  - Lower sample rate (-c)\n"
                << "  - Different event (-e)\n"
                << "  - Check available events: perf list\n"
                << "  - Intel: mem-loads:pp, mem-stores:pp\n"
                << "  - AMD: cpu/ibs_op/pp\n";
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
