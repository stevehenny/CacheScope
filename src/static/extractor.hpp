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
  const StructRegistry& get_registry() const;

private:
  int bin_fd;
  StructRegistry registry;
  DwarfContext context;
};
