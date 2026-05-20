#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>

namespace led::transport {

struct RtpPacket {
    std::uint8_t payloadType{96};
    bool marker{false};
    std::uint16_t sequenceNumber{0};
    std::uint32_t timestamp{0};
    std::uint32_t ssrc{0};
    bool hasExtension{false};
    std::uint16_t extensionProfile{0};
    std::vector<std::uint8_t> extensionData;
    std::vector<std::uint8_t> payload;
};

class H264RtpPacketizer {
public:
    explicit H264RtpPacketizer(std::uint32_t ssrc, std::uint16_t initialSequence = 0);

    [[nodiscard]] std::vector<RtpPacket> packetizeNal(
        const std::vector<std::uint8_t>& nalUnit,
        std::uint32_t timestamp,
        std::size_t maxPayloadSize);

    [[nodiscard]] std::uint16_t nextSequenceNumber() const;

private:
    std::uint16_t allocateSequence();

    std::uint32_t ssrc_{0};
    std::uint16_t nextSequence_{0};
};

}  // namespace led::transport
