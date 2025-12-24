#include "dwarf/Extractor.hpp"

#include <libdwarf-2/dwarf.h>
#include <libdwarf-2/libdwarf.h>

#include <cassert>
#include <unordered_map>

#include "common/Types.hpp"
#include "dwarf/DwarfContext.hpp"

/* ============================================================
 * Helpers
 * ============================================================ */

static std::string die_name(Dwarf_Debug dbg, Dwarf_Die die) {
  char* name = nullptr;
  if (dwarf_diename(die, &name, nullptr) == DW_DLV_OK && name) {
    std::string s{name};
    dwarf_dealloc(dbg, name, DW_DLA_STRING);
    return s;
  }
  return {};
}

static Dwarf_Off die_offset(Dwarf_Die die) {
  Dwarf_Off off = 0;
  dwarf_dieoffset(die, &off, nullptr);
  return off;
}

static Dwarf_Die resolve_type_die(Dwarf_Debug dbg, Dwarf_Die die) {
  Dwarf_Attribute attr = nullptr;
  if (dwarf_attr(die, DW_AT_type, &attr, nullptr) != DW_DLV_OK) return nullptr;

  Dwarf_Off off = 0;
  dwarf_global_formref(attr, &off, nullptr);
  dwarf_dealloc(dbg, attr, DW_DLA_ATTR);

  Dwarf_Die type_die = nullptr;
  dwarf_offdie_b(dbg, off, true, &type_die, nullptr);
  return type_die;
}

static size_t member_byte_offset(Dwarf_Debug dbg, Dwarf_Die die) {
  Dwarf_Attribute attr = nullptr;
  if (dwarf_attr(die, DW_AT_data_member_location, &attr, nullptr) != DW_DLV_OK)
    return 0;

  Dwarf_Unsigned offset = 0;
  if (dwarf_formudata(attr, &offset, nullptr) != DW_DLV_OK) {
    dwarf_dealloc(dbg, attr, DW_DLA_ATTR);
    return 0;
  }

  dwarf_dealloc(dbg, attr, DW_DLA_ATTR);
  return static_cast<size_t>(offset);
}

static TypeKind tag_to_kind(Dwarf_Half tag) {
  switch (tag) {
    case DW_TAG_base_type:
      return TypeKind::Primitive;
    case DW_TAG_pointer_type:
      return TypeKind::Pointer;
    case DW_TAG_array_type:
      return TypeKind::Array;
    case DW_TAG_structure_type:
      return TypeKind::Struct;
    case DW_TAG_class_type:
      return TypeKind::Class;
    case DW_TAG_union_type:
      return TypeKind::Union;
    case DW_TAG_enumeration_type:
      return TypeKind::Enum;
    case DW_TAG_typedef:
      return TypeKind::Typedef;
    default:
      return TypeKind::Unknown;
  }
}

/* ============================================================
 * Extractor
 * ============================================================ */

Extractor::Extractor(const std::string& bin) : context(bin) {}

void Extractor::create_registry() {
  Dwarf_Debug dbg = context.dbg();

  Dwarf_Die cu_die = nullptr;
  Dwarf_Error err  = nullptr;

  while (dwarf_next_cu_header_d(dbg, true, nullptr, nullptr, nullptr, nullptr,
                                nullptr, nullptr, nullptr, nullptr, nullptr,
                                nullptr, &err) == DW_DLV_OK) {
    if (dwarf_siblingof_b(dbg, nullptr, true, &cu_die, nullptr) == DW_DLV_OK) {
      process_die_tree(cu_die);
      dwarf_dealloc(dbg, cu_die, DW_DLA_DIE);
    }
  }
}

const Registry<std::string, StructInfo>& Extractor::get_registry() const {
  return registry;
}

/* ============================================================
 * Type creation
 * ============================================================ */

TypeInfo* Extractor::get_or_create_type(Dwarf_Die die) {
  if (!die) return nullptr;

  Dwarf_Off off = die_offset(die);
  auto it       = types.find(off);
  if (it != types.end()) return it->second.get();

  auto t        = std::make_unique<TypeInfo>();
  TypeInfo* raw = t.get();

  raw->die_offset = off;
  raw->name       = die_name(context.dbg(), die);

  Dwarf_Half tag = 0;
  dwarf_tag(die, &tag, nullptr);
  raw->kind = tag_to_kind(tag);

  Dwarf_Unsigned size = 0;
  if (dwarf_bytesize(die, &size, nullptr) == DW_DLV_OK)
    raw->size = static_cast<size_t>(size);

  types.emplace(off, std::move(t));
  return raw;
}

/* ============================================================
 * DIE walking
 * ============================================================ */

void Extractor::process_struct_die(Dwarf_Die die) {
  Dwarf_Debug dbg = context.dbg();

  TypeInfo* type = get_or_create_type(die);
  if (!type ||
      (type->kind != TypeKind::Struct && type->kind != TypeKind::Class))
    return;

  StructInfo info;
  info.name      = type->name;
  info.size      = type->size;
  info.self_type = type;

  Dwarf_Die child = nullptr;
  if (dwarf_child(die, &child, nullptr) != DW_DLV_OK) {
    registry.register_struct(info.name, info);
    return;
  }

  for (Dwarf_Die cur = child; cur;) {
    Dwarf_Half tag = 0;
    dwarf_tag(cur, &tag, nullptr);

    if (tag == DW_TAG_member) {
      auto field    = std::make_unique<FieldInfo>();
      field->name   = die_name(dbg, cur);
      field->offset = member_byte_offset(dbg, cur);

      // bitfields
      Dwarf_Attribute a = nullptr;
      if (dwarf_attr(cur, DW_AT_bit_size, &a, nullptr) == DW_DLV_OK) {
        dwarf_formudata(a, &field->bit_size, nullptr);
        dwarf_dealloc(dbg, a, DW_DLA_ATTR);
      }

      Dwarf_Die type_die = resolve_type_die(dbg, cur);
      field->type        = get_or_create_type(type_die);
      field->size        = field->type ? field->type->size : 0;

      type->fields.push_back(field.get());
      info.fields.push_back(*field);
      owned_fields.emplace_back(std::move(field));
    }

    Dwarf_Die sib = nullptr;
    if (dwarf_siblingof_b(dbg, cur, true, &sib, nullptr) != DW_DLV_OK) {
      dwarf_dealloc(dbg, cur, DW_DLA_DIE);
      break;
    }

    dwarf_dealloc(dbg, cur, DW_DLA_DIE);
    cur = sib;
  }

  registry.register_struct(info.name, info);
}

void Extractor::process_die_tree(Dwarf_Die die) {
  Dwarf_Half tag = 0;
  dwarf_tag(die, &tag, nullptr);

  if (tag == DW_TAG_structure_type || tag == DW_TAG_class_type)
    process_struct_die(die);

  Dwarf_Die child = nullptr;
  if (dwarf_child(die, &child, nullptr) != DW_DLV_OK) return;

  for (Dwarf_Die cur = child; cur;) {
    process_die_tree(cur);

    Dwarf_Die sib = nullptr;
    if (dwarf_siblingof_b(context.dbg(), cur, true, &sib, nullptr) !=
        DW_DLV_OK) {
      dwarf_dealloc(context.dbg(), cur, DW_DLA_DIE);
      break;
    }

    dwarf_dealloc(context.dbg(), cur, DW_DLA_DIE);
    cur = sib;
  }
}
