#include "common/Utils.hpp"

#include <linux/perf_event.h>
#include <perfmon/pfmlib.h>
#include <perfmon/pfmlib_perf_event.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#include <ctime>

#include "runtime/PerfEventRecorder.hpp"

#if defined(__x86_64__) || defined(__i386__)
#include <asm/perf_regs.h>
#endif

#include <sys/wait.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstring>
#include <filesystem>
#include "common/Format.hpp"
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>

namespace Utils {

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

bool parse_u64(std::string_view sv, uint64_t& out) {
  sv                = trim_copy(sv);
  const char* begin = sv.data();
  const char* end   = sv.data() + sv.size();
  auto [ptr, ec]    = std::from_chars(begin, end, out, 10);
  return ec == std::errc() && ptr == end;
}

bool apply_sysfs_field(const std::filesystem::path& path, uint64_t value,
                       perf_event_attr& attr, std::string& error) {
  std::ifstream file(path);
  if (!file) {
    error = cachescope::format("failed to open {}", path.string());
    return false;
  }
  std::string spec;
  if (!std::getline(file, spec)) {
    error = cachescope::format("failed to read {}", path.string());
    return false;
  }
  spec       = trim_copy(spec);
  auto colon = spec.find(':');
  if (colon == std::string::npos) {
    error = cachescope::format("invalid format spec '{}'", spec);
    return false;
  }
  auto reg        = trim_copy(spec.substr(0, colon));
  auto bits       = trim_copy(spec.substr(colon + 1));
  auto dash       = bits.find('-');
  auto shift_spec = dash == std::string::npos ? bits : bits.substr(0, dash);
  uint64_t shift  = 0;
  if (!parse_u64(shift_spec, shift) || shift >= 64) {
    error = cachescope::format("invalid bit shift '{}'", shift_spec);
    return false;
  }
  const uint64_t shifted = value << shift;
  if (reg == "config") {
    attr.config |= shifted;
  } else if (reg == "config1") {
    attr.config1 |= shifted;
  } else if (reg == "config2") {
    attr.config2 |= shifted;
  } else {
    error = cachescope::format("unsupported perf config field '{}'", reg);
    return false;
  }
  return true;
}

bool ensure_pfm_init(std::string& error) {
  static std::once_flag once;
  static int init_status = PFM_SUCCESS;
  std::call_once(once, []() { init_status = pfm_initialize(); });
  if (init_status != PFM_SUCCESS) {
    error = cachescope::format("libpfm init failed: {}",
                        std::string(pfm_strerror(init_status)));
    return false;
  }
  return true;
}

bool encode_ibs_op_sysfs(perf_event_attr& attr, std::string& error) {
  const std::filesystem::path pmu_path("/sys/bus/event_source/devices/ibs_op");
  std::ifstream type_file(pmu_path / "type");
  if (!type_file) {
    error =
      "ibs_op PMU not available (missing /sys/bus/event_source/devices/"
      "ibs_op/type)";
    return false;
  }
  unsigned int type = 0;
  type_file >> type;
  if (!type_file) {
    error = "failed to parse ibs_op PMU type";
    return false;
  }
  attr.type    = type;
  attr.config  = 0;
  attr.config1 = 0;
  attr.config2 = 0;
  if (!apply_sysfs_field(pmu_path / "format" / "cnt_ctl", 1, attr, error)) {
    return false;
  }
  return true;
}

bool encode_event(const std::string& event, perf_event_attr& attr,
                  std::string& error) {
  pfm_perf_encode_arg_t arg{};
  arg.attr = &attr;
  arg.size = sizeof(arg);
  int ret =
    pfm_get_os_event_encoding(event.c_str(), PFM_PLM3, PFM_OS_PERF_EVENT, &arg);
  if (ret != PFM_SUCCESS) {
    if (ret == PFM_ERR_NOTFOUND && event == "ibs_op") {
      std::string sysfs_error;
      if (encode_ibs_op_sysfs(attr, sysfs_error)) return true;
      error = cachescope::format("libpfm failed to encode '{}': {} ({})", event,
                          std::string(pfm_strerror(ret)), sysfs_error);
      return false;
    }
    error = cachescope::format("libpfm failed to encode '{}': {}", event,
                        std::string(pfm_strerror(ret)));
    return false;
  }
  return true;
}

SampleType event_type_from_name(const std::string& name) {
  auto lower = name;
  std::transform(
    lower.begin(), lower.end(), lower.begin(),
    [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (lower.find("store") != std::string::npos) return SampleType::CACHE_STORE;
  if (lower.find("fault") != std::string::npos) return SampleType::PAGE_FAULT;
  return SampleType::CACHE_LOAD;
}

bool is_ibs_op_event(std::string_view name) {
  std::string lower{name};
  std::transform(
    lower.begin(), lower.end(), lower.begin(),
    [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return lower.find("ibs_op") != std::string::npos;
}

bool setup_event(pid_t pid, const std::string& name, int sample_period, int cpu,
                 bool inherit, PerfEventHandle& out, std::string& error) {
  std::memset(&out.attr, 0, sizeof(out.attr));
  if (!encode_event(name, out.attr, error)) return false;

  const bool ibs_op = is_ibs_op_event(name);
  out.attr.size     = sizeof(out.attr);
  if (ibs_op) {
    out.attr.sample_type = PERF_SAMPLE_IP | PERF_SAMPLE_TID | PERF_SAMPLE_TIME |
                           PERF_SAMPLE_ADDR | PERF_SAMPLE_CPU |
                           PERF_SAMPLE_DATA_SRC | PERF_SAMPLE_PHYS_ADDR;
  } else {
    out.attr.sample_type = PERF_SAMPLE_IP | PERF_SAMPLE_TID | PERF_SAMPLE_TIME |
                           PERF_SAMPLE_ADDR | PERF_SAMPLE_CPU |
                           PERF_SAMPLE_REGS_USER | PERF_SAMPLE_PHYS_ADDR;
  }
  out.attr.sample_period  = static_cast<uint64_t>(sample_period);
  out.attr.disabled       = 1;
  out.attr.exclude_kernel = ibs_op ? 0 : 1;
  out.attr.exclude_hv     = ibs_op ? 0 : 1;
  out.attr.inherit        = inherit ? 1 : 0;
  out.attr.precise_ip     = ibs_op ? 0 : 2;

#if defined(__x86_64__) || defined(__i386__)
  if (!ibs_op) {
    out.attr.sample_regs_user =
      (1ULL << PERF_REG_X86_SP) | (1ULL << PERF_REG_X86_BP);
  }
#else
  out.attr.sample_regs_user = 0;
#endif

  out.type = event_type_from_name(name);
  const uint32_t requested_precise_ip = out.attr.precise_ip;
  auto try_open = [&]() {
    int opened = perf_event_open_sys(&out.attr, pid, cpu, -1, 0);
    if (opened == -1 && out.attr.precise_ip != 0 &&
        (errno == EINVAL || errno == EOPNOTSUPP || errno == ENOENT)) {
      out.attr.precise_ip = 0;
      opened = perf_event_open_sys(&out.attr, pid, cpu, -1, 0);
    }
    return opened;
  };

  int fd = try_open();
  if (fd == -1 && (out.attr.sample_type & PERF_SAMPLE_PHYS_ADDR) != 0 &&
      (errno == EINVAL || errno == EOPNOTSUPP || errno == ENOENT ||
       errno == EACCES || errno == EPERM)) {
    out.attr.sample_type &= ~PERF_SAMPLE_PHYS_ADDR;
    out.attr.precise_ip = requested_precise_ip;
    fd                  = try_open();
  }
  if (fd == -1) {
    error = cachescope::format("perf_event_open failed for '{}': {}", name,
                        std::string(std::strerror(errno)));
    return false;
  }

  const size_t page_size = static_cast<size_t>(sysconf(_SC_PAGESIZE));
  const size_t mmap_len  = (1 + kMmapPages) * page_size;
  void* mmap_base =
    mmap(nullptr, mmap_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (mmap_base == MAP_FAILED) {
    error = cachescope::format("mmap failed for '{}': {}", name,
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

bool parse_sample(const perf_event_attr& attr, SampleType type,
                  const uint8_t* data, size_t size,
                  std::vector<PerfSample>& output) {
  const uint8_t* cursor = data;
  const uint8_t* end = data + size;

  auto read_u32 = [&](uint32_t& value) {
    if (static_cast<size_t>(end - cursor) < sizeof(value)) return false;
    std::memcpy(&value, cursor, sizeof(value));
    cursor += sizeof(value);
    return true;
  };
  auto read_u64 = [&](uint64_t& value) {
    if (static_cast<size_t>(end - cursor) < sizeof(value)) return false;
    std::memcpy(&value, cursor, sizeof(value));
    cursor += sizeof(value);
    return true;
  };

  PerfSample sample{};
  sample.event_type = type;
  uint64_t value64 = 0;
  uint32_t value32 = 0;

  if ((attr.sample_type & PERF_SAMPLE_IP) != 0) {
    if (!read_u64(value64)) return false;
    sample.ip = static_cast<int64_t>(value64);
  }
  if ((attr.sample_type & PERF_SAMPLE_TID) != 0) {
    if (!read_u32(sample.pid) || !read_u32(sample.tid)) return false;
  }
  if ((attr.sample_type & PERF_SAMPLE_TIME) != 0) {
    if (!read_u64(value64)) return false;
    sample.time_stamp = static_cast<int64_t>(value64);
  }
  if ((attr.sample_type & PERF_SAMPLE_ADDR) != 0) {
    if (!read_u64(value64)) return false;
    sample.addr = static_cast<int64_t>(value64);
  }
  if ((attr.sample_type & PERF_SAMPLE_CPU) != 0) {
    if (!read_u32(sample.cpu) || !read_u32(value32)) return false;
  }

  if ((attr.sample_type & PERF_SAMPLE_REGS_USER) != 0) {
    uint64_t abi = 0;
    if (!read_u64(abi)) return false;
    if (abi != PERF_SAMPLE_REGS_ABI_NONE) {
      uint64_t mask = attr.sample_regs_user;
      for (uint32_t index = 0; mask != 0; ++index) {
        if ((mask & 1ULL) != 0) {
          if (!read_u64(value64)) return false;
#if defined(__x86_64__) || defined(__i386__)
          if (index == PERF_REG_X86_SP) {
            sample.sp = static_cast<int64_t>(value64);
          }
          if (index == PERF_REG_X86_BP) {
            sample.bp = static_cast<int64_t>(value64);
          }
#endif
        }
        mask >>= 1U;
      }
    }
  }

  if ((attr.sample_type & PERF_SAMPLE_DATA_SRC) != 0) {
    if (!read_u64(value64)) return false;
    sample.data_source = value64;
  }
  if ((attr.sample_type & PERF_SAMPLE_PHYS_ADDR) != 0) {
    if (!read_u64(value64)) return false;
    sample.phys_addr = static_cast<int64_t>(value64);
  }
  if (cursor != end) return false;
  output.push_back(std::move(sample));
  return true;
}

PerfReadStats read_event_samples(PerfEventHandle& event,
                                 std::vector<PerfSample>& output) {
  PerfReadStats stats;
  if (event.mmap_base == MAP_FAILED) return stats;

  const size_t page_size = static_cast<size_t>(sysconf(_SC_PAGESIZE));
  if (event.mmap_len <= page_size) {
    ++stats.malformed;
    return stats;
  }
  auto* metadata =
    static_cast<perf_event_mmap_page*>(event.mmap_base);
  char* data = static_cast<char*>(event.mmap_base) + page_size;
  const size_t data_size = event.mmap_len - page_size;
  if ((data_size & (data_size - 1)) != 0) {
    ++stats.malformed;
    return stats;
  }

  uint64_t head = metadata->data_head;
  std::atomic_thread_fence(std::memory_order_acquire);
  uint64_t tail = metadata->data_tail;
  if (head < tail) {
    ++stats.malformed;
    metadata->data_tail = head;
    return stats;
  }
  if (head - tail > data_size) {
    ++stats.malformed;
    tail = head - data_size;
  }

  const size_t samples_before = output.size();
  while (tail < head) {
    const uint64_t remaining = head - tail;
    if (remaining < sizeof(perf_event_header)) {
      ++stats.malformed;
      tail = head;
      break;
    }

    perf_event_header header{};
    if (!read_buffer(data, data_size, tail, &header, sizeof(header)) ||
        header.size < sizeof(perf_event_header) ||
        header.size > data_size || header.size > remaining) {
      ++stats.malformed;
      tail = head;
      break;
    }

    std::vector<uint8_t> record(header.size);
    if (!read_buffer(data, data_size, tail, record.data(), record.size())) {
      ++stats.malformed;
      tail = head;
      break;
    }
    const uint8_t* payload = record.data() + sizeof(perf_event_header);
    const size_t payload_size =
      record.size() - sizeof(perf_event_header);

    if (header.type == PERF_RECORD_SAMPLE) {
      if (!parse_sample(event.attr, event.type, payload, payload_size,
                        output)) {
        ++stats.malformed;
      }
    } else if (header.type == PERF_RECORD_LOST) {
      if (payload_size != 2 * sizeof(uint64_t)) {
        ++stats.malformed;
      } else {
        uint64_t lost = 0;
        std::memcpy(&lost, payload + sizeof(uint64_t), sizeof(lost));
        stats.lost += lost;
      }
    } else if (header.type == PERF_RECORD_THROTTLE) {
      ++stats.throttled;
    }
    tail += header.size;
  }

  stats.samples = output.size() - samples_before;
  std::atomic_thread_fence(std::memory_order_release);
  metadata->data_tail = tail;
  return stats;
}

ProcMapInfo read_proc_maps(pid_t pid, const std::string& binary) {
  ProcMapInfo info;
  const auto bin_name = std::filesystem::path(binary).filename().string();
  std::string bin_path;
  try {
    bin_path = std::filesystem::canonical(binary).string();
  } catch (...) {
    bin_path = binary;
  }

  std::ifstream maps(cachescope::format("/proc/{}/maps", pid));
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
    if (!(iss >> addr_range >> perms >> offset_str >> dev >> inode)) continue;
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

}  // namespace Utils
