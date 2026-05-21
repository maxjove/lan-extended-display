#pragma once

#include "led/common/status.h"
#include "led/protocol/messages.h"

#include <cstdint>
#include <mutex>
#include <string>

namespace led::client {

struct RawFrameInfo {
    std::uint32_t width{0};
    std::uint32_t height{0};
    std::uint32_t stride{0};
    std::uint64_t bytes{0};
    std::uint64_t ptsNs{0};
    std::string format;
};

struct RendererStats {
    std::uint64_t rawFrames{0};
    std::uint64_t bytes{0};
    RawFrameInfo lastFrame;
};

class Renderer {
public:
    Status openFullscreen(const protocol::Resolution& resolution);
    Status close();
    Status updateLocalCursor(double normalizedX, double normalizedY);
    Status submitRawFrame(const RawFrameInfo& frame);
    Status submitRawFrameData(const RawFrameInfo& frame, const std::uint8_t* data);
    Status submitRawFrameRegion(
        const RawFrameInfo& frame,
        const std::uint8_t* data,
        std::uint32_t x,
        std::uint32_t y);

    [[nodiscard]] RendererStats stats() const;

private:
    protocol::Resolution resolution_{};
    RendererStats stats_{};
    mutable std::mutex mutex_;
    bool fullscreen_{false};
    void* xDisplay_{nullptr};
    unsigned long xWindow_{0};
    void* xGc_{nullptr};
    void* xShmImage_{nullptr};
    void* xShmInfo_{nullptr};
    std::uint64_t xShmBytes_{0};
    bool xShmAvailable_{false};
    bool xShmAttached_{false};
    void* glContext_{nullptr};
    unsigned int glTexture_{0};
    unsigned int glYTexture_{0};
    unsigned int glUTexture_{0};
    unsigned int glVTexture_{0};
    unsigned int glUvTexture_{0};
    unsigned int glYuvProgram_{0};
    unsigned int glNv12Program_{0};
    std::uint32_t glTextureWidth_{0};
    std::uint32_t glTextureHeight_{0};
    std::uint32_t glYuvWidth_{0};
    std::uint32_t glYuvHeight_{0};
    bool glAvailable_{false};
    double localCursorX_{0.5};
    double localCursorY_{0.5};
    bool localCursorVisible_{false};
    int xScreen_{0};
    std::uint32_t windowWidth_{0};
    std::uint32_t windowHeight_{0};
};

}  // namespace led::client
