#include "trace/Trace.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace cachescope::trace {
namespace {

constexpr std::array<std::uint8_t, 8> kMagic{
  'C', 'S', 'T', 'R', 'A', 'C', 'E', '\0'};

class FileDescriptor {
public:
  explicit FileDescriptor(int fd = -1) : fd_(fd) {}
  ~FileDescriptor() {
    if (fd_ >= 0) close(fd_);
  }
  FileDescriptor(const FileDescriptor&) = delete;
  FileDescriptor& operator=(const FileDescriptor&) = delete;
  int get() const { return fd_; }

private:
  int fd_;
};

Error io_error(std::string code, std::string operation, std::string message,
               int value = errno) {
  return Error::from_errno(ErrorCategory::Io, std::move(code),
                           std::move(operation), std::move(message), value);
}

Error schema_error(std::string code, std::string operation,
                   std::string message) {
  return Error{ErrorCategory::Schema, std::move(code), std::move(operation),
               std::move(message), {}, {}};
}

template <typename T>
void append_le(std::vector<std::uint8_t>& out, T value) {
  static_assert(std::is_unsigned_v<T>);
  for (std::size_t i = 0; i < sizeof(T); ++i) {
    out.push_back(static_cast<std::uint8_t>(value & 0xffU));
    value >>= 8U;
  }
}

template <typename T>
bool take_le(std::span<const std::uint8_t>& input, T& value) {
  static_assert(std::is_unsigned_v<T>);
  if (input.size() < sizeof(T)) return false;
  value = 0;
  for (std::size_t i = 0; i < sizeof(T); ++i) {
    value |= static_cast<T>(input[i]) << (i * 8U);
  }
  input = input.subspan(sizeof(T));
  return true;
}

bool write_all(int fd, std::span<const std::uint8_t> data) {
  while (!data.empty()) {
    const auto written = ::write(fd, data.data(), data.size());
    if (written < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (written == 0) {
      errno = EIO;
      return false;
    }
    data = data.subspan(static_cast<std::size_t>(written));
  }
  return true;
}

enum class ReadState { Ok, Eof, Error };

ReadState read_exact(int fd, std::span<std::uint8_t> output,
                     bool allow_initial_eof = false) {
  std::size_t total = 0;
  while (total < output.size()) {
    const auto count = ::read(fd, output.data() + total, output.size() - total);
    if (count < 0) {
      if (errno == EINTR) continue;
      return ReadState::Error;
    }
    if (count == 0) {
      if (allow_initial_eof && total == 0) return ReadState::Eof;
      errno = EPIPE;
      return ReadState::Error;
    }
    total += static_cast<std::size_t>(count);
  }
  return ReadState::Ok;
}

nlohmann::ordered_json metadata_json(const TraceMetadata& metadata) {
  nlohmann::ordered_json capabilities{
    {"cpu_vendor", metadata.capabilities.cpu_vendor},
    {"perf_events", metadata.capabilities.perf_events},
    {"intel_pebs", metadata.capabilities.intel_pebs},
    {"amd_ibs", metadata.capabilities.amd_ibs},
    {"physical_addresses", metadata.capabilities.physical_addresses},
    {"user_registers", metadata.capabilities.user_registers},
    {"unavailable", metadata.capabilities.unavailable},
  };
  return nlohmann::ordered_json{
    {"tool_version", metadata.tool_version},
    {"schema_version", metadata.schema_version},
    {"command", metadata.command},
    {"target_path", metadata.target_path},
    {"target_build_id", metadata.target_build_id},
    {"kernel_release", metadata.kernel_release},
    {"cpu_model", metadata.cpu_model},
    {"capabilities", std::move(capabilities)},
    {"event_encodings", metadata.event_encodings},
    {"clock_source", metadata.clock_source},
    {"start_time_unix_ns", metadata.start_time_unix_ns},
  };
}

TraceMetadata parse_metadata(const nlohmann::json& value) {
  TraceMetadata metadata;
  metadata.tool_version = value.value("tool_version", "");
  metadata.schema_version = value.value("schema_version", "1.0");
  metadata.command = value.value("command", std::vector<std::string>{});
  metadata.target_path = value.value("target_path", "");
  metadata.target_build_id = value.value("target_build_id", "");
  metadata.kernel_release = value.value("kernel_release", "");
  metadata.cpu_model = value.value("cpu_model", "");
  metadata.event_encodings =
    value.value("event_encodings", std::vector<std::string>{});
  metadata.clock_source = value.value("clock_source", "CLOCK_MONOTONIC");
  metadata.start_time_unix_ns =
    value.value("start_time_unix_ns", std::uint64_t{});
  if (const auto it = value.find("capabilities"); it != value.end()) {
    metadata.capabilities.cpu_vendor = it->value("cpu_vendor", "");
    metadata.capabilities.perf_events = it->value("perf_events", false);
    metadata.capabilities.intel_pebs = it->value("intel_pebs", false);
    metadata.capabilities.amd_ibs = it->value("amd_ibs", false);
    metadata.capabilities.physical_addresses =
      it->value("physical_addresses", false);
    metadata.capabilities.user_registers =
      it->value("user_registers", false);
    metadata.capabilities.unavailable =
      it->value("unavailable", std::vector<std::string>{});
  }
  return metadata;
}

std::vector<std::uint8_t> encode_sample(const TraceSample& sample) {
  std::vector<std::uint8_t> payload;
  payload.reserve(89);
  append_le(payload, sample.presence);
  append_le(payload, sample.event_id);
  append_le(payload, sample.pid);
  append_le(payload, sample.tid);
  append_le(payload, sample.cpu);
  append_le(payload, sample.timestamp);
  append_le(payload, sample.ip);
  append_le(payload, sample.virtual_address);
  append_le(payload, sample.physical_address);
  append_le(payload, sample.stack_pointer);
  append_le(payload, sample.base_pointer);
  append_le(payload, sample.data_source);
  payload.push_back(static_cast<std::uint8_t>(sample.kind));
  return payload;
}

bool decode_sample(std::span<const std::uint8_t> payload, TraceSample& sample) {
  std::uint8_t kind = 0;
  if (!take_le(payload, sample.presence) ||
      !take_le(payload, sample.event_id) ||
      !take_le(payload, sample.pid) ||
      !take_le(payload, sample.tid) ||
      !take_le(payload, sample.cpu) ||
      !take_le(payload, sample.timestamp) ||
      !take_le(payload, sample.ip) ||
      !take_le(payload, sample.virtual_address) ||
      !take_le(payload, sample.physical_address) ||
      !take_le(payload, sample.stack_pointer) ||
      !take_le(payload, sample.base_pointer) ||
      !take_le(payload, sample.data_source) || payload.size() != 1) {
    return false;
  }
  kind = payload[0];
  if (kind > static_cast<std::uint8_t>(SampleKind::PageFault)) return false;
  sample.kind = static_cast<SampleKind>(kind);
  return true;
}

bool write_frame(int fd, FrameType type, std::uint16_t flags,
                 std::uint64_t sequence,
                 std::span<const std::uint8_t> payload) {
  std::vector<std::uint8_t> header;
  header.reserve(20);
  append_le(header, static_cast<std::uint16_t>(type));
  append_le(header, flags);
  append_le(header, static_cast<std::uint32_t>(payload.size()));
  append_le(header, sequence);
  append_le(header, crc32(payload.data(), payload.size()));
  return write_all(fd, header) && write_all(fd, payload);
}

}  // namespace

std::uint32_t crc32(const std::uint8_t* data, std::size_t size) {
  std::uint32_t crc = 0xffffffffU;
  for (std::size_t i = 0; i < size; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) {
      const std::uint32_t mask =
        static_cast<std::uint32_t>(-(static_cast<std::int32_t>(crc & 1U)));
      crc = (crc >> 1U) ^ (0xedb88320U & mask);
    }
  }
  return ~crc;
}

Result<void, Error> write(const std::filesystem::path& path,
                          const TraceMetadata& metadata,
                          const std::vector<TraceSample>& samples,
                          std::uint64_t lost,
                          std::uint64_t throttled) {
  const auto metadata_text = metadata_json(metadata).dump();
  if (metadata_text.size() > kMaxMetadataSize) {
    return Result<void, Error>::failure(schema_error(
      "trace.metadata_too_large", "write trace",
      "Trace metadata exceeds the 4 MiB limit"));
  }

  const int raw_fd =
    ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (raw_fd < 0) {
    return Result<void, Error>::failure(
      io_error("trace.open_failed", "open trace", path.string()));
  }
  FileDescriptor fd(raw_fd);
  if (fchmod(fd.get(), S_IRUSR | S_IWUSR) != 0) {
    return Result<void, Error>::failure(
      io_error("trace.chmod_failed", "secure trace", path.string()));
  }

  std::vector<std::uint8_t> header(kMagic.begin(), kMagic.end());
  append_le(header, kTraceMajorVersion);
  append_le(header, kTraceMinorVersion);
  append_le(header, kEndianMarker);
  append_le(header, static_cast<std::uint32_t>(metadata_text.size()));
  const auto metadata_bytes = std::span(
    reinterpret_cast<const std::uint8_t*>(metadata_text.data()),
    metadata_text.size());
  if (!write_all(fd.get(), header) || !write_all(fd.get(), metadata_bytes)) {
    return Result<void, Error>::failure(
      io_error("trace.write_failed", "write trace header", path.string()));
  }

  std::uint64_t sequence = 0;
  for (const auto& sample : samples) {
    const auto payload = encode_sample(sample);
    if (!write_frame(fd.get(), FrameType::Sample, 0, sequence++, payload)) {
      return Result<void, Error>::failure(
        io_error("trace.write_failed", "write sample frame", path.string()));
    }
  }

  for (const auto& [type, count] :
       {std::pair{FrameType::Lost, lost},
        std::pair{FrameType::Throttle, throttled}}) {
    if (count == 0) continue;
    std::vector<std::uint8_t> payload;
    append_le(payload, count);
    if (!write_frame(fd.get(), type, 0, sequence++, payload)) {
      return Result<void, Error>::failure(
        io_error("trace.write_failed", "write quality frame", path.string()));
    }
  }

  if (!write_frame(fd.get(), FrameType::Completed, 0, sequence, {})) {
    return Result<void, Error>::failure(
      io_error("trace.write_failed", "complete trace", path.string()));
  }
  if (fsync(fd.get()) != 0) {
    return Result<void, Error>::failure(
      io_error("trace.sync_failed", "sync trace", path.string()));
  }
  return Result<void, Error>::success();
}

Result<TraceData, Error> read(const std::filesystem::path& path,
                              std::size_t max_samples) {
  const int raw_fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (raw_fd < 0) {
    return Result<TraceData, Error>::failure(
      io_error("trace.open_failed", "open trace", path.string()));
  }
  FileDescriptor fd(raw_fd);

  std::array<std::uint8_t, 20> raw_header{};
  if (read_exact(fd.get(), raw_header) != ReadState::Ok) {
    return Result<TraceData, Error>::failure(schema_error(
      "trace.truncated_header", "read trace", "Trace header is truncated"));
  }
  if (!std::equal(kMagic.begin(), kMagic.end(), raw_header.begin())) {
    return Result<TraceData, Error>::failure(schema_error(
      "trace.bad_magic", "read trace", "File is not a CacheScope trace"));
  }

  auto header = std::span<const std::uint8_t>(raw_header).subspan(kMagic.size());
  std::uint16_t major = 0;
  std::uint16_t minor = 0;
  std::uint32_t endian = 0;
  std::uint32_t metadata_size = 0;
  if (!take_le(header, major) || !take_le(header, minor) ||
      !take_le(header, endian) || !take_le(header, metadata_size)) {
    return Result<TraceData, Error>::failure(schema_error(
      "trace.invalid_header", "read trace", "Trace header is invalid"));
  }
  if (major != kTraceMajorVersion) {
    return Result<TraceData, Error>::failure(schema_error(
      "trace.unsupported_major", "read trace",
      "Unsupported trace major version " + std::to_string(major)));
  }
  if (endian != kEndianMarker) {
    return Result<TraceData, Error>::failure(schema_error(
      "trace.unsupported_endian", "read trace",
      "Trace endianness marker is invalid"));
  }
  if (metadata_size > kMaxMetadataSize) {
    return Result<TraceData, Error>::failure(schema_error(
      "trace.metadata_too_large", "read trace",
      "Trace metadata exceeds the 4 MiB limit"));
  }

  std::vector<std::uint8_t> metadata_bytes(metadata_size);
  if (read_exact(fd.get(), metadata_bytes) != ReadState::Ok) {
    return Result<TraceData, Error>::failure(schema_error(
      "trace.truncated_metadata", "read trace",
      "Trace metadata is truncated"));
  }

  TraceData data;
  data.minor_version = minor;
  try {
    data.metadata = parse_metadata(nlohmann::json::parse(metadata_bytes));
  } catch (const std::exception& exception) {
    return Result<TraceData, Error>::failure(schema_error(
      "trace.invalid_metadata", "parse trace metadata", exception.what()));
  }

  std::uint64_t expected_sequence = 0;
  while (true) {
    std::array<std::uint8_t, 20> raw_frame{};
    const auto state = read_exact(fd.get(), raw_frame, true);
    if (state == ReadState::Eof) break;
    if (state == ReadState::Error) {
      return Result<TraceData, Error>::failure(schema_error(
        "trace.truncated_frame_header", "read trace",
        "Trace frame header is truncated"));
    }

    auto frame = std::span<const std::uint8_t>(raw_frame);
    std::uint16_t raw_type = 0;
    std::uint16_t flags = 0;
    std::uint32_t payload_size = 0;
    std::uint64_t sequence = 0;
    std::uint32_t checksum = 0;
    if (!take_le(frame, raw_type) || !take_le(frame, flags) ||
        !take_le(frame, payload_size) || !take_le(frame, sequence) ||
        !take_le(frame, checksum)) {
      return Result<TraceData, Error>::failure(schema_error(
        "trace.invalid_frame_header", "read trace",
        "Trace frame header is invalid"));
    }
    (void)flags;
    if (payload_size > kMaxFrameSize) {
      return Result<TraceData, Error>::failure(schema_error(
        "trace.frame_too_large", "read trace",
        "Trace frame exceeds the 1 MiB limit"));
    }
    if (sequence != expected_sequence++) {
      return Result<TraceData, Error>::failure(schema_error(
        "trace.bad_sequence", "read trace",
        "Trace frame sequence is not contiguous"));
    }

    std::vector<std::uint8_t> payload(payload_size);
    if (read_exact(fd.get(), payload) != ReadState::Ok) {
      return Result<TraceData, Error>::failure(schema_error(
        "trace.truncated_payload", "read trace",
        "Trace frame payload is truncated"));
    }
    if (crc32(payload.data(), payload.size()) != checksum) {
      return Result<TraceData, Error>::failure(schema_error(
        "trace.checksum_mismatch", "read trace",
        "Trace frame checksum does not match"));
    }

    const auto type = static_cast<FrameType>(raw_type);
    if (type == FrameType::Sample) {
      TraceSample sample;
      if (!decode_sample(payload, sample)) {
        ++data.quality.malformed_records;
        return Result<TraceData, Error>::failure(schema_error(
          "trace.invalid_sample", "decode sample",
          "Sample frame has an invalid payload"));
      }
      if (max_samples == 0) {
        ++data.quality.evicted_samples;
        data.quality.truncated = true;
      } else {
        if (data.samples.size() == max_samples) {
          data.samples.erase(data.samples.begin());
          ++data.quality.evicted_samples;
          data.quality.truncated = true;
        }
        data.samples.push_back(sample);
      }
      ++data.quality.samples;
    } else if (type == FrameType::Lost || type == FrameType::Throttle) {
      auto count_payload = std::span<const std::uint8_t>(payload);
      std::uint64_t count = 0;
      if (!take_le(count_payload, count) || !count_payload.empty()) {
        return Result<TraceData, Error>::failure(schema_error(
          "trace.invalid_quality_record", "decode quality frame",
          "Quality frame has an invalid payload"));
      }
      if (type == FrameType::Lost) data.quality.lost += count;
      else data.quality.throttled += count;
    } else if (type == FrameType::Completed) {
      if (!payload.empty()) {
        return Result<TraceData, Error>::failure(schema_error(
          "trace.invalid_completed", "decode completed frame",
          "Completed frame must have an empty payload"));
      }
      data.quality.completed = true;
    } else if (type != FrameType::Mmap && type != FrameType::Mmap2 &&
               type != FrameType::Comm && type != FrameType::Fork &&
               type != FrameType::Exit && type != FrameType::Unthrottle) {
      ++data.quality.unknown_records;
    }
  }

  if (!data.quality.completed) {
    return Result<TraceData, Error>::failure(schema_error(
      "trace.incomplete", "read trace",
      "Trace has no completed frame and may be from an interrupted capture"));
  }
  return Result<TraceData, Error>::success(std::move(data));
}

}  // namespace cachescope::trace
