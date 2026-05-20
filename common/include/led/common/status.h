#pragma once

#include <string>
#include <utility>

namespace led {

enum class StatusCode {
    ok,
    invalid_argument,
    invalid_state,
    unavailable,
    not_implemented,
    internal_error
};

class Status {
public:
    Status() = default;

    Status(StatusCode code, std::string message)
        : code_(code), message_(std::move(message)) {}

    static Status ok() { return {}; }

    static Status invalidArgument(std::string message) {
        return {StatusCode::invalid_argument, std::move(message)};
    }

    static Status invalidState(std::string message) {
        return {StatusCode::invalid_state, std::move(message)};
    }

    static Status unavailable(std::string message) {
        return {StatusCode::unavailable, std::move(message)};
    }

    static Status notImplemented(std::string message) {
        return {StatusCode::not_implemented, std::move(message)};
    }

    static Status internalError(std::string message) {
        return {StatusCode::internal_error, std::move(message)};
    }

    [[nodiscard]] bool isOk() const { return code_ == StatusCode::ok; }
    [[nodiscard]] StatusCode code() const { return code_; }
    [[nodiscard]] const std::string& message() const { return message_; }

private:
    StatusCode code_{StatusCode::ok};
    std::string message_;
};

}  // namespace led
