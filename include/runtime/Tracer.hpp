#pragma once

#include <linux/perf_event.h>
#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <vector>

#include "runtime/MemAccess.hpp"
#include "runtime/TracerConfig.hpp"

class Tracer {
public:
  explicit Tracer(pid_t pid, const TracerConfig& cfg);
  ~Tracer();

  Tracer(const Tracer&)            = delete;
  Tracer& operator=(const Tracer&) = delete;

  Tracer(Tracer&&)            = delete;
  Tracer& operator=(Tracer&&) = delete;

  void start();
  void stop();

  std::vector<MemAccess> drain();

private:
  int _perf_fd{-1};
  pid_t _pid{-1};

  void* _mmap_buf{nullptr};
  size_t _mmap_size{0};

  perf_event_mmap_page* _meta{nullptr};
  uint64_t _sample_type{0};

  uint8_t* _data{nullptr};

  size_t _page_size{0};
  size_t _data_pages{0};
  size_t _data_mask{0};
  uint64_t _tail{0};

  std::vector<MemAccess> _samples;
};
