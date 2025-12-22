
#include "dwarf/DwarfContext.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <stdexcept>

DwarfContext::DwarfContext(const std::string& bin) {
  _fd = open(bin.c_str(), O_RDONLY);
  if (_fd < 0) throw std::runtime_error("ERROR: Failed to open binary");

  if (dwarf_init_b(_fd, DW_GROUPNUMBER_ANY, nullptr, nullptr, &_dbg, &_err) !=
      DW_DLV_OK) {
    close(_fd);
    throw std::runtime_error("ERROR: dwarf_init_b failed");
  }
}

DwarfContext::~DwarfContext() {
  if (_dbg) dwarf_finish(_dbg);
  if (_fd >= 0) close(_fd);
}

Dwarf_Debug DwarfContext::dbg() const { return _dbg; }
