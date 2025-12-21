#include "registry.hpp"

#include "schema.hpp"

void StructRegistry::register_struct(StructSchema schema) {
  this->_schemas[schema.name] = schema;
}

StructSchema& StructRegistry::lookup(const string& name) {
  return this->_schemas[name];
}
const unordered_map<string, StructSchema>& StructRegistry::get_map() const {
  return _schemas;
}
