#pragma once
#include <vector>

#include "runtime/MemAccess.hpp"

class Tracer {
public:
  void start();
  void stop();
  std::vector<MemAccess> drain();
};
