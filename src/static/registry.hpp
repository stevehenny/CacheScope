#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "schema.hpp"

using std::string, std::string_view, std::vector, std::unordered_map;

template <typename K, typename V>
class Registry {
public:
  void register_struct(K key, V entry) { this->_entries[key] = entry; }
  V& lookup(const K& key) {
    auto it = _entries.find(key);
    if (it == _entries.end()) {
      return nullptr;
    }
    return it->second;
  }
  const unordered_map<K, V>& get_map() const { return _entries; }

private:
  unordered_map<K, V> _entries;
};
