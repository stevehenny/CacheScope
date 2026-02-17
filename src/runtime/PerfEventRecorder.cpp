#include "runtime/PerfEventRecorder.hpp"

#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#include <perfmon/pfmlib.h>
#include <perfmon/pfmlib_perf_event.h>

#if defined(__x86_64__) || defined(__i386__)
#include <asm/perf_regs.h>
#endif

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>
#include <sys/wait.h>

namespace {

constexpr size_t kMmapPages = 128;

int perf_event_open_sys(perf_event_attr* attr, pid_t pid, int cpu, int group_fd,
                        unsigned long flags) {
  return static_cast<int>(
    syscall(__NR_perf_event_open, attr, pid, cpu, group_fd, flags));
}

std::string trim_copy(std::string_view sv) {
  auto b = sv.find_first_not_of(" \t\n");
  auto e = sv.find_last_not_of(" \t\n");
  if (b == std::string_view::npos) return {};
  return std::string(sv.substr(b, e - b + 1));
}

bool ensure_pfm_init(std::string& error) {
  static std::once_flag once;
  static int init_status = PFM_SUCCESS;
  std::call_once(once, []() { init_status = pfm_initialize(); });
  if (init_status != PFM_SUCCESS) {
    error = std::format("libpfm init failed: {}",
                        std::string(pfm_strerror(init_status)));
    return false;
  }
  return true;
}

bool encode_event(const std::string& event, perf_event_attr& attr,
                  std::string& error) {
  pfm_perf_encode_arg_t arg{};
  arg.attr = &attr;
  arg.size = sizeof(arg);
  int ret  = pfm_get_os_event_encoding(event.c_str(), PFM_PLM3,
                                      PFM_OS_PERF_EVENT, &arg);
  if (ret != PFM_SUCCESS) {
    error = std::format("libpfm failed to encode '{}': {}", event,
                        std::string(pfm_strerror(ret)));
    return false;
  }
  return true;
}

SampleType event_type_from_name(const std::string& name) {
  auto lower = name;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (lower.find("store") != std::string::npos) return SampleType::CACHE_STORE;
  return SampleType::CACHE_LOAD;
}

struct PerfEventHandle {
  perf_event_attr attr{};
  int fd{-1};
  void* mmap_base{MAP_FAILED};
  size_t mmap_len{};
  SampleType type{SampleType::CACHE_LOAD};
};

bool setup_event(pid_t pid, const std::string& name, int sample_period,
                 PerfEventHandle& out, std::string& error) {
  std::memset(&out.attr, 0, sizeof(out.attr));
  if (!encode_event(name, out.attr, error)) return false;

  out.attr.size          = sizeof(out.attr);
  out.attr.sample_type   = PERF_SAMPLE_IP | PERF_SAMPLE_TID | PERF_SAMPLE_TIME |
                         PERF_SAMPLE_ADDR | PERF_SAMPLE_CPU |
                         PERF_SAMPLE_REGS_USER;
  out.attr.sample_period = static_cast<uint64_t>(sample_period);
  out.attr.disabled      = 1;
  out.attr.exclude_kernel = 1;
  out.attr.exclude_hv     = 1;
  out.attr.inherit        = 1;
  out.attr.precise_ip     = 2;

#if defined(__x86_64__) || defined(__i386__)
  out.attr.sample_regs_user =
    (1ULL << PERF_REG_X86_SP) | (1ULL << PERF_REG_X86_BP);
#else
  out.attr.sample_regs_user = 0;
#endif

  out.type = event_type_from_name(name);

  int fd = perf_event_open_sys(&out.attr, pid, -1, -1, 0);
  if (fd == -1 && errno == EINVAL && out.attr.precise_ip != 0) {
    out.attr.precise_ip = 0;
    fd                 = perf_event_open_sys(&out.attr, pid, -1, -1, 0);
  }
  if (fd == -1) {
    error = std::format("perf_event_open failed for '{}': {}", name,
                        std::string(std::strerror(errno)));
    return false;
  }

  const size_t page_size = static_cast<size_t>(sysconf(_SC_PAGESIZE));
  const size_t mmap_len  = (1 + kMmapPages) * page_size;
  void* mmap_base =
    mmap(nullptr, mmap_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (mmap_base == MAP_FAILED) {
    error = std::format("mmap failed for '{}': {}", name,
                        std::string(std::strerror(errno)));
    close(fd);
    return false;
  }

  out.fd        = fd;
  out.mmap_base = mmap_base;
  out.mmap_len  = mmap_len;
  return true;
}

void teardown_event(PerfEventHandle& ev) {
  if (ev.mmap_base != MAP_FAILED) {
    munmap(ev.mmap_base, ev.mmap_len);
    ev.mmap_base = MAP_FAILED;
  }
  if (ev.fd != -1) {
    close(ev.fd);
    ev.fd = -1;
  }
}

bool read_buffer(const char* data, size_t data_size, uint64_t offset, void* dst,
                 size_t len) {
  const size_t mask  = data_size - 1;
  const size_t begin = static_cast<size_t>(offset) & mask;
  const size_t first = std::min(len, data_size - begin);
  std::memcpy(dst, data + begin, first);
  if (len > first) {
    std::memcpy(static_cast<char*>(dst) + first, data, len - first);
  }
  return true;
}

void parse_sample(const perf_event_attr& attr, SampleType type,
                  const uint8_t* data, size_t size,
                  std::vector<PerfSample>& out) {
  const uint8_t* p   = data;
  const uint8_t* end = data + size;

  auto need = [&](size_t n) { return static_cast<size_t>(end - p) >= n; };
  auto read_u32 = [&]() {
    uint32_t v = 0;
    if (need(sizeof(v))) {
      std::memcpy(&v, p, sizeof(v));
      p += sizeof(v);
    }
    return v;
  };
  auto read_u64 = [&]() {
    uint64_t v = 0;
    if (need(sizeof(v))) {
      std::memcpy(&v, p, sizeof(v));
      p += sizeof(v);
    }
    return v;
  };

  PerfSample s{};
  s.event_type = type;

  if (attr.sample_type & PERF_SAMPLE_IP) s.ip = static_cast<int64_t>(read_u64());
  if (attr.sample_type & PERF_SAMPLE_TID) {
    s.pid = read_u32();
    s.tid = read_u32();
  }
  if (attr.sample_type & PERF_SAMPLE_TIME)
    s.time_stamp = static_cast<int64_t>(read_u64());
  if (attr.sample_type & PERF_SAMPLE_ADDR)
    s.addr = static_cast<int64_t>(read_u64());
  if (attr.sample_type & PERF_SAMPLE_CPU) {
    s.cpu = read_u32();
    (void)read_u32();
  }

  if (attr.sample_type & PERF_SAMPLE_REGS_USER) {
    const uint64_t abi = read_u64();
    if (abi != PERF_SAMPLE_REGS_ABI_NONE) {
      uint64_t mask = attr.sample_regs_user;
      for (uint32_t i = 0; mask; ++i) {
        if (mask & 1ULL) {
          const uint64_t val = read_u64();
#if defined(__x86_64__) || defined(__i386__)
          if (i == PERF_REG_X86_SP) s.sp = static_cast<int64_t>(val);
          if (i == PERF_REG_X86_BP) s.bp = static_cast<int64_t>(val);
#else
          (void)val;
#endif
        }
        mask >>= 1U;
      }
    }
  }

  out.push_back(std::move(s));
}

void read_event_samples(PerfEventHandle& ev, std::vector<PerfSample>& out) {
  if (ev.mmap_base == MAP_FAILED) return;

  const size_t page_size = static_cast<size_t>(sysconf(_SC_PAGESIZE));
  auto* meta             = static_cast<perf_event_mmap_page*>(ev.mmap_base);
  char* data             = static_cast<char*>(ev.mmap_base) + page_size;
  const size_t data_size = ev.mmap_len - page_size;

  uint64_t head = meta->data_head;
  std::atomic_thread_fence(std::memory_order_acquire);
  uint64_t tail = meta->data_tail;

  while (tail < head) {
    perf_event_header header{};
    read_buffer(data, data_size, tail, &header, sizeof(header));
    if (header.size == 0) break;

    std::vector<uint8_t> record(header.size);
    read_buffer(data, data_size, tail, record.data(), header.size);

    if (header.type == PERF_RECORD_SAMPLE) {
      const uint8_t* payload = record.data() + sizeof(perf_event_header);
      const size_t payload_sz = record.size() - sizeof(perf_event_header);
      parse_sample(ev.attr, ev.type, payload, payload_sz, out);
    }

    tail += header.size;
  }

  meta->data_tail = tail;
}

struct ProcMapInfo {
  std::vector<ProcMapRange> ranges;
  std::optional<int64_t> load_bias;
};

ProcMapInfo read_proc_maps(pid_t pid, const std::string& binary) {
  ProcMapInfo info;
  const auto bin_name = std::filesystem::path(binary).filename().string();
  std::string bin_path;
  try {
    bin_path = std::filesystem::canonical(binary).string();
  } catch (...) {
    bin_path = binary;
  }

  std::ifstream maps(std::format("/proc/{}/maps", pid));
  std::string line;
  while (std::getline(maps, line)) {
    if (line.find(bin_name) == std::string::npos &&
        line.find(bin_path) == std::string::npos)
      continue;

    std::string addr_range;
    std::string perms;
    std::string offset_str;
    std::string dev;
    std::string inode;
    std::string path;

    std::istringstream iss(line);
    if (!(iss >> addr_range >> perms >> offset_str >> dev >> inode))
      continue;
    std::getline(iss, path);
    path = trim_copy(path);
    if (path.empty()) continue;

    auto dash = addr_range.find('-');
    if (dash == std::string::npos) continue;

    auto parse_hex = [](std::string_view sv) -> std::optional<int64_t> {
      if (sv.starts_with("0x")) sv.remove_prefix(2);
      if (sv.empty()) return std::nullopt;
      return static_cast<int64_t>(std::stoull(std::string(sv), nullptr, 16));
    };

    auto start = parse_hex(addr_range.substr(0, dash));
    auto end   = parse_hex(addr_range.substr(dash + 1));
    auto off   = parse_hex(offset_str);
    if (!start || !end || !off) continue;

    info.ranges.push_back(ProcMapRange{*start, *end, *off});
  }

  if (!info.ranges.empty()) {
    std::optional<int64_t> best;
    for (const auto& r : info.ranges) {
      const int64_t bias = r.start - r.offset;
      if (!best || bias < *best) best = bias;
    }
    info.load_bias = best;
  }

  return info;
}

ProcMapInfo wait_for_proc_maps(pid_t pid, const std::string& binary) {
  for (int i = 0; i < 50; ++i) {
    auto info = read_proc_maps(pid, binary);
    if (!info.ranges.empty()) return info;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return {};
}

}  // namespace

std::vector<std::string> PerfEventRecorder::split_event_spec(
  std::string_view spec) {
  std::vector<std::string> out;
  size_t start = 0;
  while (start < spec.size()) {
    auto end = spec.find(',', start);
    if (end == std::string_view::npos) end = spec.size();
    auto tok = trim_copy(spec.substr(start, end - start));
    if (!tok.empty()) out.push_back(std::move(tok));
    start = end + 1;
  }
  return out;
}

PerfRecordResult PerfEventRecorder::record(const std::string& binary,
                                           const std::string& event_spec,
                                           int sample_period, bool verbose) {
  PerfRecordResult result;
  if (!ensure_pfm_init(result.error)) return result;

  auto events = split_event_spec(event_spec);
  if (events.empty()) {
    result.error = "No perf events specified";
    return result;
  }

  pid_t pid = fork();
  if (pid == -1) {
    result.error = std::format("fork failed: {}", std::string(std::strerror(errno)));
    return result;
  }

  if (pid == 0) {
    execl(binary.c_str(), binary.c_str(), nullptr);
    _exit(127);
  }

  std::vector<PerfEventHandle> handles;
  handles.reserve(events.size());

  for (const auto& ev : events) {
    PerfEventHandle h;
    if (!setup_event(pid, ev, sample_period, h, result.error)) {
      for (auto& open : handles) teardown_event(open);
      int status = 0;
      waitpid(pid, &status, 0);
      return result;
    }
    handles.push_back(std::move(h));
  }

  for (auto& h : handles) {
    ioctl(h.fd, PERF_EVENT_IOC_RESET, 0);
    ioctl(h.fd, PERF_EVENT_IOC_ENABLE, 0);
  }

  auto maps = wait_for_proc_maps(pid, binary);
  result.binary_maps = maps.ranges;
  result.load_bias   = maps.load_bias;

  int status = 0;
  waitpid(pid, &status, 0);

  for (auto& h : handles) {
    ioctl(h.fd, PERF_EVENT_IOC_DISABLE, 0);
  }

  for (auto& h : handles) {
    read_event_samples(h, result.samples);
  }

  for (auto& h : handles) teardown_event(h);

  if (verbose && !result.samples.empty()) {
    std::cout << std::format("Collected {} samples via perf_event_open\n",
                             result.samples.size());
  }

  return result;
}
