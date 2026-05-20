#include "led/client/signaling_client.h"

#include "led/common/logger.h"

namespace led::client {

Status SignalingClient::connectAndReceiveOffer(
    const std::string& hostAddress,
    std::uint16_t port,
    const protocol::ClientHello& hello,
    protocol::SessionOffer& offer) {
    transport::TcpStream stream;
    auto status = stream.connect(transport::UdpEndpoint{hostAddress, port});
    if (!status.isOk()) {
        return status;
    }

    status = stream.sendLine(protocol::serializeClientHello(hello));
    if (!status.isOk()) {
        return status;
    }

    std::string line;
    status = stream.readLine(line);
    if (!status.isOk()) {
        return status;
    }

    status = protocol::parseSessionOffer(line, offer);
    if (!status.isOk()) {
        return status;
    }

    logInfo("client signaling received SESSION_OFFER from host");
    return Status::ok();
}

protocol::ClientHello defaultClientHello() {
    protocol::ClientHello hello;
    hello.device.deviceId = "linux-arm64-client";
    hello.device.name = "Linux-ARM64-Client";
    hello.device.platform = "linux-arm64";
    hello.device.version = "0.1.0";
    return hello;
}

}  // namespace led::client
