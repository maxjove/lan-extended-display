#pragma once

#include "led/common/status.h"
#include "led/host/capture_engine.h"
#include "led/protocol/messages.h"

#include <cstdint>
#include <vector>

namespace led::host {

struct EncodedFrame {
    std::uint64_t frameId{0};
    std::uint64_t timestampUs{0};
    bool keyFrame{false};
    std::vector<std::uint8_t> payload;
    std::vector<std::vector<std::uint8_t>> nalUnits;
};

class Encoder {
public:
    Status configure(const protocol::VideoMode& mode);
    Status start();
    Status stop();
    Status requestIdr();

    [[nodiscard]] EncodedFrame encode(const CapturedFrame& frame) const;
    [[nodiscard]] const protocol::VideoMode& mode() const;
    [[nodiscard]] const char* backendName() const;

private:
    Status startMediaFoundation();
    void stopMediaFoundation();
    [[nodiscard]] EncodedFrame encodePlaceholder(const CapturedFrame& frame) const;
    [[nodiscard]] EncodedFrame encodeMediaFoundation(const CapturedFrame& frame) const;

    protocol::VideoMode mode_{protocol::defaultStandardMode()};
    bool running_{false};
    bool mediaFoundationStarted_{false};
    bool hardwareMediaFoundation_{false};
    bool comInitialized_{false};
    void* transform_{nullptr};
    mutable std::vector<std::vector<std::uint8_t>> parameterSets_;
    mutable bool warnedFallback_{false};
};

}  // namespace led::host
