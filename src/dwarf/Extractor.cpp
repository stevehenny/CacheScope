#include "dwarf/Extractor.hpp"

#include <libdwarf-2/dwarf.h>
#include <libdwarf-2/libdwarf.h>

#include <cassert>
#include <cstdint>
#include <memory>
#include <unordered_map>

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
  return "<anonymous>";
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
    case DW_TAG_subroutine_type:
      return TypeKind::Function;
    case DW_TAG_const_type:
      return TypeKind::Const;
    case DW_TAG_volatile_type:
      return TypeKind::Volatile;
    case DW_TAG_reference_type:
      return TypeKind::Reference;
    default:
      return TypeKind::Unknown;
  }
}

/* ============================================================
 * DW_OP_fbreg extraction
 * ============================================================ */

static bool extract_fbreg_offset(Dwarf_Debug dbg, Dwarf_Die die,
                                 int64_t& out_offset) {
  Dwarf_Attribute attr = nullptr;
  if (dwarf_attr(die, DW_AT_location, &attr, nullptr) != DW_DLV_OK) {
    Dwarf_Attribute abs = nullptr;
    if (dwarf_attr(die, DW_AT_abstract_origin, &abs, nullptr) != DW_DLV_OK)
      return false;

    Dwarf_Off off = 0;
    dwarf_global_formref(abs, &off, nullptr);
    dwarf_dealloc(dbg, abs, DW_DLA_ATTR);

    Dwarf_Die origin = nullptr;
    if (dwarf_offdie_b(dbg, off, true, &origin, nullptr) != DW_DLV_OK)
      return false;

    bool ok = extract_fbreg_offset(dbg, origin, out_offset);
    dwarf_dealloc(dbg, origin, DW_DLA_DIE);
    return ok;
  }

  Dwarf_Unsigned exprlen = 0;
  Dwarf_Ptr expr         = nullptr;
  if (dwarf_formexprloc(attr, &exprlen, &expr, nullptr) != DW_DLV_OK) {
    dwarf_dealloc(dbg, attr, DW_DLA_ATTR);
    return false;
  }

  const uint8_t* p   = static_cast<const uint8_t*>(expr);
  const uint8_t* end = p + exprlen;

  if (p >= end || *p != DW_OP_fbreg) {
    dwarf_dealloc(dbg, attr, DW_DLA_ATTR);
    return false;
  }

  ++p;
  int64_t value = 0;
  int shift     = 0;
  while (p < end) {
    uint8_t byte = *p++;
    value |= int64_t(byte & 0x7f) << shift;
    shift += 7;
    if ((byte & 0x80) == 0) break;
  }
  if (shift < 64 && (value & (1LL << (shift - 1)))) value |= (-1LL) << shift;

  out_offset = value;
  dwarf_dealloc(dbg, attr, DW_DLA_ATTR);
  return true;
}

/* ============================================================
 * Extractor
 * ============================================================ */

Extractor::Extractor(const std::string& bin) : context(bin) {}

void Extractor::create_registry() {
  Dwarf_Debug dbg  = context.dbg();
  Dwarf_Die cu_die = nullptr;

  while (dwarf_next_cu_header_d(dbg, true, nullptr, nullptr, nullptr, nullptr,
                                nullptr, nullptr, nullptr, nullptr, nullptr,
                                nullptr, nullptr) == DW_DLV_OK) {
    if (dwarf_siblingof_b(dbg, nullptr, true, &cu_die, nullptr) == DW_DLV_OK) {
      process_die_tree(cu_die);
      dwarf_dealloc(dbg, cu_die, DW_DLA_DIE);
    }
  }
}

/* ============================================================
 * Type creation
 * ============================================================ */

TypeInfo* Extractor::get_or_create_type(Dwarf_Die die, int depth) {
  if (!die || depth > 10) return nullptr;  // Limit recursion

  Dwarf_Off off = die_offset(die);

  auto it = types.find(off);
  if (it != types.end()) {
    if (!it->second) return nullptr;
    return it->second.get();
  }
  // placeholder for recursion detection
  types[off] = nullptr;

  char* raw_name = nullptr;
  dwarf_diename(die, &raw_name, nullptr);
  std::string n = raw_name ? raw_name : "<anonymous>";
  if (raw_name) dwarf_dealloc(context.dbg(), raw_name, DW_DLA_STRING);

  // Bail out early for STL internals
  if (n.find("std::") != std::string::npos ||
      n.find("_Hash_node") != std::string::npos ||
      n.find("_Hashtable") != std::string::npos ||
      n.find("_List_node") != std::string::npos ||
      n.find("_Rb_tree_node") != std::string::npos) {
    auto t        = std::make_unique<TypeInfo>();
    t->die_offset = off;
    t->name       = "<STL:" + n + ">";
    t->kind       = TypeKind::Unknown;
    t->size       = 0;
    types[off]    = std::move(t);
    return types[off].get();
  }

  auto t        = std::make_unique<TypeInfo>();
  TypeInfo* raw = t.get();
  types[off]    = std::move(t);

  raw->die_offset = off;
  raw->name       = n;

  Dwarf_Half tag = 0;
  dwarf_tag(die, &tag, nullptr);
  raw->kind = tag_to_kind(tag);

  Dwarf_Unsigned size = 0;
  if (dwarf_bytesize(die, &size, nullptr) == DW_DLV_OK)
    raw->size = static_cast<size_t>(size);

  // ---------- Pointer ----------
  if (raw->kind == TypeKind::Pointer) {
    Dwarf_Die pointee_die = resolve_type_die(context.dbg(), die);
    raw->pointee          = get_or_create_type(pointee_die, depth + 1);
    raw->size             = raw->size ? raw->size : sizeof(void*);
    raw->name             = raw->pointee ? raw->pointee->name + "*" : "void*";
  }

  // ---------- Typedef ----------
  else if (raw->kind == TypeKind::Typedef) {
    Dwarf_Die target  = resolve_type_die(context.dbg(), die);
    TypeInfo* aliased = get_or_create_type(target, depth + 1);
    if (aliased) {
      raw->pointee = aliased;
      raw->size    = aliased->size;
      raw->name    = aliased->name;
    }
  }

  // ---------- Array ----------
  else if (raw->kind == TypeKind::Array) {
    Dwarf_Die elem_die = resolve_type_die(context.dbg(), die);
    raw->element       = get_or_create_type(elem_die, depth + 1);
    raw->array_len     = 0;

    Dwarf_Die child = nullptr;
    if (dwarf_child(die, &child, nullptr) == DW_DLV_OK) {
      for (Dwarf_Die cur = child; cur;) {
        Dwarf_Half ctag = 0;
        dwarf_tag(cur, &ctag, nullptr);
        if (ctag == DW_TAG_subrange_type) {
          Dwarf_Attribute attr = nullptr;
          Dwarf_Unsigned upper = 0;
          if (dwarf_attr(cur, DW_AT_upper_bound, &attr, nullptr) == DW_DLV_OK) {
            dwarf_formudata(attr, &upper, nullptr);
            raw->array_len = upper + 1;
            dwarf_dealloc(context.dbg(), attr, DW_DLA_ATTR);
          }
        }

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

    if (raw->element) {
      raw->size = raw->element->size * (raw->array_len ? raw->array_len : 1);
      raw->name =
        raw->element->name +
        (raw->array_len ? "[" + std::to_string(raw->array_len) + "]" : "[]");
    } else {
      raw->name = "<unknown>[]";
    }
  }

  // ---------- Const / Volatile / Reference ----------
  else if (raw->kind == TypeKind::Const || raw->kind == TypeKind::Volatile ||
           raw->kind == TypeKind::Reference) {
    Dwarf_Die target = resolve_type_die(context.dbg(), die);
    TypeInfo* base   = get_or_create_type(target, depth + 1);
    raw->pointee     = base;
    raw->size        = base ? base->size : 0;
    raw->name =
      (tag == DW_TAG_const_type      ? "const "
       : tag == DW_TAG_volatile_type ? "volatile "
                                     : "") +
      (raw->kind == TypeKind::Reference ? (base ? base->name + "&" : "&")
                                        : (base ? base->name : "<unknown>"));
  }

  // ---------- Struct / Class ----------
  else if (raw->kind == TypeKind::Struct || raw->kind == TypeKind::Class) {
    Dwarf_Bool is_decl = 0;
    if (dwarf_hasattr(die, DW_AT_declaration, &is_decl, nullptr) != DW_DLV_OK)
      is_decl = 0;

    if (is_decl) return raw;

    process_struct_die(die);
  }

  return raw;
}

TypeInfo* Extractor::get_or_create_type(Dwarf_Die die) {
  return get_or_create_type(die, 0);
}

/* ============================================================
 * Stack variable extraction
 * ============================================================ */

void Extractor::process_stack_variable(Dwarf_Die die,
                                       const std::string& function) {
  int64_t offset = 0;
  if (!extract_fbreg_offset(context.dbg(), die, offset)) return;

  DwarfStackObject obj;
  obj.function     = function;
  obj.name         = die_name(context.dbg(), die);
  obj.frame_offset = offset;

  Dwarf_Die type_die = resolve_type_die(context.dbg(), die);
  obj.type           = get_or_create_type(type_die);
  obj.size           = obj.type ? obj.type->size : 0;

  if (obj.name != "<anonymous>") stack_objects.push_back(obj);
}

/* ============================================================
 * Subprogram
 * ============================================================ */

void Extractor::process_subprogram_die(Dwarf_Die die) {
  std::string function = die_name(context.dbg(), die);

  Dwarf_Die child = nullptr;
  if (dwarf_child(die, &child, nullptr) != DW_DLV_OK) return;

  for (Dwarf_Die cur = child; cur;) {
    Dwarf_Half tag = 0;
    dwarf_tag(cur, &tag, nullptr);

    if (tag == DW_TAG_variable || tag == DW_TAG_formal_parameter)
      process_stack_variable(cur, function);

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

/* ============================================================
 * Struct extraction
 * ============================================================ */

void Extractor::process_struct_die(Dwarf_Die die) {
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
      auto field  = std::make_unique<FieldInfo>();
      field->name = die_name(context.dbg(), cur);

      Dwarf_Die type_die = resolve_type_die(context.dbg(), cur);
      field->type        = get_or_create_type(type_die);
      field->size        = field->type ? field->type->size : 0;

      type->fields.push_back(field.get());
      info.fields.push_back(*field);
      owned_fields.emplace_back(std::move(field));
    }

    Dwarf_Die sib = nullptr;
    if (dwarf_siblingof_b(context.dbg(), cur, true, &sib, nullptr) !=
        DW_DLV_OK) {
      dwarf_dealloc(context.dbg(), cur, DW_DLA_DIE);
      break;
    }
    dwarf_dealloc(context.dbg(), cur, DW_DLA_DIE);
    cur = sib;
  }

  registry.register_struct(info.name, info);
}

/* ============================================================
 * DIE traversal
 * ============================================================ */

void Extractor::process_die_tree(Dwarf_Die die) {
  Dwarf_Half tag = 0;
  dwarf_tag(die, &tag, nullptr);

  if (tag == DW_TAG_structure_type || tag == DW_TAG_class_type)
    process_struct_die(die);
  else if (tag == DW_TAG_subprogram) {
    process_subprogram_die(die);
    return;
  }

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

const std::vector<DwarfStackObject>& Extractor::get_stack_objects() const {
  return stack_objects;
}

const Registry<std::string, StructInfo>& Extractor::get_registry() const {
  return registry;
}
