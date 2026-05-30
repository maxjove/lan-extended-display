#include "led/common/logger.h"
#include "led/host/display_manager.h"
#include "led/host/cursor_suppressor.h"
#include "led/host/input_injector.h"
#include "led/host/input_receiver.h"
#include "led/host/jpeg_encoder.h"
#include "led/host/signaling_server.h"
#include "led/host/session_manager.h"
#include "led/host/video_sender.h"
#include "led/protocol/control_messages.h"
#include "led/protocol/messages.h"
#include "led/session/session_state.h"
#include "led/transport/h264_annex_b.h"
#include "led/transport/mjpeg_packet.h"
#include "led/transport/tcp_socket.h"
#include "led/transport/udp_socket.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

constexpr const char* kLiveCaptureStopEventName = "Local\\LanExtendedDisplayHostStop";
constexpr std::uint16_t kFrameAckPort = 17692;
constexpr std::uint32_t kMaxDirtyRectsPerFrame = 8;
constexpr std::uint64_t kSubFrameIdStride = 16;
#if defined(_WIN32)
constexpr const wchar_t* kDriverCreateMonitorEventName = L"Global\\LanExtendedDisplayCreateMonitor";
constexpr const wchar_t* kDriverDestroyMonitorEventName = L"Global\\LanExtendedDisplayDestroyMonitor";
#endif

bool hostDiagnosticsEnabled() {
    const char* value = std::getenv("LED_HOST_DIAGNOSTICS");
    if (value == nullptr) {
        value = std::getenv("LED_ENABLE_STATS");
    }
    if (value == nullptr) {
        return false;
    }
    const std::string flag = value;
    return flag == "1" || flag == "true" || flag == "TRUE" || flag == "on" || flag == "ON";
}

bool signalDriverEvent(const wchar_t* eventName) {
#if defined(_WIN32)
    HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE, eventName);
    if (event == nullptr) {
        return false;
    }
    const BOOL ok = SetEvent(event);
    CloseHandle(event);
    return ok != FALSE;
#else
    return false;
#endif
}

bool signalDriverCreateMonitor() {
#if defined(_WIN32)
    return signalDriverEvent(kDriverCreateMonitorEventName);
#else
    return false;
#endif
}

bool signalDriverDestroyMonitor() {
#if defined(_WIN32)
    return signalDriverEvent(kDriverDestroyMonitorEventName);
#else
    return false;
#endif
}

class DriverMonitorLease {
public:
    DriverMonitorLease() = default;
    DriverMonitorLease(const DriverMonitorLease&) = delete;
    DriverMonitorLease& operator=(const DriverMonitorLease&) = delete;
    ~DriverMonitorLease() {
        if (armed_) {
            signalDriverDestroyMonitor();
        }
    }

    void requestCreate() {
        signalDriverCreateMonitor();
        armed_ = true;
    }

    void release() {
        if (armed_) {
            signalDriverDestroyMonitor();
            armed_ = false;
        }
    }

    void disarm() {
        armed_ = false;
    }

private:
    bool armed_{false};
};

struct FrameAckStats {
    std::uint64_t acks{0};
    std::uint64_t unknownAcks{0};
    double averageRttUs{0.0};
    std::uint64_t maxRttUs{0};
    std::uint64_t lastFrameId{0};
};

enum class FrameAckStage {
    receive,
    render,
};

struct ParsedFrameAck {
    std::uint64_t frameId{0};
    FrameAckStage stage{FrameAckStage::receive};
};

struct FrameAckTracker {
    std::mutex mutex;
    std::unordered_map<std::uint64_t, std::chrono::steady_clock::time_point> sentFrames;
    FrameAckStats receiveStats;
    FrameAckStats renderStats;
    std::chrono::steady_clock::time_point lastReceiveAck{};
    std::chrono::steady_clock::time_point lastRenderAck{};
};

std::string parseAckField(const std::string& message, const char* key) {
    const auto begin = message.find(key);
    if (begin == std::string::npos) {
        return {};
    }
    const auto valueBegin = begin + std::char_traits<char>::length(key);
    const auto end = message.find(';', valueBegin);
    return message.substr(valueBegin, end == std::string::npos ? std::string::npos : end - valueBegin);
}

std::optional<ParsedFrameAck> parseFrameAck(const std::string& message) {
    constexpr const char* prefix = "LED_FRAME_ACK_V1";
    if (message.rfind(prefix, 0) != 0) {
        return std::nullopt;
    }
    const auto frameText = parseAckField(message, "frame=");
    if (frameText.empty()) {
        return std::nullopt;
    }
    try {
        ParsedFrameAck ack;
        ack.frameId = static_cast<std::uint64_t>(std::stoull(frameText));
        ack.stage = parseAckField(message, "stage=") == "render" ? FrameAckStage::render : FrameAckStage::receive;
        return ack;
    } catch (...) {
        return std::nullopt;
    }
}

void noteFrameSent(FrameAckTracker& tracker, std::uint64_t frameId) {
    if (frameId == 0) {
        return;
    }
    std::scoped_lock lock(tracker.mutex);
    tracker.sentFrames[frameId] = std::chrono::steady_clock::now();
    if (tracker.sentFrames.size() > 240) {
        const auto cutoff = frameId > 240 ? frameId - 240 : 0;
        for (auto it = tracker.sentFrames.begin(); it != tracker.sentFrames.end();) {
            if (it->first < cutoff) {
                it = tracker.sentFrames.erase(it);
            } else {
                ++it;
            }
        }
    }
}

void updateAckStats(FrameAckStats& stats, std::uint64_t frameId, std::uint64_t rttUs) {
    ++stats.acks;
    stats.lastFrameId = std::max(stats.lastFrameId, frameId);
    stats.maxRttUs = std::max(stats.maxRttUs, rttUs);
    if (stats.acks == 1) {
        stats.averageRttUs = static_cast<double>(rttUs);
    } else {
        stats.averageRttUs += (static_cast<double>(rttUs) - stats.averageRttUs) /
            static_cast<double>(stats.acks);
    }
}

void noteFrameAck(FrameAckTracker& tracker, const ParsedFrameAck& ack) {
    const auto now = std::chrono::steady_clock::now();
    std::scoped_lock lock(tracker.mutex);
    auto found = tracker.sentFrames.find(ack.frameId);
    auto& stats = ack.stage == FrameAckStage::render ? tracker.renderStats : tracker.receiveStats;
    if (found == tracker.sentFrames.end()) {
        ++stats.unknownAcks;
        return;
    }
    const auto rttUs = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(now - found->second).count());
    updateAckStats(stats, ack.frameId, rttUs);
    if (ack.stage == FrameAckStage::render) {
        tracker.lastRenderAck = now;
    } else {
        tracker.lastReceiveAck = now;
    }
    if (ack.stage == FrameAckStage::render) {
        tracker.sentFrames.erase(found);
    }
}

std::pair<FrameAckStats, FrameAckStats> snapshotFrameAckStats(FrameAckTracker& tracker) {
    std::scoped_lock lock(tracker.mutex);
    return {tracker.receiveStats, tracker.renderStats};
}

bool frameAckTimedOut(
    FrameAckTracker& tracker,
    std::chrono::steady_clock::time_point now,
    std::chrono::seconds timeout) {
    std::scoped_lock lock(tracker.mutex);
    if (tracker.receiveStats.acks == 0 && tracker.renderStats.acks == 0) {
        return false;
    }
    const auto latestAck = tracker.lastReceiveAck > tracker.lastRenderAck
        ? tracker.lastReceiveAck
        : tracker.lastRenderAck;
    return latestAck.time_since_epoch().count() != 0 && now - latestAck > timeout;
}

struct MjpegLiveStats {
    std::atomic_uint64_t encodedBytes{0};
    std::atomic_uint64_t packetsSent{0};
    std::atomic_uint64_t sendFailures{0};
    std::atomic_uint64_t dirtyFrames{0};
    std::atomic_uint64_t fullFrames{0};
    std::atomic_uint64_t skippedFrames{0};
    std::atomic_uint64_t idleDiffSkippedFrames{0};
    std::atomic_uint64_t idleCaptureSkippedFrames{0};
    std::atomic_uint64_t nativeDirtyFrames{0};
    std::atomic_uint64_t partialCopyFrames{0};
    std::atomic_uint64_t dxgiDirtyFrames{0};
    std::atomic_uint64_t dxgiDirtyRects{0};
    std::atomic_uint64_t dxgiDirtyAreaPixels{0};
    std::atomic_uint64_t tileDiffFrames{0};
    std::atomic_uint64_t dirtyRectsSent{0};
    std::atomic_uint64_t idleRefreshFrames{0};
    std::atomic_uint64_t dirtyAreaPixels{0};
    std::atomic_uint64_t encodeTasksDropped{0};
    std::atomic_uint64_t captureUs{0};
    std::atomic_uint64_t dirtyUs{0};
    std::atomic_uint64_t encodeUs{0};
    std::atomic_uint64_t packetizeUs{0};
    std::atomic_uint64_t sendUs{0};
    std::atomic_uint64_t encodedRects{0};
    std::atomic_uint64_t qualityBoostedRects{0};
};

struct MjpegLiveSnapshot {
    std::uint64_t encodedBytes{0};
    std::uint64_t packetsSent{0};
    std::uint64_t fullFrames{0};
    std::uint64_t dirtyFrames{0};
    std::uint64_t skippedFrames{0};
    std::uint64_t idleCaptureSkippedFrames{0};
    std::uint64_t partialCopyFrames{0};
    std::uint64_t dxgiDirtyFrames{0};
    std::uint64_t dxgiDirtyRects{0};
    std::uint64_t dxgiDirtyAreaPixels{0};
    std::uint64_t dirtyRectsSent{0};
    std::uint64_t dirtyAreaPixels{0};
    std::uint64_t encodeTasksDropped{0};
    std::uint64_t captureUs{0};
    std::uint64_t dirtyUs{0};
    std::uint64_t encodeUs{0};
    std::uint64_t packetizeUs{0};
    std::uint64_t sendUs{0};
    std::uint64_t encodedRects{0};
    std::uint64_t qualityBoostedRects{0};
    std::uint32_t capturedFrames{0};
};

MjpegLiveSnapshot snapshotMjpegLiveStats(const MjpegLiveStats& stats, std::uint32_t capturedFrames) {
    return MjpegLiveSnapshot{
        stats.encodedBytes.load(std::memory_order_relaxed),
        stats.packetsSent.load(std::memory_order_relaxed),
        stats.fullFrames.load(std::memory_order_relaxed),
        stats.dirtyFrames.load(std::memory_order_relaxed),
        stats.skippedFrames.load(std::memory_order_relaxed),
        stats.idleCaptureSkippedFrames.load(std::memory_order_relaxed),
        stats.partialCopyFrames.load(std::memory_order_relaxed),
        stats.dxgiDirtyFrames.load(std::memory_order_relaxed),
        stats.dxgiDirtyRects.load(std::memory_order_relaxed),
        stats.dxgiDirtyAreaPixels.load(std::memory_order_relaxed),
        stats.dirtyRectsSent.load(std::memory_order_relaxed),
        stats.dirtyAreaPixels.load(std::memory_order_relaxed),
        stats.encodeTasksDropped.load(std::memory_order_relaxed),
        stats.captureUs.load(std::memory_order_relaxed),
        stats.dirtyUs.load(std::memory_order_relaxed),
        stats.encodeUs.load(std::memory_order_relaxed),
        stats.packetizeUs.load(std::memory_order_relaxed),
        stats.sendUs.load(std::memory_order_relaxed),
        stats.encodedRects.load(std::memory_order_relaxed),
        stats.qualityBoostedRects.load(std::memory_order_relaxed),
        capturedFrames,
    };
}

std::uint64_t delta(std::uint64_t current, std::uint64_t previous) {
    return current >= previous ? current - previous : 0;
}

struct DirtyRect {
    std::uint32_t x{0};
    std::uint32_t y{0};
    std::uint32_t width{0};
    std::uint32_t height{0};
    bool fullFrame{true};
    bool empty{false};
};

DirtyRect fullDirtyRect(std::uint32_t width, std::uint32_t height) {
    return DirtyRect{0, 0, width, height, true, width == 0 || height == 0};
}

std::uint64_t dirtyRectArea(const DirtyRect& rect) {
    if (rect.empty) {
        return 0;
    }
    return static_cast<std::uint64_t>(rect.width) * rect.height;
}

std::uint64_t elapsedUsSince(std::chrono::steady_clock::time_point start) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start).count());
}

float selectJpegQualityForDirtyRect(
    const DirtyRect& dirty,
    std::uint32_t frameWidth,
    std::uint32_t frameHeight,
    float baseQuality,
    float idleRefreshQuality,
    bool idleRefresh,
    bool& boosted) {
    boosted = false;
    if (idleRefresh) {
        return idleRefreshQuality;
    }
    if (dirty.empty || dirty.fullFrame || frameWidth == 0 || frameHeight == 0) {
        return baseQuality;
    }

    const auto fullArea = static_cast<double>(frameWidth) * static_cast<double>(frameHeight);
    if (fullArea <= 0.0) {
        return baseQuality;
    }
    const auto dirtyRatio = static_cast<double>(dirtyRectArea(dirty)) / fullArea;
    float selected = baseQuality;
    if (dirtyRatio <= 0.03) {
        selected = std::max(selected, 0.96F);
    } else if (dirtyRatio <= 0.15) {
        selected = std::max(selected, 0.92F);
    }
    boosted = selected > baseQuality;
    return selected;
}

DirtyRect unionDirtyRects(const DirtyRect& a, const DirtyRect& b) {
    if (a.empty) {
        return b;
    }
    if (b.empty) {
        return a;
    }
    const auto minX = std::min(a.x, b.x);
    const auto minY = std::min(a.y, b.y);
    const auto maxX = std::max(a.x + a.width - 1, b.x + b.width - 1);
    const auto maxY = std::max(a.y + a.height - 1, b.y + b.height - 1);
    return DirtyRect{minX, minY, maxX - minX + 1, maxY - minY + 1, a.fullFrame || b.fullFrame, false};
}

bool dirtyRectsShouldMerge(const DirtyRect& a, const DirtyRect& b) {
    const auto merged = unionDirtyRects(a, b);
    const auto separateArea = dirtyRectArea(a) + dirtyRectArea(b);
    return dirtyRectArea(merged) <= separateArea + separateArea / 4;
}

DirtyRect dirtyRectFromBounds(
    std::uint32_t frameWidth,
    std::uint32_t frameHeight,
    std::uint32_t minX,
    std::uint32_t minY,
    std::uint32_t maxX,
    std::uint32_t maxY,
    std::uint32_t alignPixels,
    double fullFrameAreaRatio) {
    if (frameWidth == 0 || frameHeight == 0 || maxX < minX || maxY < minY) {
        return DirtyRect{0, 0, 0, 0, false, true};
    }

    const auto align = std::max<std::uint32_t>(1, alignPixels);
    minX = (minX / align) * align;
    minY = (minY / align) * align;
    maxX = std::min<std::uint32_t>(frameWidth - 1, ((maxX + align) / align) * align + (align - 1));
    maxY = std::min<std::uint32_t>(frameHeight - 1, ((maxY + align) / align) * align + (align - 1));
    const auto width = maxX - minX + 1;
    const auto height = maxY - minY + 1;
    const auto area = static_cast<double>(width) * static_cast<double>(height);
    const auto fullArea = static_cast<double>(frameWidth) * static_cast<double>(frameHeight);
    if (fullArea <= 0.0 || area / fullArea >= fullFrameAreaRatio) {
        return fullDirtyRect(frameWidth, frameHeight);
    }
    return DirtyRect{minX, minY, width, height, false, false};
}

std::vector<DirtyRect> reduceDirtyRects(
    std::vector<DirtyRect> rects,
    std::uint32_t frameWidth,
    std::uint32_t frameHeight,
    double fullFrameAreaRatio) {
    if (rects.empty()) {
        return {};
    }

    while (rects.size() > kMaxDirtyRectsPerFrame) {
        std::size_t bestA = 0;
        std::size_t bestB = 1;
        std::uint64_t bestPenalty = UINT64_MAX;
        for (std::size_t a = 0; a < rects.size(); ++a) {
            for (std::size_t b = a + 1; b < rects.size(); ++b) {
                const auto merged = unionDirtyRects(rects[a], rects[b]);
                const auto penalty = dirtyRectArea(merged) - dirtyRectArea(rects[a]) - dirtyRectArea(rects[b]);
                if (penalty < bestPenalty) {
                    bestPenalty = penalty;
                    bestA = a;
                    bestB = b;
                }
            }
        }
        rects[bestA] = unionDirtyRects(rects[bestA], rects[bestB]);
        rects.erase(rects.begin() + static_cast<std::ptrdiff_t>(bestB));
    }

    DirtyRect unionRect{};
    bool hasUnion = false;
    std::uint64_t totalArea = 0;
    for (const auto& rect : rects) {
        unionRect = hasUnion ? unionDirtyRects(unionRect, rect) : rect;
        hasUnion = true;
        totalArea += dirtyRectArea(rect);
    }
    const auto fullArea = static_cast<double>(frameWidth) * static_cast<double>(frameHeight);
    if (fullArea <= 0.0 || static_cast<double>(dirtyRectArea(unionRect)) / fullArea >= fullFrameAreaRatio) {
        return {fullDirtyRect(frameWidth, frameHeight)};
    }
    if (rects.size() <= 1 || static_cast<double>(totalArea) >= static_cast<double>(dirtyRectArea(unionRect)) * 0.85) {
        return {unionRect};
    }
    return rects;
}

std::vector<DirtyRect> computeNativeDirtyRects(
    const led::host::CapturedFrame& frame,
    std::uint32_t alignPixels,
    double fullFrameAreaRatio) {
    if (frame.bgra.empty() || frame.width == 0 || frame.height == 0) {
        return {DirtyRect{0, 0, 0, 0, true, true}};
    }
    if (frame.dirtyRects.empty()) {
        return {};
    }

    std::vector<DirtyRect> rects;
    for (const auto& rect : frame.dirtyRects) {
        if (rect.width == 0 || rect.height == 0 || rect.x >= frame.width || rect.y >= frame.height) {
            continue;
        }
        const auto right = std::min<std::uint32_t>(frame.width, rect.x + rect.width);
        const auto bottom = std::min<std::uint32_t>(frame.height, rect.y + rect.height);
        if (right <= rect.x || bottom <= rect.y) {
            continue;
        }
        auto dirty = dirtyRectFromBounds(
            frame.width,
            frame.height,
            rect.x,
            rect.y,
            right - 1,
            bottom - 1,
            alignPixels,
            fullFrameAreaRatio);
        if (dirty.empty) {
            continue;
        }
        if (dirty.fullFrame) {
            return {fullDirtyRect(frame.width, frame.height)};
        }

        bool merged = false;
        for (auto& existing : rects) {
            if (dirtyRectsShouldMerge(existing, dirty)) {
                existing = unionDirtyRects(existing, dirty);
                merged = true;
                break;
            }
        }
        if (!merged) {
            rects.push_back(dirty);
        }
    }

    if (rects.empty()) {
        return {};
    }

    return reduceDirtyRects(std::move(rects), frame.width, frame.height, fullFrameAreaRatio);
}

std::vector<DirtyRect> computeDirtyRects(
    const led::host::CapturedFrame& current,
    const led::host::CapturedFrame& previous,
    std::uint32_t alignPixels,
    double fullFrameAreaRatio) {
    if (current.bgra.empty() || current.width == 0 || current.height == 0) {
        return {DirtyRect{0, 0, 0, 0, true, true}};
    }
    if (previous.bgra.size() != current.bgra.size() ||
        previous.width != current.width ||
        previous.height != current.height) {
        return {fullDirtyRect(current.width, current.height)};
    }

    constexpr std::uint32_t kTilePixels = 32;
    constexpr std::uint32_t kBytesPerPixel = 4;
    const auto tileColumns = (current.width + kTilePixels - 1) / kTilePixels;
    const auto tileRows = (current.height + kTilePixels - 1) / kTilePixels;
    std::vector<std::uint8_t> changedTiles(static_cast<std::size_t>(tileColumns) * tileRows, 0);

    for (std::uint32_t tileY = 0; tileY < current.height; tileY += kTilePixels) {
        const auto tileBottom = std::min<std::uint32_t>(current.height, tileY + kTilePixels);
        for (std::uint32_t tileX = 0; tileX < current.width; tileX += kTilePixels) {
            const auto tileRight = std::min<std::uint32_t>(current.width, tileX + kTilePixels);
            const auto rowBytes = static_cast<std::size_t>(tileRight - tileX) * kBytesPerPixel;
            bool tileChanged = false;
            for (std::uint32_t y = tileY; y < tileBottom; ++y) {
                const auto offset =
                    (static_cast<std::size_t>(y) * current.width + tileX) * kBytesPerPixel;
                if (std::memcmp(current.bgra.data() + offset, previous.bgra.data() + offset, rowBytes) != 0) {
                    tileChanged = true;
                    break;
                }
            }
            if (tileChanged) {
                const auto column = tileX / kTilePixels;
                const auto row = tileY / kTilePixels;
                changedTiles[static_cast<std::size_t>(row) * tileColumns + column] = 1;
            }
        }
    }

    std::vector<std::uint8_t> visited(changedTiles.size(), 0);
    std::vector<DirtyRect> rects;
    std::vector<std::uint32_t> stack;
    for (std::uint32_t row = 0; row < tileRows; ++row) {
        for (std::uint32_t column = 0; column < tileColumns; ++column) {
            const auto startIndex = static_cast<std::size_t>(row) * tileColumns + column;
            if (!changedTiles[startIndex] || visited[startIndex]) {
                continue;
            }

            std::uint32_t minTileX = column;
            std::uint32_t minTileY = row;
            std::uint32_t maxTileX = column;
            std::uint32_t maxTileY = row;
            visited[startIndex] = 1;
            stack.clear();
            stack.push_back(static_cast<std::uint32_t>(startIndex));
            while (!stack.empty()) {
                const auto index = stack.back();
                stack.pop_back();
                const auto currentRow = index / tileColumns;
                const auto currentColumn = index % tileColumns;
                minTileX = std::min(minTileX, currentColumn);
                minTileY = std::min(minTileY, currentRow);
                maxTileX = std::max(maxTileX, currentColumn);
                maxTileY = std::max(maxTileY, currentRow);

                const std::int32_t neighbors[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
                for (const auto& neighbor : neighbors) {
                    const auto nextColumn = static_cast<std::int32_t>(currentColumn) + neighbor[0];
                    const auto nextRow = static_cast<std::int32_t>(currentRow) + neighbor[1];
                    if (nextColumn < 0 || nextRow < 0 ||
                        nextColumn >= static_cast<std::int32_t>(tileColumns) ||
                        nextRow >= static_cast<std::int32_t>(tileRows)) {
                        continue;
                    }
                    const auto nextIndex =
                        static_cast<std::size_t>(nextRow) * tileColumns + static_cast<std::uint32_t>(nextColumn);
                    if (!changedTiles[nextIndex] || visited[nextIndex]) {
                        continue;
                    }
                    visited[nextIndex] = 1;
                    stack.push_back(static_cast<std::uint32_t>(nextIndex));
                }
            }

            const auto minX = minTileX * kTilePixels;
            const auto minY = minTileY * kTilePixels;
            const auto maxX = std::min<std::uint32_t>(current.width - 1, (maxTileX + 1) * kTilePixels - 1);
            const auto maxY = std::min<std::uint32_t>(current.height - 1, (maxTileY + 1) * kTilePixels - 1);
            auto dirty = dirtyRectFromBounds(
                current.width,
                current.height,
                minX,
                minY,
                maxX,
                maxY,
                alignPixels,
                fullFrameAreaRatio);
            if (dirty.fullFrame) {
                return {fullDirtyRect(current.width, current.height)};
            }
            if (!dirty.empty) {
                rects.push_back(dirty);
            }
        }
    }

    if (rects.empty()) {
        return {};
    }

    return reduceDirtyRects(std::move(rects), current.width, current.height, fullFrameAreaRatio);
}

struct MjpegEncodeTask {
    led::host::CapturedFrame frame;
    std::vector<DirtyRect> dirtyRects;
    bool idleRefresh{false};
};

std::thread startFrameAckListener(FrameAckTracker& tracker, std::atomic_bool& stop) {
    return std::thread([&tracker, &stop]() {
        led::transport::UdpSocket socket;
        auto status = socket.bind(kFrameAckPort);
        if (!status.isOk()) {
            led::logWarn("frame ack listener disabled: " + status.message());
            return;
        }
        socket.setReceiveTimeoutMs(250);
        led::logInfo("host frame ack listener bound on UDP port " + std::to_string(kFrameAckPort));
        while (!stop.load()) {
            std::vector<std::uint8_t> bytes;
            led::transport::UdpEndpoint source;
            status = socket.receiveFrom(512, bytes, source);
            if (!status.isOk()) {
                continue;
            }
            const std::string message(bytes.begin(), bytes.end());
            const auto ack = parseFrameAck(message);
            if (ack.has_value()) {
                noteFrameAck(tracker, *ack);
            }
        }
        socket.close();
    });
}

std::uint16_t parsePort(const char* value, std::uint16_t fallback) {
    try {
        const auto parsed = static_cast<unsigned long>(std::stoul(value));
        if (parsed <= 65535) {
            return static_cast<std::uint16_t>(parsed);
        }
    } catch (...) {
    }
    return fallback;
}

#if defined(_WIN32)
struct LiveCaptureStopEvent {
    HANDLE handle{nullptr};

    LiveCaptureStopEvent() {
        handle = CreateEventA(nullptr, TRUE, FALSE, kLiveCaptureStopEventName);
        if (handle != nullptr) {
            ResetEvent(handle);
        }
    }

    ~LiveCaptureStopEvent() {
        if (handle != nullptr) {
            CloseHandle(handle);
        }
    }

    bool requested() const {
        return handle != nullptr && WaitForSingleObject(handle, 0) == WAIT_OBJECT_0;
    }
};

void enableProcessDpiAwareness() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
}
#else
struct LiveCaptureStopEvent {
    bool requested() const {
        return false;
    }
};

void enableProcessDpiAwareness() {}
#endif

std::uint32_t parseU32(const char* value, std::uint32_t fallback) {
    try {
        return static_cast<std::uint32_t>(std::stoul(value));
    } catch (...) {
    }
    return fallback;
}

led::protocol::VideoMode makeVideoMode(std::uint32_t width, std::uint32_t height, std::uint32_t fps, std::uint32_t bitrateKbps) {
    auto mode = led::protocol::defaultStandardMode();
    if (width > 0) {
        mode.resolution.width = width;
    }
    if (height > 0) {
        mode.resolution.height = height;
    }
    if (fps > 0) {
        mode.resolution.refreshRate = fps;
    }
    if (bitrateKbps > 0) {
        mode.bitrateKbps = bitrateKbps;
    }
    return mode;
}

void printInputReceiverStats(const led::host::InputReceiverStats& stats) {
    std::cout << "Input receiver stats: packets=" << stats.packetsReceived
              << " bytes=" << stats.bytesReceived
              << " malformed=" << stats.malformedPackets
              << " events=" << stats.eventsReceived
              << " moves=" << stats.pointerMoves
              << " buttons=" << stats.pointerButtons
              << " wheels=" << stats.wheelEvents
              << " keys=" << stats.keyEvents
              << " touches=" << stats.touchEvents
              << " sequence_gaps=" << stats.sequenceGaps
              << " estimated_lost=" << stats.estimatedLostEvents
              << " out_of_order=" << stats.outOfOrderEvents << '\n';
    if (stats.timingEvents > 0) {
        std::cout << "Input timing stats: samples=" << stats.timingEvents
                  << " min_us=" << stats.minLatencyUs
                  << " avg_us=" << stats.averageLatencyUs
                  << " max_us=" << stats.maxLatencyUs
                  << " jitter_us=" << stats.jitterUs
                  << " clock_skewed=" << (stats.minLatencyUs < 0 || stats.maxLatencyUs < 0 ? "yes" : "no")
                  << '\n';
    }
}

led::Status acceptReadyClient(
    std::uint16_t controlPort,
    std::uint16_t rtpPort,
    const led::protocol::VideoMode& videoMode,
    led::transport::TcpStream& stream,
    led::protocol::ClientHello& hello,
    led::protocol::ClientReady& ready,
    int acceptTimeoutMs = 0) {
    led::protocol::SessionOffer offer;
    offer.videoMode = videoMode;
    offer.rtpPort = rtpPort;
    offer.sessionToken = "local-session-token";

    led::transport::TcpListener listener;
    auto status = listener.bindAndListen(controlPort);
    if (!status.isOk()) {
        return status;
    }
    if (acceptTimeoutMs > 0) {
        status = listener.setAcceptTimeoutMs(acceptTimeoutMs);
        if (!status.isOk()) {
            return status;
        }
    }

    std::cout << "Host test signaling listening on TCP " << listener.localPort()
              << ", offering RTP " << offer.rtpPort << '\n';

    status = listener.accept(stream);
    listener.close();
    if (!status.isOk()) {
        return status;
    }

    std::string line;
    status = stream.readLine(line);
    if (!status.isOk()) {
        return status;
    }

    status = led::protocol::parseClientHello(line, hello);
    if (!status.isOk()) {
        return status;
    }

    status = stream.sendLine(led::protocol::serializeSessionOffer(offer));
    if (!status.isOk()) {
        return status;
    }

    status = stream.readLine(line);
    if (!status.isOk()) {
        return status;
    }

    status = led::protocol::parseClientReady(line, ready);
    if (!status.isOk()) {
        return status;
    }
    if (ready.sessionToken != offer.sessionToken) {
        return led::Status::invalidArgument("CLIENT_READY token mismatch");
    }

    return led::Status::ok();
}

int runListenMode(int argc, char** argv) {
    const auto controlPort = argc >= 3 ? parsePort(argv[2], 17660) : std::uint16_t{17660};
    const auto rtpPort = argc >= 4 ? parsePort(argv[3], 17670) : std::uint16_t{17670};

    led::protocol::SessionOffer offer;
    offer.videoMode = led::protocol::defaultStandardMode();
    offer.rtpPort = rtpPort;
    offer.sessionToken = "local-session-token";

    led::host::SignalingServer server;
    auto status = server.listen(controlPort);
    if (!status.isOk()) {
        led::logError(status.message());
        return 1;
    }

    std::cout << "Host signaling listening on TCP " << server.port()
              << ", offering RTP " << offer.rtpPort << '\n';
    std::cout << "Waiting for one client hello...\n";

    led::protocol::ClientHello hello;
    status = server.acceptClientAndSendOffer(offer, hello);
    server.close();
    if (!status.isOk()) {
        led::logError(status.message());
        return 1;
    }

    std::cout << "Client accepted: " << hello.device.name
              << " (" << hello.device.platform << ")\n";
    std::cout << "Session offer sent: "
              << offer.videoMode.resolution.width << 'x'
              << offer.videoMode.resolution.height << '@'
              << offer.videoMode.resolution.refreshRate
              << " codec=" << led::protocol::toString(offer.videoMode.codec)
              << " rtp=" << offer.rtpPort << '\n';
    return 0;
}

int runListDisplaysMode() {
    const auto bounds = led::host::DisplayManager::currentDesktopBounds();
    std::cout << "Virtual desktop: origin=" << bounds.left << ',' << bounds.top
              << " size=" << bounds.width << 'x' << bounds.height << '\n';

    const auto displays = led::host::DisplayManager::enumerateDisplays();
    if (displays.empty()) {
        std::cout << "No active displays found.\n";
        return 0;
    }

    for (const auto& display : displays) {
        std::cout << "Display: " << display.deviceName
                  << " origin=" << display.originX << ',' << display.originY
                  << " size=" << display.width << 'x' << display.height
                  << " primary=" << (display.primary ? "yes" : "no")
                  << " led_virtual=" << (display.ledVirtual ? "yes" : "no") << '\n';
    }
    return 0;
}

int runSendTestNalMode(int argc, char** argv) {
    const auto controlPort = argc >= 3 ? parsePort(argv[2], 17660) : std::uint16_t{17660};
    const auto rtpPort = argc >= 4 ? parsePort(argv[3], 17670) : std::uint16_t{17670};

    led::transport::TcpStream stream;
    led::protocol::ClientHello hello;
    led::protocol::ClientReady ready;
    auto status = acceptReadyClient(controlPort, rtpPort, led::protocol::defaultStandardMode(), stream, hello, ready);
    if (!status.isOk()) {
        led::logError(status.message());
        return 1;
    }

    std::vector<std::uint8_t> testNal(3000, 0x33);
    testNal.front() = 0x65;

    const auto target = led::transport::UdpEndpoint{stream.peerEndpoint().address, ready.rtpPort};
    led::host::VideoSender sender;
    sender.setEmbedSendTime(true);
    status = sender.open(target);
    if (!status.isOk()) {
        led::logError(status.message());
        return 1;
    }
    status = sender.sendNal(testNal, 90000);
    sender.close();
    if (!status.isOk()) {
        led::logError(status.message());
        return 1;
    }

    std::cout << "Client ready: " << hello.device.name
              << " peer=" << target.address << ':' << target.port << '\n';
    std::cout << "Sent test H.264 NAL bytes=" << testNal.size() << '\n';
    return 0;
}

int runSendTestStreamMode(int argc, char** argv) {
    const auto controlPort = argc >= 3 ? parsePort(argv[2], 17660) : std::uint16_t{17660};
    const auto rtpPort = argc >= 4 ? parsePort(argv[3], 17670) : std::uint16_t{17670};
    const auto frameCount = argc >= 5 ? parseU32(argv[4], 120) : std::uint32_t{120};
    const auto fps = argc >= 6 ? parseU32(argv[5], 60) : std::uint32_t{60};

    led::transport::TcpStream stream;
    led::protocol::ClientHello hello;
    led::protocol::ClientReady ready;
    auto status = acceptReadyClient(controlPort, rtpPort, led::protocol::defaultStandardMode(), stream, hello, ready);
    if (!status.isOk()) {
        led::logError(status.message());
        return 1;
    }

    const auto target = led::transport::UdpEndpoint{stream.peerEndpoint().address, ready.rtpPort};
    led::host::VideoSender sender;
    sender.setEmbedSendTime(true);
    status = sender.open(target);
    if (!status.isOk()) {
        led::logError(status.message());
        return 1;
    }

    const auto frameInterval = std::chrono::microseconds(1000000 / (fps == 0 ? 60 : fps));
    const auto timestampStep = 90000 / (fps == 0 ? 60 : fps);
    auto nextFrameTime = std::chrono::steady_clock::now();

    for (std::uint32_t frame = 0; frame < frameCount; ++frame) {
        std::vector<std::uint8_t> nal(3000, static_cast<std::uint8_t>(frame & 0xFF));
        nal.front() = 0x65;
        status = sender.sendNal(nal, 90000 + frame * timestampStep);
        if (!status.isOk()) {
            sender.close();
            led::logError(status.message());
            return 1;
        }

        nextFrameTime += frameInterval;
        std::this_thread::sleep_until(nextFrameTime);
    }

    sender.close();
    std::cout << "Client ready: " << hello.device.name
              << " peer=" << target.address << ':' << target.port << '\n';
    std::cout << "Sent test stream frames=" << frameCount
              << " fps=" << (fps == 0 ? 60 : fps)
              << " nal_bytes=3000\n";
    return 0;
}

int runSendCaptureTestStreamMode(int argc, char** argv) {
    const auto controlPort = argc >= 3 ? parsePort(argv[2], 17660) : std::uint16_t{17660};
    const auto rtpPort = argc >= 4 ? parsePort(argv[3], 17670) : std::uint16_t{17670};
    const auto frameCount = argc >= 5 ? parseU32(argv[4], 120) : std::uint32_t{120};
    const auto fps = argc >= 6 ? parseU32(argv[5], 60) : std::uint32_t{60};
    const auto width = argc >= 7 ? parseU32(argv[6], 1920) : std::uint32_t{1920};
    const auto height = argc >= 8 ? parseU32(argv[7], 1080) : std::uint32_t{1080};
    const auto bitrateKbps = argc >= 9 ? parseU32(argv[8], 20000) : std::uint32_t{20000};
    const auto actualFps = fps == 0 ? std::uint32_t{60} : fps;
    const auto mode = makeVideoMode(width, height, actualFps, bitrateKbps);

    led::transport::TcpStream stream;
    led::protocol::ClientHello hello;
    led::protocol::ClientReady ready;
    auto status = acceptReadyClient(controlPort, rtpPort, mode, stream, hello, ready);
    if (!status.isOk()) {
        led::logError(status.message());
        return 1;
    }

    led::host::CaptureEngine capture;
    status = capture.start(mode.resolution);
    if (!status.isOk()) {
        led::logError(status.message());
        return 1;
    }

    led::host::Encoder encoder;
    status = encoder.configure(mode);
    if (!status.isOk()) {
        capture.stop();
        led::logError(status.message());
        return 1;
    }
    status = encoder.start();
    if (!status.isOk()) {
        capture.stop();
        led::logError(status.message());
        return 1;
    }

    const auto target = led::transport::UdpEndpoint{stream.peerEndpoint().address, ready.rtpPort};
    led::host::VideoSender sender;
    sender.setEmbedSendTime(true);
    status = sender.open(target);
    if (!status.isOk()) {
        encoder.stop();
        capture.stop();
        led::logError(status.message());
        return 1;
    }

    const auto frameInterval = std::chrono::microseconds(1000000 / actualFps);
    auto nextFrameTime = std::chrono::steady_clock::now();
    std::uint64_t bytesSent = 0;
    std::uint64_t nalUnitsSent = 0;
    std::uint64_t keyFrames = 0;
    const std::string encoderBackend = encoder.backendName();

    for (std::uint32_t index = 0; index < frameCount; ++index) {
        led::host::CapturedFrame frame;
        status = capture.captureNextFrame(frame);
        if (!status.isOk()) {
            sender.close();
            encoder.stop();
            capture.stop();
            led::logError(status.message());
            return 1;
        }

        const auto encoded = encoder.encode(frame);
        if (encoded.keyFrame) {
            ++keyFrames;
        }
        bytesSent += encoded.payload.size();
        nalUnitsSent += encoded.nalUnits.size();
        status = sender.sendFrame(encoded);
        if (!status.isOk()) {
            sender.close();
            encoder.stop();
            capture.stop();
            led::logError(status.message());
            return 1;
        }

        nextFrameTime += frameInterval;
        std::this_thread::sleep_until(nextFrameTime);
    }

    sender.close();
    encoder.stop();
    capture.stop();

    std::cout << "Client ready: " << hello.device.name
              << " peer=" << target.address << ':' << target.port << '\n';
    std::cout << "Sent capture test stream frames=" << frameCount
              << " fps=" << actualFps
              << " mode=" << mode.resolution.width << 'x' << mode.resolution.height
              << " bitrate_kbps=" << mode.bitrateKbps
              << " backend=" << encoderBackend
              << " nals=" << nalUnitsSent
              << " keyframes=" << keyFrames
              << " encoded_bytes=" << bytesSent << '\n';
    return 0;
}

int runServeLiveCaptureMode(int argc, char** argv) {
    LiveCaptureStopEvent stopEvent;
    const auto controlPort = argc >= 3 ? parsePort(argv[2], 17660) : std::uint16_t{17660};
    const auto rtpPort = argc >= 4 ? parsePort(argv[3], 17670) : std::uint16_t{17670};
    const auto inputPort = argc >= 5 ? parsePort(argv[4], 17691) : std::uint16_t{17691};
    const auto frameCount = argc >= 6 ? parseU32(argv[5], 0) : std::uint32_t{0};
    const auto fps = argc >= 7 ? parseU32(argv[6], 30) : std::uint32_t{30};
    led::host::InputInjectionBackend backend = led::host::InputInjectionBackend::dryRun;
    if (argc >= 8 && !led::host::parseInputInjectionBackend(argv[7], backend)) {
        std::cerr << "Usage: led_host_app --serve-live-capture [control-port] [rtp-port] [input-port] [frames] [fps] [dry-run|sendinput] [bitrate-kbps] [width] [height]\n";
        return 1;
    }
    const auto bitrateKbps = argc >= 9 ? parseU32(argv[8], 20000) : std::uint32_t{20000};
    const auto width = argc >= 10 ? parseU32(argv[9], 1920) : std::uint32_t{1920};
    const auto height = argc >= 11 ? parseU32(argv[10], 1080) : std::uint32_t{1080};
    const auto actualFps = fps == 0 ? std::uint32_t{30} : fps;
    const auto mode = makeVideoMode(width, height, actualFps, bitrateKbps);

    DriverMonitorLease driverMonitor;
    led::host::DisplayManager displayManager;
    driverMonitor.requestCreate();
    auto status = displayManager.createVirtualDisplay(mode.resolution);
    if (!status.isOk()) {
        led::logError(status.message());
        return 1;
    }

    led::host::InputReceiver inputReceiver;
    status = inputReceiver.bind(inputPort);
    if (!status.isOk()) {
        displayManager.destroyVirtualDisplay();
        displayManager.restoreDisplayLayout();
        led::logError(status.message());
        return 1;
    }
    inputReceiver.setReceiveTimeoutMs(200);
    led::host::InputInjector injector(displayManager, backend);
    std::atomic_bool stopInput{false};
    led::transport::UdpEndpoint lastInputSource;
    led::Status inputStatus;
    const bool diagnosticsEnabled = hostDiagnosticsEnabled();
    std::thread inputThread([&]() {
        auto lastStatsLog = std::chrono::steady_clock::now();
        while (!stopInput.load() && !stopEvent.requested()) {
            led::protocol::InputEvent event;
            led::transport::UdpEndpoint source;
            auto receiveStatus = inputReceiver.receive(event, source);
            const auto now = std::chrono::steady_clock::now();
            if (diagnosticsEnabled && now - lastStatsLog >= std::chrono::seconds(1)) {
                const auto stats = inputReceiver.stats();
                if (stats.eventsReceived > 0) {
                    std::cout << "Host input live stats: events=" << stats.eventsReceived
                              << " moves=" << stats.pointerMoves
                              << " buttons=" << stats.pointerButtons
                              << " keys=" << stats.keyEvents
                              << " avg_ms=" << (stats.averageLatencyUs / 1000.0)
                              << " jitter_ms=" << (stats.jitterUs / 1000.0)
                              << " sequence_gaps=" << stats.sequenceGaps << '\n';
                }
                lastStatsLog = now;
            }
            if (!receiveStatus.isOk()) {
                continue;
            }
            lastInputSource = source;
            auto injectStatus = injector.inject(event);
            if (!injectStatus.isOk()) {
                inputStatus = injectStatus;
                stopInput = true;
                return;
            }
        }
    });

    led::transport::TcpStream stream;
    led::protocol::ClientHello hello;
    led::protocol::ClientReady ready;
    status = acceptReadyClient(controlPort, rtpPort, mode, stream, hello, ready, 10000);
    if (!status.isOk()) {
        stopInput = true;
        if (inputThread.joinable()) {
            inputThread.join();
        }
        inputReceiver.close();
        displayManager.destroyVirtualDisplay();
        displayManager.restoreDisplayLayout();
        led::logError(status.message());
        return 1;
    }

    led::host::CaptureEngine capture;
    const auto& display = displayManager.virtualDisplay();
    status = capture.startRegion(mode.resolution, display.originX, display.originY, display.deviceName);
    if (!status.isOk()) {
        stopInput = true;
        if (inputThread.joinable()) {
            inputThread.join();
        }
        inputReceiver.close();
        displayManager.destroyVirtualDisplay();
        displayManager.restoreDisplayLayout();
        led::logError(status.message());
        return 1;
    }

    led::host::Encoder encoder;
    status = encoder.configure(mode);
    if (!status.isOk()) {
        capture.stop();
        stopInput = true;
        if (inputThread.joinable()) {
            inputThread.join();
        }
        inputReceiver.close();
        displayManager.destroyVirtualDisplay();
        displayManager.restoreDisplayLayout();
        led::logError(status.message());
        return 1;
    }
    status = encoder.start();
    if (!status.isOk()) {
        capture.stop();
        stopInput = true;
        if (inputThread.joinable()) {
            inputThread.join();
        }
        inputReceiver.close();
        displayManager.destroyVirtualDisplay();
        displayManager.restoreDisplayLayout();
        led::logError(status.message());
        return 1;
    }

    const auto target = led::transport::UdpEndpoint{stream.peerEndpoint().address, ready.rtpPort};
    led::host::VideoSender sender;
    sender.setEmbedSendTime(true);
    status = sender.open(target);
    if (!status.isOk()) {
        encoder.stop();
        capture.stop();
        stopInput = true;
        if (inputThread.joinable()) {
            inputThread.join();
        }
        inputReceiver.close();
        displayManager.destroyVirtualDisplay();
        displayManager.restoreDisplayLayout();
        led::logError(status.message());
        return 1;
    }

    FrameAckTracker frameAckTracker;
    auto frameAckThread = startFrameAckListener(frameAckTracker, stopInput);
    const auto frameInterval = std::chrono::microseconds(1000000 / actualFps);
    auto nextFrameTime = std::chrono::steady_clock::now();
    std::uint32_t capturedFrames = 0;
    std::uint64_t bytesSent = 0;
    std::uint64_t nalUnitsSent = 0;
    std::uint64_t keyFrames = 0;
    const std::string encoderBackend = encoder.backendName();
    auto lastVideoStatsLog = std::chrono::steady_clock::now();
    auto lastCaptureRebindAttempt = std::chrono::steady_clock::now();
    auto recoverCapture = [&]() -> led::Status {
        for (int attempt = 0; attempt < 80 && !stopInput.load() && !stopEvent.requested(); ++attempt) {
            signalDriverCreateMonitor();
            auto displayStatus = displayManager.createVirtualDisplay(mode.resolution);
            if (displayStatus.isOk()) {
                const auto& recoveredDisplay = displayManager.virtualDisplay();
                auto restartStatus = capture.startRegion(
                    mode.resolution,
                    recoveredDisplay.originX,
                    recoveredDisplay.originY,
                    recoveredDisplay.deviceName);
                if (restartStatus.isOk()) {
                    led::logInfo(
                        "host capture rebound to LED virtual display " + recoveredDisplay.deviceName +
                        " at " + std::to_string(recoveredDisplay.originX) + "," +
                        std::to_string(recoveredDisplay.originY));
                    return led::Status::ok();
                }
                capture.stop();
                displayStatus = restartStatus;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            status = displayStatus;
        }
        return status.isOk() ? led::Status::unavailable("capture recovery timed out") : status;
    };

    while (!stopInput.load() && !stopEvent.requested() && (frameCount == 0 || capturedFrames < frameCount)) {
        led::host::CapturedFrame frame;
        status = capture.captureNextFrame(frame);
        if (!status.isOk()) {
            led::logWarn("capture failed, rebinding virtual display: " + status.message());
            capture.stop();
            status = recoverCapture();
            if (!status.isOk()) {
                break;
            }
            continue;
        }

        const auto encoded = encoder.encode(frame);
        if (encoded.keyFrame) {
            ++keyFrames;
        }
        const bool hasEncodedData = !encoded.payload.empty() || !encoded.nalUnits.empty();
        bytesSent += encoded.payload.size();
        nalUnitsSent += encoded.nalUnits.size();
        if (hasEncodedData) {
            noteFrameSent(frameAckTracker, encoded.frameId);
        }
        status = sender.sendFrame(encoded);
        if (!status.isOk()) {
            break;
        }

        ++capturedFrames;
        const auto now = std::chrono::steady_clock::now();
        if (!capture.dxgiCapture_ && now - lastCaptureRebindAttempt >= std::chrono::seconds(2)) {
            lastCaptureRebindAttempt = now;
            led::logWarn("capture is running on fallback backend; trying to restore LED virtual display binding");
            capture.stop();
            const auto recoverStatus = recoverCapture();
            if (!recoverStatus.isOk()) {
                led::logWarn("LED virtual display restore attempt failed: " + recoverStatus.message());
                auto fallbackStatus = capture.startRegion(
                    mode.resolution,
                    displayManager.virtualDisplay().originX,
                    displayManager.virtualDisplay().originY,
                    displayManager.virtualDisplay().deviceName);
                if (!fallbackStatus.isOk()) {
                    status = fallbackStatus;
                    break;
                }
            }
        }
        if (diagnosticsEnabled && now - lastVideoStatsLog >= std::chrono::seconds(1)) {
            const auto [receiveAckStats, renderAckStats] = snapshotFrameAckStats(frameAckTracker);
            std::cout << "Host video live stats: frames=" << capturedFrames
                      << " nals=" << nalUnitsSent
                      << " keyframes=" << keyFrames
                      << " encoded_kb=" << (bytesSent / 1024)
                      << " fps_target=" << actualFps
                      << " receive_ack_count=" << receiveAckStats.acks
                      << " receive_ack_avg_ms=" << (receiveAckStats.averageRttUs / 1000.0)
                      << " receive_ack_max_ms=" << (receiveAckStats.maxRttUs / 1000.0)
                      << " render_ack_count=" << renderAckStats.acks
                      << " render_ack_avg_ms=" << (renderAckStats.averageRttUs / 1000.0)
                      << " render_ack_max_ms=" << (renderAckStats.maxRttUs / 1000.0)
                      << " render_ack_last_frame=" << renderAckStats.lastFrameId
                      << " ack_unknown=" << (receiveAckStats.unknownAcks + renderAckStats.unknownAcks) << '\n';
            lastVideoStatsLog = now;
        }
        nextFrameTime += frameInterval;
        std::this_thread::sleep_until(nextFrameTime);
    }

    sender.close();
    encoder.stop();
    capture.stop();
    stopInput = true;
    if (frameAckThread.joinable()) {
        frameAckThread.join();
    }
    if (inputThread.joinable()) {
        inputThread.join();
    }
    const auto inputStats = inputReceiver.stats();
    inputReceiver.close();
    displayManager.destroyVirtualDisplay();
    displayManager.restoreDisplayLayout();
    driverMonitor.release();

    if (!status.isOk()) {
        led::logError(status.message());
        return 1;
    }
    if (!inputStatus.isOk()) {
        led::logError(inputStatus.message());
        return 1;
    }

    std::cout << "Client ready: " << hello.device.name
              << " peer=" << target.address << ':' << target.port
              << " input_port=" << inputPort
              << " input_backend=" << injector.backendName() << '\n';
    std::cout << "Served live capture frames=" << capturedFrames
              << " fps=" << actualFps
              << " mode=" << mode.resolution.width << 'x' << mode.resolution.height
              << " bitrate_kbps=" << mode.bitrateKbps
              << " backend=" << encoderBackend
              << " nals=" << nalUnitsSent
              << " keyframes=" << keyFrames
              << " encoded_bytes=" << bytesSent << '\n';
    if (inputStats.eventsReceived > 0) {
        std::cout << "Last input source: " << lastInputSource.address << ':' << lastInputSource.port << '\n';
    }
    printInputReceiverStats(inputStats);
    return 0;
}

int runServeMjpegCaptureMode(int argc, char** argv) {
    LiveCaptureStopEvent stopEvent;
    const auto controlPort = argc >= 3 ? parsePort(argv[2], 17660) : std::uint16_t{17660};
    const auto udpPort = argc >= 4 ? parsePort(argv[3], 17670) : std::uint16_t{17670};
    const auto frameCount = argc >= 5 ? parseU32(argv[4], 0) : std::uint32_t{0};
    const auto fps = argc >= 6 ? parseU32(argv[5], 60) : std::uint32_t{60};
    const auto qualityPercent = argc >= 7 ? parseU32(argv[6], 55) : std::uint32_t{55};
    const auto width = argc >= 8 ? parseU32(argv[7], 1920) : std::uint32_t{1920};
    const auto height = argc >= 9 ? parseU32(argv[8], 1080) : std::uint32_t{1080};
    const auto inputPort = argc >= 10 ? parsePort(argv[9], 17691) : std::uint16_t{17691};
    led::host::InputInjectionBackend backend = led::host::InputInjectionBackend::sendInput;
    if (argc >= 11 && !led::host::parseInputInjectionBackend(argv[10], backend)) {
        std::cerr << "Usage: led_host_app --serve-mjpeg-capture [control-port] [udp-port] [frames] [fps] [quality-percent] [width] [height] [input-port] [dry-run|sendinput] [keep-monitor-on-exit]\n";
        return 1;
    }
    const bool keepMonitorOnExit = argc >= 12 && std::string(argv[11]) == "keep-monitor-on-exit";
    const auto actualFps = fps == 0 ? std::uint32_t{60} : fps;
    const auto mode = makeVideoMode(width, height, actualFps, 0);

    DriverMonitorLease driverMonitor;
    led::host::DisplayManager displayManager;
    driverMonitor.requestCreate();
    auto status = displayManager.createVirtualDisplay(mode.resolution);
    if (!status.isOk()) {
        led::logError(status.message());
        return 1;
    }

    led::host::InputReceiver inputReceiver;
    status = inputReceiver.bind(inputPort);
    if (!status.isOk()) {
        displayManager.destroyVirtualDisplay();
        displayManager.restoreDisplayLayout();
        led::logError(status.message());
        return 1;
    }
    inputReceiver.setReceiveTimeoutMs(200);
    led::host::InputInjector injector(displayManager, backend);
    std::atomic_bool stopInput{false};
    led::Status inputStatus;
    const bool diagnosticsEnabled = hostDiagnosticsEnabled();
    std::thread inputThread([&]() {
        auto lastStatsLog = std::chrono::steady_clock::now();
        while (!stopInput.load() && !stopEvent.requested()) {
            led::protocol::InputEvent event;
            led::transport::UdpEndpoint source;
            auto receiveStatus = inputReceiver.receive(event, source);
            const auto now = std::chrono::steady_clock::now();
            if (diagnosticsEnabled && now - lastStatsLog >= std::chrono::seconds(1)) {
                const auto stats = inputReceiver.stats();
                if (stats.eventsReceived > 0) {
                    std::cout << "Host input live stats: events=" << stats.eventsReceived
                              << " moves=" << stats.pointerMoves
                              << " buttons=" << stats.pointerButtons
                              << " keys=" << stats.keyEvents
                              << " avg_ms=" << (stats.averageLatencyUs / 1000.0)
                              << " jitter_ms=" << (stats.jitterUs / 1000.0)
                              << " sequence_gaps=" << stats.sequenceGaps << '\n';
                }
                lastStatsLog = now;
            }
            if (!receiveStatus.isOk()) {
                continue;
            }
            auto injectStatus = injector.inject(event);
            if (!injectStatus.isOk()) {
                inputStatus = injectStatus;
                stopInput = true;
                return;
            }
        }
    });
    auto stopInputThread = [&]() {
        stopInput = true;
        if (inputThread.joinable()) {
            inputThread.join();
        }
        inputReceiver.close();
    };

    led::transport::TcpStream stream;
    led::protocol::ClientHello hello;
    led::protocol::ClientReady ready;
      status = acceptReadyClient(controlPort, udpPort, mode, stream, hello, ready, 10000);
      if (!status.isOk()) {
          stopInputThread();
          displayManager.destroyVirtualDisplay();
          displayManager.restoreDisplayLayout();
          led::logError(status.message());
        return 1;
    }

    led::host::CaptureEngine capture;
    const auto& display = displayManager.virtualDisplay();
      status = capture.startRegion(mode.resolution, display.originX, display.originY, display.deviceName);
      if (!status.isOk()) {
          stopInputThread();
          displayManager.destroyVirtualDisplay();
          displayManager.restoreDisplayLayout();
          led::logError(status.message());
        return 1;
    }

    led::transport::UdpSocket socket;
      status = socket.open();
      if (!status.isOk()) {
          capture.stop();
          stopInputThread();
          displayManager.destroyVirtualDisplay();
          displayManager.restoreDisplayLayout();
          led::logError(status.message());
        return 1;
    }
    if (const auto sendBufferStatus = socket.setSendBufferBytes(4 * 1024 * 1024); !sendBufferStatus.isOk()) {
        led::logWarn("MJPEG UDP send buffer tuning failed: " + sendBufferStatus.message());
    }
    const auto target = led::transport::UdpEndpoint{stream.peerEndpoint().address, ready.rtpPort};

    std::atomic_bool stopAck{false};
    FrameAckTracker frameAckTracker;
    auto frameAckThread = startFrameAckListener(frameAckTracker, stopAck);
    const auto frameInterval = std::chrono::microseconds(1000000 / actualFps);
    const auto clientLivenessTimeout = std::chrono::seconds(60);
    auto nextFrameTime = std::chrono::steady_clock::now();
    auto lastVideoStatsLog = nextFrameTime;
    std::uint32_t capturedFrames = 0;
    MjpegLiveStats liveStats;
    auto previousLiveSnapshot = snapshotMjpegLiveStats(liveStats, capturedFrames);
    const float quality = std::clamp(static_cast<float>(qualityPercent) / 100.0F, 0.1F, 1.0F);
    const float idleRefreshQuality = std::max(quality, 0.85F);
    led::host::CapturedFrame previousFrame;
    const auto startupKeyFrameInterval = std::max<std::uint32_t>(1, actualFps / 5);
    const auto startupKeyFrameWindow = std::max<std::uint32_t>(1, actualFps * 3);
    const auto activeKeyFrameInterval = std::max<std::uint32_t>(1, actualFps * 5);
    const auto idleKeyFrameInterval = std::max<std::uint32_t>(1, actualFps * 8);
    const auto idleDiffWarmupFrames = std::max<std::uint32_t>(1, actualFps / 3);
    const auto idleDiffProbeInterval = std::max<std::uint32_t>(1, actualFps / 15);
    const auto idleCaptureProbeFps = std::min<std::uint32_t>(actualFps, 20);
    const auto idleCaptureProbeInterval =
        std::chrono::microseconds(1000000 / std::max<std::uint32_t>(1, idleCaptureProbeFps));
    const auto encodeSubmitFps = std::min<std::uint32_t>(actualFps, 30);
    const auto encodeSubmitInterval = std::chrono::microseconds(1000000 / std::max<std::uint32_t>(1, encodeSubmitFps));
    auto nextEncodeSubmitTime = std::chrono::steady_clock::now();
    auto nextIdleCaptureProbeTime = std::chrono::steady_clock::now();
    std::uint32_t idleFrames = 0;
    bool forceRecoveryFrame = false;
    auto recoverCapture = [&]() -> led::Status {
        for (int attempt = 0; attempt < 180 && !stopEvent.requested(); ++attempt) {
            signalDriverCreateMonitor();
            auto displayStatus = displayManager.createVirtualDisplay(mode.resolution);
            if (displayStatus.isOk()) {
                const auto& recoveredDisplay = displayManager.virtualDisplay();
                auto restartStatus = capture.startRegion(
                    mode.resolution,
                    recoveredDisplay.originX,
                    recoveredDisplay.originY,
                    recoveredDisplay.deviceName);
                if (restartStatus.isOk()) {
                    led::logInfo(
                        "host MJPEG capture rebound to LED virtual display " + recoveredDisplay.deviceName +
                        " at " + std::to_string(recoveredDisplay.originX) + "," +
                        std::to_string(recoveredDisplay.originY));
                    return led::Status::ok();
                }
                capture.stop();
                displayStatus = restartStatus;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            status = displayStatus;
        }
        return status.isOk() ? led::Status::unavailable("MJPEG capture recovery timed out") : status;
    };

    std::mutex encodeMutex;
    std::condition_variable encodeCv;
    std::optional<MjpegEncodeTask> pendingEncodeTask;
    bool encodeStop = false;
    std::atomic_bool encodeRecoveryFrameRequested{false};
    led::Status encodeWorkerStatus = led::Status::ok();
    std::mutex encodeStatusMutex;
    std::thread encodeThread([&]() {
        led::host::EncodedJpegFrame jpeg;
        while (true) {
            MjpegEncodeTask task;
            {
                std::unique_lock lock(encodeMutex);
                encodeCv.wait(lock, [&]() { return encodeStop || pendingEncodeTask.has_value(); });
                if (encodeStop && !pendingEncodeTask.has_value()) {
                    break;
                }
                task = std::move(*pendingEncodeTask);
                pendingEncodeTask.reset();
            }

            std::uint64_t frameEncodedBytes = 0;
            std::uint64_t frameDirtyAreaPixels = 0;
            bool frameHasFullRect = false;
            for (std::size_t rectIndex = 0; rectIndex < task.dirtyRects.size(); ++rectIndex) {
                const auto& dirty = task.dirtyRects[rectIndex];
                if (dirty.empty) {
                    continue;
                }

                bool qualityBoosted = false;
                const auto rectQuality = selectJpegQualityForDirtyRect(
                    dirty,
                    task.frame.width,
                    task.frame.height,
                    quality,
                    idleRefreshQuality,
                    task.idleRefresh,
                    qualityBoosted);
                const auto encodeStart = std::chrono::steady_clock::now();
                auto encodeStatus = led::host::encodeBgraJpegRect(
                    task.frame,
                    dirty.x,
                    dirty.y,
                    dirty.width,
                    dirty.height,
                    rectQuality,
                    jpeg);
                liveStats.encodeUs.fetch_add(elapsedUsSince(encodeStart), std::memory_order_relaxed);
                if (!encodeStatus.isOk()) {
                    std::scoped_lock lock(encodeStatusMutex);
                    encodeWorkerStatus = encodeStatus;
                    break;
                }

                const auto packetFrameId = jpeg.frameId * kSubFrameIdStride + rectIndex;
                const auto packetizeStart = std::chrono::steady_clock::now();
                const auto packets = led::transport::packetizeMjpegFrameRect(
                    jpeg.jpegBytes,
                    packetFrameId,
                    jpeg.timestampUs,
                    dirty.fullFrame,
                    task.frame.width,
                    task.frame.height,
                    dirty.x,
                    dirty.y,
                    dirty.width,
                    dirty.height,
                    1200);
                liveStats.packetizeUs.fetch_add(elapsedUsSince(packetizeStart), std::memory_order_relaxed);
                if (packets.empty()) {
                    std::scoped_lock lock(encodeStatusMutex);
                    encodeWorkerStatus = led::Status::unavailable("MJPEG packetization produced no packets");
                    break;
                }

                noteFrameSent(frameAckTracker, packetFrameId);
                bool sendOk = true;
                const auto sendStart = std::chrono::steady_clock::now();
                for (const auto& packet : packets) {
                    const auto sendStatus = socket.sendTo(packet, target);
                    if (!sendStatus.isOk()) {
                        sendOk = false;
                        break;
                    }
                    liveStats.packetsSent.fetch_add(1, std::memory_order_relaxed);
                }
                liveStats.sendUs.fetch_add(elapsedUsSince(sendStart), std::memory_order_relaxed);
                if (!sendOk) {
                    liveStats.sendFailures.fetch_add(1, std::memory_order_relaxed);
                    encodeRecoveryFrameRequested = true;
                    led::logWarn("MJPEG UDP send failed, keeping session alive");
                    break;
                }

                frameEncodedBytes += jpeg.jpegBytes.size();
                frameDirtyAreaPixels += static_cast<std::uint64_t>(dirty.width) * dirty.height;
                frameHasFullRect = frameHasFullRect || dirty.fullFrame;
                liveStats.dirtyRectsSent.fetch_add(1, std::memory_order_relaxed);
                liveStats.encodedRects.fetch_add(1, std::memory_order_relaxed);
                if (qualityBoosted) {
                    liveStats.qualityBoostedRects.fetch_add(1, std::memory_order_relaxed);
                }
            }

            if (frameEncodedBytes == 0) {
                continue;
            }
            liveStats.encodedBytes.fetch_add(frameEncodedBytes, std::memory_order_relaxed);
            liveStats.dirtyAreaPixels.fetch_add(frameDirtyAreaPixels, std::memory_order_relaxed);
            if (frameHasFullRect) {
                liveStats.fullFrames.fetch_add(1, std::memory_order_relaxed);
            } else {
                liveStats.dirtyFrames.fetch_add(1, std::memory_order_relaxed);
            }
            if (task.idleRefresh) {
                liveStats.idleRefreshFrames.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });

    while (!stopEvent.requested() && (frameCount == 0 || capturedFrames < frameCount)) {
        const auto loopNow = std::chrono::steady_clock::now();
        if (stream.peerClosed()) {
            status = led::Status::unavailable("MJPEG client control connection closed");
            led::logWarn(status.message());
            break;
        }
        if (frameAckTimedOut(frameAckTracker, loopNow, clientLivenessTimeout)) {
            status = led::Status::unavailable("MJPEG client acknowledgements timed out");
            led::logWarn(status.message());
            break;
        }
        {
            std::scoped_lock lock(encodeStatusMutex);
            if (!encodeWorkerStatus.isOk()) {
                status = encodeWorkerStatus;
                break;
            }
        }
        if (encodeRecoveryFrameRequested.exchange(false)) {
            previousFrame = {};
            forceRecoveryFrame = true;
            idleFrames = 0;
            nextIdleCaptureProbeTime = std::chrono::steady_clock::now();
        }

        const auto preCaptureNow = std::chrono::steady_clock::now();
        if (idleFrames >= idleDiffWarmupFrames && preCaptureNow < nextIdleCaptureProbeTime) {
            ++idleFrames;
            ++capturedFrames;
            liveStats.skippedFrames.fetch_add(1, std::memory_order_relaxed);
            liveStats.idleCaptureSkippedFrames.fetch_add(1, std::memory_order_relaxed);
            nextFrameTime += frameInterval;
            std::this_thread::sleep_until(nextFrameTime);
            continue;
        }
        if (idleFrames >= idleDiffWarmupFrames) {
            nextIdleCaptureProbeTime = preCaptureNow + idleCaptureProbeInterval;
        }

        led::host::CapturedFrame frame;
        const auto captureStart = std::chrono::steady_clock::now();
        status = capture.captureNextFrame(frame);
        liveStats.captureUs.fetch_add(elapsedUsSince(captureStart), std::memory_order_relaxed);
        if (!status.isOk()) {
            led::logWarn("MJPEG capture failed, rebinding virtual display: " + status.message());
            capture.stop();
            status = recoverCapture();
            if (!status.isOk()) {
                break;
            }
            previousFrame = {};
            forceRecoveryFrame = true;
            idleFrames = 0;
            nextIdleCaptureProbeTime = std::chrono::steady_clock::now();
            nextEncodeSubmitTime = std::chrono::steady_clock::now();
            nextFrameTime = std::chrono::steady_clock::now();
            continue;
        }
        if (frame.partialCopyUsed) {
            liveStats.partialCopyFrames.fetch_add(1, std::memory_order_relaxed);
        }
        if (frame.captureDirtyRectCount > 0) {
            liveStats.dxgiDirtyFrames.fetch_add(1, std::memory_order_relaxed);
            liveStats.dxgiDirtyRects.fetch_add(frame.captureDirtyRectCount, std::memory_order_relaxed);
            liveStats.dxgiDirtyAreaPixels.fetch_add(frame.captureDirtyAreaPixels, std::memory_order_relaxed);
        }

        const bool startupKeyFrameWindowActive = capturedFrames < startupKeyFrameWindow;
        const auto keyFrameInterval =
            startupKeyFrameWindowActive
                ? startupKeyFrameInterval
                : (idleFrames >= idleDiffWarmupFrames ? idleKeyFrameInterval : activeKeyFrameInterval);
        const bool forceKeyFrame =
            forceRecoveryFrame || previousFrame.bgra.empty() || (capturedFrames % keyFrameInterval) == 0;
        const bool skipIdleDiff =
            !forceKeyFrame &&
            !frame.dirtyRectsKnown &&
            !previousFrame.bgra.empty() &&
            idleFrames >= idleDiffWarmupFrames &&
            (idleFrames % idleDiffProbeInterval) != 0;
        if (skipIdleDiff) {
            ++idleFrames;
            ++capturedFrames;
            liveStats.skippedFrames.fetch_add(1, std::memory_order_relaxed);
            liveStats.idleDiffSkippedFrames.fetch_add(1, std::memory_order_relaxed);
            nextFrameTime += frameInterval;
            std::this_thread::sleep_until(nextFrameTime);
            continue;
        }
        const auto dirtyStart = std::chrono::steady_clock::now();
        std::vector<DirtyRect> dirtyRects = forceKeyFrame
            ? std::vector<DirtyRect>{fullDirtyRect(frame.width, frame.height)}
            : (frame.dirtyRectsKnown
                ? computeNativeDirtyRects(frame, 16, 0.70)
                : computeDirtyRects(frame, previousFrame, 16, 0.70));
        liveStats.dirtyUs.fetch_add(elapsedUsSince(dirtyStart), std::memory_order_relaxed);
        if (!forceKeyFrame && frame.dirtyRectsKnown) {
            liveStats.nativeDirtyFrames.fetch_add(1, std::memory_order_relaxed);
        } else if (!forceKeyFrame) {
            liveStats.tileDiffFrames.fetch_add(1, std::memory_order_relaxed);
        }
        bool idleRefresh = false;
        if (dirtyRects.empty() || (dirtyRects.size() == 1 && dirtyRects.front().empty)) {
            ++idleFrames;
            const auto refreshInterval = idleKeyFrameInterval;
            if (idleFrames >= refreshInterval) {
                dirtyRects = {fullDirtyRect(frame.width, frame.height)};
                idleRefresh = true;
                idleFrames = 0;
            } else {
                ++capturedFrames;
                liveStats.skippedFrames.fetch_add(1, std::memory_order_relaxed);
                nextFrameTime += frameInterval;
                std::this_thread::sleep_until(nextFrameTime);
                continue;
            }
        } else {
            idleFrames = 0;
            nextIdleCaptureProbeTime = std::chrono::steady_clock::now();
        }
        if (dirtyRects.empty() || (dirtyRects.size() == 1 && dirtyRects.front().empty)) {
            ++capturedFrames;
            liveStats.skippedFrames.fetch_add(1, std::memory_order_relaxed);
            nextFrameTime += frameInterval;
            std::this_thread::sleep_until(nextFrameTime);
            continue;
        }

        const auto submitNow = std::chrono::steady_clock::now();
        const bool submitImmediately = forceKeyFrame || idleRefresh || forceRecoveryFrame;
        if (!submitImmediately && submitNow < nextEncodeSubmitTime) {
            ++capturedFrames;
            liveStats.skippedFrames.fetch_add(1, std::memory_order_relaxed);
            nextFrameTime += frameInterval;
            std::this_thread::sleep_until(nextFrameTime);
            continue;
        }
        nextEncodeSubmitTime = submitNow + encodeSubmitInterval;

        {
            std::scoped_lock lock(encodeMutex);
            if (pendingEncodeTask.has_value()) {
                liveStats.encodeTasksDropped.fetch_add(1, std::memory_order_relaxed);
            }
            previousFrame = frame;
            pendingEncodeTask = MjpegEncodeTask{std::move(frame), std::move(dirtyRects), idleRefresh};
        }
        encodeCv.notify_one();

        forceRecoveryFrame = false;
        ++capturedFrames;
        const auto now = std::chrono::steady_clock::now();
        if (diagnosticsEnabled && now - lastVideoStatsLog >= std::chrono::seconds(1)) {
            const auto [receiveAckStats, renderAckStats] = snapshotFrameAckStats(frameAckTracker);
            const auto snapshot = snapshotMjpegLiveStats(liveStats, capturedFrames);
            const auto windowFrames = delta(snapshot.capturedFrames, previousLiveSnapshot.capturedFrames);
            const auto windowEncodedRects = delta(snapshot.encodedRects, previousLiveSnapshot.encodedRects);
            const auto windowEncodedBytes = delta(snapshot.encodedBytes, previousLiveSnapshot.encodedBytes);
            const auto windowDirtyAreaPixels = delta(snapshot.dirtyAreaPixels, previousLiveSnapshot.dirtyAreaPixels);
            const auto windowDxgiDirtyAreaPixels =
                delta(snapshot.dxgiDirtyAreaPixels, previousLiveSnapshot.dxgiDirtyAreaPixels);
            const auto windowQualityBoostedRects =
                delta(snapshot.qualityBoostedRects, previousLiveSnapshot.qualityBoostedRects);
            const auto fullFrames = liveStats.fullFrames.load(std::memory_order_relaxed);
            const auto dirtyFrames = liveStats.dirtyFrames.load(std::memory_order_relaxed);
            const auto idleRefreshFrames = liveStats.idleRefreshFrames.load(std::memory_order_relaxed);
            const auto skippedFrames = liveStats.skippedFrames.load(std::memory_order_relaxed);
            const auto idleDiffSkippedFrames = liveStats.idleDiffSkippedFrames.load(std::memory_order_relaxed);
            const auto idleCaptureSkippedFrames = liveStats.idleCaptureSkippedFrames.load(std::memory_order_relaxed);
            const auto nativeDirtyFrames = liveStats.nativeDirtyFrames.load(std::memory_order_relaxed);
            const auto partialCopyFrames = liveStats.partialCopyFrames.load(std::memory_order_relaxed);
            const auto dxgiDirtyFrames = liveStats.dxgiDirtyFrames.load(std::memory_order_relaxed);
            const auto dxgiDirtyRects = liveStats.dxgiDirtyRects.load(std::memory_order_relaxed);
            const auto tileDiffFrames = liveStats.tileDiffFrames.load(std::memory_order_relaxed);
            const auto dirtyRectsSent = liveStats.dirtyRectsSent.load(std::memory_order_relaxed);
            const auto sendFailures = liveStats.sendFailures.load(std::memory_order_relaxed);
            const auto encodeTasksDropped = liveStats.encodeTasksDropped.load(std::memory_order_relaxed);
            const auto captureUs = snapshot.captureUs;
            const auto dirtyUs = snapshot.dirtyUs;
            const auto encodeUs = snapshot.encodeUs;
            const auto packetizeUs = snapshot.packetizeUs;
            const auto sendUs = snapshot.sendUs;
            const auto encodedRects = snapshot.encodedRects;
            std::cout << "Host MJPEG live stats: frames=" << capturedFrames
                      << " packets=" << snapshot.packetsSent
                      << " encoded_kb=" << (snapshot.encodedBytes / 1024)
                      << " avg_frame_kb=" << (capturedFrames == 0 ? 0 : (snapshot.encodedBytes / 1024 / capturedFrames))
                      << " full=" << fullFrames
                      << " dirty=" << dirtyFrames
                      << " idle_refresh=" << idleRefreshFrames
                      << " skipped=" << skippedFrames
                      << " idle_capture_skipped=" << idleCaptureSkippedFrames
                      << " idle_diff_skipped=" << idleDiffSkippedFrames
                      << " native_dirty=" << nativeDirtyFrames
                      << " partial_copy=" << partialCopyFrames
                      << " dxgi_dirty_frames=" << dxgiDirtyFrames
                      << " dxgi_dirty_rects=" << dxgiDirtyRects
                      << " tile_diff=" << tileDiffFrames
                      << " dirty_rects_sent=" << dirtyRectsSent
                      << " quality_boosted=" << snapshot.qualityBoostedRects
                      << " encode_dropped=" << encodeTasksDropped
                      << " send_failures=" << sendFailures
                      << " avg_capture_ms=" << (capturedFrames == 0 ? 0.0 : (static_cast<double>(captureUs) / 1000.0 / capturedFrames))
                      << " avg_dirty_ms=" << (capturedFrames == 0 ? 0.0 : (static_cast<double>(dirtyUs) / 1000.0 / capturedFrames))
                      << " avg_encode_ms=" << (encodedRects == 0 ? 0.0 : (static_cast<double>(encodeUs) / 1000.0 / encodedRects))
                      << " avg_packetize_ms=" << (encodedRects == 0 ? 0.0 : (static_cast<double>(packetizeUs) / 1000.0 / encodedRects))
                      << " avg_send_ms=" << (encodedRects == 0 ? 0.0 : (static_cast<double>(sendUs) / 1000.0 / encodedRects))
                      << " win_frame_delta=" << windowFrames
                      << " win_encoded_kb=" << (windowEncodedBytes / 1024)
                      << " win_quality_boosted=" << windowQualityBoostedRects
                      << " win_encode_dropped=" << delta(snapshot.encodeTasksDropped, previousLiveSnapshot.encodeTasksDropped)
                      << " win_partial_copy=" << delta(snapshot.partialCopyFrames, previousLiveSnapshot.partialCopyFrames)
                      << " win_dxgi_dirty=" << delta(snapshot.dxgiDirtyFrames, previousLiveSnapshot.dxgiDirtyFrames)
                      << " win_dxgi_dirty_pct=" << (windowFrames == 0 ? 0.0 :
                          (static_cast<double>(windowDxgiDirtyAreaPixels) * 100.0 /
                           (static_cast<double>(windowFrames) * mode.resolution.width * mode.resolution.height)))
                      << " win_capture_ms=" << (windowFrames == 0 ? 0.0 :
                          (static_cast<double>(delta(snapshot.captureUs, previousLiveSnapshot.captureUs)) / 1000.0 / windowFrames))
                      << " win_dirty_ms=" << (windowFrames == 0 ? 0.0 :
                          (static_cast<double>(delta(snapshot.dirtyUs, previousLiveSnapshot.dirtyUs)) / 1000.0 / windowFrames))
                      << " win_encode_ms=" << (windowEncodedRects == 0 ? 0.0 :
                          (static_cast<double>(delta(snapshot.encodeUs, previousLiveSnapshot.encodeUs)) / 1000.0 / windowEncodedRects))
                      << " win_packetize_ms=" << (windowEncodedRects == 0 ? 0.0 :
                          (static_cast<double>(delta(snapshot.packetizeUs, previousLiveSnapshot.packetizeUs)) / 1000.0 / windowEncodedRects))
                      << " win_send_ms=" << (windowEncodedRects == 0 ? 0.0 :
                          (static_cast<double>(delta(snapshot.sendUs, previousLiveSnapshot.sendUs)) / 1000.0 / windowEncodedRects))
                      << " win_dirty_pct=" << (windowFrames == 0 ? 0.0 :
                          (static_cast<double>(windowDirtyAreaPixels) * 100.0 /
                           (static_cast<double>(windowFrames) * mode.resolution.width * mode.resolution.height)))
                      << " avg_dirty_pct=" << (capturedFrames == 0 ? 0.0 :
                          (static_cast<double>(snapshot.dirtyAreaPixels) * 100.0 /
                           (static_cast<double>(capturedFrames) * mode.resolution.width * mode.resolution.height)))
                      << " fps_target=" << actualFps
                      << " receive_ack_count=" << receiveAckStats.acks
                      << " receive_ack_avg_ms=" << (receiveAckStats.averageRttUs / 1000.0)
                      << " receive_ack_max_ms=" << (receiveAckStats.maxRttUs / 1000.0)
                      << " render_ack_count=" << renderAckStats.acks
                      << " render_ack_avg_ms=" << (renderAckStats.averageRttUs / 1000.0)
                      << " render_ack_max_ms=" << (renderAckStats.maxRttUs / 1000.0)
                      << " render_ack_last_frame=" << renderAckStats.lastFrameId
                      << " ack_unknown=" << (receiveAckStats.unknownAcks + renderAckStats.unknownAcks) << '\n';
            previousLiveSnapshot = snapshot;
            lastVideoStatsLog = now;
        }
        nextFrameTime += frameInterval;
        std::this_thread::sleep_until(nextFrameTime);
    }

    {
        std::scoped_lock lock(encodeMutex);
        encodeStop = true;
        pendingEncodeTask.reset();
    }
    encodeCv.notify_one();
    if (encodeThread.joinable()) {
        encodeThread.join();
    }

    socket.close();
      capture.stop();
      stopAck = true;
      if (frameAckThread.joinable()) {
          frameAckThread.join();
      }
      stopInputThread();
      displayManager.destroyVirtualDisplay();
      displayManager.restoreDisplayLayout();
      (void)keepMonitorOnExit;
      driverMonitor.release();
      if (status.isOk() && !inputStatus.isOk()) {
          status = inputStatus;
      }
    if (!status.isOk()) {
        led::logError(status.message());
        return 1;
    }

    const auto encodedBytes = liveStats.encodedBytes.load(std::memory_order_relaxed);
    const auto packetsSent = liveStats.packetsSent.load(std::memory_order_relaxed);
    const auto sendFailures = liveStats.sendFailures.load(std::memory_order_relaxed);
    const auto dirtyFrames = liveStats.dirtyFrames.load(std::memory_order_relaxed);
    const auto fullFrames = liveStats.fullFrames.load(std::memory_order_relaxed);
    const auto idleRefreshFrames = liveStats.idleRefreshFrames.load(std::memory_order_relaxed);
    const auto skippedFrames = liveStats.skippedFrames.load(std::memory_order_relaxed);
    const auto idleCaptureSkippedFrames = liveStats.idleCaptureSkippedFrames.load(std::memory_order_relaxed);
    const auto partialCopyFrames = liveStats.partialCopyFrames.load(std::memory_order_relaxed);
    const auto dxgiDirtyFrames = liveStats.dxgiDirtyFrames.load(std::memory_order_relaxed);
    const auto dxgiDirtyRects = liveStats.dxgiDirtyRects.load(std::memory_order_relaxed);
    const auto dirtyRectsSent = liveStats.dirtyRectsSent.load(std::memory_order_relaxed);
    const auto captureUs = liveStats.captureUs.load(std::memory_order_relaxed);
    const auto dirtyUs = liveStats.dirtyUs.load(std::memory_order_relaxed);
    const auto encodeUs = liveStats.encodeUs.load(std::memory_order_relaxed);
    const auto packetizeUs = liveStats.packetizeUs.load(std::memory_order_relaxed);
    const auto sendUs = liveStats.sendUs.load(std::memory_order_relaxed);
    const auto encodedRects = liveStats.encodedRects.load(std::memory_order_relaxed);
    const auto qualityBoostedRects = liveStats.qualityBoostedRects.load(std::memory_order_relaxed);
    std::cout << "Client ready: " << hello.device.name
              << " peer=" << target.address << ':' << target.port << '\n';
    std::cout << "Served MJPEG capture frames=" << capturedFrames
              << " fps=" << actualFps
              << " mode=" << mode.resolution.width << 'x' << mode.resolution.height
              << " quality=" << quality
              << " full=" << fullFrames
              << " dirty=" << dirtyFrames
              << " idle_refresh=" << idleRefreshFrames
              << " skipped=" << skippedFrames
              << " idle_capture_skipped=" << idleCaptureSkippedFrames
              << " partial_copy=" << partialCopyFrames
              << " dxgi_dirty_frames=" << dxgiDirtyFrames
              << " dxgi_dirty_rects=" << dxgiDirtyRects
              << " dirty_rects_sent=" << dirtyRectsSent
              << " quality_boosted=" << qualityBoostedRects
              << " send_failures=" << sendFailures
              << " packets=" << packetsSent
              << " encoded_bytes=" << encodedBytes
              << " avg_capture_ms=" << (capturedFrames == 0 ? 0.0 : (static_cast<double>(captureUs) / 1000.0 / capturedFrames))
              << " avg_dirty_ms=" << (capturedFrames == 0 ? 0.0 : (static_cast<double>(dirtyUs) / 1000.0 / capturedFrames))
              << " avg_encode_ms=" << (encodedRects == 0 ? 0.0 : (static_cast<double>(encodeUs) / 1000.0 / encodedRects))
              << " avg_packetize_ms=" << (encodedRects == 0 ? 0.0 : (static_cast<double>(packetizeUs) / 1000.0 / encodedRects))
              << " avg_send_ms=" << (encodedRects == 0 ? 0.0 : (static_cast<double>(sendUs) / 1000.0 / encodedRects))
              << '\n';
    return 0;
}

int runSendH264FileMode(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "Usage: led_host_app --send-h264-file <file.h264> [control-port] [rtp-port] [fps] [loops]\n";
        return 1;
    }

    const char* path = argv[2];
    const auto controlPort = argc >= 4 ? parsePort(argv[3], 17660) : std::uint16_t{17660};
    const auto rtpPort = argc >= 5 ? parsePort(argv[4], 17670) : std::uint16_t{17670};
    const auto fps = argc >= 6 ? parseU32(argv[5], 60) : std::uint32_t{60};
    const auto loops = argc >= 7 ? parseU32(argv[6], 1) : std::uint32_t{1};

    std::vector<std::uint8_t> fileBytes;
    auto status = led::transport::readBinaryFile(path, fileBytes);
    if (!status.isOk()) {
        led::logError(status.message());
        return 1;
    }

    const auto nalUnits = led::transport::splitAnnexB(fileBytes);
    if (nalUnits.empty()) {
        led::logError("no Annex B H.264 NAL units found in input file");
        return 1;
    }

    led::transport::TcpStream stream;
    led::protocol::ClientHello hello;
    led::protocol::ClientReady ready;
    status = acceptReadyClient(controlPort, rtpPort, led::protocol::defaultStandardMode(), stream, hello, ready);
    if (!status.isOk()) {
        led::logError(status.message());
        return 1;
    }

    const auto target = led::transport::UdpEndpoint{stream.peerEndpoint().address, ready.rtpPort};
    led::host::VideoSender sender;
    sender.setEmbedSendTime(true);
    status = sender.open(target);
    if (!status.isOk()) {
        led::logError(status.message());
        return 1;
    }

    const auto frameInterval = std::chrono::microseconds(1000000 / (fps == 0 ? 60 : fps));
    const auto timestampStep = 90000 / (fps == 0 ? 60 : fps);
    auto nextFrameTime = std::chrono::steady_clock::now();
    std::uint32_t frameIndex = 0;
    std::uint64_t bytesSent = 0;

    const auto loopCount = loops == 0 ? std::uint32_t{1} : loops;
    for (std::uint32_t loop = 0; loop < loopCount; ++loop) {
        for (const auto& nal : nalUnits) {
            const auto type = led::transport::h264NalType(nal.bytes);
            const bool parameterSet = type == 7 || type == 8 || type == 9;
            const auto timestamp = 90000 + frameIndex * timestampStep;
            status = sender.sendNal(nal.bytes, timestamp);
            if (!status.isOk()) {
                sender.close();
                led::logError(status.message());
                return 1;
            }
            bytesSent += nal.bytes.size();

            if (!parameterSet) {
                ++frameIndex;
                nextFrameTime += frameInterval;
                std::this_thread::sleep_until(nextFrameTime);
            }
        }
    }

    sender.close();
    std::cout << "Client ready: " << hello.device.name
              << " peer=" << target.address << ':' << target.port << '\n';
    std::cout << "Sent Annex B file NAL units=" << nalUnits.size()
              << " loops=" << loopCount
              << " frame_like_nals=" << frameIndex
              << " bytes=" << bytesSent
              << " fps=" << (fps == 0 ? 60 : fps) << '\n';
    return 0;
}

int runServeH264FileWithInputMode(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "Usage: led_host_app --serve-h264-file <file.h264> [control-port] [rtp-port] [input-port] [fps] [loops] [dry-run|sendinput]\n";
        return 1;
    }

    const char* path = argv[2];
    const auto controlPort = argc >= 4 ? parsePort(argv[3], 17660) : std::uint16_t{17660};
    const auto rtpPort = argc >= 5 ? parsePort(argv[4], 17670) : std::uint16_t{17670};
    const auto inputPort = argc >= 6 ? parsePort(argv[5], 17691) : std::uint16_t{17691};
    const auto fps = argc >= 7 ? parseU32(argv[6], 60) : std::uint32_t{60};
    const auto loops = argc >= 8 ? parseU32(argv[7], 1) : std::uint32_t{1};
    led::host::InputInjectionBackend backend = led::host::InputInjectionBackend::dryRun;
    if (argc >= 9 && !led::host::parseInputInjectionBackend(argv[8], backend)) {
        std::cerr << "Usage: led_host_app --serve-h264-file <file.h264> [control-port] [rtp-port] [input-port] [fps] [loops] [dry-run|sendinput]\n";
        return 1;
    }

    std::vector<std::uint8_t> fileBytes;
    auto status = led::transport::readBinaryFile(path, fileBytes);
    if (!status.isOk()) {
        led::logError(status.message());
        return 1;
    }

    const auto nalUnits = led::transport::splitAnnexB(fileBytes);
    if (nalUnits.empty()) {
        led::logError("no Annex B H.264 NAL units found in input file");
        return 1;
    }

    led::host::SessionManager sessionManager;
    status = sessionManager.start(led::protocol::defaultStandardMode());
    if (!status.isOk()) {
        led::logError(status.message());
        return 1;
    }

    led::host::InputReceiver inputReceiver;
    status = inputReceiver.bind(inputPort);
    if (!status.isOk()) {
        sessionManager.stop();
        led::logError(status.message());
        return 1;
    }
    inputReceiver.setReceiveTimeoutMs(200);
    led::host::InputInjector injector(sessionManager.displayManager(), backend);
    std::atomic_bool stopInput{false};
    led::transport::UdpEndpoint lastInputSource;
    led::Status inputStatus;
    std::thread inputThread([&]() {
        while (!stopInput.load()) {
            led::protocol::InputEvent event;
            led::transport::UdpEndpoint source;
            auto receiveStatus = inputReceiver.receive(event, source);
            if (!receiveStatus.isOk()) {
                continue;
            }
            lastInputSource = source;
            auto injectStatus = injector.inject(event);
            if (!injectStatus.isOk()) {
                inputStatus = injectStatus;
                stopInput = true;
                return;
            }
        }
    });

    led::transport::TcpStream stream;
    led::protocol::ClientHello hello;
    led::protocol::ClientReady ready;
    status = acceptReadyClient(controlPort, rtpPort, led::protocol::defaultStandardMode(), stream, hello, ready);
    if (!status.isOk()) {
        stopInput = true;
        if (inputThread.joinable()) {
            inputThread.join();
        }
        inputReceiver.close();
        sessionManager.stop();
        led::logError(status.message());
        return 1;
    }

    const auto target = led::transport::UdpEndpoint{stream.peerEndpoint().address, ready.rtpPort};
    led::host::VideoSender sender;
    sender.setEmbedSendTime(true);
    status = sender.open(target);
    if (!status.isOk()) {
        stopInput = true;
        if (inputThread.joinable()) {
            inputThread.join();
        }
        inputReceiver.close();
        sessionManager.stop();
        led::logError(status.message());
        return 1;
    }

    const auto actualFps = fps == 0 ? std::uint32_t{60} : fps;
    const auto frameInterval = std::chrono::microseconds(1000000 / actualFps);
    const auto timestampStep = 90000 / actualFps;
    auto nextFrameTime = std::chrono::steady_clock::now();
    std::uint32_t frameIndex = 0;
    std::uint64_t bytesSent = 0;

    const auto loopCount = loops == 0 ? std::uint32_t{1} : loops;
    for (std::uint32_t loop = 0; loop < loopCount; ++loop) {
        for (const auto& nal : nalUnits) {
            const auto type = led::transport::h264NalType(nal.bytes);
            const bool parameterSet = type == 7 || type == 8 || type == 9;
            const auto timestamp = 90000 + frameIndex * timestampStep;
            status = sender.sendNal(nal.bytes, timestamp);
            if (!status.isOk() || stopInput.load()) {
                sender.close();
                stopInput = true;
                if (inputThread.joinable()) {
                    inputThread.join();
                }
                const auto finalStatus = status.isOk() ? inputStatus : status;
                inputReceiver.close();
                sessionManager.stop();
                led::logError(finalStatus.message());
                return 1;
            }
            bytesSent += nal.bytes.size();

            if (!parameterSet) {
                ++frameIndex;
                nextFrameTime += frameInterval;
                std::this_thread::sleep_until(nextFrameTime);
            }
        }
    }

    sender.close();
    stopInput = true;
    if (inputThread.joinable()) {
        inputThread.join();
    }
    const auto inputStats = inputReceiver.stats();
    inputReceiver.close();
    sessionManager.stop();

    std::cout << "Client ready: " << hello.device.name
              << " peer=" << target.address << ':' << target.port
              << " input_port=" << inputPort
              << " input_backend=" << injector.backendName() << '\n';
    std::cout << "Served Annex B file NAL units=" << nalUnits.size()
              << " loops=" << loopCount
              << " frame_like_nals=" << frameIndex
              << " bytes=" << bytesSent
              << " fps=" << actualFps << '\n';
    if (inputStats.eventsReceived > 0) {
        std::cout << "Last input source: " << lastInputSource.address << ':' << lastInputSource.port << '\n';
    }
    printInputReceiverStats(inputStats);
    return 0;
}

int runReceiveTestInputMode(int argc, char** argv) {
    const auto inputPort = argc >= 3 ? parsePort(argv[2], 17691) : std::uint16_t{17691};
    const auto expectedEvents = argc >= 4 ? parseU32(argv[3], 10) : std::uint32_t{10};
    led::host::InputInjectionBackend backend = led::host::InputInjectionBackend::dryRun;
    if (argc >= 5 && !led::host::parseInputInjectionBackend(argv[4], backend)) {
        std::cerr << "Usage: led_host_app --receive-test-input [input-port] [expected-events] [dry-run|sendinput]\n";
        return 1;
    }

    led::host::SessionManager sessionManager;
    auto status = sessionManager.start(led::protocol::defaultStandardMode());
    if (!status.isOk()) {
        led::logError(status.message());
        return 1;
    }

    led::host::InputReceiver receiver;
    status = receiver.bind(inputPort);
    if (!status.isOk()) {
        sessionManager.stop();
        led::logError(status.message());
        return 1;
    }
    receiver.setReceiveTimeoutMs(3000);

    led::host::InputInjector injector(sessionManager.displayManager(), backend);
    std::uint32_t receivedEvents = 0;
    std::uint64_t lastSequence = 0;
    led::transport::UdpEndpoint source;
    const auto start = std::chrono::steady_clock::now();
    while (receivedEvents < expectedEvents) {
        led::protocol::InputEvent event;
        status = receiver.receive(event, source);
        if (!status.isOk()) {
            receiver.close();
            sessionManager.stop();
            led::logError(status.message());
            return 1;
        }

        status = injector.inject(event);
        if (!status.isOk()) {
            receiver.close();
            sessionManager.stop();
            led::logError(status.message());
            return 1;
        }

        ++receivedEvents;
        lastSequence = event.sequence;
    }

    const auto elapsed = std::chrono::steady_clock::now() - start;
    receiver.close();
    sessionManager.stop();

    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    const auto stats = receiver.stats();
    std::cout << "Received input events count=" << receivedEvents
              << " last_seq=" << lastSequence
              << " backend=" << injector.backendName()
              << " elapsed_ms=" << elapsedMs
              << " from " << source.address << ':' << source.port << '\n';
    printInputReceiverStats(stats);
    return 0;
}

int runSkeletonMode() {
    led::logInfo("LAN extended display Windows host skeleton starting");

    led::host::SessionManager sessionManager;
    const auto mode = led::protocol::defaultStandardMode();
    const auto status = sessionManager.start(mode);

    if (!status.isOk()) {
        led::logError(status.message());
        return 1;
    }

    const auto& display = sessionManager.displayManager().virtualDisplay();
    std::cout << "Host skeleton state: " << led::session::toString(sessionManager.state()) << '\n';
    std::cout << "Virtual display: "
              << display.resolution.width << 'x' << display.resolution.height
              << "@" << display.resolution.refreshRate << "Hz\n";
    std::cout << "Codec: " << led::protocol::toString(mode.codec)
              << ", preset: " << led::protocol::toString(mode.preset)
              << ", bitrate: " << mode.bitrateKbps << " kbps\n";

    sessionManager.stop();
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    enableProcessDpiAwareness();
    std::cout.setf(std::ios::unitbuf);
    std::cerr.setf(std::ios::unitbuf);

    if (argc >= 2 && std::string(argv[1]) == "--restore-cursor") {
        led::host::CursorSuppressor cursorSuppressor;
        const auto status = cursorSuppressor.hideSystemCursor();
        if (status.isOk()) {
            cursorSuppressor.restoreSystemCursor();
        } else {
            led::logWarn("cursor restore helper could not hide first; forcing restore attempt: " + status.message());
            cursorSuppressor.restoreSystemCursor();
        }
        return 0;
    }
    if (argc >= 2 && std::string(argv[1]) == "--list-displays") {
        return runListDisplaysMode();
    }
    if (argc >= 2 && std::string(argv[1]) == "--listen") {
        return runListenMode(argc, argv);
    }
    if (argc >= 2 && std::string(argv[1]) == "--send-test-nal") {
        return runSendTestNalMode(argc, argv);
    }
    if (argc >= 2 && std::string(argv[1]) == "--send-test-stream") {
        return runSendTestStreamMode(argc, argv);
    }
    if (argc >= 2 && std::string(argv[1]) == "--send-capture-test-stream") {
        return runSendCaptureTestStreamMode(argc, argv);
    }
    if (argc >= 2 && std::string(argv[1]) == "--serve-live-capture") {
        return runServeLiveCaptureMode(argc, argv);
    }
    if (argc >= 2 && std::string(argv[1]) == "--serve-mjpeg-capture") {
        return runServeMjpegCaptureMode(argc, argv);
    }
    if (argc >= 2 && std::string(argv[1]) == "--send-h264-file") {
        return runSendH264FileMode(argc, argv);
    }
    if (argc >= 2 && std::string(argv[1]) == "--serve-h264-file") {
        return runServeH264FileWithInputMode(argc, argv);
    }
    if (argc >= 2 && std::string(argv[1]) == "--receive-test-input") {
        return runReceiveTestInputMode(argc, argv);
    }

    return runSkeletonMode();
}
