#pragma once

#include "led/common/status.h"
#include "led/protocol/messages.h"
#include "led/client/renderer.h"

#include <deque>
#include <functional>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace led::client {

struct DecoderStats {
    std::uint64_t nalUnits{0};
    std::uint64_t bytes{0};
    std::uint64_t keyFrames{0};
    std::uint64_t backendAccepted{0};
    std::uint64_t decodedFrames{0};
};

struct DecoderFactoryInfo {
    std::string name;
    std::string klass;
    std::string description;
    unsigned int rank{0};
};

class Decoder {
public:
    void setInputPipeline(std::string sourceCaps, std::string parserPipeline);
    void setSinkPipeline(std::string sinkPipeline);
    void setRenderer(Renderer* renderer);
    void setRenderedFrameCallback(std::function<void(std::uint64_t)> callback);

    Status configure(const protocol::VideoMode& mode);
    Status start();
    Status pushNal(const std::vector<std::uint8_t>& nalUnit);
    Status pushNalWithFrameId(const std::vector<std::uint8_t>& nalUnit, std::uint64_t frameId);
    Status pushAccessUnit(const std::vector<std::vector<std::uint8_t>>& nalUnits);
    Status pushAccessUnitWithFrameId(
        const std::vector<std::vector<std::uint8_t>>& nalUnits,
        std::uint64_t frameId);
    Status pushEncodedBufferWithFrameId(const std::vector<std::uint8_t>& bytes, std::uint64_t frameId);
    void drainForMs(unsigned int milliseconds);
    Status stop();

    [[nodiscard]] DecoderStats stats() const;
    [[nodiscard]] const char* backendName() const;
    [[nodiscard]] const std::string& sinkPipeline() const;
    [[nodiscard]] static std::vector<DecoderFactoryInfo> availableGstreamerFactories();
    void noteDecodedFrame(const RawFrameInfo& frame);
    void noteDecodedFrameData(const RawFrameInfo& frame, const std::uint8_t* data);

private:
    Status startBackend();
    Status pushNalToBackend(const std::vector<std::uint8_t>& nalUnit);
    Status pushEncodedBufferToBackend(const std::vector<std::uint8_t>& bytes, std::uint64_t frameId);
    Status pushAccessUnitToBackend(
        const std::vector<std::vector<std::uint8_t>>& nalUnits,
        std::uint64_t frameId);
    void stopBackend();
    void rememberFrameId(std::uint64_t frameId);
    [[nodiscard]] std::uint64_t takeDecodedFrameId(std::uint64_t ptsNs);

    protocol::VideoMode mode_{protocol::defaultStandardMode()};
    std::string sourceCaps_{"video/x-h264,stream-format=byte-stream,alignment=nal"};
    std::string parserPipeline_{"h264parse name=parser config-interval=-1 disable-passthrough=true"};
    std::string sinkPipeline_{"fakesink sync=false"};
    DecoderStats stats_{};
    mutable std::mutex mutex_;
    std::deque<std::uint64_t> pendingFrameIds_;
    std::uint64_t lastQueuedFrameId_{0};
    std::function<void(std::uint64_t)> renderedFrameCallback_;
    Renderer* renderer_{nullptr};
    bool running_{false};

#if defined(LED_HAS_GSTREAMER)
    void* pipeline_{nullptr};
    void* appsrc_{nullptr};
    void* decodedSink_{nullptr};
#endif
};

}  // namespace led::client
