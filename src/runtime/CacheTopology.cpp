#include "runtime/CacheTopology.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <fstream>
#include <ranges>
#include <string>
#include <unordered_set>

namespace {

std::optional<std::string> read_value(const std::filesystem::path& path) {
  std::ifstream input(path);
  std::string value;
  if (!input || !std::getline(input, value)) return std::nullopt;
  while (!value.empty() &&
         std::isspace(static_cast<unsigned char>(value.back()))) {
    value.pop_back();
  }
  return value;
}

template <typename T>
std::optional<T> parse_integer(std::string_view value) {
  T parsed{};
  const auto* begin = value.data();
  const auto* end   = begin + value.size();
  const auto [ptr, error] = std::from_chars(begin, end, parsed);
  if (error != std::errc() || ptr != end) return std::nullopt;
  return parsed;
}

bool numbered_name(std::string_view value, std::string_view prefix,
                   uint32_t& number) {
  if (!value.starts_with(prefix)) return false;
  const auto suffix = value.substr(prefix.size());
  auto parsed       = parse_integer<uint32_t>(suffix);
  if (!parsed) return false;
  number = *parsed;
  return true;
}

CacheInfo fallback_l1() {
  CacheInfo cache;
  cache.level               = 1;
  cache.id                  = 0;
  cache.type                = "Data";
  cache.size_bytes          = 32 * 1024;
  cache.line_size           = 64;
  cache.sets                = 64;
  cache.associativity       = 8;
  cache.shared_cpu_list     = "all (fallback)";
  cache.detected_from_sysfs = false;
  return cache;
}

}  // namespace

bool CacheInfo::contains_cpu(uint32_t cpu) const {
  return shared_cpus.empty() ||
         std::ranges::find(shared_cpus, cpu) != shared_cpus.end();
}

std::optional<size_t> CacheTopology::parse_size_bytes(std::string_view value) {
  while (!value.empty() &&
         std::isspace(static_cast<unsigned char>(value.front()))) {
    value.remove_prefix(1);
  }
  while (!value.empty() &&
         std::isspace(static_cast<unsigned char>(value.back()))) {
    value.remove_suffix(1);
  }
  if (value.empty()) return std::nullopt;

  size_t multiplier = 1;
  const char suffix = value.back();
  if (!std::isdigit(static_cast<unsigned char>(suffix))) {
    value.remove_suffix(1);
    switch (static_cast<char>(
      std::toupper(static_cast<unsigned char>(suffix)))) {
      case 'K':
        multiplier = 1024;
        break;
      case 'M':
        multiplier = 1024 * 1024;
        break;
      case 'G':
        multiplier = 1024ULL * 1024ULL * 1024ULL;
        break;
      case 'B':
        break;
      default:
        return std::nullopt;
    }
  }

  auto amount = parse_integer<size_t>(value);
  if (!amount || *amount > static_cast<size_t>(-1) / multiplier) {
    return std::nullopt;
  }
  return *amount * multiplier;
}

std::vector<uint32_t> CacheTopology::parse_cpu_list(std::string_view value) {
  std::vector<uint32_t> cpus;
  size_t start = 0;
  while (start < value.size()) {
    const size_t comma = value.find(',', start);
    const size_t end =
      comma == std::string_view::npos ? value.size() : comma;
    const auto token = value.substr(start, end - start);
    const size_t dash = token.find('-');

    auto first = parse_integer<uint32_t>(
      token.substr(0, dash == std::string_view::npos ? token.size() : dash));
    if (first) {
      uint32_t last = *first;
      if (dash != std::string_view::npos) {
        auto parsed_last = parse_integer<uint32_t>(token.substr(dash + 1));
        if (!parsed_last || *parsed_last < *first) {
          first.reset();
        } else {
          last = *parsed_last;
        }
      }
      if (first) {
        for (uint32_t cpu = *first; cpu <= last; ++cpu) {
          cpus.push_back(cpu);
          if (cpu == static_cast<uint32_t>(-1)) break;
        }
      }
    }

    if (comma == std::string_view::npos) break;
    start = comma + 1;
  }

  std::ranges::sort(cpus);
  cpus.erase(std::unique(cpus.begin(), cpus.end()), cpus.end());
  return cpus;
}

std::vector<CacheInfo> CacheTopology::discover(
  const std::filesystem::path& cpu_root) {
  std::vector<CacheInfo> caches;
  std::unordered_set<std::string> instances;
  std::error_code error;

  for (std::filesystem::directory_iterator cpu_it(cpu_root, error), end;
       !error && cpu_it != end; cpu_it.increment(error)) {
    uint32_t cpu = 0;
    if (!numbered_name(cpu_it->path().filename().string(), "cpu", cpu)) {
      continue;
    }

    const auto cache_root = cpu_it->path() / "cache";
    std::error_code cache_error;
    for (std::filesystem::directory_iterator index_it(cache_root, cache_error),
         index_end;
         !cache_error && index_it != index_end;
         index_it.increment(cache_error)) {
      uint32_t index = 0;
      if (!numbered_name(index_it->path().filename().string(), "index",
                         index)) {
        continue;
      }
      (void)index;

      const auto path = index_it->path();
      const auto type = read_value(path / "type");
      const auto level_value = read_value(path / "level");
      const auto id_value = read_value(path / "id");
      const auto size_value = read_value(path / "size");
      const auto line_value = read_value(path / "coherency_line_size");
      const auto sets_value = read_value(path / "number_of_sets");
      const auto ways_value = read_value(path / "ways_of_associativity");
      const auto shared_value = read_value(path / "shared_cpu_list");
      if (!type || (*type != "Data" && *type != "Unified") ||
          !level_value || !size_value || !line_value || !sets_value ||
          !ways_value) {
        continue;
      }

      auto level = parse_integer<int>(*level_value);
      auto id = id_value ? parse_integer<int>(*id_value) : std::optional<int>{0};
      auto size = parse_size_bytes(*size_value);
      auto line = parse_integer<size_t>(*line_value);
      auto sets = parse_integer<size_t>(*sets_value);
      auto ways = parse_integer<size_t>(*ways_value);
      if (!level || !id || !size || !line || !sets || !ways || *line == 0 ||
          *sets == 0 || *ways == 0) {
        continue;
      }

      const std::string shared =
        shared_value ? *shared_value : std::to_string(cpu);
      const std::string key = std::to_string(*level) + ":" + *type + ":" +
                              std::to_string(*id) + ":" + shared;
      if (!instances.insert(key).second) continue;

      CacheInfo cache;
      cache.level               = *level;
      cache.id                  = *id;
      cache.type                = *type;
      cache.size_bytes          = *size;
      cache.line_size           = *line;
      cache.sets                = *sets;
      cache.associativity       = *ways;
      cache.shared_cpu_list     = shared;
      cache.shared_cpus         = parse_cpu_list(shared);
      cache.detected_from_sysfs = true;
      caches.push_back(std::move(cache));
    }
  }

  std::ranges::sort(caches, [](const CacheInfo& a, const CacheInfo& b) {
    if (a.level != b.level) return a.level < b.level;
    if (a.type != b.type) return a.type < b.type;
    if (a.id != b.id) return a.id < b.id;
    return a.shared_cpu_list < b.shared_cpu_list;
  });

  if (caches.empty()) caches.push_back(fallback_l1());
  return caches;
}
