#pragma once
#include <linux/perf_event.h>
#include <perfmon/pfmlib.h>
#include <perfmon/pfmlib_perf_event.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#include "runtime/PerfEventRecorder.hpp"

#if defined(__x86_64__) || defined(__i386__)
#include <asm/perf_regs.h>
#endif

#include <sys/wait.h>

#include <cctype>
#include <cerrno>
#include <cstring>
#include <filesystem>

namespace Utils {

constexpr size_t kMmapPages = 128;

int perf_event_open_sys(perf_event_attr* attr, pid_t pid, int cpu, int group_fd,
                        unsigned long flags);

std::string trim_copy(std::string_view sv);

bool parse_u64(std::string_view sv, uint64_t& out);

bool apply_sysfs_field(const std::filesystem::path& path, uint64_t value,
                       perf_event_attr& attr, std::string& error);
bool ensure_pfm_init(std::string& error);

bool encode_ibs_op_sysfs(perf_event_attr& attr, std::string& error);

bool encode_event(const std::string& event, perf_event_attr& attr,
                  std::string& error);

SampleType event_type_from_name(const std::string& name);

bool is_ibs_op_event(std::string_view name);

struct PerfEventHandle {
  perf_event_attr attr{};
  int fd{-1};
  void* mmap_base{MAP_FAILED};
  size_t mmap_len{};
  SampleType type{SampleType::CACHE_LOAD};
};

bool setup_event(pid_t pid, const std::string& name, int sample_period, int cpu,
                 PerfEventHandle& out, std::string& error);

void teardown_event(PerfEventHandle& ev);

bool read_buffer(const char* data, size_t data_size, uint64_t offset, void* dst,
                 size_t len);

void parse_sample(const perf_event_attr& attr, SampleType type,
                  const uint8_t* data, size_t size,
                  std::vector<PerfSample>& out);

void read_event_samples(PerfEventHandle& ev, std::vector<PerfSample>& out);

struct ProcMapInfo {
  std::vector<ProcMapRange> ranges;
  std::optional<int64_t> load_bias;
};

ProcMapInfo read_proc_maps(pid_t pid, const std::string& binary);

ProcMapInfo wait_for_proc_maps(pid_t pid, const std::string& binary);

}  // namespace Utils
