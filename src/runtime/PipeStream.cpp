#include "runtime/PipeStream.hpp"

#include <array>
#include <stdexcept>
#include <string>
#include <vector>

PipeStream::PipeStream(const std::string& cmd)
  : pipe_(popen(cmd.c_str(), "r")) {
  if (!pipe_) {
    throw std::runtime_error("Failed to open pipe: " + cmd);
  }
}

PipeStream::~PipeStream() {
  if (pipe_) pclose(pipe_);
}

PipeStream::PipeStream(PipeStream&& other) noexcept : pipe_(other.pipe_) {
  other.pipe_ = nullptr;
}

std::string PipeStream::read_all() {
  std::string result;
  std::array<char, 4096> buffer;
  while (fgets(buffer.data(), buffer.size(), pipe_)) {
    result += buffer.data();
  }
  return result;
}

std::vector<std::string> PipeStream::read_lines() {
  std::vector<std::string> lines;
  std::array<char, 4096> buffer;
  while (fgets(buffer.data(), buffer.size(), pipe_)) {
    lines.emplace_back(buffer.data());
  }
  return lines;
}
