#pragma once

#include "led/protocol/messages.h"

#include <cstdint>

namespace led::client {

class InputCapture {
public:
    [[nodiscard]] protocol::InputEvent pointerMove(double normalizedX, double normalizedY);
    [[nodiscard]] protocol::InputEvent pointerButton(double normalizedX, double normalizedY, std::int32_t button, bool down);
    [[nodiscard]] protocol::InputEvent wheel(std::int32_t delta);
    [[nodiscard]] protocol::InputEvent key(std::int32_t virtualKey, bool down);

private:
    [[nodiscard]] std::uint64_t nextSequence();

    std::uint64_t nextSequence_{1};
};

}  // namespace led::client
