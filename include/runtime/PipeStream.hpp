#pragma once

#include <string>
#include <vector>

class PipeStream {
public:
  explicit PipeStream(const std::string& cmd);
  ~PipeStream();

  // Disable copy, enable move
  PipeStream(const PipeStream&)            = delete;
  PipeStream& operator=(const PipeStream&) = delete;
  PipeStream(PipeStream&& other) noexcept;

  std::string read_all();

  std::vector<std::string> read_lines();

private:
  FILE* pipe_;
};
