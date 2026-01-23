#pragma once
#include <fstream>
#include <string>

struct Vendor {
  // Detect CPU vendor from /proc/cpuinfo
  std::string detect_cpu_vendor() {
    std::ifstream cpuinfo("/proc/cpuinfo");
    std::string line;
    while (std::getline(cpuinfo, line)) {
      if (line.find("vendor_id") != std::string::npos) {
        if (line.find("GenuineIntel") != std::string::npos) return "intel";
        if (line.find("AuthenticAMD") != std::string::npos) return "amd";
      }
    }
    return "unknown";
  }
  std::string vendor_str = detect_cpu_vendor();
};
