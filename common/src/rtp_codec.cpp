#include "led/transport/rtp_codec.h"

#include <algorithm>
#include <cstddef>

namespace led::transport {

namespace {

constexpr std::uint8_t kSendTimeExtensionId = 1;
constexpr std::uint8_t kFrameIdExtensionId = 2;

void writeU16(std::vector<std::uint8_t>& bytes, std::uint16_t value) {
    bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>(value & 0xFF));
}

void writeU32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    bytes.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>(value & 0xFF));
}

void writeU64(std::vector<std::uint8_t>& bytes, std::uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) {
        bytes.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFF));
    }
}

std::uint16_t readU16(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    return static_cast<std::uint16_t>((bytes[offset] << 8) | bytes[offset + 1]);
}

std::uint32_t readU32(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    return (static_cast<std::uint32_t>(bytes[offset]) << 24) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 16) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 8) |
           static_cast<std::uint32_t>(bytes[offset + 3]);
}

std::uint64_t readU64(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < 8; ++index) {
        value = (value << 8) | bytes[offset + index];
    }
    return value;
}

}  // namespace

std::vector<std::uint8_t> serializeRtpPacket(const RtpPacket& packet) {
    std::vector<std::uint8_t> bytes;
    const auto extensionPaddedBytes = packet.hasExtension
        ? ((packet.extensionData.size() + 3) / 4) * 4
        : std::size_t{0};
    bytes.reserve(kRtpFixedHeaderSize + (packet.hasExtension ? 4 + extensionPaddedBytes : 0) + packet.payload.size());

    bytes.push_back(static_cast<std::uint8_t>(0x80 | (packet.hasExtension ? 0x10 : 0x00)));
    bytes.push_back(static_cast<std::uint8_t>((packet.marker ? 0x80 : 0x00) | (packet.payloadType & 0x7F)));
    writeU16(bytes, packet.sequenceNumber);
    writeU32(bytes, packet.timestamp);
    writeU32(bytes, packet.ssrc);
    if (packet.hasExtension) {
        writeU16(bytes, packet.extensionProfile);
        writeU16(bytes, static_cast<std::uint16_t>(extensionPaddedBytes / 4));
        bytes.insert(bytes.end(), packet.extensionData.begin(), packet.extensionData.end());
        bytes.resize(kRtpFixedHeaderSize + 4 + extensionPaddedBytes, 0);
    }
    bytes.insert(bytes.end(), packet.payload.begin(), packet.payload.end());
    return bytes;
}

Status parseRtpPacket(const std::vector<std::uint8_t>& bytes, RtpPacket& packet) {
    if (bytes.size() < kRtpFixedHeaderSize) {
        return Status::invalidArgument("RTP packet is shorter than fixed header");
    }

    const auto version = static_cast<std::uint8_t>(bytes[0] >> 6);
    const auto csrcCount = static_cast<std::uint8_t>(bytes[0] & 0x0F);
    const bool hasExtension = (bytes[0] & 0x10) != 0;
    if (version != 2) {
        return Status::invalidArgument("RTP packet version is not 2");
    }
    if (csrcCount != 0) {
        return Status::invalidArgument("RTP CSRC lists are not supported yet");
    }

    packet.marker = (bytes[1] & 0x80) != 0;
    packet.payloadType = static_cast<std::uint8_t>(bytes[1] & 0x7F);
    packet.sequenceNumber = readU16(bytes, 2);
    packet.timestamp = readU32(bytes, 4);
    packet.ssrc = readU32(bytes, 8);
    packet.hasExtension = false;
    packet.extensionProfile = 0;
    packet.extensionData.clear();

    std::size_t payloadOffset = kRtpFixedHeaderSize;
    if (hasExtension) {
        if (bytes.size() < payloadOffset + 4) {
            return Status::invalidArgument("RTP extension header is truncated");
        }
        packet.hasExtension = true;
        packet.extensionProfile = readU16(bytes, payloadOffset);
        const auto extensionWords = readU16(bytes, payloadOffset + 2);
        payloadOffset += 4;
        const auto extensionBytes = static_cast<std::size_t>(extensionWords) * 4;
        if (bytes.size() < payloadOffset + extensionBytes) {
            return Status::invalidArgument("RTP extension payload is truncated");
        }
        packet.extensionData.assign(
            bytes.begin() + static_cast<std::ptrdiff_t>(payloadOffset),
            bytes.begin() + static_cast<std::ptrdiff_t>(payloadOffset + extensionBytes));
        payloadOffset += extensionBytes;
    }

    packet.payload.assign(bytes.begin() + static_cast<std::ptrdiff_t>(payloadOffset), bytes.end());
    return Status::ok();
}

std::vector<std::uint8_t> makeSendTimeExtension(std::uint64_t sendTimeUs) {
    std::vector<std::uint8_t> extension;
    extension.reserve(12);
    extension.push_back(kSendTimeExtensionId);
    extension.push_back(8);
    writeU64(extension, sendTimeUs);
    return extension;
}

std::vector<std::uint8_t> makeTelemetryExtension(std::uint64_t sendTimeUs, std::uint64_t frameId) {
    auto extension = makeSendTimeExtension(sendTimeUs);
    extension.push_back(kFrameIdExtensionId);
    extension.push_back(8);
    writeU64(extension, frameId);
    return extension;
}

bool parseSendTimeExtension(const RtpPacket& packet, std::uint64_t& sendTimeUs) {
    if (!packet.hasExtension || packet.extensionProfile != kLedRtpExtensionProfile) {
        return false;
    }

    std::size_t offset = 0;
    while (offset + 2 <= packet.extensionData.size()) {
        const auto id = packet.extensionData[offset];
        const auto length = packet.extensionData[offset + 1];
        offset += 2;
        if (id == 0 && length == 0) {
            break;
        }
        if (offset + length > packet.extensionData.size()) {
            return false;
        }
        if (id == kSendTimeExtensionId && length == 8) {
            sendTimeUs = readU64(packet.extensionData, offset);
            return true;
        }
        offset += length;
    }
    return false;
}

bool parseFrameIdExtension(const RtpPacket& packet, std::uint64_t& frameId) {
    if (!packet.hasExtension || packet.extensionProfile != kLedRtpExtensionProfile) {
        return false;
    }

    std::size_t offset = 0;
    while (offset + 2 <= packet.extensionData.size()) {
        const auto id = packet.extensionData[offset];
        const auto length = packet.extensionData[offset + 1];
        offset += 2;
        if (id == 0 && length == 0) {
            break;
        }
        if (offset + length > packet.extensionData.size()) {
            return false;
        }
        if (id == kFrameIdExtensionId && length == 8) {
            frameId = readU64(packet.extensionData, offset);
            return true;
        }
        offset += length;
    }
    return false;
}

}  // namespace led::transport
