
#include "extractor.hpp"

#include <fcntl.h>
#include <libdwarf-2/dwarf.h>
#include <libdwarf-2/libdwarf.h>
#include <libelf.h>
#include <unistd.h>

#include "DwarfContext.hpp"

Extractor::Extractor(const string& bin) : context(DwarfContext{bin}) {}

Extractor::~Extractor() = default;

void Extractor::create_registry() {
  Dwarf_Debug dbg = context.dbg();

  // Next steps go here:
  // - iterate compilation units
  // - traverse DIE tree
  // - extract structs and create registry
}

const StructRegistry& Extractor::get_registry() const { return registry; }
