#pragma once

#include <libdwarf-2/libdwarf.h>

#include <string>

class DwarfContext {
public:
  explicit DwarfContext(const std::string& bin);
  ~DwarfContext();

  Dwarf_Debug dbg() const;

private:
  int _fd{-1};
  Dwarf_Debug _dbg{nullptr};
  Dwarf_Error _err{nullptr};
};
