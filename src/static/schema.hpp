#pragma once
#include <string>
#include <vector>

using std::string, std::vector;

struct FieldInfo {
  string name;
  size_t offset;
  size_t size;
};

struct StructSchema {
  string name;
  size_t size;
  vector<FieldInfo> fields;
};
