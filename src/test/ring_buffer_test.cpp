#include <linux/perf_event.h>
#include <unistd.h>

#include <cstring>
#include <iostream>
#include <string_view>
#include <vector>

#include "common/Utils.hpp"

namespace {

bool expect(bool condition, std::string_view message) {
  if (condition) return true;
  std::cerr << "FAILED: " << message << '\n';
  return false;
}

void write_wrapped(char* data, size_t size, uint64_t offset,
                   const void* source, size_t length) {
  const size_t begin = static_cast<size_t>(offset) & (size - 1);
  const size_t first = std::min(length, size - begin);
  std::memcpy(data + begin, source, first);
  if (length > first) {
    std::memcpy(data, static_cast<const char*>(source) + first,
                length - first);
  }
}

}  // namespace

int main() {
  const size_t page_size = static_cast<size_t>(sysconf(_SC_PAGESIZE));
  const size_t data_size = page_size;
  std::vector<char> mapping(page_size + data_size);
  auto* metadata =
    reinterpret_cast<perf_event_mmap_page*>(mapping.data());
  char* data = mapping.data() + page_size;

  Utils::PerfEventHandle handle;
  handle.mmap_base = mapping.data();
  handle.mmap_len = mapping.size();
  handle.attr.sample_type = PERF_SAMPLE_IP | PERF_SAMPLE_TID;
  handle.type = SampleType::CACHE_LOAD;

  struct SampleRecord {
    perf_event_header header;
    uint64_t ip;
    uint32_t pid;
    uint32_t tid;
  } sample{{PERF_RECORD_SAMPLE, 0, sizeof(SampleRecord)},
           0x401000, 10, 11};

  const uint64_t start = data_size - 4;
  write_wrapped(data, data_size, start, &sample, sizeof(sample));
  metadata->data_tail = start;
  metadata->data_head = start + sizeof(sample);

  std::vector<PerfSample> samples;
  auto stats = Utils::read_event_samples(handle, samples);
  bool ok = true;
  ok &= expect(stats.samples == 1 && samples.size() == 1,
               "a wrapped sample should parse");
  ok &= expect(samples[0].ip == 0x401000 && samples[0].pid == 10 &&
                 samples[0].tid == 11,
               "wrapped sample fields should be retained");
  ok &= expect(metadata->data_tail == metadata->data_head,
               "consumer tail should advance to head");

  perf_event_header malformed{PERF_RECORD_SAMPLE, 0, 2};
  std::memset(data, 0, data_size);
  std::memcpy(data, &malformed, sizeof(malformed));
  metadata->data_tail = 0;
  metadata->data_head = sizeof(malformed);
  stats = Utils::read_event_samples(handle, samples);
  ok &= expect(stats.malformed == 1,
               "undersized record headers should be rejected safely");
  ok &= expect(metadata->data_tail == metadata->data_head,
               "malformed records should not stall the ring");

  struct LostRecord {
    perf_event_header header;
    uint64_t id;
    uint64_t lost;
  } lost{{PERF_RECORD_LOST, 0, sizeof(LostRecord)}, 5, 17};
  std::memset(data, 0, data_size);
  std::memcpy(data, &lost, sizeof(lost));
  metadata->data_tail = 0;
  metadata->data_head = sizeof(lost);
  stats = Utils::read_event_samples(handle, samples);
  ok &= expect(stats.lost == 17, "lost perf records should be counted");

  sample.header.size = sizeof(perf_event_header) + sizeof(uint64_t);
  std::memset(data, 0, data_size);
  std::memcpy(data, &sample, sample.header.size);
  metadata->data_tail = 0;
  metadata->data_head = sample.header.size;
  stats = Utils::read_event_samples(handle, samples);
  ok &= expect(stats.malformed == 1,
               "truncated sample masks should be rejected");

  handle.mmap_base = MAP_FAILED;
  return ok ? 0 : 1;
}
