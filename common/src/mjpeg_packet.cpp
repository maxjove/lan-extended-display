#include "led/transport/mjpeg_packet.h"

#include <algorithm>

namespace led::transport {

namespace {

constexpr std::uint32_t kMagic = 0x4C45444A;  // LEDJ
constexpr std::uint8_t kVersion = 2;
constexpr std::uint8_t kFlagKeyFrame = 0x01;
constexpr std::size_t kLegacyHeaderBytes = 32;
constexpr std::size_t kHeaderBytes = 56;

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

std::vector<std::vector<std::uint8_t>> packetizeMjpegFrame(
    const std::vector<std::uint8_t>& jpegBytes,
    std::uint64_t frameId,
    std::uint64_t timestampUs,
    std::size_t maxDatagramBytes) {
    return packetizeMjpegFrameRect(
        jpegBytes,
        frameId,
        timestampUs,
        true,
        0,
        0,
        0,
        0,
        0,
        0,
        maxDatagramBytes);
}

std::vector<std::vector<std::uint8_t>> packetizeMjpegFrameRect(
    const std::vector<std::uint8_t>& jpegBytes,
    std::uint64_t frameId,
    std::uint64_t timestampUs,
    bool keyFrame,
    std::uint32_t canvasWidth,
    std::uint32_t canvasHeight,
    std::uint32_t rectX,
    std::uint32_t rectY,
    std::uint32_t rectWidth,
    std::uint32_t rectHeight,
    std::size_t maxDatagramBytes) {
    std::vector<std::vector<std::uint8_t>> packets;
    if (jpegBytes.empty() || maxDatagramBytes <= kHeaderBytes) {
        return packets;
    }

    const auto payloadCapacity = maxDatagramBytes - kHeaderBytes;
    const auto partCountSize = (jpegBytes.size() + payloadCapacity - 1) / payloadCapacity;
    if (partCountSize == 0 || partCountSize > 65535) {
        return packets;
    }

    const auto partCount = static_cast<std::uint16_t>(partCountSize);
    packets.reserve(partCount);
    for (std::uint16_t part = 0; part < partCount; ++part) {
        const auto offset = static_cast<std::size_t>(part) * payloadCapacity;
        const auto payloadBytes = std::min(payloadCapacity, jpegBytes.size() - offset);
        std::vector<std::uint8_t> packet;
        packet.reserve(kHeaderBytes + payloadBytes);
        writeU32(packet, kMagic);
        packet.push_back(kVersion);
        packet.push_back(keyFrame ? kFlagKeyFrame : 0);
        writeU16(packet, static_cast<std::uint16_t>(kHeaderBytes));
        writeU16(packet, part);
        writeU16(packet, partCount);
        writeU64(packet, frameId);
        writeU64(packet, timestampUs);
        writeU32(packet, static_cast<std::uint32_t>(payloadBytes));
        writeU32(packet, canvasWidth);
        writeU32(packet, canvasHeight);
        writeU32(packet, rectX);
        writeU32(packet, rectY);
        writeU32(packet, rectWidth);
        writeU32(packet, rectHeight);
        packet.insert(packet.end(), jpegBytes.begin() + static_cast<std::ptrdiff_t>(offset),
            jpegBytes.begin() + static_cast<std::ptrdiff_t>(offset + payloadBytes));
        packets.push_back(std::move(packet));
    }
    return packets;
}

Status parseMjpegDatagram(const std::vector<std::uint8_t>& bytes, MjpegDatagram& datagram) {
    datagram = {};
    if (bytes.size() < kLegacyHeaderBytes) {
        return Status::invalidArgument("MJPEG datagram is shorter than header");
    }
    if (readU32(bytes, 0) != kMagic || (bytes[4] != 1 && bytes[4] != kVersion)) {
        return Status::invalidArgument("MJPEG datagram has invalid magic or version");
    }
    const auto headerBytes = readU16(bytes, 6);
    if ((headerBytes != kLegacyHeaderBytes && headerBytes != kHeaderBytes) || bytes.size() < headerBytes) {
        return Status::invalidArgument("MJPEG datagram has invalid header length");
    }
    datagram.partIndex = readU16(bytes, 8);
    datagram.partCount = readU16(bytes, 10);
    datagram.frameId = readU64(bytes, 12);
    datagram.timestampUs = readU64(bytes, 20);
    datagram.keyFrame = (bytes[5] & kFlagKeyFrame) != 0;
    const auto payloadBytes = readU32(bytes, 28);
    if (headerBytes >= kHeaderBytes) {
        datagram.canvasWidth = readU32(bytes, 32);
        datagram.canvasHeight = readU32(bytes, 36);
        datagram.rectX = readU32(bytes, 40);
        datagram.rectY = readU32(bytes, 44);
        datagram.rectWidth = readU32(bytes, 48);
        datagram.rectHeight = readU32(bytes, 52);
    }
    if (datagram.partCount == 0 || datagram.partIndex >= datagram.partCount ||
        bytes.size() != headerBytes + payloadBytes) {
        return Status::invalidArgument("MJPEG datagram has invalid part metadata");
    }
    datagram.payload.assign(bytes.begin() + static_cast<std::ptrdiff_t>(headerBytes), bytes.end());
    return Status::ok();
}

Status MjpegFrameAssembler::pushDatagram(const MjpegDatagram& datagram, ReassembledMjpegFrame& frame) {
    frame = {};
    if (datagram.partCount == 0 || datagram.partIndex >= datagram.partCount) {
        return Status::invalidArgument("invalid MJPEG part");
    }
    if (frameId_ != datagram.frameId || partCount_ != datagram.partCount) {
        reset();
        frameId_ = datagram.frameId;
        timestampUs_ = datagram.timestampUs;
        keyFrame_ = datagram.keyFrame;
        canvasWidth_ = datagram.canvasWidth;
        canvasHeight_ = datagram.canvasHeight;
        rectX_ = datagram.rectX;
        rectY_ = datagram.rectY;
        rectWidth_ = datagram.rectWidth;
        rectHeight_ = datagram.rectHeight;
        partCount_ = datagram.partCount;
        parts_.assign(partCount_, {});
        received_.assign(partCount_, false);
    }
    if (!received_[datagram.partIndex]) {
        parts_[datagram.partIndex] = datagram.payload;
        received_[datagram.partIndex] = true;
        ++receivedParts_;
    }
    if (receivedParts_ != partCount_) {
        return Status::ok();
    }

    std::size_t totalBytes = 0;
    for (const auto& part : parts_) {
        totalBytes += part.size();
    }
    frame.frameId = frameId_;
    frame.timestampUs = timestampUs_;
    frame.keyFrame = keyFrame_;
    frame.canvasWidth = canvasWidth_;
    frame.canvasHeight = canvasHeight_;
    frame.rectX = rectX_;
    frame.rectY = rectY_;
    frame.rectWidth = rectWidth_;
    frame.rectHeight = rectHeight_;
    frame.jpegBytes.reserve(totalBytes);
    for (const auto& part : parts_) {
        frame.jpegBytes.insert(frame.jpegBytes.end(), part.begin(), part.end());
    }
    reset();
    return Status::ok();
}

void MjpegFrameAssembler::reset() {
    frameId_ = 0;
    timestampUs_ = 0;
    keyFrame_ = true;
    canvasWidth_ = 0;
    canvasHeight_ = 0;
    rectX_ = 0;
    rectY_ = 0;
    rectWidth_ = 0;
    rectHeight_ = 0;
    partCount_ = 0;
    receivedParts_ = 0;
    parts_.clear();
    received_.clear();
}

}  // namespace led::transport
