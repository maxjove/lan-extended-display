#pragma once

#include "led/common/status.h"
#include "led/transport/h264_rtp_depacketizer.h"
#include "led/transport/udp_socket.h"

#include <cstdint>
#include <vector>

namespace led::client {

struct VideoReceiverStats {
    std::uint64_t packetsReceived{0};
    std::uint64_t bytesReceived{0};
    std::uint64_t rtpPayloadBytes{0};
    std::uint64_t malformedPackets{0};
    std::uint64_t depacketizerDrops{0};
    std::uint64_t sequenceGaps{0};
    std::uint64_t estimatedLostPackets{0};
    std::uint64_t outOfOrderPackets{0};
    std::uint64_t nalUnits{0};
    std::uint64_t idrNalUnits{0};
    std::uint64_t timingPackets{0};
    std::int64_t minLatencyUs{0};
    std::int64_t maxLatencyUs{0};
    double averageLatencyUs{0.0};
    double jitterUs{0.0};
    std::uint16_t lastSequenceNumber{0};
    std::uint32_t lastTimestamp{0};
    std::uint64_t lastFrameId{0};
    bool hasLastFrameId{false};
};

struct ReceivedNal {
    std::vector<std::uint8_t> nalUnit;
    std::uint32_t rtpTimestamp{0};
    bool endOfFrame{false};
    std::uint64_t frameId{0};
    bool hasFrameId{false};
};

class VideoReceiver {
public:
    Status bind(std::uint16_t port);
    Status setReceiveTimeoutMs(std::uint32_t timeoutMs);
    Status receiveNal(std::vector<std::uint8_t>& nalUnit, transport::UdpEndpoint& source);
    Status receiveNal(ReceivedNal& nal, transport::UdpEndpoint& source);
    Status close();

    [[nodiscard]] std::uint16_t port() const;
    [[nodiscard]] VideoReceiverStats stats() const;

private:
    void notePacket(const transport::RtpPacket& packet, std::size_t packetBytes);
    void noteNal(const std::vector<std::uint8_t>& nalUnit);

    transport::UdpSocket socket_;
    transport::H264RtpDepacketizer depacketizer_;
    VideoReceiverStats stats_{};
    std::uint16_t port_{0};
    std::uint16_t expectedSequenceNumber_{0};
    std::int64_t lastTransitUs_{0};
    bool hasExpectedSequenceNumber_{false};
    bool hasLastTransit_{false};
    bool bound_{false};
};

}  // namespace led::client
