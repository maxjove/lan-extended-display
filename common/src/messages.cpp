#include "led/protocol/messages.h"

#include <sstream>
#include <string>
#include <unordered_map>

namespace led::protocol {

namespace {

std::unordered_map<std::string, std::string> parseKeyValues(std::istringstream& stream) {
    std::unordered_map<std::string, std::string> values;
    std::string token;
    while (stream >> token) {
        const auto separator = token.find('=');
        if (separator == std::string::npos) {
            continue;
        }
        values[token.substr(0, separator)] = token.substr(separator + 1);
    }
    return values;
}

bool parseU64(
    const std::unordered_map<std::string, std::string>& values,
    const std::string& key,
    std::uint64_t& value) {
    const auto found = values.find(key);
    if (found == values.end()) {
        return false;
    }
    try {
        value = static_cast<std::uint64_t>(std::stoull(found->second));
    } catch (...) {
        return false;
    }
    return true;
}

bool parseI32(
    const std::unordered_map<std::string, std::string>& values,
    const std::string& key,
    std::int32_t& value) {
    const auto found = values.find(key);
    if (found == values.end()) {
        return false;
    }
    try {
        value = static_cast<std::int32_t>(std::stol(found->second));
    } catch (...) {
        return false;
    }
    return true;
}

bool parseDouble(
    const std::unordered_map<std::string, std::string>& values,
    const std::string& key,
    double& value) {
    const auto found = values.find(key);
    if (found == values.end()) {
        return false;
    }
    try {
        value = std::stod(found->second);
    } catch (...) {
        return false;
    }
    return true;
}

}  // namespace

const char* toString(Codec codec) {
    switch (codec) {
    case Codec::h264:
        return "h264";
    case Codec::h265:
        return "h265";
    case Codec::av1:
        return "av1";
    }
    return "unknown";
}

const char* toString(QualityPreset preset) {
    switch (preset) {
    case QualityPreset::smooth:
        return "smooth";
    case QualityPreset::standard:
        return "standard";
    case QualityPreset::sharp:
        return "sharp";
    case QualityPreset::compatibility:
        return "compatibility";
    }
    return "unknown";
}

const char* toString(MessageType type) {
    switch (type) {
    case MessageType::hello:
        return "hello";
    case MessageType::pairingRequest:
        return "pairing_request";
    case MessageType::pairingResult:
        return "pairing_result";
    case MessageType::capabilityOffer:
        return "capability_offer";
    case MessageType::sessionStart:
        return "session_start";
    case MessageType::sessionStop:
        return "session_stop";
    case MessageType::heartbeat:
        return "heartbeat";
    case MessageType::networkStats:
        return "network_stats";
    case MessageType::inputEvent:
        return "input_event";
    case MessageType::requestIdr:
        return "request_idr";
    }
    return "unknown";
}

const char* toString(InputKind kind) {
    switch (kind) {
    case InputKind::pointerMove:
        return "pointer_move";
    case InputKind::pointerButton:
        return "pointer_button";
    case InputKind::wheel:
        return "wheel";
    case InputKind::key:
        return "key";
    case InputKind::touch:
        return "touch";
    }
    return "unknown";
}

bool parseCodec(const std::string& value, Codec& codec) {
    if (value == "h264") {
        codec = Codec::h264;
        return true;
    }
    if (value == "h265") {
        codec = Codec::h265;
        return true;
    }
    if (value == "av1") {
        codec = Codec::av1;
        return true;
    }
    return false;
}

bool parseQualityPreset(const std::string& value, QualityPreset& preset) {
    if (value == "smooth") {
        preset = QualityPreset::smooth;
        return true;
    }
    if (value == "standard") {
        preset = QualityPreset::standard;
        return true;
    }
    if (value == "sharp") {
        preset = QualityPreset::sharp;
        return true;
    }
    if (value == "compatibility") {
        preset = QualityPreset::compatibility;
        return true;
    }
    return false;
}

bool parseInputKind(const std::string& value, InputKind& kind) {
    if (value == "pointer_move") {
        kind = InputKind::pointerMove;
        return true;
    }
    if (value == "pointer_button") {
        kind = InputKind::pointerButton;
        return true;
    }
    if (value == "wheel") {
        kind = InputKind::wheel;
        return true;
    }
    if (value == "key") {
        kind = InputKind::key;
        return true;
    }
    if (value == "touch") {
        kind = InputKind::touch;
        return true;
    }
    return false;
}

std::string serializeInputEvent(const InputEvent& event) {
    std::ostringstream stream;
    stream << "INPUT_EVENT"
           << " seq=" << event.sequence
           << " kind=" << toString(event.kind)
           << " x=" << event.normalizedX
           << " y=" << event.normalizedY
           << " v0=" << event.value0
           << " v1=" << event.value1
           << " send_us=" << event.sendTimeUs
           << " reliable=" << (event.reliable ? 1 : 0);
    return stream.str();
}

bool parseInputEvent(const std::string& text, InputEvent& event) {
    std::istringstream stream(text);
    std::string type;
    stream >> type;
    if (type != "INPUT_EVENT") {
        return false;
    }

    const auto values = parseKeyValues(stream);
    if (!parseU64(values, "seq", event.sequence)) {
        return false;
    }

    const auto kindValue = values.find("kind");
    if (kindValue == values.end() || !parseInputKind(kindValue->second, event.kind)) {
        return false;
    }

    if (!parseDouble(values, "x", event.normalizedX) ||
        !parseDouble(values, "y", event.normalizedY) ||
        !parseI32(values, "v0", event.value0) ||
        !parseI32(values, "v1", event.value1)) {
        return false;
    }

    const auto reliableValue = values.find("reliable");
    event.reliable = reliableValue != values.end() && reliableValue->second == "1";
    event.sendTimeUs = 0;
    const auto sendTimeValue = values.find("send_us");
    if (sendTimeValue != values.end()) {
        try {
            event.sendTimeUs = static_cast<std::uint64_t>(std::stoull(sendTimeValue->second));
        } catch (...) {
            return false;
        }
    }
    return true;
}

VideoMode defaultStandardMode() {
    return VideoMode{
        Resolution{1920, 1080, 60},
        Codec::h264,
        QualityPreset::standard,
        20000,
        true,
    };
}

VideoMode defaultSharpMode() {
    return VideoMode{
        Resolution{1920, 1080, 60},
        Codec::h264,
        QualityPreset::sharp,
        30000,
        true,
    };
}

std::vector<VideoMode> defaultVideoModes() {
    return {
        VideoMode{Resolution{1280, 720, 60}, Codec::h264, QualityPreset::smooth, 8000, true},
        defaultStandardMode(),
        defaultSharpMode(),
        VideoMode{Resolution{1920, 1080, 30}, Codec::h264, QualityPreset::compatibility, 12000, true},
    };
}

}  // namespace led::protocol
