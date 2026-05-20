#include "led/host/video_sender.h"

#include "led/common/logger.h"
#include "led/transport/rtp_codec.h"

#include <chrono>
#include <string>

namespace led::host {

namespace {

std::uint64_t currentUnixTimeUs() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now).count());
}

}  // namespace

VideoSender::VideoSender(std::uint32_t ssrc)
    : packetizer_(ssrc) {}

Status VideoSender::open(const transport::UdpEndpoint& endpoint) {
    auto status = socket_.open();
    if (!status.isOk()) {
        return status;
    }

    endpoint_ = endpoint;
    open_ = true;
    logInfo("host video sender opened for " + endpoint.address + ":" + std::to_string(endpoint.port));
    return Status::ok();
}

Status VideoSender::sendNal(const std::vector<std::uint8_t>& nalUnit, std::uint32_t rtpTimestamp) {
    if (!open_) {
        return Status::invalidState("cannot send video before sender is open");
    }
    if (nalUnit.empty()) {
        return Status::invalidArgument("cannot send an empty H.264 NAL");
    }

    auto packets = packetizer_.packetizeNal(nalUnit, rtpTimestamp, maxPayloadSize_);
    if (packets.empty()) {
        return Status::invalidArgument("H.264 NAL could not be packetized");
    }

    for (auto& packet : packets) {
        if (embedSendTime_) {
            packet.hasExtension = true;
            packet.extensionProfile = transport::kLedRtpExtensionProfile;
            packet.extensionData = transport::makeSendTimeExtension(currentUnixTimeUs());
        }
        const auto bytes = transport::serializeRtpPacket(packet);
        auto status = socket_.sendTo(bytes, endpoint_);
        if (!status.isOk()) {
            return status;
        }
    }
    return Status::ok();
}

Status VideoSender::sendFrame(const EncodedFrame& frame) {
    const auto timestamp = rtpTimestampFromMicroseconds(frame.timestampUs);
    if (!frame.nalUnits.empty()) {
        for (const auto& nalUnit : frame.nalUnits) {
            auto status = sendNal(nalUnit, timestamp);
            if (!status.isOk()) {
                return status;
            }
        }
        return Status::ok();
    }
    if (frame.payload.empty()) {
        return Status::ok();
    }
    return sendNal(frame.payload, timestamp);
}

Status VideoSender::close() {
    socket_.close();
    open_ = false;
    logInfo("host video sender closed");
    return Status::ok();
}

void VideoSender::setMaxPayloadSize(std::size_t maxPayloadSize) {
    maxPayloadSize_ = maxPayloadSize;
}

void VideoSender::setEmbedSendTime(bool enabled) {
    embedSendTime_ = enabled;
}

std::uint32_t rtpTimestampFromMicroseconds(std::uint64_t timestampUs) {
    return static_cast<std::uint32_t>((timestampUs * 90) / 1000);
}

}  // namespace led::host
