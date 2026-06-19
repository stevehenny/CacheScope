#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "runtime/PerfEventRecorder.hpp"

using std::string, std::unordered_map, std::vector;

using ProcID = uint64_t;
class ProcHandler {
public:
  void record(ProcID pid);

private:
  ProcID pid;
  uint32_t t_count;
  std::vector<uint32_t> tids;
  PerfEventRecorder results;
};
