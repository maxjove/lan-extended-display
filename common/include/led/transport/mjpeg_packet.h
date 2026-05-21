#pragma once

#include "led/common/status.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace led::transport {

struct MjpegDatagram {
    std::uint16_t partIndex{0};
    std::uint16_t partCount{0};
    std::uint64_t frameId{0};
    std::uint64_t timestampUs{0};
    bool keyFrame{true};
    std::uint32_t canvasWidth{0};
    std::uint32_t canvasHeight{0};
    std::uint32_t rectX{0};
    std::uint32_t rectY{0};
    std::uint32_t rectWidth{0};
    std::uint32_t rectHeight{0};
    std::vector<std::uint8_t> payload;
};

struct ReassembledMjpegFrame {
    std::uint64_t frameId{0};
    std::uint64_t timestampUs{0};
    bool keyFrame{true};
    std::uint32_t canvasWidth{0};
    std::uint32_t canvasHeight{0};
    std::uint32_t rectX{0};
    std::uint32_t rectY{0};
    std::uint32_t rectWidth{0};
    std::uint32_t rectHeight{0};
    std::vector<std::uint8_t> jpegBytes;
};

[[nodiscard]] std::vector<std::vector<std::uint8_t>> packetizeMjpegFrame(
    const std::vector<std::uint8_t>& jpegBytes,
    std::uint64_t frameId,
    std::uint64_t timestampUs,
    std::size_t maxDatagramBytes);

[[nodiscard]] std::vector<std::vector<std::uint8_t>> packetizeMjpegFrameRect(
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
    std::size_t maxDatagramBytes);

[[nodiscard]] Status parseMjpegDatagram(const std::vector<std::uint8_t>& bytes, MjpegDatagram& datagram);

class MjpegFrameAssembler {
public:
    [[nodiscard]] Status pushDatagram(const MjpegDatagram& datagram, ReassembledMjpegFrame& frame);
    void reset();

private:
    std::uint64_t frameId_{0};
    std::uint64_t timestampUs_{0};
    bool keyFrame_{true};
    std::uint32_t canvasWidth_{0};
    std::uint32_t canvasHeight_{0};
    std::uint32_t rectX_{0};
    std::uint32_t rectY_{0};
    std::uint32_t rectWidth_{0};
    std::uint32_t rectHeight_{0};
    std::uint16_t partCount_{0};
    std::uint16_t receivedParts_{0};
    std::vector<std::vector<std::uint8_t>> parts_;
    std::vector<bool> received_;
};

}  // namespace led::transport
