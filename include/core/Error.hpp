#pragma once

#include <cerrno>
#include <string>
#include <system_error>
#include <utility>

namespace cachescope {

enum class ErrorCategory {
  Usage,
  Unsupported,
  Permission,
  Io,
  Schema,
  Collection,
  Interrupted,
  Internal,
};

struct Error {
  ErrorCategory category{ErrorCategory::Internal};
  std::string code;
  std::string operation;
  std::string message;
  std::error_code system_error;
  std::string remediation;

  static Error from_errno(ErrorCategory category, std::string code,
                          std::string operation, std::string message,
                          int value = errno, std::string remediation = {}) {
    return Error{category, std::move(code), std::move(operation),
                 std::move(message),
                 std::error_code(value, std::generic_category()),
                 std::move(remediation)};
  }
};

inline int exit_status(const Error& error) {
  switch (error.category) {
    case ErrorCategory::Usage: return 2;
    case ErrorCategory::Unsupported: return 3;
    case ErrorCategory::Permission: return 4;
    case ErrorCategory::Io:
    case ErrorCategory::Schema: return 5;
    case ErrorCategory::Collection: return 6;
    case ErrorCategory::Interrupted: return 130;
    case ErrorCategory::Internal: return 70;
  }
  return 70;
}

}  // namespace cachescope
