#pragma once

#include "led/common/status.h"

#include <atomic>
#include <cstdint>
#include <string>

namespace led::client {

class Renderer;

struct X11InputCaptureOptions {
    std::string hostAddress;
    std::uint16_t inputPort{17691};
    std::uint32_t maxEvents{0};
    std::uint32_t idleTimeoutMs{0};
    bool grabPointer{true};
    bool grabKeyboard{true};
    bool hideLocalCursor{false};
    Renderer* renderer{nullptr};
};

struct X11InputCaptureStats {
    std::uint64_t eventsSent{0};
    std::uint64_t pointerMoves{0};
    std::uint64_t pointerButtons{0};
    std::uint64_t wheelEvents{0};
    std::uint64_t keyEvents{0};
    std::uint64_t ignoredEvents{0};
};

class X11InputCapture {
public:
    [[nodiscard]] static bool available();

    Status run(
        const X11InputCaptureOptions& options,
        const std::atomic_bool* stopRequested,
        X11InputCaptureStats& stats);
};

}  // namespace led::client
