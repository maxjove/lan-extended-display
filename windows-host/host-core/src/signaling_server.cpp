#include "led/host/signaling_server.h"

#include "led/common/logger.h"

#include <string>

namespace led::host {

Status SignalingServer::listen(std::uint16_t port) {
    auto status = listener_.bindAndListen(port);
    if (!status.isOk()) {
        return status;
    }

    logInfo("host signaling server listening on TCP port " + std::to_string(listener_.localPort()));
    return Status::ok();
}

Status SignalingServer::acceptClientAndSendOffer(
    const protocol::SessionOffer& offer,
    protocol::ClientHello& hello) {
    transport::TcpStream stream;
    auto status = listener_.accept(stream);
    if (!status.isOk()) {
        return status;
    }

    std::string line;
    status = stream.readLine(line);
    if (!status.isOk()) {
        return status;
    }

    status = protocol::parseClientHello(line, hello);
    if (!status.isOk()) {
        return status;
    }

    logInfo("host signaling received CLIENT_HELLO from " + hello.device.name);
    return stream.sendLine(protocol::serializeSessionOffer(offer));
}

Status SignalingServer::close() {
    listener_.close();
    logInfo("host signaling server closed");
    return Status::ok();
}

std::uint16_t SignalingServer::port() const {
    return listener_.localPort();
}

}  // namespace led::host
