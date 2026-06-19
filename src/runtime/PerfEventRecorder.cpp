
#include "runtime/PerfEventRecorder.hpp"

#include <linux/perf_event.h>
#include <perfmon/pfmlib.h>
#include <perfmon/pfmlib_perf_event.h>
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
#include <cctype>
#include <cerrno>
#include <cstring>
#include <format>
#include <iostream>

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

PerfRecordResult PerfEventRecorder::record(const std::string& binary,
                                           const std::string& event_spec,
                                           int sample_period, bool verbose) {
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

  if (pid == 0) {
    raise(SIGSTOP);
    execl(binary.c_str(), binary.c_str(), nullptr);
    _exit(127);
  }

  int status = 0;
  if (waitpid(pid, &status, WUNTRACED) == -1) {
    result.error =
      std::format("waitpid failed: {}", std::string(std::strerror(errno)));
    return result;
  }
  if (!WIFSTOPPED(status)) {
    result.error = "child did not stop before exec";
    return result;
  }

  std::vector<Utils::PerfEventHandle> handles;
  const int cpu_count =
    std::max(1, static_cast<int>(sysconf(_SC_NPROCESSORS_ONLN)));
  handles.reserve(events.size() * static_cast<size_t>(cpu_count));

  auto open_event = [&](const std::string& ev, int cpu) {
    Utils::PerfEventHandle h;
    if (!setup_event(pid, ev, sample_period, cpu, h, result.error)) {
      for (auto& open : handles) teardown_event(open);
      int status = 0;
      waitpid(pid, &status, 0);
      return false;
    }
    handles.push_back(std::move(h));
    return true;
  };

  for (const auto& ev : events) {
    if (Utils::is_ibs_op_event(ev)) {
      for (int cpu = 0; cpu < cpu_count; ++cpu) {
        if (!open_event(ev, cpu)) return result;
      }
    } else {
      if (!open_event(ev, -1)) return result;
    }
  }

  for (auto& h : handles) {
    ioctl(h.fd, PERF_EVENT_IOC_RESET, 0);
    ioctl(h.fd, PERF_EVENT_IOC_ENABLE, 0);
  }

  if (kill(pid, SIGCONT) != 0) {
    result.error = std::format("failed to resume child: {}",
                               std::string(std::strerror(errno)));
    for (auto& h : handles) teardown_event(h);
    return result;
  }

  auto maps          = Utils::wait_for_proc_maps(pid, binary);
  result.binary_maps = maps.ranges;
  result.load_bias   = maps.load_bias;

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
