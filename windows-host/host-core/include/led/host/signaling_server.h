#pragma once

#include "led/common/status.h"
#include "led/protocol/control_messages.h"
#include "led/transport/tcp_socket.h"

#include <cstdint>

namespace led::host {

class SignalingServer {
public:
    Status listen(std::uint16_t port);
    Status acceptClientAndSendOffer(const protocol::SessionOffer& offer, protocol::ClientHello& hello);
    Status close();

    [[nodiscard]] std::uint16_t port() const;

private:
    transport::TcpListener listener_;
};

}  // namespace led::host
