#pragma once

#include <optional>
#include <utility>
#include <variant>

namespace cachescope {

template <typename T, typename E>
class Result {
public:
  static Result success(T value) { return Result(std::move(value)); }
  static Result failure(E error) { return Result(std::move(error)); }

  [[nodiscard]] bool has_value() const {
    return std::holds_alternative<T>(storage_);
  }
  explicit operator bool() const { return has_value(); }
  T& value() { return std::get<T>(storage_); }
  const T& value() const { return std::get<T>(storage_); }
  E& error() { return std::get<E>(storage_); }
  const E& error() const { return std::get<E>(storage_); }

private:
  explicit Result(T value) : storage_(std::move(value)) {}
  explicit Result(E error) : storage_(std::move(error)) {}
  std::variant<T, E> storage_;
};

template <typename E>
class Result<void, E> {
public:
  static Result success() { return Result(); }
  static Result failure(E error) { return Result(std::move(error)); }

  [[nodiscard]] bool has_value() const { return !error_.has_value(); }
  explicit operator bool() const { return has_value(); }
  E& error() { return *error_; }
  const E& error() const { return *error_; }

private:
  Result() = default;
  explicit Result(E error) : error_(std::move(error)) {}
  std::optional<E> error_;
};

}  // namespace cachescope
