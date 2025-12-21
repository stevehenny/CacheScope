#pragma once

#include <libdwarf-2/libdwarf.h>

#include <string>

#include "DwarfContext.hpp"
#include "registry.hpp"

using std::string;

class Extractor {
public:
  Extractor(const string& bin);
  ~Extractor();
  void create_registry();
  void process_die_tree(Dwarf_Die cu_die);
  void process_die(Dwarf_Die cu_die);
  const StructRegistry& get_registry() const;

private:
  int bin_fd;
  StructRegistry registry;
  DwarfContext context;
};
