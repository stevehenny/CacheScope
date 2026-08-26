#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

#include "core/Error.hpp"
#include "core/Models.hpp"
#include "core/Result.hpp"

namespace cachescope::trace {

inline constexpr std::uint16_t kTraceMajorVersion = 1;
inline constexpr std::uint16_t kTraceMinorVersion = 0;
inline constexpr std::uint32_t kEndianMarker = 0x01020304U;
inline constexpr std::uint32_t kMaxMetadataSize = 4U * 1024U * 1024U;
inline constexpr std::uint32_t kMaxFrameSize = 1U * 1024U * 1024U;

enum class FrameType : std::uint16_t {
  Sample = 1,
  Mmap = 2,
  Mmap2 = 3,
  Comm = 4,
  Fork = 5,
  Exit = 6,
  Lost = 7,
  Throttle = 8,
  Unthrottle = 9,
  Completed = 10,
};

enum class SampleKind : std::uint8_t {
  CacheLoad = 0,
  CacheStore = 1,
  PageFault = 2,
};

enum SamplePresence : std::uint64_t {
  HasIp = 1ULL << 0U,
  HasVirtualAddress = 1ULL << 1U,
  HasPhysicalAddress = 1ULL << 2U,
  HasStackPointer = 1ULL << 3U,
  HasBasePointer = 1ULL << 4U,
  HasDataSource = 1ULL << 5U,
};

struct TraceSample {
  std::uint64_t presence{};
  std::uint32_t event_id{};
  std::uint32_t pid{};
  std::uint32_t tid{};
  std::uint32_t cpu{};
  std::uint64_t timestamp{};
  std::uint64_t ip{};
  std::uint64_t virtual_address{};
  std::uint64_t physical_address{};
  std::uint64_t stack_pointer{};
  std::uint64_t base_pointer{};
  std::uint64_t data_source{};
  SampleKind kind{SampleKind::CacheLoad};
};

struct TraceData {
  TraceMetadata metadata;
  std::vector<TraceSample> samples;
  SampleQuality quality;
  std::uint16_t minor_version{};
};

Result<void, Error> write(const std::filesystem::path& path,
                          const TraceMetadata& metadata,
                          const std::vector<TraceSample>& samples,
                          std::uint64_t lost = 0,
                          std::uint64_t throttled = 0);

Result<TraceData, Error> read(const std::filesystem::path& path,
                              std::size_t max_samples = 1'000'000);

std::uint32_t crc32(const std::uint8_t* data, std::size_t size);

}  // namespace cachescope::trace
