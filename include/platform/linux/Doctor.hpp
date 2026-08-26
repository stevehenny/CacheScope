#pragma once

#include <string>
#include <vector>

#include "core/Models.hpp"

namespace cachescope::linux_platform {

enum class CheckStatus { Pass, Warning, Failure };

struct DoctorCheck {
  std::string name;
  CheckStatus status{CheckStatus::Warning};
  std::string detail;
  std::string remediation;
};

struct DoctorReport {
  PmuCapabilities capabilities;
  std::string kernel_release;
  std::string cpu_model;
  std::vector<DoctorCheck> checks;

  bool supported() const;
  bool permission_ready() const;
};

DoctorReport inspect_system();

}  // namespace cachescope::linux_platform
