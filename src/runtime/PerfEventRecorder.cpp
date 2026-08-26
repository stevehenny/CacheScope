#include "runtime/PerfEventRecorder.hpp"

#include <fcntl.h>
#include <linux/perf_event.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <filesystem>
#include "common/Format.hpp"
#include <iostream>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "common/Utils.hpp"

namespace {

class ScopedFd {
public:
  explicit ScopedFd(int fd = -1) : fd_(fd) {}
  ~ScopedFd() {
    if (fd_ >= 0) close(fd_);
  }
  ScopedFd(const ScopedFd&) = delete;
  ScopedFd& operator=(const ScopedFd&) = delete;
  ScopedFd(ScopedFd&& other) noexcept
    : fd_(std::exchange(other.fd_, -1)) {}
  ScopedFd& operator=(ScopedFd&& other) noexcept {
    if (this != &other) {
      if (fd_ >= 0) close(fd_);
      fd_ = std::exchange(other.fd_, -1);
    }
    return *this;
  }
  int get() const { return fd_; }
  int release() { return std::exchange(fd_, -1); }

private:
  int fd_;
};

int open_pidfd(pid_t pid) {
#ifdef __NR_pidfd_open
  return static_cast<int>(syscall(__NR_pidfd_open, pid, 0));
#else
  (void)pid;
  errno = ENOSYS;
  return -1;
#endif
}

std::vector<pid_t> list_threads(pid_t pid) {
  std::vector<pid_t> tids;
  const std::filesystem::path task_dir = cachescope::format("/proc/{}/task", pid);
  std::error_code error;
  std::filesystem::directory_iterator it(task_dir, error);
  if (error) return tids;

  for (; it != std::filesystem::directory_iterator(); it.increment(error)) {
    if (error) break;
    const auto name = it->path().filename().string();
    pid_t tid = 0;
    const auto [end, conversion_error] =
      std::from_chars(name.data(), name.data() + name.size(), tid);
    if (conversion_error == std::errc() &&
        end == name.data() + name.size()) {
      tids.push_back(tid);
    }
  }
  std::ranges::sort(tids);
  tids.erase(std::unique(tids.begin(), tids.end()), tids.end());
  return tids;
}

bool open_events(pid_t pid, const std::vector<std::string>& events,
                 int sample_period,
                 std::vector<Utils::PerfEventHandle>& handles,
                 std::string& error, bool inherit) {
  const int cpu_count =
    std::max(1, static_cast<int>(sysconf(_SC_NPROCESSORS_ONLN)));

  auto open_event = [&](const std::string& event, int cpu) {
    Utils::PerfEventHandle handle;
    if (!setup_event(pid, event, sample_period, cpu, inherit, handle, error)) {
      for (auto& open : handles) teardown_event(open);
      handles.clear();
      return false;
    }
    handles.push_back(std::move(handle));
    return true;
  };

  for (const auto& event : events) {
    if (Utils::is_ibs_op_event(event)) {
      for (int cpu = 0; cpu < cpu_count; ++cpu) {
        if (!open_event(event, cpu)) return false;
      }
    } else if (!open_event(event, -1)) {
      return false;
    }
  }
  return true;
}

using ThreadEventMap =
  std::unordered_map<pid_t, std::vector<Utils::PerfEventHandle>>;

void enable_events(std::vector<Utils::PerfEventHandle>& handles) {
  for (auto& handle : handles) {
    ioctl(handle.fd, PERF_EVENT_IOC_RESET, 0);
    ioctl(handle.fd, PERF_EVENT_IOC_ENABLE, 0);
  }
}

void enable_events(ThreadEventMap& handles_by_tid) {
  for (auto& [tid, handles] : handles_by_tid) {
    (void)tid;
    enable_events(handles);
  }
}

void disable_events(std::vector<Utils::PerfEventHandle>& handles) {
  for (auto& handle : handles) {
    ioctl(handle.fd, PERF_EVENT_IOC_DISABLE, 0);
  }
}

void disable_events(ThreadEventMap& handles_by_tid) {
  for (auto& [tid, handles] : handles_by_tid) {
    (void)tid;
    disable_events(handles);
  }
}

void teardown_events(std::vector<Utils::PerfEventHandle>& handles) {
  for (auto& handle : handles) teardown_event(handle);
  handles.clear();
}

void teardown_events(ThreadEventMap& handles_by_tid) {
  for (auto& [tid, handles] : handles_by_tid) {
    (void)tid;
    teardown_events(handles);
  }
  handles_by_tid.clear();
}

void merge_read_stats(PerfRecordResult& result,
                      const Utils::PerfReadStats& stats) {
  result.lost_records += stats.lost;
  result.throttled_records += stats.throttled;
  result.malformed_records += stats.malformed;
}

size_t drain_samples(std::vector<Utils::PerfEventHandle>& handles,
                     PerfRecordResult& result) {
  const size_t before = result.samples.size();
  for (auto& handle : handles) {
    merge_read_stats(result, read_event_samples(handle, result.samples));
  }
  return result.samples.size() - before;
}

size_t drain_samples(ThreadEventMap& handles_by_tid,
                     PerfRecordResult& result) {
  const size_t before = result.samples.size();
  for (auto& [tid, handles] : handles_by_tid) {
    (void)tid;
    for (auto& handle : handles) {
      merge_read_stats(result, read_event_samples(handle, result.samples));
    }
  }
  return result.samples.size() - before;
}

std::vector<pollfd> poll_descriptors(
  int pidfd, const std::vector<Utils::PerfEventHandle>& handles) {
  std::vector<pollfd> descriptors;
  descriptors.reserve(handles.size() + 1);
  if (pidfd >= 0) descriptors.push_back({pidfd, POLLIN, 0});
  for (const auto& handle : handles) {
    descriptors.push_back({handle.fd, POLLIN, 0});
  }
  return descriptors;
}

std::vector<pollfd> poll_descriptors(int pidfd,
                                     const ThreadEventMap& handles_by_tid) {
  std::vector<pollfd> descriptors;
  size_t count = 1;
  for (const auto& [tid, handles] : handles_by_tid) {
    (void)tid;
    count += handles.size();
  }
  descriptors.reserve(count);
  if (pidfd >= 0) descriptors.push_back({pidfd, POLLIN, 0});
  for (const auto& [tid, handles] : handles_by_tid) {
    (void)tid;
    for (const auto& handle : handles) {
      descriptors.push_back({handle.fd, POLLIN, 0});
    }
  }
  return descriptors;
}

void set_target_status(PerfRecordResult& result, int status) {
  if (WIFEXITED(status)) result.target_exit_status = WEXITSTATUS(status);
  if (WIFSIGNALED(status)) result.target_signal = WTERMSIG(status);
}

}  // namespace

std::vector<std::string> PerfEventRecorder::split_event_spec(
  std::string_view spec) {
  std::vector<std::string> output;
  size_t start = 0;
  while (start < spec.size()) {
    auto end = spec.find(',', start);
    if (end == std::string_view::npos) end = spec.size();
    auto event = Utils::trim_copy(spec.substr(start, end - start));
    if (!event.empty()) output.push_back(std::move(event));
    start = end + 1;
  }
  return output;
}

PerfRecordResult PerfEventRecorder::record_binary(
  const std::string& binary, const std::string& event_spec,
  int sample_period, bool verbose,
  const std::vector<std::string>& arguments,
  const std::optional<std::filesystem::path>& working_directory) {
  PerfRecordResult result;
  if (!Utils::ensure_pfm_init(result.error)) return result;

  const auto events = split_event_spec(event_spec);
  if (events.empty()) {
    result.error = "No perf events specified";
    return result;
  }

  int status_pipe[2]{-1, -1};
  if (pipe2(status_pipe, O_CLOEXEC) != 0) {
    result.error = cachescope::format("exec-status pipe failed: {}",
                               std::strerror(errno));
    return result;
  }
  ScopedFd status_read(status_pipe[0]);
  ScopedFd status_write(status_pipe[1]);

  const pid_t pid = fork();
  if (pid < 0) {
    result.error = cachescope::format("fork failed: {}", std::strerror(errno));
    return result;
  }

  if (pid == 0) {
    status_read.release();
    if (working_directory &&
        chdir(working_directory->c_str()) != 0) {
      const int child_errno = errno;
      const ssize_t status_write_result = ::write(status_write.get(), &child_errno, sizeof(child_errno));
      (void)status_write_result;
      _exit(126);
    }

    if (raise(SIGSTOP) != 0) {
      const int child_errno = errno;
      const ssize_t status_write_result = ::write(status_write.get(), &child_errno, sizeof(child_errno));
      (void)status_write_result;
      _exit(126);
    }

    std::vector<char*> argv;
    argv.reserve(arguments.size() + 2);
    argv.push_back(const_cast<char*>(binary.c_str()));
    for (const auto& argument : arguments) {
      argv.push_back(const_cast<char*>(argument.c_str()));
    }
    argv.push_back(nullptr);
    execv(binary.c_str(), argv.data());
    const int child_errno = errno;
    const ssize_t status_write_result = ::write(status_write.get(), &child_errno, sizeof(child_errno));
      (void)status_write_result;
    _exit(127);
  }

  status_write = ScopedFd();
  ScopedFd pidfd(open_pidfd(pid));
  auto reap_and_kill = [&]() {
    int cleanup_status = 0;
    (void)kill(pid, SIGKILL);
    while (waitpid(pid, &cleanup_status, 0) < 0 && errno == EINTR) {
    }
  };

  int wait_status = 0;
  pid_t wait_result = -1;
  do {
    wait_result = waitpid(pid, &wait_status, WUNTRACED);
  } while (wait_result < 0 && errno == EINTR);
  if (wait_result < 0) {
    result.error = cachescope::format("waitpid failed: {}", std::strerror(errno));
    reap_and_kill();
    return result;
  }
  if (!WIFSTOPPED(wait_status)) {
    int child_errno = 0;
    ssize_t status_read_result = -1;
    do {
      status_read_result =
        ::read(status_read.get(), &child_errno, sizeof(child_errno));
    } while (status_read_result < 0 && errno == EINTR);
    if (status_read_result < 0) {
      result.error =
        cachescope::format("exec-status read failed: {}", std::strerror(errno));
      set_target_status(result, wait_status);
      return result;
    }
    result.error = child_errno != 0
      ? cachescope::format("target setup failed: {}", std::strerror(child_errno))
      : "child did not stop before exec";
    set_target_status(result, wait_status);
    return result;
  }

  std::vector<Utils::PerfEventHandle> handles;
  if (!open_events(pid, events, sample_period, handles, result.error, true)) {
    reap_and_kill();
    return result;
  }
  enable_events(handles);

  if (kill(pid, SIGCONT) != 0) {
    result.error =
      cachescope::format("failed to resume child: {}", std::strerror(errno));
    teardown_events(handles);
    reap_and_kill();
    return result;
  }

  int exec_errno = 0;
  ssize_t exec_read = -1;
  do {
    exec_read = ::read(status_read.get(), &exec_errno, sizeof(exec_errno));
  } while (exec_read < 0 && errno == EINTR);
  if (exec_read > 0) {
    result.error =
      cachescope::format("execv '{}' failed: {}", binary, std::strerror(exec_errno));
    teardown_events(handles);
    int child_status = 0;
    while (waitpid(pid, &child_status, 0) < 0 && errno == EINTR) {
    }
    set_target_status(result, child_status);
    return result;
  }
  if (exec_read < 0) {
    result.error =
      cachescope::format("exec-status read failed: {}", std::strerror(errno));
    teardown_events(handles);
    reap_and_kill();
    return result;
  }

  const auto maps = Utils::wait_for_proc_maps(pid, binary);
  result.binary_maps = maps.ranges;
  result.load_bias = maps.load_bias;

  bool child_exited = false;
  while (!child_exited) {
    auto descriptors = poll_descriptors(pidfd.get(), handles);
    int poll_result = -1;
    do {
      poll_result = poll(descriptors.data(), descriptors.size(), 250);
    } while (poll_result < 0 && errno == EINTR);
    if (poll_result < 0) {
      result.error = cachescope::format("poll failed: {}", std::strerror(errno));
      teardown_events(handles);
      reap_and_kill();
      return result;
    }

    drain_samples(handles, result);
    int child_status = 0;
    pid_t reaped = -1;
    do {
      reaped = waitpid(pid, &child_status, WNOHANG);
    } while (reaped < 0 && errno == EINTR);
    if (reaped < 0) {
      result.error = cachescope::format("waitpid failed: {}", std::strerror(errno));
      teardown_events(handles);
      reap_and_kill();
      return result;
    }
    if (reaped == pid) {
      set_target_status(result, child_status);
      child_exited = true;
    }
  }

  drain_samples(handles, result);
  disable_events(handles);
  teardown_events(handles);
  if (verbose) {
    std::cout << cachescope::format("Collected {} samples via perf_event_open\n",
                             result.samples.size());
  }
  return result;
}

PerfRecordResult PerfEventRecorder::record_pid(
  pid_t pid, const std::string& binary, const std::string& event_spec,
  int sample_period, bool verbose, MonitorUpdateCallback on_update) {
  PerfRecordResult result;
  if (!Utils::ensure_pfm_init(result.error)) return result;

  const auto events = split_event_spec(event_spec);
  if (events.empty()) {
    result.error = "No perf events specified";
    return result;
  }

  ScopedFd pidfd(open_pidfd(pid));
  if (pidfd.get() < 0) {
    result.error = cachescope::format("pidfd_open failed for pid {}: {}", pid,
                               std::strerror(errno));
    return result;
  }

  ThreadEventMap handles_by_tid;
  const auto initial_tids = list_threads(pid);
  if (initial_tids.empty()) {
    result.error = cachescope::format("No threads found for pid {}", pid);
    return result;
  }
  for (const pid_t tid : initial_tids) {
    auto& handles = handles_by_tid[tid];
    if (!open_events(tid, events, sample_period, handles, result.error,
                     false)) {
      if (errno == ESRCH) {
        handles_by_tid.erase(tid);
        continue;
      }
      teardown_events(handles_by_tid);
      return result;
    }
  }
  enable_events(handles_by_tid);
  if (on_update) on_update(result.samples, 0, false);

  const auto maps = Utils::wait_for_proc_maps(pid, binary);
  result.binary_maps = maps.ranges;
  result.load_bias = maps.load_bias;

  bool exited = false;
  while (!exited) {
    auto descriptors = poll_descriptors(pidfd.get(), handles_by_tid);
    int poll_result = -1;
    do {
      poll_result = poll(descriptors.data(), descriptors.size(), 250);
    } while (poll_result < 0 && errno == EINTR);
    if (poll_result < 0) {
      result.error = cachescope::format("poll failed: {}", std::strerror(errno));
      teardown_events(handles_by_tid);
      return result;
    }
    if (!descriptors.empty() &&
        (descriptors.front().revents & (POLLIN | POLLHUP)) != 0) {
      exited = true;
    }

    const auto current_tids = list_threads(pid);
    std::unordered_set<pid_t> live(current_tids.begin(), current_tids.end());
    for (auto it = handles_by_tid.begin(); it != handles_by_tid.end();) {
      if (!live.contains(it->first)) {
        drain_samples(it->second, result);
        teardown_events(it->second);
        it = handles_by_tid.erase(it);
      } else {
        ++it;
      }
    }
    for (const pid_t tid : current_tids) {
      if (handles_by_tid.contains(tid)) continue;
      auto& handles = handles_by_tid[tid];
      if (!open_events(tid, events, sample_period, handles, result.error,
                       false)) {
        if (errno == ESRCH) {
          handles_by_tid.erase(tid);
          continue;
        }
        teardown_events(handles_by_tid);
        return result;
      }
      enable_events(handles);
    }

    const size_t drained = drain_samples(handles_by_tid, result);
    if (on_update) on_update(result.samples, drained, false);
  }

  const size_t drained = drain_samples(handles_by_tid, result);
  if (on_update) on_update(result.samples, drained, true);
  disable_events(handles_by_tid);
  teardown_events(handles_by_tid);
  if (verbose) {
    std::cout << cachescope::format("Collected {} samples via perf_event_open\n",
                             result.samples.size());
  }
  return result;
}
