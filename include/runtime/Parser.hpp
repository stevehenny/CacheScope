#pragma once
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

#include "Vendor.hpp"
#include "common/Types.hpp"
#include "runtime/PipeStream.hpp"

struct Parser {
  Vendor vendor;

  // Get appropriate memory sampling events for the CPU
  std::string get_default_mem_events() {
    if (vendor.vendor_str == "intel") {
      // Intel PEBS events
      return "mem-loads:pp,mem-stores:pp";
    } else if (vendor.vendor_str == "amd") {
      // AMD IBS op sampling captures memory accesses
      return "ibs_op//";
    }
    // Fallback to generic events that should work on most x86
    return "cpu-cycles";
  }

  inline std::string_view trim(std::string_view sv) {
    auto b = sv.find_first_not_of(" \t\n");
    auto e = sv.find_last_not_of(" \t\n");
    if (b == std::string_view::npos) return {};
    return sv.substr(b, e - b + 1);
  }

  inline std::string_view base_symbol(std::string_view sym) {
    sym = trim(sym);
    // perf often prints "foo+0xNN"; DWARF subprogram DIE names are just "foo".
    auto plus = sym.find('+');
    if (plus != std::string_view::npos) sym = sym.substr(0, plus);
    // perf can also print demangled signatures like "foo(int)".
    auto paren = sym.find('(');
    if (paren != std::string_view::npos) sym = sym.substr(0, paren);
    return trim(sym);
  }

  std::optional<int64_t> parse_hex_u64(std::string_view sv) {
    sv = trim(sv);
    if (sv.starts_with("0x")) sv.remove_prefix(2);
    if (sv.empty()) return std::nullopt;
    int64_t v = 0;
    v         = std::stoull(std::string(sv), nullptr, 16);
    return v;
  }

  inline std::string lower_copy(std::string_view sv) {
    std::string out;
    out.reserve(sv.size());
    for (char c : sv)
      out.push_back(
        static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return out;
  }

  void parse_user_regs_from_uregs_tokens(
    const std::vector<std::string_view>& toks, size_t start_idx,
    PerfSample& s) {
    auto try_parse_named_reg = [&](std::string_view tok, std::string_view name,
                                   int64_t& out) -> bool {
      tok = trim(tok);
      while (!tok.empty() && (tok.back() == ',' || tok.back() == ';'))
        tok.remove_suffix(1);

      auto lt = lower_copy(tok);
      auto ln = std::string(name);

      // Accept forms: "sp:", "sp:0x...", "sp=0x..." (and also "rbp:" for bp).
      if (lt == ln + ":") {
        return false;
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

  std::optional<int64_t> get_load_bias_from_perf_mmaps(
    const std::string& perf_data_file, const std::string& binary_path,
    uint32_t /*pid*/) {
    const auto bin_name =
      std::filesystem::path(binary_path).filename().string();
    // Don't rely on perf's --pid semantics (pid vs tid varies by perf version);
    // just scan mmap events and match by pathname.
    std::string cmd = std::format(
      "perf script --show-mmap-events -i {} 2>/dev/null | head -n 20000",
      perf_data_file);

    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return std::nullopt;

    std::optional<int64_t> any_start;
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

  std::optional<int64_t> dwarf_reg_value(const PerfSample& s,
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

  std::optional<int64_t> compute_cfa_for_sample(Dwarf_Fde* fde_data,
                                                const PerfSample& s,
                                                int64_t pc_query) {
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

    if (dwarf_get_fde_info_for_cfa_reg3(
          fde, pc_query, &value_type, &offset_relevant, &regnum, &offset_or_len,
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
    return static_cast<int64_t>(cfa_i64);
  }

  // Parse a single perf script line
  std::optional<PerfSample> parse_perf_line(std::string_view line) {
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
    // tid/pid [cpu] [time] event: addr ip sym... (dso)
    if (toks.size() < 5) return std::nullopt;

    size_t idx = 0;

    // Handle optional comm name (non pid/tid token)
    if (toks[0].find('/') == std::string_view::npos) {
      idx++;
      if (toks.size() - idx < 5) return std::nullopt;
    }

    // pid/tid (perf prints "pid/tid" in the first column)
    auto slash = toks[idx].find('/');
    if (slash == std::string_view::npos) return std::nullopt;

    s.pid = std::stoul(std::string(toks[idx].substr(0, slash)));
    s.tid = std::stoul(std::string(toks[idx].substr(slash + 1)));
    idx++;

    // [cpu]
    if (toks[idx].front() != '[' || toks[idx].back() != ']')
      return std::nullopt;

    s.cpu = std::stoul(std::string(toks[idx].substr(1, toks[idx].size() - 2)));
    idx++;

    // Optional time token (when perf script -F includes time)
    // Usually formatted like "12345.678901" or "12345.678901:".
    if (idx < toks.size()) {
      auto tt = toks[idx];
      if (!tt.empty() && tt.back() == ':') tt = tt.substr(0, tt.size() - 1);

      if (tt.find_first_not_of("0123456789.") == std::string_view::npos &&
          tt.find('.') != std::string_view::npos) {
        auto dot     = tt.find('.');
        int64_t secs = 0, nsecs = 0;
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
    // (ibs_op, mem-loads/stores) the first is typically the accessed address
    // and the second is the instruction pointer.
    s.addr = std::stoull(std::string(toks[idx]), nullptr, 16);
    idx++;
    s.ip = std::stoull(std::string(toks[idx]), nullptr, 16);
    idx++;

    // Remaining tokens contain sym and dso, but sym can include whitespace
    // (e.g. "thread_method(PaddedCounter*, int)"). dso is reliably a single
    // token like
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
    return lines | std::views::transform([this](const auto& line) {
             return this->parse_perf_line(line);
           }) |
           std::views::filter([](const auto& opt) { return opt.has_value(); }) |
           std::views::transform([](auto&& opt) { return std::move(*opt); });
  }
};
