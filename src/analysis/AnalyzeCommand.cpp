#include "analysis/AnalyzeCommand.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "common/Types.hpp"
#include "dwarf/DwarfContext.hpp"
#include "dwarf/Extractor.hpp"
#include "report/JsonReport.hpp"
#include "report/TextReport.hpp"
#include "runtime/CacheTopology.hpp"
#include "runtime/FalseSharingAnalysis.hpp"
#include "runtime/SampleStats.hpp"
#include "runtime/Thrashing.hpp"

namespace {

std::optional<std::string> resolve_binary_from_pid(pid_t pid,
                                                   std::string& error) {
  try {
    return std::filesystem::read_symlink(std::format("/proc/{}/exe", pid))
      .string();
  } catch (const std::exception& e) {
    error =
      std::format("Failed to resolve executable for pid {}: {}", pid, e.what());
    return std::nullopt;
  }
}

void render_live_monitor(const std::string& binary, pid_t pid,
                         const std::string& event,
                         const std::vector<PerfSample>& samples,
                         const std::vector<CacheInfo>& caches, bool done,
                         std::chrono::steady_clock::time_point started_at) {
  const auto now = std::chrono::steady_clock::now();
  const auto elapsed =
    std::chrono::duration_cast<std::chrono::seconds>(now - started_at).count();
  const auto stats     = SampleStats::compute(samples);
  const auto hot_lines = FalseSharingAnalysis::find_hot_cache_lines(samples);
  const auto thrashing = ThrashingAnalysis::detect(samples, caches);

  std::cout << "\033[2J\033[H";
  std::cout << "=== Live Monitor ===\n";
  std::cout << std::format("PID: {}  Binary: {}\n", pid, binary);
  std::cout << std::format("Event: {}  Elapsed: {}s\n", event, elapsed);
  std::cout << std::format("Samples: {}  With address: {}  With IP: {}\n",
                           stats.total_samples, stats.samples_with_addr,
                           stats.samples_with_ip);
  std::cout << std::format("Threads: {}  CPUs: {}\n", stats.unique_threads,
                           stats.unique_cpus);

  if (hot_lines.empty()) {
    std::cout << "False-sharing confidence: none yet\n";
  } else {
    std::cout << "Top cache lines:\n";
    for (size_t i = 0; i < std::min<size_t>(hot_lines.size(), 3); ++i) {
      const auto& line           = hot_lines[i];
      std::vector<uint32_t> tids = line.tids;
      std::sort(tids.begin(), tids.end());
      tids.erase(std::unique(tids.begin(), tids.end()), tids.end());

      std::vector<int64_t> offsets;
      offsets.reserve(line.addrs.size());
      for (auto a : line.addrs) offsets.push_back(a - line.base_addr);
      std::sort(offsets.begin(), offsets.end());
      offsets.erase(std::unique(offsets.begin(), offsets.end()), offsets.end());

      const double confidence =
        line.bounce_score * line.private_offset_fraction;
      std::cout << std::format(
        "  #{} 0x{:x}  confidence={:.3f}  samples={}  reads={}  writes={}  "
        "threads={}  offsets={}  shared={}  private={:.2f}  bounce={:.3f}\n",
        i + 1, line.base_addr, confidence, line.sample_count, line.sample_reads,
        line.sample_writes, tids.size(), offsets.size(),
        line.shared_offset_count, line.private_offset_fraction,
        line.bounce_score);
    }
  }

  if (thrashing.empty()) {
    std::cout << "Cache-thrashing confidence: none yet\n";
  } else {
    const auto& top = thrashing.front();
    std::cout << std::format(
      "Top thrashing episode: L{} {} / set {} / CPUs {}  score={:.3f}  "
      "reloads={}/{}  lines={}\n",
      top.cache_level, top.cache_type, top.cache_set, top.shared_cpu_list,
      top.score, top.eviction_reloads, top.evictions, top.unique_lines);
  }

  std::cout << std::format("State: {}\n", done ? "complete" : "running");
  std::cout << std::flush;
}

const DwarfGlobalObject* find_global_for_addr(
  const std::vector<StaticRange>& ranges, int64_t addr, int64_t& base_out) {
  if (ranges.empty()) return nullptr;
  auto it = std::upper_bound(
    ranges.begin(), ranges.end(), addr,
    [](int64_t a, const StaticRange& r) { return a < r.start; });
  if (it == ranges.begin()) return nullptr;
  --it;
  if (addr >= it->start && addr < it->end) {
    base_out = it->start;
    return it->obj;
  }
  return nullptr;
}

bool is_ibs_op_event_spec(std::string_view spec) {
  std::string lower(spec);
  std::transform(
    lower.begin(), lower.end(), lower.begin(),
    [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return lower.find("ibs_op") != std::string::npos;
}

TypeInfo* unwrap_type(TypeInfo* t) {
  while (t &&
         (t->kind == TypeKind::Typedef || t->kind == TypeKind::Const ||
          t->kind == TypeKind::Volatile || t->kind == TypeKind::Reference) &&
         t->pointee) {
    t = t->pointee;
  }
  return t;
}

std::string field_path_for_offset(TypeInfo* type, int64_t off, int depth = 0) {
  type = unwrap_type(type);
  if (!type || off < 0 || depth > 12) return "<unknown>";

  const int64_t type_sz = static_cast<int64_t>(type->size);
  if (type_sz > 0 && off >= type_sz) return "<oob>";

  if (type->kind == TypeKind::Array) {
    TypeInfo* elem = unwrap_type(type->element);
    const int64_t esz =
      (elem && elem->size) ? static_cast<int64_t>(elem->size) : 1;
    const int64_t idx = esz ? (off / esz) : 0;
    const int64_t sub = esz ? (off - idx * esz) : off;
    std::string head  = std::format("[{}]", idx);
    if (elem &&
        (elem->kind == TypeKind::Struct || elem->kind == TypeKind::Class ||
         elem->kind == TypeKind::Array)) {
      return head + "." + field_path_for_offset(elem, sub, depth + 1);
    }
    return head;
  }

  if (type->kind != TypeKind::Struct && type->kind != TypeKind::Class)
    return "<self>";

  FieldInfo* best  = nullptr;
  int64_t best_off = -1;
  for (auto* f : type->fields) {
    if (!f) continue;
    const int64_t fo = static_cast<int64_t>(f->offset);
    const int64_t fs = f->size
                         ? static_cast<int64_t>(f->size)
                         : (f->type ? static_cast<int64_t>(f->type->size) : 0);
    if (fo <= off && (fs == 0 || off < fo + fs)) {
      if (fo >= best_off) {
        best_off = fo;
        best     = f;
      }
    }
  }

  if (!best) return "<unknown>";

  std::string head  = best->name.empty() ? "<anon>" : best->name;
  TypeInfo* ft      = unwrap_type(best->type);
  const int64_t sub = off - static_cast<int64_t>(best->offset);
  if (ft &&
      (ft->kind == TypeKind::Struct || ft->kind == TypeKind::Class ||
       ft->kind == TypeKind::Array) &&
      sub >= 0) {
    auto tail = field_path_for_offset(ft, sub, depth + 1);
    if (tail != "<self>") return head + "." + tail;
  }
  return head;
}

}  // namespace

AnalyzeCommand::AnalyzeCommand(Parser& parser, PerfEventRecorder& recorder)
  : parser_(parser), recorder_(recorder) {}

void AnalyzeCommand::run(const AnalyzeOptions& options) {
  std::string binary                  = options.binary;
  const std::string& default_events   = options.events;
  const int sample_rate               = options.sample_rate;
  const std::string& report_md_path   = options.report_md_path;
  const std::string& report_json_path = options.report_json_path;
  const bool verbose                  = options.verbose;
  const bool monitoring               = options.pid.has_value();
  std::unique_ptr<Extractor> ext;
  std::vector<DwarfStackObject> stack_objects;
  const auto cache_topology = CacheTopology::discover();

  if (monitoring) {
    if (*options.pid <= 0) {
      std::cerr << "Monitor pid must be positive\n";
      return;
    }
    if (binary.empty()) {
      std::string resolve_error;
      auto resolved = resolve_binary_from_pid(*options.pid, resolve_error);
      if (!resolved) {
        std::cerr << resolve_error << "\n";
        return;
      }
      binary = std::move(*resolved);
    }
  }

  if (binary.empty()) {
    std::cerr << "No binary provided\n";
    return;
  }

  // Phase 1: DWARF extraction
  if (monitoring) {
    std::cout
      << "=== Phase 1: DWARF Analysis (skipped in monitor mode) ===\n\n";
  } else {
    std::cout << "=== Phase 1: DWARF Analysis ===\n";
    ext = std::make_unique<Extractor>(binary);
    ext->create_registry();
    if (ext->get_registry().get_map().empty()) {
      std::cerr
        << "No DWARF information found. Ensure the binary is compiled with "
           "-g and has not been stripped.\n";
      return;
    }

    if (verbose) {
      for (const auto& [k, v] : ext->get_registry().get_map()) {
        std::cout << std::format("{}: {} bytes\n", k, v.size);
      }
    }

    stack_objects = ext->get_stack_objects();
    std::cout << std::format("Found {} stack objects\n\n",
                             stack_objects.size());
  }

  // Phase 2: Run perf record
  std::cout << std::format("=== Phase 2: Performance {} ===\n",
                           monitoring ? "Monitoring" : "Recording");
  if (monitoring) {
    std::cout << std::format(
      "Monitoring pid {} ({}) with event '{}' (period={}) via "
      "perf_event_open\n",
      *options.pid, binary, default_events, sample_rate);
  } else {
    std::cout << std::format(
      "Recording {} with event '{}' (period={}) via perf_event_open\n", binary,
      default_events, sample_rate);
  }

  const auto monitor_started_at = std::chrono::steady_clock::now();
  auto monitor_last_render_at   = monitor_started_at;
  bool monitor_first_render     = true;

  auto record =
    monitoring
      ? recorder_.record_pid(
          *options.pid, binary, default_events, sample_rate, verbose,
          [&](const std::vector<PerfSample>& samples, size_t new_samples,
              bool done) {
            const auto now = std::chrono::steady_clock::now();
            if (!monitor_first_render && !done &&
                now - monitor_last_render_at < std::chrono::seconds(1)) {
              return;
            }
            if (monitor_first_render) monitor_first_render = false;
            monitor_last_render_at = now;
            render_live_monitor(binary, *options.pid, default_events, samples,
                                cache_topology, done, monitor_started_at);
            (void)new_samples;
          })
      : recorder_.record_binary(binary, default_events, sample_rate, verbose);
  if (!record.ok()) {
    std::cerr << std::format("Perf recording failed: {}\n", record.error);
    return;
  }

  std::cout << (monitoring ? "Monitoring completed\n\n"
                           : "Recording completed\n\n");

  // Phase 3: Parse samples
  std::cout << "=== Phase 3: Sample Parsing ===\n";

  auto samples = std::move(record.samples);

  // Filter to samples attributed to the target binary (reduces libc/pthread
  // noise).
  const auto bin_name = std::filesystem::path(binary).filename().string();
  auto in_binary      = [&](int64_t ip) {
    for (const auto& r : record.binary_maps) {
      if (ip >= r.start && ip < r.end) return true;
    }
    return false;
  };
  const bool have_maps         = !record.binary_maps.empty();
  const bool allow_unknown_dso = is_ibs_op_event_spec(default_events);
  for (auto& s : samples) {
    if (s.ip != 0 && in_binary(s.ip)) s.dso = binary;
  }
  size_t before = samples.size();
  std::erase_if(samples, [&](const PerfSample& s) {
    if (s.dso.empty()) return have_maps && !allow_unknown_dso;
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
              << "  - Intel: mem-loads, mem-stores\n"
              << "  - AMD: ibs_op\n";
    return;
  }

  // Compute statistics
  auto stats = SampleStats::compute(samples);
  std::cout << stats;

  // Show sample preview
  if (verbose || samples.size() <= 20) {
    std::cout << "\n=== Sample Preview ===\n";
    for (size_t i = 0; i < std::min(samples.size(), size_t{10}); ++i) {
      std::cout << std::format("Sample #{}:\n", i + 1) << samples[i] << "\n";
    }
  }

  // Phase 4: False sharing analysis
  auto hot_lines = FalseSharingAnalysis::find_hot_cache_lines(samples);
  FalseSharingAnalysis::print(hot_lines);
  auto thrashing = ThrashingAnalysis::detect(samples, cache_topology);
  ThrashingAnalysis::print(thrashing, cache_topology);

  if (!report_md_path.empty() || !report_json_path.empty()) {
    Report report =
      Report::from_analysis(binary, default_events, sample_rate, stats,
                            hot_lines, thrashing, cache_topology);

    if (!report_md_path.empty()) {
      std::ofstream out(report_md_path);
      if (!out) {
        std::cerr << std::format("Failed to open Markdown report: {}\n",
                                 report_md_path);
      } else {
        TextReport::write_markdown(out, report);
        if (verbose) {
          std::cout << std::format("Wrote Markdown report: {}\n",
                                   report_md_path);
        }
      }
    }

    if (!report_json_path.empty()) {
      std::ofstream out(report_json_path);
      if (!out) {
        std::cerr << std::format("Failed to open JSON report: {}\n",
                                 report_json_path);
      } else {
        JsonReport::write(out, report);
        if (verbose) {
          std::cout << std::format("Wrote JSON report: {}\n", report_json_path);
        }
      }
    }
  }

  if (monitoring) {
    return;
  }

  // Phase 5: Runtime attribution (stack locals)
  std::cout << "=== Phase 5: Runtime Attribution (Stack) ===\n";

  int64_t load_bias = 0;
  if (record.load_bias) {
    load_bias = *record.load_bias;
    if (verbose) {
      std::cout << std::format("Detected load bias (proc maps): 0x{:x}\n",
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
      dwarf_get_fde_list_eh(frame_ctx->dbg(), &cie_data, &cie_count, &fde_data,
                            &fde_count, &frame_err) == DW_DLV_OK;
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
            "Inferred load bias (FDE vs runtime IP): 0x{:x}\n", inferred_bias);
        }
      }
    }
  }

  auto fn_ranges = ext->get_function_ranges();
  std::ranges::sort(fn_ranges, [](const auto& a, const auto& b) {
    if (a.low_pc != b.low_pc) return a.low_pc < b.low_pc;
    return a.high_pc < b.high_pc;
  });

  auto find_fn = [&](int64_t rel_ip) -> const DwarfFunctionRange* {
    auto it = std::upper_bound(
      fn_ranges.begin(), fn_ranges.end(), rel_ip,
      [](int64_t val, const auto& r) { return val < r.low_pc; });
    if (it == fn_ranges.begin()) return nullptr;
    --it;
    if (rel_ip >= it->low_pc && rel_ip < it->high_pc) return &*it;
    return nullptr;
  };

  for (auto& s : samples) {
    if (s.ip == 0 || s.dso.empty()) continue;
    const DwarfFunctionRange* fn = nullptr;
    if (s.ip >= load_bias) fn = find_fn(s.ip - load_bias);
    if (!fn && inferred_bias && s.ip >= inferred_bias)
      fn = find_fn(s.ip - inferred_bias);
    if (!fn) fn = find_fn(s.ip);
    if (fn) s.symbol = fn->name;
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

    auto fn = std::string(parser_.base_symbol(s.symbol));
    auto it = by_function.find(fn);
    if (it == by_function.end()) continue;

    // Map runtime IP to a DWARF PC for CFI lookup (handle PIE/ASLR via
    // load_bias).
    auto try_cfa = [&](int64_t pc) {
      return parser_.compute_cfa_for_sample(fde_data, s, pc);
    };

    std::optional<int64_t> cfa;
    // Try raw runtime IP first (non-PIE / already-relocated FDEs)
    cfa = try_cfa(s.ip);
    // Then try subtracting known/perf-inferred biases.
    if (!cfa && load_bias && s.ip >= load_bias) cfa = try_cfa(s.ip - load_bias);
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

  // NOTE: Keep FDE/CIE data alive for Phase 6/7 (we also need CFA there).

  if (verbose) {
    std::cout << std::format("CFA computed: {}  CFA miss: {}\n", cfa_ok,
                             cfa_miss);
  }

  std::cout << std::format("Stack-attributed samples: {} / {}\n\n", stack_hits,
                           samples.size());

  if (verbose && !var_hits.empty()) {
    std::vector<std::pair<std::string, size_t>> ranked(var_hits.begin(),
                                                       var_hits.end());
    std::ranges::sort(ranked, [](const auto& a, const auto& b) {
      if (a.second != b.second) return a.second > b.second;
      return a.first < b.first;
    });

    std::cout << "Top stack variables by hits:\n";
    for (size_t i = 0; i < std::min<size_t>(ranked.size(), 10); ++i) {
      std::cout << std::format("  {}: {}\n", ranked[i].first, ranked[i].second);
    }
    std::cout << "\n";
  }

  // Phase 6: Static attribution (globals) + bridge to struct fields
  std::cout << "=== Phase 6: Static Attribution ===\n";

  const auto& globals = ext->get_global_objects();
  std::vector<StaticRange> global_ranges;
  global_ranges.reserve(globals.size());

  std::unordered_set<int64_t> global_starts;
  global_starts.reserve(globals.size() * 2);

  for (const auto& g : globals) {
    if (g.addr == 0 || g.size <= 0) continue;

    auto add_range = [&](int64_t start) {
      if (start == 0) return;
      if (!global_starts.insert(start).second) return;
      global_ranges.push_back(StaticRange{start, start + g.size, &g});
    };

    // DWARF globals may be link-time VMAs; perf samples are runtime
    // (PIE/ASLR). Add multiple hypotheses and let the address match decide.
    add_range(g.addr);
    if (load_bias) add_range(g.addr + load_bias);
    if (inferred_bias) add_range(g.addr + inferred_bias);
  }
  std::sort(global_ranges.begin(), global_ranges.end(),
            [](const auto& a, const auto& b) { return a.start < b.start; });
  // Only attribute samples from our binary.
  std::vector<const PerfSample*> bin_samples;
  bin_samples.reserve(samples.size());
  for (const auto& s : samples) {
    if (s.addr == 0) continue;
    if (!s.dso.empty() && (s.dso.find(bin_name) == std::string::npos &&
                           s.dso.find(binary) == std::string::npos))
      continue;
    bin_samples.push_back(&s);
  }

  // Resolve each sample to (Global/Stack var + byte offset + field path).
  std::unordered_map<int64_t, std::vector<ResolvedVariable>> by_cacheline;

  auto add_resolved = [&](const PerfSample& s, const ResolvedVariable& rv) {
    const int64_t base =
      (s.addr / static_cast<int64_t>(FalseSharingAnalysis::CACHE_LINE_SIZE)) *
      static_cast<int64_t>(FalseSharingAnalysis::CACHE_LINE_SIZE);
    auto& resolved = by_cacheline[base];
    const bool already_present =
      std::ranges::any_of(resolved, [&](const ResolvedVariable& existing) {
        return existing.kind == rv.kind && existing.name == rv.name &&
               existing.address == rv.address && existing.size == rv.size;
      });
    if (!already_present) resolved.push_back(rv);
  };

  // Prebuild stack lookup by function for reuse.
  std::unordered_map<std::string, std::vector<const DwarfStackObject*>>
    by_function2;
  by_function2.reserve(stack_objects.size());
  for (const auto& o : stack_objects) {
    by_function2[o.function].push_back(&o);
  }

  for (const auto* ps : bin_samples) {
    const auto& s = *ps;

    // 1) Try global/static variable
    int64_t gbase = 0;
    if (auto* g = find_global_for_addr(global_ranges, s.addr, gbase)) {
      ResolvedVariable rv;
      rv.name      = g->name;
      rv.type_name = g->type ? g->type->name : "<unknown>";
      rv.address   = gbase;
      rv.size      = static_cast<size_t>(g->size);
      rv.offset    = s.addr - gbase;
      rv.kind      = ResolvedVariable::Kind::Global;
      add_resolved(s, rv);
      continue;
    }

    // 2) Try stack local variable (requires CFI)
    if (!have_frames || s.ip == 0 || s.sp == 0 || s.symbol.empty()) continue;

    auto fn = std::string(parser_.base_symbol(s.symbol));
    auto it = by_function2.find(fn);
    if (it == by_function2.end()) continue;

    auto try_cfa = [&](int64_t pc) {
      return parser_.compute_cfa_for_sample(fde_data, s, pc);
    };

    std::optional<int64_t> cfa;
    cfa = try_cfa(s.ip);
    if (!cfa && load_bias && s.ip >= load_bias) cfa = try_cfa(s.ip - load_bias);
    if (!cfa && inferred_bias && s.ip >= inferred_bias)
      cfa = try_cfa(s.ip - inferred_bias);
    if (!cfa) continue;

    for (const auto* obj : it->second) {
      const int64_t cfa_i64 = static_cast<int64_t>(*cfa);
      const int64_t loc     = cfa_i64 + obj->frame_offset;
      if (loc < 0) continue;
      const int64_t var_addr = static_cast<int64_t>(loc);
      const int64_t var_end  = var_addr + obj->size;
      if (s.addr < var_addr || s.addr >= var_end) continue;

      ResolvedVariable rv;
      rv.name      = obj->function + "::" + obj->name;
      rv.type_name = obj->type ? obj->type->name : "<unknown>";
      rv.address   = var_addr;
      rv.size      = static_cast<size_t>(obj->size);
      rv.offset    = s.addr - var_addr;
      rv.kind      = ResolvedVariable::Kind::Stack;
      add_resolved(s, rv);
      break;
    }
  }

  // Quick type lookup maps (avoid repeated linear scans)
  std::unordered_map<std::string, TypeInfo*> global_type;
  global_type.reserve(globals.size());
  for (const auto& g : globals) global_type[g.name] = g.type;

  std::unordered_map<std::string, TypeInfo*> stack_type;
  stack_type.reserve(stack_objects.size());
  for (const auto& o : stack_objects)
    stack_type[o.function + "::" + o.name] = o.type;

  // Print per-hot-cache-line per-thread hotspots (variable + field path)
  std::cout << "\n=== Phase 7: Field Attribution (Bridge) ===\n\n";

  std::cout << std::format("Resolved variable cache lines: {}\n\n",
                           by_cacheline.size());

  using AttributionCounts =
    std::unordered_map<uint32_t, std::unordered_map<std::string, size_t>>;

  auto cache_line_base = [](int64_t address) {
    return
      (address /
       static_cast<int64_t>(FalseSharingAnalysis::CACHE_LINE_SIZE)) *
      static_cast<int64_t>(FalseSharingAnalysis::CACHE_LINE_SIZE);
  };

  auto add_attribution = [&](AttributionCounts& counts, const PerfSample& s,
                             const std::vector<ResolvedVariable>& resolved) {
    for (const auto& rv : resolved) {
      const int64_t start = rv.address;
      const int64_t end   = rv.address + static_cast<int64_t>(rv.size);
      if (s.addr < start || s.addr >= end) continue;

      TypeInfo* type = nullptr;
      if (rv.kind == ResolvedVariable::Kind::Global) {
        if (auto it = global_type.find(rv.name); it != global_type.end()) {
          type = it->second;
        }
      } else if (rv.kind == ResolvedVariable::Kind::Stack) {
        if (auto it = stack_type.find(rv.name); it != stack_type.end()) {
          type = it->second;
        }
      }

      const auto offset = s.addr - rv.address;
      const auto path   = field_path_for_offset(type, offset);
      const auto label =
        std::format("{} +0x{:x} ({})", rv.name, offset, path);
      ++counts[s.tid][label];
      return;
    }
  };

  auto print_attributions = [](AttributionCounts& counts,
                               std::string_view cause) {
    std::vector<uint32_t> tids;
    tids.reserve(counts.size());
    for (const auto& [tid, _] : counts) tids.push_back(tid);
    std::ranges::sort(tids);

    for (uint32_t tid : tids) {
      auto& per_variable = counts.at(tid);
      std::vector<std::pair<std::string, size_t>> ranked(per_variable.begin(),
                                                         per_variable.end());
      std::ranges::sort(ranked, [](const auto& a, const auto& b) {
        if (a.second != b.second) return a.second > b.second;
        return a.first < b.first;
      });

      std::cout << std::format("  TID {}:\n", tid);
      for (size_t i = 0; i < std::min<size_t>(ranked.size(), 5); ++i) {
        std::cout << std::format(
          "    {}  ({} samples; cause: {})\n", ranked[i].first,
          ranked[i].second, cause);
      }
    }
  };

  size_t attributed_false_sharing_lines = 0;
  for (size_t i = 0; i < std::min(hot_lines.size(), size_t{10}); ++i) {
    const auto& line = hot_lines[i];
    auto it          = by_cacheline.find(line.base_addr);
    if (it == by_cacheline.end() || it->second.empty()) continue;

    AttributionCounts counts;
    for (const auto* ps : bin_samples) {
      const auto& s = *ps;
      if (cache_line_base(s.addr) != line.base_addr) continue;
      add_attribution(counts, s, it->second);
    }
    if (counts.empty()) continue;

    ++attributed_false_sharing_lines;
    std::cout << std::format(
      "False sharing - hot line #{} 0x{:x}:\n", i + 1, line.base_addr);
    print_attributions(counts, "false sharing");
    std::cout << "\n";
  }

  if (attributed_false_sharing_lines == 0) {
    std::cout << (hot_lines.empty()
                    ? "No false sharing detected; no variables to attribute.\n\n"
                    : "No resolved variables for detected false sharing.\n\n");
  }

  size_t attributed_thrashing_events = 0;
  for (size_t i = 0; i < std::min(thrashing.size(), size_t{10}); ++i) {
    const auto& event = thrashing[i];
    const auto cache_it = std::ranges::find_if(
      cache_topology, [&](const CacheInfo& cache) {
        return cache.level == event.cache_level && cache.id == event.cache_id &&
               cache.type == event.cache_type &&
               cache.shared_cpu_list == event.shared_cpu_list;
      });
    if (cache_it == cache_topology.end()) continue;

    const auto& cache = *cache_it;
    AttributionCounts counts;
    for (const auto* ps : bin_samples) {
      const auto& s = *ps;
      if (s.event_type == SampleType::PAGE_FAULT ||
          !cache.contains_cpu(s.cpu)) {
        continue;
      }

      const bool event_has_time =
        event.start_time_ns != 0 || event.end_time_ns != 0;
      if (event_has_time &&
          (s.time_stamp < event.start_time_ns ||
           s.time_stamp > event.end_time_ns)) {
        continue;
      }

      int64_t set_address = s.addr;
      if (event.address_basis == AddressBasis::Physical) {
        if (s.phys_addr <= 0) continue;
        set_address = s.phys_addr;
      }
      if (set_address <= 0 || cache.line_size == 0 || cache.sets == 0) {
        continue;
      }

      const auto line = static_cast<uint64_t>(set_address) / cache.line_size;
      if (line % cache.sets != event.cache_set) continue;

      auto resolved_it = by_cacheline.find(cache_line_base(s.addr));
      if (resolved_it == by_cacheline.end()) continue;
      add_attribution(counts, s, resolved_it->second);
    }
    if (counts.empty()) continue;

    ++attributed_thrashing_events;
    std::cout << std::format(
      "Cache thrashing - event #{} L{} {} id={} / set {} / CPUs {}:\n",
      i + 1, event.cache_level, event.cache_type, event.cache_id,
      event.cache_set, event.shared_cpu_list);
    print_attributions(counts, "cache thrashing");
    std::cout << "\n";
  }

  if (attributed_thrashing_events == 0) {
    std::cout << (thrashing.empty()
                    ? "No cache thrashing detected; no variables to attribute.\n\n"
                    : "No resolved variables for detected cache thrashing.\n\n");
  }

  if (have_frames && frame_ctx) {
    dwarf_fde_cie_list_dealloc(frame_ctx->dbg(), cie_data, cie_count, fde_data,
                               fde_count);
  }
}
