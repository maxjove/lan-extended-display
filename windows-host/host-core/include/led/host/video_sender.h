#pragma once

#include "led/common/status.h"
#include "led/host/encoder.h"
#include "led/transport/h264_rtp_packetizer.h"
#include "led/transport/udp_socket.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace led::host {

class VideoSender {
public:
    explicit VideoSender(std::uint32_t ssrc = 0x4C454431);

    Status open(const transport::UdpEndpoint& endpoint);
    Status sendNal(const std::vector<std::uint8_t>& nalUnit, std::uint32_t rtpTimestamp);
    Status sendFrame(const EncodedFrame& frame);
    Status close();

    void setMaxPayloadSize(std::size_t maxPayloadSize);
    void setEmbedSendTime(bool enabled);

private:
    transport::UdpSocket socket_;
    transport::UdpEndpoint endpoint_{};
    transport::H264RtpPacketizer packetizer_;
    std::size_t maxPayloadSize_{1200};
    bool embedSendTime_{false};
    bool open_{false};
};

[[nodiscard]] std::uint32_t rtpTimestampFromMicroseconds(std::uint64_t timestampUs);

}  // namespace led::host
