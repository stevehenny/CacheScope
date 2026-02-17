#pragma once
#include <optional>
#include <string>
#include <string_view>

#include "Vendor.hpp"
#include "common/Types.hpp"

struct Parser {
  Vendor vendor;

  // Get appropriate memory sampling events for the CPU
  std::string get_default_mem_events() {
    if (vendor.vendor_str == "intel") {
      // Intel PEBS events
      return "mem-loads,mem-stores";
    } else if (vendor.vendor_str == "amd") {
      // AMD IBS op sampling captures memory accesses
      return "ibs_op";
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


  std::optional<int64_t> dwarf_reg_value(const PerfSample& s,
                                         Dwarf_Signed dwarf_regnum) {
#if defined(__x86_64__)
    // x86_64 DWARF register numbers: 6=RBP, 7=RSP
    switch (dwarf_regnum) {
      case 6:
        return s.bp;
      case 7:
        return s.sp;
      default:
        return std::nullopt;
    }
#elif defined(__i386__)
    // x86 DWARF register numbers: 5=EBP, 4=ESP
    switch (dwarf_regnum) {
      case 5:
        return s.bp;
      case 4:
        return s.sp;
      default:
        return std::nullopt;
    }
#else
    (void)s;
    (void)dwarf_regnum;
    return std::nullopt;
#endif
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

};
