#pragma once

#include <any>
#include <cstdint>
#include <string>
#include <unordered_map>

#include "common/Constants.hpp"

struct Allocation {
  uint64_t base;
  uint64_t size;
  uint64_t timestamp;
  uint32_t tid;
  std::string struct_name;
  std::any type;
  CScope::AllocationKind alloc_type{NONE};  // heap, mmap, stack
};

using std::unordered_map;
class AllocationTracker {
public:
  AllocationTracker();
  ~AllocationTracker();
  void on_alloc(uint64_t base, size_t size);
  void on_free(uint64_t base);

  const Allocation* lookup(uint64_t addr) const;

private:
  unordered_map<uint64_t, Allocation*> allocation_map;
};
