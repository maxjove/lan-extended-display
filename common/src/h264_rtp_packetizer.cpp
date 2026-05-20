#include "led/transport/h264_rtp_packetizer.h"

#include <algorithm>
#include <utility>

namespace led::transport {

namespace {

constexpr std::size_t kFuAHeaderSize = 2;

}  // namespace

H264RtpPacketizer::H264RtpPacketizer(std::uint32_t ssrc, std::uint16_t initialSequence)
    : ssrc_(ssrc), nextSequence_(initialSequence) {}

std::vector<RtpPacket> H264RtpPacketizer::packetizeNal(
    const std::vector<std::uint8_t>& nalUnit,
    std::uint32_t timestamp,
    std::size_t maxPayloadSize) {
    std::vector<RtpPacket> packets;
    if (nalUnit.empty() || maxPayloadSize == 0) {
        return packets;
    }

    if (nalUnit.size() <= maxPayloadSize) {
        RtpPacket packet;
        packet.marker = true;
        packet.sequenceNumber = allocateSequence();
        packet.timestamp = timestamp;
        packet.ssrc = ssrc_;
        packet.payload = nalUnit;
        packets.push_back(std::move(packet));
        return packets;
    }

    if (maxPayloadSize <= kFuAHeaderSize) {
        return packets;
    }

    const std::uint8_t nalHeader = nalUnit.front();
    const std::uint8_t forbiddenAndNri = nalHeader & 0xE0;
    const std::uint8_t nalType = nalHeader & 0x1F;
    const std::uint8_t fuIndicator = static_cast<std::uint8_t>(forbiddenAndNri | 28);
    const std::size_t fragmentCapacity = maxPayloadSize - kFuAHeaderSize;

    std::size_t offset = 1;
    bool first = true;
    while (offset < nalUnit.size()) {
        const std::size_t remaining = nalUnit.size() - offset;
        const std::size_t fragmentSize = std::min(fragmentCapacity, remaining);
        const bool last = offset + fragmentSize >= nalUnit.size();

        RtpPacket packet;
        packet.marker = last;
        packet.sequenceNumber = allocateSequence();
        packet.timestamp = timestamp;
        packet.ssrc = ssrc_;
        packet.payload.reserve(kFuAHeaderSize + fragmentSize);
        packet.payload.push_back(fuIndicator);

        std::uint8_t fuHeader = nalType;
        if (first) {
            fuHeader = static_cast<std::uint8_t>(fuHeader | 0x80);
        }
        if (last) {
            fuHeader = static_cast<std::uint8_t>(fuHeader | 0x40);
        }

        packet.payload.push_back(fuHeader);
        packet.payload.insert(
            packet.payload.end(),
            nalUnit.begin() + static_cast<std::ptrdiff_t>(offset),
            nalUnit.begin() + static_cast<std::ptrdiff_t>(offset + fragmentSize));

        packets.push_back(std::move(packet));
        first = false;
        offset += fragmentSize;
    }

    return packets;
}

std::uint16_t H264RtpPacketizer::nextSequenceNumber() const {
    return nextSequence_;
}

std::uint16_t H264RtpPacketizer::allocateSequence() {
    const auto value = nextSequence_;
    ++nextSequence_;
    return value;
}

}  // namespace led::transport
