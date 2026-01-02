#include "runtime/Tracer.hpp"

#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <thread>

#include "runtime/TracerConfig.hpp"

static inline void full_memory_barrier() {
  std::atomic_thread_fence(std::memory_order_seq_cst);
}

Tracer::Tracer(pid_t pid, const TracerConfig& cfg) : _pid(pid) {
  TracerConfig config = cfg;
  config.cpu          = TracerConfig::detect_cpu_vendor();

  perf_event_attr attr = config.build_attr();
  _sample_type         = attr.sample_type;

  // Get number of CPUs
  unsigned int num_cpus = std::thread::hardware_concurrency();
  if (num_cpus == 0) num_cpus = 4;

  size_t page_size  = sysconf(_SC_PAGESIZE);
  size_t data_pages = 8;  // Must be power of two
  size_t mmap_size  = (data_pages + 1) * page_size;

  // Create one perf event per CPU
  for (unsigned int cpu = 0; cpu < num_cpus; ++cpu) {
    PerCpuState state{};

    // Monitor this PID on this specific CPU
    state.fd = syscall(SYS_perf_event_open, &attr, _pid, cpu, -1, 0);

    if (state.fd < 0) {
      for (auto& s : _cpu_states) {
        if (s.mmap_buf && s.mmap_buf != MAP_FAILED) {
          munmap(s.mmap_buf, s.mmap_size);
        }
        if (s.fd >= 0) {
          close(s.fd);
        }
      }

      perror("perf_event_open");
      throw std::runtime_error("perf_event_open failed for CPU " +
                               std::to_string(cpu));
    }

    // mmap the ring buffer for this CPU
    state.mmap_buf =
      mmap(nullptr, mmap_size, PROT_READ | PROT_WRITE, MAP_SHARED, state.fd, 0);

    if (state.mmap_buf == MAP_FAILED) {
      perror("mmap");
      close(state.fd);

      // Clean up previously successful mmaps
      for (auto& s : _cpu_states) {
        if (s.mmap_buf && s.mmap_buf != MAP_FAILED) {
          munmap(s.mmap_buf, s.mmap_size);
        }
        if (s.fd >= 0) {
          close(s.fd);
        }
      }

      throw std::runtime_error("Failed to mmap perf buffer for CPU " +
                               std::to_string(cpu));
    }

    state.meta      = reinterpret_cast<perf_event_mmap_page*>(state.mmap_buf);
    state.data      = reinterpret_cast<uint8_t*>(state.mmap_buf) + page_size;
    state.data_mask = (data_pages * page_size) - 1;
    state.mmap_size = mmap_size;
    state.tail      = 0;

    _cpu_states.push_back(state);
  }
}

Tracer::~Tracer() {
  for (auto& state : _cpu_states) {
    if (state.mmap_buf && state.mmap_buf != MAP_FAILED) {
      munmap(state.mmap_buf, state.mmap_size);
    }
    if (state.fd >= 0) {
      close(state.fd);
    }
  }
}

void Tracer::start() {
  for (auto& state : _cpu_states) {
    ioctl(state.fd, PERF_EVENT_IOC_RESET, 0);
    ioctl(state.fd, PERF_EVENT_IOC_ENABLE, 0);
  }
}

void Tracer::stop() {
  for (auto& state : _cpu_states) {
    ioctl(state.fd, PERF_EVENT_IOC_DISABLE, 0);
  }
}

void Tracer::drain_cpu(PerCpuState& state) {
  full_memory_barrier();
  uint64_t head = state.meta->data_head;

  while (state.tail < head) {
    auto* hdr = reinterpret_cast<perf_event_header*>(
      state.data + (state.tail & state.data_mask));

    if (hdr->size == 0) break;

    if (hdr->type == PERF_RECORD_SAMPLE) {
      uint8_t* ptr =
        reinterpret_cast<uint8_t*>(hdr) + sizeof(perf_event_header);

      uint64_t ip       = 0;
      uint64_t addr     = 0;
      uint32_t pid      = 0;
      uint32_t tid      = 0;
      uint32_t cpu      = 0;
      uint64_t time     = 0;
      uint64_t data_src = 0;

      if (_sample_type & PERF_SAMPLE_IP) {
        ip = *reinterpret_cast<uint64_t*>(ptr);
        ptr += 8;
      }

      if (_sample_type & PERF_SAMPLE_TID) {
        pid = *reinterpret_cast<uint32_t*>(ptr);
        tid = *reinterpret_cast<uint32_t*>(ptr + 4);
        ptr += 8;
      }

      if (_sample_type & PERF_SAMPLE_TIME) {
        time = *reinterpret_cast<uint64_t*>(ptr);
        ptr += 8;
      }

      if (_sample_type & PERF_SAMPLE_ADDR) {
        addr = *reinterpret_cast<uint64_t*>(ptr);
        ptr += 8;
      }

      if (_sample_type & PERF_SAMPLE_ID) {
        ptr += 8;
      }

      if (_sample_type & PERF_SAMPLE_STREAM_ID) {
        ptr += 8;
      }

      if (_sample_type & PERF_SAMPLE_CPU) {
        cpu = *reinterpret_cast<uint32_t*>(ptr);
        ptr += 8;  // cpu + reserved
      }

      if (_sample_type & PERF_SAMPLE_PERIOD) {
        ptr += 8;
      }

      if (_sample_type & PERF_SAMPLE_DATA_SRC) {
        data_src = *reinterpret_cast<uint64_t*>(ptr);
        ptr += 8;
      }

      _samples.push_back({
        .ip       = ip,
        .addr     = addr,
        .pid      = pid,
        .tid      = tid,
        .cpu      = cpu,
        .data_src = data_src,
        .is_write = false,
      });
    }

    state.tail += hdr->size;
  }

  state.meta->data_tail = state.tail;
  full_memory_barrier();
}

std::vector<MemAccess> Tracer::drain() {
  _samples.clear();

  // Drain all CPU ring buffers
  for (auto& state : _cpu_states) {
    drain_cpu(state);
  }

  return _samples;
}
