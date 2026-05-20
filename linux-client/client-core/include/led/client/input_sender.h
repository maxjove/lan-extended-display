#pragma once

#include "led/common/status.h"
#include "led/protocol/messages.h"
#include "led/transport/udp_socket.h"

#include <cstdint>
#include <string>

namespace led::client {

class InputSender {
public:
    Status open(const std::string& hostAddress, std::uint16_t inputPort);
    Status send(const protocol::InputEvent& event) const;
    void close();

private:
    transport::UdpSocket socket_;
    transport::UdpEndpoint target_;
};

}  // namespace led::client
