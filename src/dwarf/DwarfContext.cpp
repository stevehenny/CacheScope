#include "dwarf/DwarfContext.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <stdexcept>

DwarfContext::DwarfContext(const std::string& bin) {
  _fd = open(bin.c_str(), O_RDONLY);
  if (_fd < 0) throw std::runtime_error("ERROR: Failed to open binary");

#ifdef HAVE_DWARF_INIT_B
  if (dwarf_init_b(_fd, DW_GROUPNUMBER_ANY, nullptr, nullptr, &_dbg, &_err) !=
      DW_DLV_OK) {
#else
  if (dwarf_init(_fd, DW_DLC_READ, nullptr, nullptr, &_dbg, &_err) !=
      DW_DLV_OK) {
#endif
    close(_fd);
    throw std::runtime_error("ERROR: dwarf_init failed");
  }
}

DwarfContext::~DwarfContext() {
  if (_dbg) {
#ifdef HAVE_DWARF_FINISH_1ARG
    dwarf_finish(_dbg);
#else
    dwarf_finish(_dbg, &_err);
#endif
  }

  if (_fd >= 0) close(_fd);
}

Dwarf_Debug DwarfContext::dbg() const { return _dbg; }
