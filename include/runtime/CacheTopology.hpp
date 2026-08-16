#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct CacheInfo {
  int level{};
  int id{};
  std::string type;
  size_t size_bytes{};
  size_t line_size{};
  size_t sets{};
  size_t associativity{};
  std::string shared_cpu_list;
  std::vector<uint32_t> shared_cpus;
  bool detected_from_sysfs{};

  bool contains_cpu(uint32_t cpu) const;
};

class CacheTopology {
public:
  static std::vector<CacheInfo> discover(
    const std::filesystem::path& cpu_root =
      "/sys/devices/system/cpu");

  static std::optional<size_t> parse_size_bytes(std::string_view value);
  static std::vector<uint32_t> parse_cpu_list(std::string_view value);
};
