#pragma once

#include <libdwarf-2/libdwarf.h>

#include <memory>
#include <string>

#include "DwarfContext.hpp"
#include "common/Registry.hpp"
#include "common/Types.hpp"
using std::string;

class Extractor {
public:
  explicit Extractor(const std::string& binary);
  void create_registry();
  const Registry<std::string, StructInfo>& get_registry() const;

private:
  void process_die_tree(Dwarf_Die die);
  void process_struct_die(Dwarf_Die die);
  TypeInfo* get_or_create_type(Dwarf_Die die);

  DwarfContext context;
  Registry<std::string, StructInfo> registry;

  std::unordered_map<Dwarf_Off, std::unique_ptr<TypeInfo>> types;
  std::vector<std::unique_ptr<FieldInfo>> owned_fields;
};
