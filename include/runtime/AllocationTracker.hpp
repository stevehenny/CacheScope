#pragma once

#include <any>
#include <cstdint>
#include <string>
#include <unordered_map>

struct MemAlloc {
  uint64_t base;
  uint64_t size;
  std::any type;
};

struct Allocation {
  uint64_t base;
  uint64_t size;
  std::string struct_name;
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
