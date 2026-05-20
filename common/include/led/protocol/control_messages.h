#pragma once

#include "led/common/status.h"
#include "led/protocol/messages.h"

#include <cstdint>
#include <string>

namespace led::protocol {

struct ClientHello {
    DeviceInfo device;
};

struct SessionOffer {
    VideoMode videoMode{defaultStandardMode()};
    std::uint16_t rtpPort{17670};
    std::string sessionToken;
};

struct ClientReady {
    std::uint16_t rtpPort{17670};
    std::string sessionToken;
};

[[nodiscard]] std::string serializeClientHello(const ClientHello& hello);
[[nodiscard]] Status parseClientHello(const std::string& line, ClientHello& hello);

[[nodiscard]] std::string serializeSessionOffer(const SessionOffer& offer);
[[nodiscard]] Status parseSessionOffer(const std::string& line, SessionOffer& offer);

[[nodiscard]] std::string serializeClientReady(const ClientReady& ready);
[[nodiscard]] Status parseClientReady(const std::string& line, ClientReady& ready);

}  // namespace led::protocol
