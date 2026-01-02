#pragma once
#include <fstream>
#include <string>
#include <vector>

#include "runtime/HeapAllocationTracker.hpp"
template <typename T>
class Parser {
public:
  Parser(std::string binary)
    : _stream(std::ifstream{binary, std::ios::binary}) {
    populate_allocs();
  }
  ~Parser() = default;

  void populate_allocs() {
    T alloc;
    while (_stream.read(reinterpret_cast<char*>(&alloc), sizeof(T))) {
      _allocs.emplace_back(alloc);
    }
  }
  std::vector<T>& get_allocs() { return _allocs; }

private:
  std::ifstream _stream;
  std::vector<T> _allocs;
};
