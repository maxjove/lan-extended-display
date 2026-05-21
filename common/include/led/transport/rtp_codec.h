#pragma once

#include "led/common/status.h"
#include "led/transport/h264_rtp_packetizer.h"

#include <cstdint>
#include <cstddef>
#include <vector>

namespace led::transport {

constexpr std::size_t kRtpFixedHeaderSize = 12;
constexpr std::uint16_t kLedRtpExtensionProfile = 0x4C45;

[[nodiscard]] std::vector<std::uint8_t> serializeRtpPacket(const RtpPacket& packet);
[[nodiscard]] Status parseRtpPacket(const std::vector<std::uint8_t>& bytes, RtpPacket& packet);
[[nodiscard]] std::vector<std::uint8_t> makeSendTimeExtension(std::uint64_t sendTimeUs);
[[nodiscard]] std::vector<std::uint8_t> makeTelemetryExtension(std::uint64_t sendTimeUs, std::uint64_t frameId);
[[nodiscard]] bool parseSendTimeExtension(const RtpPacket& packet, std::uint64_t& sendTimeUs);
[[nodiscard]] bool parseFrameIdExtension(const RtpPacket& packet, std::uint64_t& frameId);

}  // namespace led::transport
