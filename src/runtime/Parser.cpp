#include "runtime/Parser.hpp"

#include "runtime/AllocationTracker.hpp"

Parser::Parser(std::string binary)
  : _stream(std::ifstream{binary.c_str(), std::ios::binary}) {
  populate_allocs();
}

Parser::~Parser() = default;

void Parser::populate_allocs() {
  Allocation alloc;
  while (_stream.read(reinterpret_cast<char*>(&alloc), sizeof(Allocation))) {
    _allocs.emplace_back(alloc);
  }
}

std::vector<Allocation>& Parser::get_allocs() { return _allocs; }
