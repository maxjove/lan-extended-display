#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace led::protocol {

enum class Codec {
    h264,
    h265,
    av1
};

enum class QualityPreset {
    smooth,
    standard,
    sharp,
    compatibility
};

enum class MessageType {
    hello,
    pairingRequest,
    pairingResult,
    capabilityOffer,
    sessionStart,
    sessionStop,
    heartbeat,
    networkStats,
    inputEvent,
    requestIdr
};

struct Resolution {
    std::uint32_t width{1920};
    std::uint32_t height{1080};
    std::uint32_t refreshRate{60};
};

struct VideoMode {
    Resolution resolution{};
    Codec codec{Codec::h264};
    QualityPreset preset{QualityPreset::standard};
    std::uint32_t bitrateKbps{20000};
    bool lowLatency{true};
};

struct DeviceInfo {
    std::string deviceId;
    std::string name;
    std::string platform;
    std::string version;
    std::vector<VideoMode> supportedVideoModes;
};

enum class InputKind {
    pointerMove,
    pointerButton,
    wheel,
    key,
    touch
};

struct InputEvent {
    InputKind kind{InputKind::pointerMove};
    std::uint64_t sequence{0};
    double normalizedX{0.0};
    double normalizedY{0.0};
    std::int32_t value0{0};
    std::int32_t value1{0};
    std::uint64_t sendTimeUs{0};
    bool reliable{false};
};

[[nodiscard]] const char* toString(Codec codec);
[[nodiscard]] const char* toString(QualityPreset preset);
[[nodiscard]] const char* toString(MessageType type);
[[nodiscard]] const char* toString(InputKind kind);
[[nodiscard]] bool parseCodec(const std::string& value, Codec& codec);
[[nodiscard]] bool parseQualityPreset(const std::string& value, QualityPreset& preset);
[[nodiscard]] bool parseInputKind(const std::string& value, InputKind& kind);

[[nodiscard]] std::string serializeInputEvent(const InputEvent& event);
[[nodiscard]] bool parseInputEvent(const std::string& text, InputEvent& event);

[[nodiscard]] VideoMode defaultStandardMode();
[[nodiscard]] VideoMode defaultSharpMode();
[[nodiscard]] std::vector<VideoMode> defaultVideoModes();

}  // namespace led::protocol
