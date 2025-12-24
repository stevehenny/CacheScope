#pragma once
#include <libdwarf-2/libdwarf.h>

#include <string>
#include <vector>

using std::string, std::vector;

// Forward declarations (this solves the cycle)
struct FieldInfo;
struct TypeInfo;

enum class TypeKind {
  Primitive,
  Pointer,
  Array,
  Struct,
  Class,
  Union,
  Enum,
  Typedef,
  Unknown
};

struct TypeInfo {
  string name;
  TypeKind kind;
  size_t size  = 0;
  size_t align = 0;

  // Relationships
  TypeInfo* pointee = nullptr;
  TypeInfo* element = nullptr;
  size_t array_len  = 0;

  vector<TypeInfo*> bases;
  vector<FieldInfo*> fields;

  // Flags
  bool is_const    = false;
  bool is_volatile = false;
  bool is_signed   = false;

  // DWARF identity
  Dwarf_Off die_offset = 0;
};

struct FieldInfo {
  string name;
  size_t offset;
  size_t size;
  Dwarf_Unsigned bit_size   = 0;
  Dwarf_Unsigned bit_offset = 0;

  TypeInfo* type;
};

struct StructInfo {
  string name;
  size_t size;
  vector<FieldInfo> fields;
  TypeInfo* self_type;
};
