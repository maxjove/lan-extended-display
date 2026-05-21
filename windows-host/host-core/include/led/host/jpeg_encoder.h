#pragma once

#include "led/common/status.h"
#include "led/host/capture_engine.h"

#include <cstdint>
#include <vector>

namespace led::host {

struct EncodedJpegFrame {
    std::uint64_t frameId{0};
    std::uint64_t timestampUs{0};
    std::uint32_t width{0};
    std::uint32_t height{0};
    std::vector<std::uint8_t> jpegBytes;
};

[[nodiscard]] Status encodeBgraJpeg(
    const CapturedFrame& frame,
    float quality,
    EncodedJpegFrame& encoded);

[[nodiscard]] Status encodeBgraJpegRect(
    const CapturedFrame& frame,
    std::uint32_t x,
    std::uint32_t y,
    std::uint32_t width,
    std::uint32_t height,
    float quality,
    EncodedJpegFrame& encoded);

}  // namespace led::host
