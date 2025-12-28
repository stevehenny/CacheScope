#pragma once

#include <libdwarf-2/dwarf.h>
#include <libdwarf-2/libdwarf.h>

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "DwarfContext.hpp"
#include "common/Registry.hpp"
#include "common/Types.hpp"

class Extractor {
public:
  explicit Extractor(const std::string& binary);

  void create_registry();

  const Registry<std::string, StructInfo>& get_registry() const;
  const std::unordered_map<Dwarf_Off, std::unique_ptr<TypeInfo>>& get_types()
    const;
  const std::vector<std::unique_ptr<FieldInfo>>& get_owned_fields() const;

  const std::vector<DwarfStackObject>& get_stack_objects() const;

private:
  void process_die_tree(Dwarf_Die die);
  void process_struct_die(Dwarf_Die die);
  TypeInfo* get_or_create_type(Dwarf_Die die, int depth);

  TypeInfo* get_or_create_type(Dwarf_Die die);

  void process_subprogram_die(Dwarf_Die die);
  void process_stack_variable(Dwarf_Die var_die,
                              const std::string& function_name);

  DwarfContext context;
  Registry<std::string, StructInfo> registry;

  std::unordered_map<Dwarf_Off, std::unique_ptr<TypeInfo>> types;
  std::vector<std::unique_ptr<FieldInfo>> owned_fields;
  std::vector<DwarfStackObject> stack_objects;
};
