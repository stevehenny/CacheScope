#pragma once

#include <cstddef>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace cachescope {
namespace detail {

template <typename T>
std::string format_value(T&& value, std::string_view spec) {
  std::ostringstream output;

  if (spec == "x") {
    using Value = std::remove_cvref_t<T>;
    if constexpr (std::is_integral_v<Value>) {
      output << std::hex << value;
    } else if constexpr (std::is_enum_v<Value>) {
      output << std::hex << static_cast<std::underlying_type_t<Value>>(value);
    } else {
      throw std::invalid_argument("hex format requires an integral value");
    }
  } else if (!spec.empty() && spec.back() == 'f') {
    const auto dot = spec.find('.');
    const auto precision_end = spec.size() - 1;
    if (dot != std::string_view::npos && dot + 1 < precision_end) {
      int precision = 0;
      for (size_t i = dot + 1; i < precision_end; ++i) {
        if (spec[i] < '0' || spec[i] > '9') {
          throw std::invalid_argument("invalid fixed-point precision");
        }
        precision = precision * 10 + (spec[i] - '0');
      }
      output << std::fixed << std::setprecision(precision);
    }
    if (dot != 0 && dot != std::string_view::npos) {
      int width = 0;
      for (size_t i = 0; i < dot; ++i) {
        if (spec[i] < '0' || spec[i] > '9') {
          throw std::invalid_argument("invalid fixed-point width");
        }
        width = width * 10 + (spec[i] - '0');
      }
      output << std::setw(width);
    }
    output << value;
  } else if (spec.empty()) {
    output << value;
  } else {
    throw std::invalid_argument("unsupported format specifier");
  }

  return output.str();
}

inline void append_format(std::string& output, std::string_view pattern) {
  output.append(pattern);
}

template <typename T, typename... Rest>
void append_format(std::string& output, std::string_view pattern, T&& value,
                   Rest&&... rest) {
  const auto open = pattern.find('{');
  if (open == std::string_view::npos) {
    throw std::invalid_argument("too many format arguments");
  }
  const auto close = pattern.find('}', open + 1);
  if (close == std::string_view::npos) {
    throw std::invalid_argument("unterminated format field");
  }

  output.append(pattern.substr(0, open));
  auto spec = pattern.substr(open + 1, close - open - 1);
  if (!spec.empty() && spec.front() == ':') spec.remove_prefix(1);
  output.append(format_value(std::forward<T>(value), spec));
  append_format(output, pattern.substr(close + 1),
                std::forward<Rest>(rest)...);
}

}  // namespace detail

template <typename... Args>
std::string format(std::string_view pattern, Args&&... args) {
  std::string output;
  output.reserve(pattern.size() + sizeof...(Args) * 8);
  detail::append_format(output, pattern, std::forward<Args>(args)...);
  return output;
}

}  // namespace cachescope
