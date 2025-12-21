#pragma once
#include <libdwarf-2/libdwarf.h>

#include <string>
#include <vector>

using std::string, std::vector;

struct FieldInfo {
  string name;
  size_t offset;
  size_t size;
  Dwarf_Unsigned bit_size   = 0;  // 0 means not a bitfield
  Dwarf_Unsigned bit_offset = 0;
  string type_name;
};

struct StructSchema {
  string name;
  size_t size;
  vector<FieldInfo> fields;
};

struct Test {
  string test1;
  size_t test2;
  char test3;
};
