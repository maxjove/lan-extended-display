#include "led/transport/h264_rtp_depacketizer.h"

#include "led/transport/rtp_codec.h"

namespace led::transport {

namespace {

constexpr std::uint8_t kNalTypeMask = 0x1F;
constexpr std::uint8_t kNalHeaderForbiddenAndNriMask = 0xE0;
constexpr std::uint8_t kFuAType = 28;
constexpr std::uint8_t kFuStartBit = 0x80;
constexpr std::uint8_t kFuEndBit = 0x40;

bool isExpectedSequence(std::uint16_t actual, std::uint16_t expected) {
    return actual == expected;
}

std::uint16_t nextSequence(std::uint16_t sequence) {
    return static_cast<std::uint16_t>(sequence + 1);
}

}  // namespace

Status H264RtpDepacketizer::pushPacket(const RtpPacket& packet, ReassembledNal& nal) {
    nal = {};
    if (packet.payload.empty()) {
        return Status::invalidArgument("RTP packet has an empty H.264 payload");
    }

    const auto nalType = static_cast<std::uint8_t>(packet.payload.front() & kNalTypeMask);
    if (nalType > 0 && nalType < 24) {
        return pushSingleNal(packet, nal);
    }

    if (nalType == kFuAType) {
        return pushFuA(packet, nal);
    }

    reset();
    return Status::invalidArgument("unsupported H.264 RTP payload type");
}

void H264RtpDepacketizer::reset() {
    assemblingFuA_ = false;
    currentTimestamp_ = 0;
    expectedSequence_ = 0;
    currentFrameId_ = 0;
    currentHasFrameId_ = false;
    currentNal_.clear();
}

Status H264RtpDepacketizer::pushSingleNal(const RtpPacket& packet, ReassembledNal& nal) {
    reset();
    nal.timestamp = packet.timestamp;
    nal.lastSequenceNumber = packet.sequenceNumber;
    nal.endOfFrame = packet.marker;
    nal.hasFrameId = parseFrameIdExtension(packet, nal.frameId);
    nal.nalUnit = packet.payload;
    return Status::ok();
}

Status H264RtpDepacketizer::pushFuA(const RtpPacket& packet, ReassembledNal& nal) {
    if (packet.payload.size() < 3) {
        reset();
        return Status::invalidArgument("FU-A RTP payload is too short");
    }

    const auto fuIndicator = packet.payload[0];
    const auto fuHeader = packet.payload[1];
    const bool start = (fuHeader & kFuStartBit) != 0;
    const bool end = (fuHeader & kFuEndBit) != 0;
    const auto originalNalType = static_cast<std::uint8_t>(fuHeader & kNalTypeMask);

    if (start) {
        currentNal_.clear();
        const auto reconstructedNalHeader = static_cast<std::uint8_t>(
            (fuIndicator & kNalHeaderForbiddenAndNriMask) | originalNalType);
        currentNal_.push_back(reconstructedNalHeader);
        currentNal_.insert(currentNal_.end(), packet.payload.begin() + 2, packet.payload.end());
        assemblingFuA_ = true;
        currentTimestamp_ = packet.timestamp;
        expectedSequence_ = nextSequence(packet.sequenceNumber);
        currentHasFrameId_ = parseFrameIdExtension(packet, currentFrameId_);
    } else {
        if (!assemblingFuA_) {
            return Status::invalidState("received FU-A continuation before start");
        }
        if (packet.timestamp != currentTimestamp_) {
            reset();
            return Status::invalidState("FU-A timestamp changed before end");
        }
        if (!isExpectedSequence(packet.sequenceNumber, expectedSequence_)) {
            reset();
            return Status::invalidState("FU-A sequence number gap detected");
        }

        currentNal_.insert(currentNal_.end(), packet.payload.begin() + 2, packet.payload.end());
        expectedSequence_ = nextSequence(packet.sequenceNumber);
    }

    if (!end) {
        return Status::ok();
    }

    nal.timestamp = packet.timestamp;
    nal.lastSequenceNumber = packet.sequenceNumber;
    nal.endOfFrame = packet.marker;
    nal.frameId = currentFrameId_;
    nal.hasFrameId = currentHasFrameId_;
    nal.nalUnit = currentNal_;
    reset();
    return Status::ok();
}

}  // namespace led::transport
