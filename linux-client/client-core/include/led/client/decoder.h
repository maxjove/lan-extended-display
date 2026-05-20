#pragma once

#include "led/common/status.h"
#include "led/protocol/messages.h"
#include "led/client/renderer.h"

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
    void setSinkPipeline(std::string sinkPipeline);
    void setRenderer(Renderer* renderer);

    Status configure(const protocol::VideoMode& mode);
    Status start();
    Status pushNal(const std::vector<std::uint8_t>& nalUnit);
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
    void stopBackend();

    protocol::VideoMode mode_{protocol::defaultStandardMode()};
    std::string sinkPipeline_{"fakesink sync=false"};
    DecoderStats stats_{};
    mutable std::mutex mutex_;
    Renderer* renderer_{nullptr};
    bool running_{false};

#if defined(LED_HAS_GSTREAMER)
    void* pipeline_{nullptr};
    void* appsrc_{nullptr};
    void* decodedSink_{nullptr};
#endif
};

}  // namespace led::client
