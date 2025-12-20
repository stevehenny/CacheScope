#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "schema.hpp"

using std::string, std::string_view, std::vector, std::unordered_map;

class StructRegistry {
public:
  void register_struct(StructSchema schema);
  const StructSchema* lookup(string_view name) const;

private:
  unordered_map<string, StructSchema> _schemas;
};
