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

#include "runtime/TracerConfig.hpp"

// ------------------------------------------------------------
// Helpers
// ------------------------------------------------------------
static inline void full_memory_barrier() {
  std::atomic_thread_fence(std::memory_order_seq_cst);
}

// ------------------------------------------------------------
// Tracer implementation
// ------------------------------------------------------------
Tracer::Tracer(pid_t pid, const TracerConfig& cfg) : _pid(pid) {
  TracerConfig config = cfg;
  config.cpu          = TracerConfig::detect_cpu_vendor();

  perf_event_attr attr = config.build_attr();

  _perf_fd = syscall(SYS_perf_event_open, &attr, _pid, -1, -1, 0);

  if (_perf_fd < 0) {
    perror("perf_event_open");
    throw std::runtime_error("perf_event_open failed");
  }
  _perf_fd = syscall(SYS_perf_event_open, &attr, _pid, -1, -1, 0);
  if (_perf_fd < 0) {
    perror("perf_event_open");
    throw std::runtime_error("Failure to open perf event");
  }

  size_t page_size  = sysconf(_SC_PAGESIZE);
  size_t data_pages = 8;  // must be power of two
  _mmap_size        = (data_pages + 1) * page_size;

  _mmap_buf =
    mmap(nullptr, _mmap_size, PROT_READ | PROT_WRITE, MAP_SHARED, _perf_fd, 0);

  if (_mmap_buf == MAP_FAILED) {
    perror("mmap");
    throw std::runtime_error("Failed to mmap perf buffer");
  }

  _meta = reinterpret_cast<perf_event_mmap_page*>(_mmap_buf);
  _data = reinterpret_cast<uint8_t*>(_mmap_buf) + page_size;

  _data_mask = (data_pages * page_size) - 1;
  _tail      = 0;
}

Tracer::~Tracer() {
  if (_mmap_buf && _mmap_buf != MAP_FAILED) munmap(_mmap_buf, _mmap_size);

  if (_perf_fd >= 0) close(_perf_fd);
}

void Tracer::start() {
  ioctl(_perf_fd, PERF_EVENT_IOC_RESET, 0);
  ioctl(_perf_fd, PERF_EVENT_IOC_ENABLE, 0);
}

void Tracer::stop() { ioctl(_perf_fd, PERF_EVENT_IOC_DISABLE, 0); }

// ------------------------------------------------------------
// Drain perf ring buffer (CORRECT WAY)
// ------------------------------------------------------------
std::vector<MemAccess> Tracer::drain() {
  full_memory_barrier();
  uint64_t head = _meta->data_head;

  while (_tail < head) {
    auto* hdr =
      reinterpret_cast<perf_event_header*>(_data + (_tail & _data_mask));

    if (hdr->size == 0) break;

    if (hdr->type == PERF_RECORD_SAMPLE) {
      uint8_t* ptr = reinterpret_cast<uint8_t*>(hdr) + sizeof(*hdr);

      uint64_t ip = *reinterpret_cast<uint64_t*>(ptr);
      ptr += sizeof(uint64_t);

      uint64_t addr = *reinterpret_cast<uint64_t*>(ptr);
      ptr += sizeof(uint64_t);

      uint32_t pid = *reinterpret_cast<uint32_t*>(ptr);
      uint32_t tid = *reinterpret_cast<uint32_t*>(ptr + 4);
      ptr += 8;

      uint32_t cpu = *reinterpret_cast<uint32_t*>(ptr);
      ptr += 8;  // cpu + reserved

      _samples.push_back({
        .ip       = ip,
        .addr     = addr,
        .tid      = tid,
        .cpu      = cpu,
        .is_write = false,
      });
    }

    _tail += hdr->size;
  }

  _meta->data_tail = _tail;
  full_memory_barrier();

  return _samples;
}
