
#include "dwarf/Extractor.hpp"

#include <fcntl.h>
#include <libdwarf-2/dwarf.h>
#include <libdwarf-2/libdwarf.h>
#include <libelf.h>
#include <unistd.h>

#include <stdexcept>

#include "common/Types.hpp"
#include "dwarf/DwarfContext.hpp"

/* ------------------------- Helpers ------------------------- */

static std::string die_name(Dwarf_Debug dbg, Dwarf_Die die) {
  char* name = nullptr;
  if (dwarf_diename(die, &name, nullptr) == DW_DLV_OK) {
    std::string s{name};
    dwarf_dealloc(dbg, name, DW_DLA_STRING);
    return s;
  }
  return {};
}

static Dwarf_Die resolve_type_die(Dwarf_Debug dbg, Dwarf_Die die) {
  Dwarf_Error err      = nullptr;
  Dwarf_Attribute attr = nullptr;

  if (dwarf_attr(die, DW_AT_type, &attr, &err) != DW_DLV_OK) return nullptr;

  Dwarf_Off off;
  dwarf_global_formref(attr, &off, &err);

  Dwarf_Die type_die = nullptr;
  dwarf_offdie_b(dbg, off, true, &type_die, &err);

  dwarf_dealloc(dbg, attr, DW_DLA_ATTR);
  return type_die;
}

static Dwarf_Die unwrap_type(Dwarf_Debug dbg, Dwarf_Die die) {
  while (die) {
    Dwarf_Half tag;
    dwarf_tag(die, &tag, nullptr);

    if (tag == DW_TAG_typedef || tag == DW_TAG_const_type ||
        tag == DW_TAG_volatile_type || tag == DW_TAG_restrict_type) {
      Dwarf_Die next = resolve_type_die(dbg, die);
      dwarf_dealloc(dbg, die, DW_DLA_DIE);
      die = next;
    } else {
      break;
    }
  }
  return die;
}

/* ------------------------- Extractor ------------------------- */
Extractor::Extractor(const string& bin) : context(DwarfContext{bin}) {}

Extractor::~Extractor() = default;

void Extractor::create_registry() {
  Dwarf_Debug dbg = context.dbg();
  Dwarf_Error err = nullptr;

  Dwarf_Unsigned cu_header_length = 0;
  Dwarf_Off abbrev_offset         = 0;
  Dwarf_Unsigned next_cu_offset   = 0;
  Dwarf_Half version              = 0;
  Dwarf_Half address_size         = 0;
  Dwarf_Half length_size          = 0;
  Dwarf_Half extension_size       = 0;
  Dwarf_Half header_cu_type       = 0;
  Dwarf_Unsigned type_offset      = 0;
  Dwarf_Sig8 type_signature{};
  Dwarf_Die cu_die = nullptr;

  while (true) {
    int res = dwarf_next_cu_header_e(
      dbg, true, &cu_die, &cu_header_length, &version, &abbrev_offset,
      &address_size, &length_size, &extension_size, &type_signature,
      &type_offset, &next_cu_offset, &header_cu_type, &err);

    if (res == DW_DLV_NO_ENTRY) break;

    if (res == DW_DLV_ERROR)
      throw std::runtime_error("dwarf_next_cu_header_e failed");

    process_die_tree(cu_die);
    dwarf_dealloc(dbg, cu_die, DW_DLA_DIE);
  }
}

void Extractor::process_die_tree(Dwarf_Die die) {
  process_die(die);

  Dwarf_Die child = nullptr;
  Dwarf_Error err = nullptr;

  if (dwarf_child(die, &child, &err) == DW_DLV_OK) {
    for (Dwarf_Die cur = child; cur;) {
      process_die_tree(cur);

      Dwarf_Die sibling = nullptr;
      if (dwarf_siblingof_b(context.dbg(), cur, true, &sibling, &err) !=
          DW_DLV_OK)
        break;

      dwarf_dealloc(context.dbg(), cur, DW_DLA_DIE);
      cur = sibling;
    }
  }
}

void Extractor::process_die(Dwarf_Die die) {
  Dwarf_Debug dbg = context.dbg();
  Dwarf_Half tag;

  if (dwarf_tag(die, &tag, nullptr) != DW_DLV_OK) return;

  if (tag != DW_TAG_structure_type) return;

  StructSchema schema;
  schema.name = die_name(dbg, die);

  Dwarf_Unsigned struct_size = 0;
  if (dwarf_bytesize(die, &struct_size, nullptr) == DW_DLV_OK)
    schema.size = struct_size;

  Dwarf_Die child = nullptr;
  Dwarf_Error err = nullptr;

  if (dwarf_child(die, &child, &err) != DW_DLV_OK) {
    registry.register_struct(schema.name, schema);
    return;
  }

  for (Dwarf_Die cur = child; cur;) {
    Dwarf_Half child_tag;
    dwarf_tag(cur, &child_tag, nullptr);

    /* -------- Fields -------- */
    if (child_tag == DW_TAG_member || child_tag == DW_TAG_inheritance) {
      FieldInfo field;

      field.name =
        (child_tag == DW_TAG_inheritance) ? "<base>" : die_name(dbg, cur);

      /* Offset */
      Dwarf_Attribute loc_attr = nullptr;
      Dwarf_Unsigned offset    = 0;
      if (dwarf_attr(cur, DW_AT_data_member_location, &loc_attr, &err) ==
          DW_DLV_OK) {
        dwarf_formudata(loc_attr, &offset, &err);
        dwarf_dealloc(dbg, loc_attr, DW_DLA_ATTR);
      }
      field.offset = offset;

      /* Bitfields */
      Dwarf_Attribute bit_attr = nullptr;
      if (dwarf_attr(cur, DW_AT_bit_size, &bit_attr, &err) == DW_DLV_OK) {
        dwarf_formudata(bit_attr, &field.bit_size, &err);
        dwarf_dealloc(dbg, bit_attr, DW_DLA_ATTR);

        if (dwarf_attr(cur, DW_AT_bit_offset, &bit_attr, &err) == DW_DLV_OK) {
          dwarf_formudata(bit_attr, &field.bit_offset, &err);
          dwarf_dealloc(dbg, bit_attr, DW_DLA_ATTR);
        }
      }

      /* Type */
      Dwarf_Die type_die = unwrap_type(dbg, resolve_type_die(dbg, cur));
      if (type_die) {
        field.type_name = die_name(dbg, type_die);

        Dwarf_Unsigned sz = 0;
        if (dwarf_bytesize(type_die, &sz, nullptr) == DW_DLV_OK)
          field.size = sz;

        dwarf_dealloc(dbg, type_die, DW_DLA_DIE);
      }

      schema.fields.push_back(std::move(field));
    }

    Dwarf_Die sibling = nullptr;
    if (dwarf_siblingof_b(dbg, cur, true, &sibling, &err) != DW_DLV_OK) break;

    dwarf_dealloc(dbg, cur, DW_DLA_DIE);
    cur = sibling;
  }

  registry.register_struct(schema.name, schema);
}

const Registry<string, StructSchema>& Extractor::get_registry() const {
  return registry;
}
