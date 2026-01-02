#pragma once
#include <linux/perf_event.h>
#include <unistd.h>

#include <cstdint>
#include <vector>

struct MemAccess {
  uint64_t ip;
  uint64_t addr;
  uint32_t pid;
  uint32_t tid;
  uint32_t cpu;
  uint64_t data_src;
  bool is_write;
};

class TracerConfig;

class Tracer {
public:
  Tracer(pid_t pid, const TracerConfig& cfg);
  ~Tracer();

  void start();
  void stop();
  std::vector<MemAccess> drain();

private:
  struct PerCpuState {
    int fd;
    void* mmap_buf;
    perf_event_mmap_page* meta;
    uint8_t* data;
    uint64_t data_mask;
    size_t mmap_size;
    uint64_t tail;
  };

  std::vector<PerCpuState> _cpu_states;
  pid_t _pid;
  uint64_t _sample_type;
  std::vector<MemAccess> _samples;

  void drain_cpu(PerCpuState& state);
};
