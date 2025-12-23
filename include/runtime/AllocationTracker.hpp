#pragma once

#include <any>
#include <cstdint>
#include <string>

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

class AllocationTracker {
public:
  void on_alloc(uint64_t base, size_t size);
  void on_free(uint64_t base);

  const Allocation* lookup(uint64_t addr) const;
};
