#pragma once

#include "led/common/status.h"
#include "led/protocol/control_messages.h"
#include "led/transport/tcp_socket.h"

#include <cstdint>
#include <string>

namespace led::client {

class SignalingClient {
public:
    Status connectAndReceiveOffer(
        const std::string& hostAddress,
        std::uint16_t port,
        const protocol::ClientHello& hello,
        protocol::SessionOffer& offer);
};

[[nodiscard]] protocol::ClientHello defaultClientHello();

}  // namespace led::client
