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
      if (line.find("GenuineIntel") != std::string::npos) return "intel";
      if (line.find("AuthenticAMD") != std::string::npos) return "amd";
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

static inline std::string_view base_symbol(std::string_view sym) {
  sym = trim(sym);
  // perf often prints "foo+0xNN"; DWARF subprogram DIE names are just "foo".
  auto plus = sym.find('+');
  if (plus != std::string_view::npos) sym = sym.substr(0, plus);
  // perf can also print demangled signatures like "foo(int)".
  auto paren = sym.find('(');
  if (paren != std::string_view::npos) sym = sym.substr(0, paren);
  return trim(sym);
}

static std::optional<uint64_t> parse_hex_u64(std::string_view sv) {
  sv = trim(sv);
  if (sv.starts_with("0x")) sv.remove_prefix(2);
  if (sv.empty()) return std::nullopt;
  uint64_t v = 0;
  v          = std::stoull(std::string(sv), nullptr, 16);
  return v;
}

static inline std::string lower_copy(std::string_view sv) {
  std::string out;
  out.reserve(sv.size());
  for (char c : sv)
    out.push_back(
      static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  return out;
}

static void parse_user_regs_from_uregs_tokens(
  const std::vector<std::string_view>& toks, size_t start_idx, PerfSample& s) {
  auto try_parse_named_reg = [&](std::string_view tok, std::string_view name,
                                 uint64_t& out) -> bool {
    tok = trim(tok);
    while (!tok.empty() && (tok.back() == ',' || tok.back() == ';'))
      tok.remove_suffix(1);

    auto lt = lower_copy(tok);
    auto ln = std::string(name);

    // Accept forms: "sp:", "sp:0x...", "sp=0x..." (and also "rbp:" for bp).
    if (lt == ln + ":") {
      return false;  // value is in next token
    }

    auto starts_with = [&](const std::string& prefix) {
      return lt.rfind(prefix, 0) == 0;
    };

    std::string_view val;
    if (starts_with(ln + ":")) {
      val = tok.substr(ln.size() + 1);
    } else if (starts_with(ln + "=")) {
      val = tok.substr(ln.size() + 1);
    } else {
      return false;
    }

    if (auto v = parse_hex_u64(val)) {
      out = *v;
      return true;
    }
    return false;
  };

  for (size_t i = start_idx; i < toks.size(); ++i) {
    auto tok = trim(toks[i]);
    auto lt  = lower_copy(tok);

    // "SP:" or "sp:" with value in next token
    if (lt == "sp:" && i + 1 < toks.size()) {
      if (auto v = parse_hex_u64(toks[i + 1])) s.sp = *v;
      continue;
    }

    // "BP:"/"RBP:" or "bp:" with value in next token
    if ((lt == "bp:" || lt == "rbp:") && i + 1 < toks.size()) {
      if (auto v = parse_hex_u64(toks[i + 1])) s.bp = *v;
      continue;
    }

    (void)try_parse_named_reg(tok, "sp", s.sp);
    (void)try_parse_named_reg(tok, "bp", s.bp);
    (void)try_parse_named_reg(tok, "rbp", s.bp);
  }
}

static std::optional<uint64_t> get_load_bias_from_perf_mmaps(
  const std::string& perf_data_file, const std::string& binary_path,
  uint32_t pid) {
  const auto bin_name = std::filesystem::path(binary_path).filename().string();
  std::string cmd     = std::format(
    "perf script --show-mmap-events --pid {} -i {} 2>/dev/null | head -n 5000",
    pid, perf_data_file);

  FILE* pipe = popen(cmd.c_str(), "r");
  if (!pipe) return std::nullopt;

  std::optional<uint64_t> any_start;
  std::array<char, 4096> buffer{};
  int count{};
  bool found = false;

  while (fgets(buffer.data(), buffer.size(), pipe)) {
    ++count;
    std::string line(buffer.data());
    if (line.find("PERF_RECORD_MMAP") == std::string::npos) continue;
    if (line.find(binary_path) == std::string::npos &&
        line.find(bin_name) == std::string::npos)
      continue;

    auto lb = line.find('[');
    auto rb = line.find(']', lb);
    if (lb == std::string::npos || rb == std::string::npos) continue;

    auto inside    = std::string_view(line).substr(lb + 1, rb - lb - 1);
    auto start_end = inside.find('(');
    if (start_end == std::string::npos) continue;
    auto at_pos = inside.find('@');
    if (at_pos == std::string::npos) continue;

    auto start_sv = trim(inside.substr(0, start_end));
    auto pgoff_sv = trim(inside.substr(at_pos + 1));
    auto sp       = pgoff_sv.find(' ');
    if (sp != std::string_view::npos) pgoff_sv = pgoff_sv.substr(0, sp);

    auto start = parse_hex_u64(start_sv);
    auto pgoff = parse_hex_u64(pgoff_sv);
    if (!start || !pgoff) continue;

    any_start = *start;
    if (*pgoff == 0) {
      found = true;
      break;
    }
  }

  int status = pclose(pipe);
  (void)status;  // Optionally handle status

  if (found) return any_start;
  return any_start;
}

static std::optional<uint64_t> dwarf_reg_value(const PerfSample& s,
                                               Dwarf_Signed dwarf_regnum) {
  // x86_64 DWARF register numbers: 6=RBP, 7=RSP
  switch (dwarf_regnum) {
    case 6:
      return s.bp;
    case 7:
      return s.sp;
    default:
      return std::nullopt;
  }
}

static std::optional<uint64_t> compute_cfa_for_sample(Dwarf_Fde* fde_data,
                                                      const PerfSample& s,
                                                      uint64_t pc_query) {
  if (!fde_data) return std::nullopt;

  Dwarf_Fde fde   = nullptr;
  Dwarf_Addr lopc = 0, hipc = 0;
  Dwarf_Error err = nullptr;
  if (dwarf_get_fde_at_pc(fde_data, pc_query, &fde, &lopc, &hipc, &err) !=
      DW_DLV_OK) {
    return std::nullopt;
  }

  Dwarf_Small value_type       = 0;
  Dwarf_Signed offset_relevant = 0;
  Dwarf_Signed regnum          = 0;
  Dwarf_Signed offset_or_len   = 0;
  Dwarf_Ptr block_ptr          = nullptr;
  Dwarf_Addr row_pc            = 0;

  if (dwarf_get_fde_info_for_cfa_reg3(fde, pc_query, &value_type,
                                      &offset_relevant, &regnum, &offset_or_len,
                                      &block_ptr, &row_pc, &err) != DW_DLV_OK) {
    return std::nullopt;
  }

  if (value_type != DW_EXPR_OFFSET && value_type != DW_EXPR_VAL_OFFSET) {
    // CFA expressions (DW_CFA_def_cfa_expression) are possible but uncommon.
    return std::nullopt;
  }

  auto base = dwarf_reg_value(s, regnum);
  if (!base || *base == 0) return std::nullopt;

  const int64_t base_i64 = static_cast<int64_t>(*base);
  const int64_t cfa_i64  = base_i64 + static_cast<int64_t>(offset_or_len);
  if (cfa_i64 < 0) return std::nullopt;
  return static_cast<uint64_t>(cfa_i64);
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
      auto dot      = tt.find('.');
      uint64_t secs = 0, nsecs = 0;
      try {
        secs      = std::stoull(std::string(tt.substr(0, dot)));
        auto frac = std::string(tt.substr(dot + 1));
        if (frac.size() > 9) frac.resize(9);
        while (frac.size() < 9) frac.push_back('0');
        nsecs = std::stoull(frac);
      } catch (...) {
        secs  = 0;
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

  if (event_str == "mem-stores:pp" ||
      event_str.find("store") != std::string::npos)
    s.event_type = SampleType::CACHE_STORE;
  else if (event_str == "mem-loads:pp" ||
           event_str.find("load") != std::string::npos)
    s.event_type = SampleType::CACHE_LOAD;
  else
    s.event_type = SampleType::CACHE_LOAD;  // Generic / IBS: treat as access

  // perf prints two addresses for these events; for memory-access sampling
  // (ibs_op, mem-loads/stores) the first is typically the accessed address and
  // the second is the instruction pointer.
  s.addr = std::stoull(std::string(toks[idx]), nullptr, 16);
  idx++;
  s.ip = std::stoull(std::string(toks[idx]), nullptr, 16);
  idx++;

  // Remaining tokens contain sym and dso, but sym can include whitespace (e.g.
  // "thread_method(PaddedCounter*, int)"). dso is reliably a single token like
  // "(/path/to/bin)" or "([kernel.kallsyms])".
  size_t dso_idx = toks.size();
  for (size_t i = idx; i < toks.size(); ++i) {
    auto t = trim(toks[i]);
    if (t.size() >= 2 && t.front() == '(' && t.back() == ')') {
      dso_idx = i;
      break;
    }
  }

  if (dso_idx != toks.size()) {
    // symbol is everything between idx and dso_idx
    std::string sym;
    for (size_t i = idx; i < dso_idx; ++i) {
      if (!sym.empty()) sym.push_back(' ');
      sym += std::string(toks[i]);
    }
    s.symbol = sym;

    auto dso_tok = trim(toks[dso_idx]);
    dso_tok.remove_prefix(1);
    dso_tok.remove_suffix(1);
    s.dso = std::string(dso_tok);

    idx = dso_idx + 1;
  } else {
    // Fallback: old behavior
    if (idx < toks.size()) {
      s.symbol = std::string(toks[idx]);
      idx++;
    }
    if (idx < toks.size()) {
      s.dso = std::string(toks[idx]);
      idx++;
    }
  }

  // Optional sampled user registers (we record SP/BP via perf record
  // --user-regs=sp,bp). perf formatting varies across versions.
  parse_user_regs_from_uregs_tokens(toks, idx, s);

  return s;
}

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

// Parse perf script output
std::vector<PerfSample> parse_perf_data(const std::string& perf_data_file) {
  std::string cmd = std::format(
    "perf script -i {} -F tid,pid,cpu,time,event,ip,addr,sym,dso,uregs "
    "2>/dev/null",
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
    "perf script -i {} -F tid,pid,cpu,time,ip,addr,sym,dso,uregs 2>/dev/null",
    perf_data_file);

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

    uint64_t load_bias = 0;
    if (auto lb =
          get_load_bias_from_perf_mmaps(output_file, binary, samples[0].pid)) {
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

    uint64_t inferred_bias = 0;
    if (have_frames && fde_data && fde_count > 0) {
      uint64_t min_fde_lopc = 0;
      bool have_any = false;
      for (Dwarf_Signed i = 0; i < fde_count; ++i) {
        Dwarf_Fde fde = fde_data[i];
        if (!fde) continue;

        Dwarf_Addr lopc = 0;
        Dwarf_Unsigned len = 0;
        Dwarf_Ptr fde_bytes = nullptr;
        Dwarf_Unsigned fde_bytes_len = 0;
        Dwarf_Off cie_offset = 0;
        Dwarf_Signed cie_index = 0;
        Dwarf_Off fde_offset = 0;
        Dwarf_Error e = nullptr;

        if (dwarf_get_fde_range(fde, &lopc, &len, &fde_bytes, &fde_bytes_len,
                                &cie_offset, &cie_index, &fde_offset, &e) !=
            DW_DLV_OK)
          continue;

        if (!have_any) {
          min_fde_lopc = lopc;
          have_any = true;
        } else {
          min_fde_lopc = std::min<uint64_t>(min_fde_lopc, lopc);
        }
      }

      if (have_any) {
        uint64_t min_ip = 0;
        bool have_ip = false;
        for (const auto& smp : samples) {
          if (smp.ip == 0 || smp.dso.empty()) continue;
          if (smp.dso.find(bin_name) == std::string::npos &&
              smp.dso.find(binary) == std::string::npos)
            continue;
          if (!have_ip) {
            min_ip = smp.ip;
            have_ip = true;
          } else {
            min_ip = std::min(min_ip, smp.ip);
          }
        }

        if (have_ip && min_ip > min_fde_lopc) {
          inferred_bias = min_ip - min_fde_lopc;
          if (verbose) {
            std::cout << std::format(
              "Inferred load bias (FDE vs runtime IP): 0x{:x}\n", inferred_bias);
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

    size_t cfa_ok = 0;
    size_t cfa_miss = 0;

    for (const auto& s : samples) {
      if (!have_frames || s.ip == 0 || s.sp == 0 || s.addr == 0 || s.symbol.empty())
        continue;

      // Only do stack attribution when IP is from the target binary.
      if (s.dso.empty() ||
          (s.dso.find(bin_name) == std::string::npos &&
           s.dso.find(binary) == std::string::npos))
        continue;

      auto fn = std::string(base_symbol(s.symbol));
      auto it = by_function.find(fn);
      if (it == by_function.end()) continue;

      // Map runtime IP to a DWARF PC for CFI lookup (handle PIE/ASLR via
      // load_bias).
      auto try_cfa = [&](uint64_t pc) { return compute_cfa_for_sample(fde_data, s, pc); };

      std::optional<uint64_t> cfa;
      // Try raw runtime IP first (non-PIE / already-relocated FDEs)
      cfa = try_cfa(s.ip);
      // Then try subtracting known/perf-inferred biases.
      if (!cfa && load_bias && s.ip >= load_bias) cfa = try_cfa(s.ip - load_bias);
      if (!cfa && inferred_bias && s.ip >= inferred_bias) cfa = try_cfa(s.ip - inferred_bias);

      if (!cfa) {
        ++cfa_miss;
        continue;
      }
      ++cfa_ok;

      for (const auto* obj : it->second) {
        const int64_t cfa_i64 = static_cast<int64_t>(*cfa);
        const int64_t loc     = cfa_i64 + obj->frame_offset;
        if (loc < 0) continue;
        const uint64_t var_addr = static_cast<uint64_t>(loc);
        const uint64_t var_end  = var_addr + obj->size;

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
      std::cout << std::format("CFA computed: {}  CFA miss: {}\n", cfa_ok, cfa_miss);
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
