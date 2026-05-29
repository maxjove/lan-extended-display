#pragma once

#include "led/common/status.h"
#include "led/protocol/messages.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace led::host {

struct CaptureDirtyRect {
    std::uint32_t x{0};
    std::uint32_t y{0};
    std::uint32_t width{0};
    std::uint32_t height{0};
};

struct CapturedFrame {
    std::uint64_t frameId{0};
    std::uint64_t timestampUs{0};
    std::uint32_t width{0};
    std::uint32_t height{0};
    bool dirtyRectsKnown{false};
    std::vector<CaptureDirtyRect> dirtyRects;
    std::vector<std::uint8_t> bgra;
};

class CaptureEngine {
public:
    Status start(const protocol::Resolution& resolution);
    Status startRegion(const protocol::Resolution& resolution, int originX, int originY, std::string deviceName = {});
    Status captureNextFrame(CapturedFrame& frame);
    Status stop();

    [[nodiscard]] bool isRunning() const;
    [[nodiscard]] CapturedFrame latestFrame() const;

public:
    // Internal backend state. Kept here so platform-specific capture helpers can stay out of the public methods.
    bool running_{false};
    bool dxgiCapture_{false};
    bool capturedRealFrame_{false};
    CapturedFrame latestFrame_{};
    std::chrono::steady_clock::time_point startTime_{};
    void* screenDc_{nullptr};
    bool screenDcCreated_{false};
    bool dxgiFallbackActive_{false};
    std::chrono::steady_clock::time_point nextDxgiRetryTime_{};
    std::uint32_t dxgiFallbackRetryCount_{0};
    void* memoryDc_{nullptr};
    void* bitmap_{nullptr};
    void* bitmapBits_{nullptr};
    void* d3dDevice_{nullptr};
    void* d3dContext_{nullptr};
    void* dxgiDuplication_{nullptr};
    void* stagingTexture_{nullptr};
    std::uint32_t sourceWidth_{0};
    std::uint32_t sourceHeight_{0};
    int sourceOriginX_{0};
    int sourceOriginY_{0};
    std::string sourceDeviceName_{};
    bool captureRegion_{false};
};

}  // namespace led::host
