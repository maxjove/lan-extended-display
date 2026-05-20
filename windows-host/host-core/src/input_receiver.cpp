#include "led/host/input_receiver.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <string>
#include <vector>

namespace led::host {

namespace {

std::uint64_t currentUnixTimeUs() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now).count());
}

}  // namespace

Status InputReceiver::bind(std::uint16_t inputPort) {
    stats_ = {};
    expectedSequence_ = 0;
    hasExpectedSequence_ = false;
    lastTransitUs_ = 0;
    hasLastTransit_ = false;
    return socket_.bind(inputPort);
}

Status InputReceiver::setReceiveTimeoutMs(std::uint32_t timeoutMs) {
    return socket_.setReceiveTimeoutMs(timeoutMs);
}

Status InputReceiver::receive(protocol::InputEvent& event, transport::UdpEndpoint& source) {
    for (;;) {
        std::vector<std::uint8_t> bytes;
        auto status = socket_.receiveFrom(1500, bytes, source);
        if (!status.isOk()) {
            return status;
        }

        ++stats_.packetsReceived;
        stats_.bytesReceived += bytes.size();

        const std::string text(bytes.begin(), bytes.end());
        if (!protocol::parseInputEvent(text, event)) {
            ++stats_.malformedPackets;
            continue;
        }

        noteEvent(event);
        return Status::ok();
    }
}

void InputReceiver::close() {
    socket_.close();
}

std::uint16_t InputReceiver::port() const {
    return socket_.localPort();
}

InputReceiverStats InputReceiver::stats() const {
    return stats_;
}

void InputReceiver::noteEvent(const protocol::InputEvent& event) {
    ++stats_.eventsReceived;
    stats_.lastSequence = event.sequence;

    switch (event.kind) {
    case protocol::InputKind::pointerMove:
        ++stats_.pointerMoves;
        break;
    case protocol::InputKind::pointerButton:
        ++stats_.pointerButtons;
        break;
    case protocol::InputKind::wheel:
        ++stats_.wheelEvents;
        break;
    case protocol::InputKind::key:
        ++stats_.keyEvents;
        break;
    case protocol::InputKind::touch:
        ++stats_.touchEvents;
        break;
    }

    if (event.sendTimeUs != 0) {
        const auto nowUs = static_cast<std::int64_t>(currentUnixTimeUs());
        const auto transitUs = nowUs - static_cast<std::int64_t>(event.sendTimeUs);
        ++stats_.timingEvents;
        if (stats_.timingEvents == 1) {
            stats_.minLatencyUs = transitUs;
            stats_.maxLatencyUs = transitUs;
            stats_.averageLatencyUs = static_cast<double>(transitUs);
        } else {
            stats_.minLatencyUs = std::min(stats_.minLatencyUs, transitUs);
            stats_.maxLatencyUs = std::max(stats_.maxLatencyUs, transitUs);
            stats_.averageLatencyUs += (static_cast<double>(transitUs) - stats_.averageLatencyUs) /
                static_cast<double>(stats_.timingEvents);
        }

        if (hasLastTransit_) {
            const auto delta = std::llabs(transitUs - lastTransitUs_);
            stats_.jitterUs += (static_cast<double>(delta) - stats_.jitterUs) / 16.0;
        }
        lastTransitUs_ = transitUs;
        hasLastTransit_ = true;
    }

    if (!hasExpectedSequence_) {
        expectedSequence_ = event.sequence + 1;
        hasExpectedSequence_ = true;
        return;
    }

    if (event.sequence > expectedSequence_) {
        ++stats_.sequenceGaps;
        stats_.estimatedLostEvents += event.sequence - expectedSequence_;
        expectedSequence_ = event.sequence + 1;
        return;
    }
    if (event.sequence < expectedSequence_) {
        ++stats_.outOfOrderEvents;
        return;
    }

    expectedSequence_ = event.sequence + 1;
}

}  // namespace led::host
