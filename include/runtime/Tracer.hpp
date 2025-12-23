#pragma once
#include <vector>

#include "runtime/MemAccess.hpp"

using std::vector;

class Tracer {
public:
  Tracer();
  ~Tracer();
  void start();
  void stop();
  std::vector<MemAccess> drain();

private:
  int perf_fd{-1};
  void* mmap_buf{nullptr};
  size_t mmap_size{0};

  vector<MemAccess> samples;
};
