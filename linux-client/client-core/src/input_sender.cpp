#include "led/client/input_sender.h"

#include <chrono>
#include <vector>

namespace led::client {

namespace {

std::uint64_t currentUnixTimeUs() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now).count());
}

}  // namespace

Status InputSender::open(const std::string& hostAddress, std::uint16_t inputPort) {
    target_ = transport::UdpEndpoint{hostAddress, inputPort};
    return socket_.open();
}

Status InputSender::send(const protocol::InputEvent& event) const {
    auto stampedEvent = event;
    stampedEvent.sendTimeUs = currentUnixTimeUs();
    const auto text = protocol::serializeInputEvent(stampedEvent);
    const std::vector<std::uint8_t> bytes(text.begin(), text.end());
    return socket_.sendTo(bytes, target_);
}

void InputSender::close() {
    socket_.close();
}

}  // namespace led::client
