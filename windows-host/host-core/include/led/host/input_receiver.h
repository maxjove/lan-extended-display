#pragma once

#include "led/common/status.h"
#include "led/protocol/messages.h"
#include "led/transport/udp_socket.h"

#include <cstdint>

namespace led::host {

struct InputReceiverStats {
    std::uint64_t packetsReceived{0};
    std::uint64_t bytesReceived{0};
    std::uint64_t malformedPackets{0};
    std::uint64_t eventsReceived{0};
    std::uint64_t pointerMoves{0};
    std::uint64_t pointerButtons{0};
    std::uint64_t wheelEvents{0};
    std::uint64_t keyEvents{0};
    std::uint64_t touchEvents{0};
    std::uint64_t sequenceGaps{0};
    std::uint64_t estimatedLostEvents{0};
    std::uint64_t outOfOrderEvents{0};
    std::uint64_t lastSequence{0};
    std::uint64_t timingEvents{0};
    std::int64_t minLatencyUs{0};
    std::int64_t maxLatencyUs{0};
    double averageLatencyUs{0.0};
    double jitterUs{0.0};
};

class InputReceiver {
public:
    Status bind(std::uint16_t inputPort);
    Status setReceiveTimeoutMs(std::uint32_t timeoutMs);
    Status receive(protocol::InputEvent& event, transport::UdpEndpoint& source);
    void close();

    [[nodiscard]] std::uint16_t port() const;
    [[nodiscard]] InputReceiverStats stats() const;

private:
    void noteEvent(const protocol::InputEvent& event);

    transport::UdpSocket socket_;
    InputReceiverStats stats_{};
    std::uint64_t expectedSequence_{0};
    std::int64_t lastTransitUs_{0};
    bool hasExpectedSequence_{false};
    bool hasLastTransit_{false};
};

}  // namespace led::host
