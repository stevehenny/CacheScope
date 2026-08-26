#include "platform/linux/Doctor.hpp"

#include <sys/utsname.h>
#include <unistd.h>

#include <charconv>
#include <filesystem>
#include <fstream>
#include <string>

#include "runtime/CacheTopology.hpp"

namespace cachescope::linux_platform {
namespace {


std::string cpu_field(std::string_view field) {
  std::ifstream input("/proc/cpuinfo");
  std::string line;
  const std::string prefix = std::string(field) + "\t:";
  while (std::getline(input, line)) {
    if (!line.starts_with(prefix)) continue;
    const auto start = line.find_first_not_of(" \t", prefix.size());
    return start == std::string::npos ? std::string{} : line.substr(start);
  }
  return {};
}

bool has_effective_capability(unsigned bit) {
  std::ifstream input("/proc/self/status");
  std::string line;
  while (std::getline(input, line)) {
    if (!line.starts_with("CapEff:")) continue;
    const auto value = line.substr(line.find_first_not_of(" \t", 7));
    std::uint64_t capabilities = 0;
    const auto [end, error] = std::from_chars(
      value.data(), value.data() + value.size(), capabilities, 16);
    return error == std::errc() && end == value.data() + value.size() &&
           bit < 64 && (capabilities & (1ULL << bit)) != 0;
  }
  return false;
}

}  // namespace

bool DoctorReport::supported() const {
  for (const auto& check : checks) {
    if (check.name == "platform" && check.status == CheckStatus::Failure) {
      return false;
    }
    if (check.name == "pmu" && check.status == CheckStatus::Failure) {
      return false;
    }
  }
  return true;
}

bool DoctorReport::permission_ready() const {
  for (const auto& check : checks) {
    if (check.name == "permissions" &&
        check.status == CheckStatus::Failure) {
      return false;
    }
  }
  return true;
}

DoctorReport inspect_system() {
  DoctorReport report;
  utsname system{};
  if (uname(&system) == 0) report.kernel_release = system.release;
  report.capabilities.cpu_vendor = cpu_field("vendor_id");
  report.cpu_model = cpu_field("model name");

#if defined(__x86_64__)
  report.checks.push_back(
    {"platform", CheckStatus::Pass, "Linux x86-64", {}});
#else
  report.checks.push_back(
    {"platform", CheckStatus::Failure,
     "Production beta supports Linux x86-64 only.",
     "Run CacheScope on Ubuntu 22.04/24.04 x86-64."});
#endif

  const bool perf_pmu =
    std::filesystem::exists("/sys/bus/event_source/devices/cpu");
  const bool ibs =
    std::filesystem::exists("/sys/bus/event_source/devices/ibs_op");
  report.capabilities.perf_events = perf_pmu || ibs;
  report.capabilities.intel_pebs =
    report.capabilities.cpu_vendor == "GenuineIntel" && perf_pmu;
  report.capabilities.amd_ibs =
    report.capabilities.cpu_vendor == "AuthenticAMD" && ibs;
  if (report.capabilities.intel_pebs || report.capabilities.amd_ibs) {
    report.checks.push_back(
      {"pmu", CheckStatus::Pass,
       report.capabilities.intel_pebs ? "Intel PMU detected; PEBS is "
                                        "validated when events are opened."
                                      : "AMD IBS operation PMU detected.",
       {}});
  } else {
    report.checks.push_back(
      {"pmu", CheckStatus::Failure,
       "No supported Intel PEBS or AMD IBS PMU was detected.",
       "Check CPU support and ensure PMU drivers are enabled."});
  }

  int paranoid = 4;
  {
    std::ifstream input("/proc/sys/kernel/perf_event_paranoid");
    input >> paranoid;
  }
  const bool cap_perfmon = has_effective_capability(38);
  const bool permission = geteuid() == 0 || cap_perfmon || paranoid <= 1;
  report.checks.push_back(
    {"permissions",
     permission ? CheckStatus::Pass : CheckStatus::Failure,
     "perf_event_paranoid=" + std::to_string(paranoid) +
       ", CAP_PERFMON=" + (cap_perfmon ? "yes" : "no"),
     permission ? std::string{}
                : "Grant CAP_PERFMON or lower perf_event_paranoid according "
                  "to your security policy."});

  const auto topology = CacheTopology::discover();
  report.checks.push_back(
    {"cache topology",
     topology.empty() ? CheckStatus::Failure : CheckStatus::Pass,
     topology.empty()
       ? "No data/unified cache geometry was discovered."
       : std::to_string(topology.size()) + " cache instances discovered.",
     topology.empty() ? "Verify Linux cache sysfs is mounted." : ""});

  report.capabilities.physical_addresses = false;
  report.capabilities.user_registers =
    report.capabilities.intel_pebs;
  report.capabilities.unavailable.push_back(
    "Physical-address sampling is validated only when a perf event is opened.");
  if (report.capabilities.amd_ibs) {
    report.capabilities.unavailable.push_back(
      "AMD IBS stack-register sampling is unavailable.");
  }
  return report;
}

}  // namespace cachescope::linux_platform
