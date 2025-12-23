#include "runtime/Tracer.hpp"

#include <linux/hw_breakpoint.h>
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <stdexcept>

// TODO: generalize Tracer class
// @param attr.type
// @param attr.config
// @param attr.sample_type (need a helper method to create bit mask for this)
// Overall going to need a lot more generalization for Tracer, might convert
// This to a template class if necessary
Tracer::Tracer() {
  perf_event_attr attr{};
  attr.type = PERF_TYPE_HARDWARE;
  attr.size = sizeof(attr);

  attr.config = PERF_COUNT_HW_CACHE_MISSES;

  attr.sample_type =
    (PERF_SAMPLE_IP | PERF_SAMPLE_ADDR | PERF_SAMPLE_TID | PERF_SAMPLE_CPU);
  attr.sample_period  = 1000;
  attr.disabled       = 1;
  attr.exclude_kernel = 1;
  attr.exclude_hv     = 1;
  attr.precise_ip     = 1;

  perf_fd = syscall(SYS_perf_event_open, &attr, 0, -1, -1, 0);

  if (perf_fd < 0) throw std::runtime_error("Failure to open perf event\n");

  mmap_size = sysconf(_SC_PAGESIZE) * 8;  // FIXME: May need to generalize size
  mmap_buf =
    mmap(nullptr, mmap_size, PROT_READ | PROT_WRITE, MAP_SHARED, perf_fd, 0);

  if (mmap_buf == MAP_FAILED) throw std::runtime_error("Failed to mmap\n");
}

Tracer::~Tracer() {
  if (mmap_buf) munmap(mmap_buf, mmap_size);

  if (perf_fd >= 0) close(perf_fd);
}

void Tracer::start() {
  ioctl(perf_fd, PERF_EVENT_IOC_RESET, 0);
  ioctl(perf_fd, PERF_EVENT_IOC_ENABLE, 0);
}

void Tracer::stop() { ioctl(perf_fd, PERF_EVENT_IOC_DISABLE, 0); }

std::vector<MemAccess> Tracer::drain() {
  struct {
    perf_event_header header;
    uint64_t ip;
    uint64_t addr;
    uint32_t tid, pid;
    uint32_t cpu, res;
  } sample;

  while (true) {
    ssize_t n = read(perf_fd, &sample, sizeof(sample));

    // if we don't read anything break
    if (n <= 0) break;

    if (sample.header.type != PERF_RECORD_SAMPLE) continue;

    samples.push_back({.ip       = sample.ip,
                       .addr     = sample.addr,
                       .tid      = sample.tid,
                       .cpu      = sample.cpu,
                       .is_write = false});
  }

  return samples;
}
