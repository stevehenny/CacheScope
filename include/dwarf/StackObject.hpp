#include <cstdint>
#include <string>

#include "common/Types.hpp"

struct StackObject {
  std::string function;
  std::string name;
  std::string file;
  uint64_t size;
  int64_t frame_offset;
  uint64_t low_pc;
  uint64_t high_pc;
  TypeInfo* type;
};
