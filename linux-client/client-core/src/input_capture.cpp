#include "led/client/input_capture.h"

namespace led::client {

protocol::InputEvent InputCapture::pointerMove(double normalizedX, double normalizedY) {
    protocol::InputEvent event;
    event.kind = protocol::InputKind::pointerMove;
    event.sequence = nextSequence();
    event.normalizedX = normalizedX;
    event.normalizedY = normalizedY;
    event.reliable = false;
    return event;
}

protocol::InputEvent InputCapture::pointerButton(
    double normalizedX,
    double normalizedY,
    std::int32_t button,
    bool down) {
    protocol::InputEvent event;
    event.kind = protocol::InputKind::pointerButton;
    event.sequence = nextSequence();
    event.normalizedX = normalizedX;
    event.normalizedY = normalizedY;
    event.value0 = button;
    event.value1 = down ? 1 : 0;
    event.reliable = true;
    return event;
}

protocol::InputEvent InputCapture::wheel(std::int32_t delta) {
    protocol::InputEvent event;
    event.kind = protocol::InputKind::wheel;
    event.sequence = nextSequence();
    event.value0 = delta;
    event.reliable = false;
    return event;
}

protocol::InputEvent InputCapture::key(std::int32_t virtualKey, bool down) {
    protocol::InputEvent event;
    event.kind = protocol::InputKind::key;
    event.sequence = nextSequence();
    event.value0 = virtualKey;
    event.value1 = down ? 1 : 0;
    event.reliable = true;
    return event;
}

std::uint64_t InputCapture::nextSequence() {
    return nextSequence_++;
}

}  // namespace led::client
