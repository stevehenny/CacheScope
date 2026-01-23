#include <sys/wait.h>
#include <unistd.h>

#include <CLI/CLI.hpp>
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <format>
#include <iostream>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <vector>

#include "common/Types.hpp"
#include "dwarf/Extractor.hpp"
#include "runtime/FalseSharingAnalysis.hpp"
#include "runtime/Parser.hpp"
#include "runtime/SampleStats.hpp"

static bool run_perf_record(const std::string& binary,
                            const std::string& output_file,
                            const std::string& event, int sample_rate) {
  pid_t perf_pid = fork();

  if (perf_pid == 0) {
    // Child: exec perf record
    auto count_str = std::to_string(sample_rate);

    execlp(
      "perf", "perf", "record", "-e", event.c_str(),
      "-d",                 // Record addresses
      "--sample-cpu",       // Record CPU
      "--user-regs=sp,bp",  // Sample stack + frame pointers for runtime vars
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

// Statistics helper

int main(int argc, char* argv[]) {
  CLI::App app("CacheScope: Analyze and visualize CPU cache behavior");
  app.require_subcommand(1);

  bool verbose = false;
  app.add_flag("-v,--verbose", verbose, "Enable verbose debugging output");

  Parser parser;
  std::string binary;
  std::string output_file    = "perf.data";
  std::string default_events = parser.get_default_mem_events();
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

    auto samples = parser.parse_perf_data(output_file);

    // Filter to samples attributed to the target binary (reduces libc/pthread
    // noise).
    const auto bin_name = std::filesystem::path(binary).filename().string();
    size_t before       = samples.size();
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
                << "  - AMD: ibs_op//\n";
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

    // Phase 5: Runtime attribution (stack locals)
    std::cout << "=== Phase 5: Runtime Attribution (Stack) ===\n";

    int64_t load_bias = 0;
    if (auto lb = parser.get_load_bias_from_perf_mmaps(output_file, binary,
                                                       samples[0].pid)) {
      load_bias = *lb;
      if (verbose) {
        std::cout << std::format("Detected load bias (perf mmaps): 0x{:x}\n",
                                 load_bias);
      }
    }

    std::unique_ptr<DwarfContext> frame_ctx;
    try {
      frame_ctx = std::make_unique<DwarfContext>(binary);
    } catch (...) {
      frame_ctx.reset();
    }

    Dwarf_Cie* cie_data    = nullptr;
    Dwarf_Fde* fde_data    = nullptr;
    Dwarf_Signed cie_count = 0;
    Dwarf_Signed fde_count = 0;
    Dwarf_Error frame_err  = nullptr;

    bool have_frames = false;
    if (frame_ctx) {
      have_frames =
        dwarf_get_fde_list_eh(frame_ctx->dbg(), &cie_data, &cie_count,
                              &fde_data, &fde_count, &frame_err) == DW_DLV_OK;
      if (!have_frames) {
        have_frames =
          dwarf_get_fde_list(frame_ctx->dbg(), &cie_data, &cie_count, &fde_data,
                             &fde_count, &frame_err) == DW_DLV_OK;
      }
    }

    int64_t inferred_bias = 0;
    if (have_frames && fde_data && fde_count > 0) {
      int64_t min_fde_lopc = 0;
      bool have_any        = false;
      for (Dwarf_Signed i = 0; i < fde_count; ++i) {
        Dwarf_Fde fde = fde_data[i];
        if (!fde) continue;

        Dwarf_Addr lopc              = 0;
        Dwarf_Unsigned len           = 0;
        Dwarf_Ptr fde_bytes          = nullptr;
        Dwarf_Unsigned fde_bytes_len = 0;
        Dwarf_Off cie_offset         = 0;
        Dwarf_Signed cie_index       = 0;
        Dwarf_Off fde_offset         = 0;
        Dwarf_Error e                = nullptr;

        if (dwarf_get_fde_range(fde, &lopc, &len, &fde_bytes, &fde_bytes_len,
                                &cie_offset, &cie_index, &fde_offset,
                                &e) != DW_DLV_OK)
          continue;

        if (!have_any) {
          min_fde_lopc = lopc;
          have_any     = true;
        } else {
          min_fde_lopc = std::min<int64_t>(min_fde_lopc, lopc);
        }
      }

      if (have_any) {
        int64_t min_ip{};
        bool have_ip = false;
        for (const auto& smp : samples) {
          if (smp.ip == 0 || smp.dso.empty()) continue;
          if (smp.dso.find(bin_name) == std::string::npos &&
              smp.dso.find(binary) == std::string::npos)
            continue;
          if (!have_ip) {
            min_ip  = smp.ip;
            have_ip = true;
          } else {
            min_ip = std::min(min_ip, smp.ip);
          }
        }

        if (have_ip && min_ip > min_fde_lopc) {
          inferred_bias = min_ip - min_fde_lopc;
          if (verbose) {
            std::cout << std::format(
              "Inferred load bias (FDE vs runtime IP): 0x{:x}\n",
              inferred_bias);
          }
        }
      }
    }

    if (!have_frames) {
      std::cerr
        << "WARNING: Failed to read DWARF CFI (.eh_frame/.debug_frame); stack "
           "attribution will be skipped.\n";
    }

    std::unordered_map<std::string, std::vector<const DwarfStackObject*>>
      by_function;
    by_function.reserve(stack_objects.size());
    for (const auto& o : stack_objects) {
      by_function[o.function].push_back(&o);
    }

    size_t stack_hits = 0;
    std::unordered_map<std::string, size_t> var_hits;

    size_t cfa_ok   = 0;
    size_t cfa_miss = 0;

    for (const auto& s : samples) {
      if (!have_frames || s.ip == 0 || s.sp == 0 || s.addr == 0 ||
          s.symbol.empty())
        continue;

      // Only do stack attribution when IP is from the target binary.
      if (s.dso.empty() || (s.dso.find(bin_name) == std::string::npos &&
                            s.dso.find(binary) == std::string::npos))
        continue;

      auto fn = std::string(parser.base_symbol(s.symbol));
      auto it = by_function.find(fn);
      if (it == by_function.end()) continue;

      // Map runtime IP to a DWARF PC for CFI lookup (handle PIE/ASLR via
      // load_bias).
      auto try_cfa = [&](int64_t pc) {
        return parser.compute_cfa_for_sample(fde_data, s, pc);
      };

      std::optional<int64_t> cfa;
      // Try raw runtime IP first (non-PIE / already-relocated FDEs)
      cfa = try_cfa(s.ip);
      // Then try subtracting known/perf-inferred biases.
      if (!cfa && load_bias && s.ip >= load_bias)
        cfa = try_cfa(s.ip - load_bias);
      if (!cfa && inferred_bias && s.ip >= inferred_bias)
        cfa = try_cfa(s.ip - inferred_bias);

      if (!cfa) {
        ++cfa_miss;
        continue;
      }
      ++cfa_ok;

      for (const auto* obj : it->second) {
        const int64_t cfa_i64 = static_cast<int64_t>(*cfa);
        const int64_t loc     = cfa_i64 + obj->frame_offset;
        if (loc < 0) continue;
        const int64_t var_addr = static_cast<int64_t>(loc);
        const int64_t var_end  = var_addr + obj->size;

        if (s.addr >= var_addr && s.addr < var_end) {
          ++stack_hits;
          ++var_hits[obj->function + "::" + obj->name];
          break;
        }
      }
    }

    if (have_frames && frame_ctx) {
      dwarf_fde_cie_list_dealloc(frame_ctx->dbg(), cie_data, cie_count,
                                 fde_data, fde_count);
    }

    if (verbose) {
      std::cout << std::format("CFA computed: {}  CFA miss: {}\n", cfa_ok,
                               cfa_miss);
    }

    std::cout << std::format("Stack-attributed samples: {} / {}\n\n",
                             stack_hits, samples.size());

    if (verbose && !var_hits.empty()) {
      std::vector<std::pair<std::string, size_t>> ranked(var_hits.begin(),
                                                         var_hits.end());
      std::ranges::sort(ranked, [](const auto& a, const auto& b) {
        if (a.second != b.second) return a.second > b.second;
        return a.first < b.first;
      });

      std::cout << "Top stack variables by hits:\n";
      for (size_t i = 0; i < std::min<size_t>(ranked.size(), 10); ++i) {
        std::cout << std::format("  {}: {}\n", ranked[i].first,
                                 ranked[i].second);
      }
      std::cout << "\n";
    }

    // Phase 6: Static attribution (globals)
    std::cout << "=== Phase 6: Static Attribution ===\n";
  });

  CLI11_PARSE(app, argc, argv);
  return 0;
}
