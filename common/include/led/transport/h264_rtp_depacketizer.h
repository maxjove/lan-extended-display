#pragma once

#include "led/common/status.h"
#include "led/transport/h264_rtp_packetizer.h"

#include <cstdint>
#include <vector>

namespace led::transport {

struct ReassembledNal {
    std::uint32_t timestamp{0};
    std::uint16_t lastSequenceNumber{0};
    bool endOfFrame{false};
    std::uint64_t frameId{0};
    bool hasFrameId{false};
    std::vector<std::uint8_t> nalUnit;
};

class H264RtpDepacketizer {
public:
    Status pushPacket(const RtpPacket& packet, ReassembledNal& nal);
    void reset();

private:
    Status pushSingleNal(const RtpPacket& packet, ReassembledNal& nal);
    Status pushFuA(const RtpPacket& packet, ReassembledNal& nal);

    bool assemblingFuA_{false};
    std::uint32_t currentTimestamp_{0};
    std::uint16_t expectedSequence_{0};
    std::uint64_t currentFrameId_{0};
    bool currentHasFrameId_{false};
    std::vector<std::uint8_t> currentNal_;
};

}  // namespace led::transport
