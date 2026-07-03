#include "runtime/PerfEventRecorder.hpp"

#include <linux/perf_event.h>
#include <perfmon/pfmlib.h>
#include <perfmon/pfmlib_perf_event.h>
#include <chrono>
#include <functional>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#include "common/Utils.hpp"

#if defined(__x86_64__) || defined(__i386__)
#include <asm/perf_regs.h>
#endif

#include <sys/wait.h>

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <format>
#include <iostream>
#include <unordered_map>
#include <unordered_set>
#include <thread>

namespace {

constexpr auto kPollInterval = std::chrono::milliseconds(50);

bool process_alive(pid_t pid) {
  if (pid <= 0) return false;
  if (kill(pid, 0) == 0) return true;
  return errno == EPERM;
}

std::vector<pid_t> list_threads(pid_t pid) {
  std::vector<pid_t> tids;
  const std::filesystem::path task_dir = std::format("/proc/{}/task", pid);
  std::error_code ec;
  std::filesystem::directory_iterator it(task_dir, ec);
  if (ec) return tids;

  for (; it != std::filesystem::directory_iterator(); it.increment(ec)) {
    if (ec) break;
    const auto name = it->path().filename().string();
    pid_t tid       = 0;
    const auto* begin = name.data();
    const auto* end   = name.data() + name.size();
    auto [ptr, conv_ec] = std::from_chars(begin, end, tid);
    if (conv_ec != std::errc() || ptr != end) continue;
    tids.push_back(tid);
  }

  std::sort(tids.begin(), tids.end());
  tids.erase(std::unique(tids.begin(), tids.end()), tids.end());
  return tids;
}

bool open_events(pid_t pid, const std::vector<std::string>& events,
                 int sample_period, std::vector<Utils::PerfEventHandle>& handles,
                 std::string& error) {
  const int cpu_count =
    std::max(1, static_cast<int>(sysconf(_SC_NPROCESSORS_ONLN)));
  handles.reserve(handles.size() + events.size() * static_cast<size_t>(cpu_count));

  auto open_event = [&](const std::string& ev, int cpu) {
    Utils::PerfEventHandle h;
    if (!setup_event(pid, ev, sample_period, cpu, true, h, error)) {
      for (auto& open : handles) teardown_event(open);
      handles.clear();
      return false;
    }
    handles.push_back(std::move(h));
    return true;
  };

  for (const auto& ev : events) {
    if (Utils::is_ibs_op_event(ev)) {
      for (int cpu = 0; cpu < cpu_count; ++cpu) {
        if (!open_event(ev, cpu)) return false;
      }
    } else {
      if (!open_event(ev, -1)) return false;
    }
  }

  return true;
}

using ThreadEventMap =
  std::unordered_map<pid_t, std::vector<Utils::PerfEventHandle>>;

bool open_thread_events(pid_t tid, const std::vector<std::string>& events,
                        int sample_period,
                        std::vector<Utils::PerfEventHandle>& handles,
                        std::string& error) {
  const int cpu_count =
    std::max(1, static_cast<int>(sysconf(_SC_NPROCESSORS_ONLN)));
  handles.reserve(handles.size() + events.size() * static_cast<size_t>(cpu_count));

  auto open_event = [&](const std::string& ev, int cpu) {
    Utils::PerfEventHandle h;
    if (!setup_event(tid, ev, sample_period, cpu, false, h, error)) {
      for (auto& open : handles) teardown_event(open);
      handles.clear();
      return false;
    }
    handles.push_back(std::move(h));
    return true;
  };

  for (const auto& ev : events) {
    if (Utils::is_ibs_op_event(ev)) {
      for (int cpu = 0; cpu < cpu_count; ++cpu) {
        if (!open_event(ev, cpu)) return false;
      }
    } else {
      if (!open_event(ev, -1)) return false;
    }
  }

  return true;
}

void enable_events(std::vector<Utils::PerfEventHandle>& handles) {
  for (auto& h : handles) {
    ioctl(h.fd, PERF_EVENT_IOC_RESET, 0);
    ioctl(h.fd, PERF_EVENT_IOC_ENABLE, 0);
  }
}

void enable_events(ThreadEventMap& handles_by_tid) {
  for (auto& [_, handles] : handles_by_tid) {
    enable_events(handles);
  }
}

void disable_events(std::vector<Utils::PerfEventHandle>& handles) {
  for (auto& h : handles) {
    ioctl(h.fd, PERF_EVENT_IOC_DISABLE, 0);
  }
}

void disable_events(ThreadEventMap& handles_by_tid) {
  for (auto& [_, handles] : handles_by_tid) {
    disable_events(handles);
  }
}

void teardown_events(std::vector<Utils::PerfEventHandle>& handles) {
  for (auto& h : handles) teardown_event(h);
}

void teardown_events(ThreadEventMap& handles_by_tid) {
  for (auto& [_, handles] : handles_by_tid) {
    teardown_events(handles);
  }
  handles_by_tid.clear();
}

size_t drain_samples(std::vector<Utils::PerfEventHandle>& handles,
                     std::vector<PerfSample>& samples) {
  const size_t before = samples.size();
  for (auto& h : handles) {
    read_event_samples(h, samples);
  }
  return samples.size() - before;
}

size_t drain_samples(ThreadEventMap& handles_by_tid,
                     std::vector<PerfSample>& samples) {
  const size_t before = samples.size();
  for (auto& [_, handles] : handles_by_tid) {
    for (auto& h : handles) {
      read_event_samples(h, samples);
    }
  }
  return samples.size() - before;
}

}  // namespace

std::vector<std::string> PerfEventRecorder::split_event_spec(
  std::string_view spec) {
  std::vector<std::string> out;
  size_t start = 0;
  while (start < spec.size()) {
    auto end = spec.find(',', start);
    if (end == std::string_view::npos) end = spec.size();
    auto tok = Utils::trim_copy(spec.substr(start, end - start));
    if (!tok.empty()) out.push_back(std::move(tok));
    start = end + 1;
  }
  return out;
}

PerfRecordResult PerfEventRecorder::record_binary(const std::string& binary,
                                                  const std::string& event_spec,
                                                  int sample_period,
                                                  bool verbose) {
  PerfRecordResult result;
  if (!Utils::ensure_pfm_init(result.error)) return result;

  auto events = split_event_spec(event_spec);
  if (events.empty()) {
    result.error = "No perf events specified";
    return result;
  }

  pid_t pid = fork();
  if (pid == -1) {
    result.error =
      std::format("fork failed: {}", std::string(std::strerror(errno)));
    return result;
  }

  auto cleanup_child = [&]() {
    int cleanup_status = 0;
    (void)kill(pid, SIGKILL);
    while (waitpid(pid, &cleanup_status, 0) == -1 && errno == EINTR) {}
  };

  if (pid == 0) {
    raise(SIGSTOP);
    execl(binary.c_str(), binary.c_str(), nullptr);
    _exit(127);
  }

  int status   = 0;
  pid_t waitrc = -1;
  do {
    waitrc = waitpid(pid, &status, WUNTRACED);
  } while (waitrc == -1 && errno == EINTR);
  if (waitrc == -1) {
    result.error =
      std::format("waitpid failed: {}", std::string(std::strerror(errno)));
    cleanup_child();
    return result;
  }
  if (!WIFSTOPPED(status)) {
    result.error = std::format("child did not stop before exec");
    cleanup_child();
    return result;
  }

  std::vector<Utils::PerfEventHandle> handles;
  if (!open_events(pid, events, sample_period, handles, result.error)) {
    cleanup_child();
    return result;
  }

  enable_events(handles);

  if (kill(pid, SIGCONT) != 0) {
    result.error = std::format("failed to resume child: {}",
                               std::string(std::strerror(errno)));
    teardown_events(handles);
    cleanup_child();
    return result;
  }

  auto maps          = Utils::wait_for_proc_maps(pid, binary);
  result.binary_maps = maps.ranges;
  result.load_bias   = maps.load_bias;

  bool child_exited = false;
  while (!child_exited) {
    drain_samples(handles, result.samples);

    int wait_status = 0;
    pid_t rc        = -1;
    do {
      rc = waitpid(pid, &wait_status, WNOHANG);
    } while (rc == -1 && errno == EINTR);
    if (rc == -1) {
      result.error =
        std::format("waitpid failed: {}", std::string(std::strerror(errno)));
      teardown_events(handles);
      cleanup_child();
      return result;
    }
    if (rc == pid) {
      child_exited = true;
      break;
    }

    std::this_thread::sleep_for(kPollInterval);
  }

  drain_samples(handles, result.samples);
  disable_events(handles);
  teardown_events(handles);

  if (verbose && !result.samples.empty()) {
    std::cout << std::format("Collected {} samples via perf_event_open\n",
                             result.samples.size());
  }

  return result;
}

PerfRecordResult PerfEventRecorder::record_pid(pid_t pid,
                                               const std::string& binary,
                                               const std::string& event_spec,
                                               int sample_period,
                                               bool verbose,
                                               MonitorUpdateCallback on_update) {
  PerfRecordResult result;
  if (!Utils::ensure_pfm_init(result.error)) return result;

  auto events = split_event_spec(event_spec);
  if (events.empty()) {
    result.error = "No perf events specified";
    return result;
  }

  ThreadEventMap handles_by_tid;
  const auto tids = list_threads(pid);
  if (tids.empty()) {
    result.error = std::format("No threads found for pid {}", pid);
    return result;
  }

  for (pid_t tid : tids) {
    auto& handles = handles_by_tid[tid];
    if (!open_thread_events(tid, events, sample_period, handles,
                            result.error)) {
      if (errno == ESRCH) {
        handles_by_tid.erase(tid);
        continue;
      }
      teardown_events(handles_by_tid);
      return result;
    }
  }

  enable_events(handles_by_tid);

  if (on_update) {
    on_update(result.samples, 0, false);
  }

  auto maps          = Utils::wait_for_proc_maps(pid, binary);
  result.binary_maps = maps.ranges;
  result.load_bias   = maps.load_bias;

  while (process_alive(pid)) {
    const auto current_tids = list_threads(pid);
    std::unordered_set<pid_t> live(current_tids.begin(), current_tids.end());

    for (auto it = handles_by_tid.begin(); it != handles_by_tid.end();) {
      if (!live.contains(it->first)) {
        drain_samples(it->second, result.samples);
        teardown_events(it->second);
        it = handles_by_tid.erase(it);
      } else {
        ++it;
      }
    }

    for (pid_t tid : current_tids) {
      if (handles_by_tid.contains(tid)) continue;
      auto& handles = handles_by_tid[tid];
      if (!open_thread_events(tid, events, sample_period, handles,
                              result.error)) {
        if (errno == ESRCH) {
          handles_by_tid.erase(tid);
          continue;
        }
        teardown_events(handles_by_tid);
        return result;
      }
      enable_events(handles);
    }

    const size_t drained = drain_samples(handles_by_tid, result.samples);
    if (on_update) {
      on_update(result.samples, drained, false);
    }
    std::this_thread::sleep_for(kPollInterval);
  }

  const size_t drained = drain_samples(handles_by_tid, result.samples);
  if (on_update) {
    on_update(result.samples, drained, true);
  }
  disable_events(handles_by_tid);
  teardown_events(handles_by_tid);

  if (verbose && !result.samples.empty()) {
    std::cout << std::format("Collected {} samples via perf_event_open\n",
                             result.samples.size());
  }

  return result;
}
