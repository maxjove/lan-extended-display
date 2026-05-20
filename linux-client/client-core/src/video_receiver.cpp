#include "led/client/video_receiver.h"

#include "led/common/logger.h"
#include "led/transport/h264_annex_b.h"
#include "led/transport/rtp_codec.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <utility>

namespace led::client {

namespace {

std::uint16_t nextSequence(std::uint16_t sequence) {
    return static_cast<std::uint16_t>(sequence + 1);
}

std::int32_t sequenceDelta(std::uint16_t actual, std::uint16_t expected) {
    return static_cast<std::int16_t>(actual - expected);
}

std::uint64_t currentUnixTimeUs() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now).count());
}

}  // namespace

Status VideoReceiver::bind(std::uint16_t port) {
    auto status = socket_.bind(port);
    if (!status.isOk()) {
        return status;
    }

    port_ = port;
    if (port_ == 0) {
        port_ = socket_.localPort();
    }
    stats_ = {};
    hasExpectedSequenceNumber_ = false;
    expectedSequenceNumber_ = 0;
    hasLastTransit_ = false;
    lastTransitUs_ = 0;
    bound_ = true;
    logInfo("client RTP receiver bound on UDP port " + std::to_string(port_));
    return Status::ok();
}

Status VideoReceiver::setReceiveTimeoutMs(std::uint32_t timeoutMs) {
    return socket_.setReceiveTimeoutMs(timeoutMs);
}

Status VideoReceiver::receiveNal(std::vector<std::uint8_t>& nalUnit, transport::UdpEndpoint& source) {
    if (!bound_) {
        return Status::invalidState("cannot receive video before binding UDP socket");
    }

    nalUnit.clear();
    for (;;) {
        std::vector<std::uint8_t> bytes;
        auto status = socket_.receiveFrom(1500, bytes, source);
        if (!status.isOk()) {
            return status;
        }

        transport::RtpPacket packet;
        status = transport::parseRtpPacket(bytes, packet);
        if (!status.isOk()) {
            ++stats_.malformedPackets;
            continue;
        }
        notePacket(packet, bytes.size());

        transport::ReassembledNal reassembled;
        status = depacketizer_.pushPacket(packet, reassembled);
        if (!status.isOk()) {
            ++stats_.depacketizerDrops;
            continue;
        }

        if (!reassembled.nalUnit.empty()) {
            nalUnit = std::move(reassembled.nalUnit);
            noteNal(nalUnit);
            return Status::ok();
        }
    }
}

Status VideoReceiver::close() {
    socket_.close();
    depacketizer_.reset();
    hasExpectedSequenceNumber_ = false;
    expectedSequenceNumber_ = 0;
    hasLastTransit_ = false;
    lastTransitUs_ = 0;
    bound_ = false;
    port_ = 0;
    logInfo("client RTP receiver closed");
    return Status::ok();
}

std::uint16_t VideoReceiver::port() const {
    return port_;
}

VideoReceiverStats VideoReceiver::stats() const {
    return stats_;
}

void VideoReceiver::notePacket(const transport::RtpPacket& packet, std::size_t packetBytes) {
    ++stats_.packetsReceived;
    stats_.bytesReceived += packetBytes;
    stats_.rtpPayloadBytes += packet.payload.size();
    stats_.lastSequenceNumber = packet.sequenceNumber;
    stats_.lastTimestamp = packet.timestamp;

    std::uint64_t sendTimeUs = 0;
    if (transport::parseSendTimeExtension(packet, sendTimeUs)) {
        const auto nowUs = static_cast<std::int64_t>(currentUnixTimeUs());
        const auto transitUs = nowUs - static_cast<std::int64_t>(sendTimeUs);
        ++stats_.timingPackets;
        if (stats_.timingPackets == 1) {
            stats_.minLatencyUs = transitUs;
            stats_.maxLatencyUs = transitUs;
            stats_.averageLatencyUs = static_cast<double>(transitUs);
        } else {
            stats_.minLatencyUs = std::min(stats_.minLatencyUs, transitUs);
            stats_.maxLatencyUs = std::max(stats_.maxLatencyUs, transitUs);
            stats_.averageLatencyUs += (static_cast<double>(transitUs) - stats_.averageLatencyUs) /
                static_cast<double>(stats_.timingPackets);
        }

        if (hasLastTransit_) {
            const auto delta = std::llabs(transitUs - lastTransitUs_);
            stats_.jitterUs += (static_cast<double>(delta) - stats_.jitterUs) / 16.0;
        }
        lastTransitUs_ = transitUs;
        hasLastTransit_ = true;
    }

    if (!hasExpectedSequenceNumber_) {
        expectedSequenceNumber_ = nextSequence(packet.sequenceNumber);
        hasExpectedSequenceNumber_ = true;
        return;
    }

    const auto delta = sequenceDelta(packet.sequenceNumber, expectedSequenceNumber_);
    if (delta > 0) {
        ++stats_.sequenceGaps;
        stats_.estimatedLostPackets += static_cast<std::uint64_t>(delta);
        expectedSequenceNumber_ = nextSequence(packet.sequenceNumber);
        return;
    }
    if (delta < 0) {
        ++stats_.outOfOrderPackets;
        return;
    }

    expectedSequenceNumber_ = nextSequence(packet.sequenceNumber);
}

void VideoReceiver::noteNal(const std::vector<std::uint8_t>& nalUnit) {
    ++stats_.nalUnits;
    if (transport::h264NalType(nalUnit) == 5) {
        ++stats_.idrNalUnits;
    }
}

}  // namespace led::client
