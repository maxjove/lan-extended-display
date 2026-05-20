#include "led/protocol/control_messages.h"

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

Status requireValue(
    const std::unordered_map<std::string, std::string>& values,
    const std::string& key,
    std::string& value) {
    const auto found = values.find(key);
    if (found == values.end() || found->second.empty()) {
        return Status::invalidArgument("missing control message field: " + key);
    }
    value = found->second;
    return Status::ok();
}

Status parseU32(
    const std::unordered_map<std::string, std::string>& values,
    const std::string& key,
    std::uint32_t& value) {
    std::string text;
    auto status = requireValue(values, key, text);
    if (!status.isOk()) {
        return status;
    }

    try {
        value = static_cast<std::uint32_t>(std::stoul(text));
    } catch (...) {
        return Status::invalidArgument("invalid unsigned integer field: " + key);
    }
    return Status::ok();
}

Status parseU16(
    const std::unordered_map<std::string, std::string>& values,
    const std::string& key,
    std::uint16_t& value) {
    std::uint32_t parsed = 0;
    auto status = parseU32(values, key, parsed);
    if (!status.isOk()) {
        return status;
    }
    if (parsed > 65535) {
        return Status::invalidArgument("field is outside uint16 range: " + key);
    }
    value = static_cast<std::uint16_t>(parsed);
    return Status::ok();
}

}  // namespace

std::string serializeClientHello(const ClientHello& hello) {
    std::ostringstream stream;
    stream << "CLIENT_HELLO"
           << " device_id=" << hello.device.deviceId
           << " name=" << hello.device.name
           << " platform=" << hello.device.platform
           << " version=" << hello.device.version;
    return stream.str();
}

Status parseClientHello(const std::string& line, ClientHello& hello) {
    std::istringstream stream(line);
    std::string type;
    stream >> type;
    if (type != "CLIENT_HELLO") {
        return Status::invalidArgument("control message is not CLIENT_HELLO");
    }

    const auto values = parseKeyValues(stream);
    auto status = requireValue(values, "device_id", hello.device.deviceId);
    if (!status.isOk()) {
        return status;
    }
    status = requireValue(values, "name", hello.device.name);
    if (!status.isOk()) {
        return status;
    }
    status = requireValue(values, "platform", hello.device.platform);
    if (!status.isOk()) {
        return status;
    }
    return requireValue(values, "version", hello.device.version);
}

std::string serializeSessionOffer(const SessionOffer& offer) {
    std::ostringstream stream;
    stream << "SESSION_OFFER"
           << " width=" << offer.videoMode.resolution.width
           << " height=" << offer.videoMode.resolution.height
           << " refresh=" << offer.videoMode.resolution.refreshRate
           << " codec=" << toString(offer.videoMode.codec)
           << " preset=" << toString(offer.videoMode.preset)
           << " bitrate=" << offer.videoMode.bitrateKbps
           << " rtp_port=" << offer.rtpPort
           << " token=" << offer.sessionToken;
    return stream.str();
}

Status parseSessionOffer(const std::string& line, SessionOffer& offer) {
    std::istringstream stream(line);
    std::string type;
    stream >> type;
    if (type != "SESSION_OFFER") {
        return Status::invalidArgument("control message is not SESSION_OFFER");
    }

    const auto values = parseKeyValues(stream);
    auto status = parseU32(values, "width", offer.videoMode.resolution.width);
    if (!status.isOk()) {
        return status;
    }
    status = parseU32(values, "height", offer.videoMode.resolution.height);
    if (!status.isOk()) {
        return status;
    }
    status = parseU32(values, "refresh", offer.videoMode.resolution.refreshRate);
    if (!status.isOk()) {
        return status;
    }
    status = parseU32(values, "bitrate", offer.videoMode.bitrateKbps);
    if (!status.isOk()) {
        return status;
    }
    status = parseU16(values, "rtp_port", offer.rtpPort);
    if (!status.isOk()) {
        return status;
    }

    std::string codecText;
    status = requireValue(values, "codec", codecText);
    if (!status.isOk()) {
        return status;
    }
    if (!parseCodec(codecText, offer.videoMode.codec)) {
        return Status::invalidArgument("unsupported codec: " + codecText);
    }

    std::string presetText;
    status = requireValue(values, "preset", presetText);
    if (!status.isOk()) {
        return status;
    }
    if (!parseQualityPreset(presetText, offer.videoMode.preset)) {
        return Status::invalidArgument("unsupported quality preset: " + presetText);
    }

    status = requireValue(values, "token", offer.sessionToken);
    if (!status.isOk()) {
        return status;
    }
    offer.videoMode.lowLatency = true;
    return Status::ok();
}

std::string serializeClientReady(const ClientReady& ready) {
    std::ostringstream stream;
    stream << "CLIENT_READY"
           << " rtp_port=" << ready.rtpPort
           << " token=" << ready.sessionToken;
    return stream.str();
}

Status parseClientReady(const std::string& line, ClientReady& ready) {
    std::istringstream stream(line);
    std::string type;
    stream >> type;
    if (type != "CLIENT_READY") {
        return Status::invalidArgument("control message is not CLIENT_READY");
    }

    const auto values = parseKeyValues(stream);
    auto status = parseU16(values, "rtp_port", ready.rtpPort);
    if (!status.isOk()) {
        return status;
    }
    return requireValue(values, "token", ready.sessionToken);
}

}  // namespace led::protocol
