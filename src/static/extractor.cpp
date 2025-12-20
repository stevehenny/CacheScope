#include "extractor.hpp"

#include <fcntl.h>
#include <libdwarf-2/dwarf.h>
#include <libdwarf-2/libdwarf.h>
#include <libelf.h>
#include <unistd.h>

#include <iostream>
#include <sstream>
#include <stdexcept>

Extractor::Extractor(const string& bin) {
  bin_fd = open(bin.c_str(), O_RDONLY);
  if (bin_fd < 0) throw std::runtime_error("ERROR: Failed to open file\n");
}

Extractor::~Extractor() { close(bin_fd); }

void Extractor::create_registry() {
  Dwarf_Debug dbg = nullptr;
  Dwarf_Error err;

  if (dwarf_init_b(bin_fd, DW_GROUPNUMBER_ANY, nullptr, nullptr, &dbg, &err))
    ;
}

const StructRegistry& Extractor::get_registry() const { return registry; }
