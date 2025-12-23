#include <cstdint>

#include "common/Registry.hpp"
#include "common/Types.hpp"
#include "runtime/AllocationTracker.hpp"

class AddressMapper {
public:
  const FieldInfo* resolve(uint64_t addr, const AllocationTracker& allocs,
                           const Registry<string, StructSchema>& structs);
};
