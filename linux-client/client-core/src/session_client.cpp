#include "led/client/session_client.h"

#include "led/common/logger.h"

namespace led::client {

Status SessionClient::connectToHost(const protocol::DeviceInfo& host, const protocol::VideoMode& mode) {
    (void)host;

    auto status = stateMachine_.apply(session::SessionEvent::startDiscovery);
    if (!status.isOk()) {
        return status;
    }

    status = stateMachine_.apply(session::SessionEvent::hostSelected);
    if (!status.isOk()) {
        return status;
    }

    status = stateMachine_.apply(session::SessionEvent::pairingAccepted);
    if (!status.isOk()) {
        return status;
    }

    status = stateMachine_.apply(session::SessionEvent::negotiationStarted);
    if (!status.isOk()) {
        return status;
    }

    status = decoder_.configure(mode);
    if (!status.isOk()) {
        stateMachine_.fail(status.message());
        return status;
    }

    status = renderer_.openFullscreen(mode.resolution);
    if (!status.isOk()) {
        stateMachine_.fail(status.message());
        return status;
    }

    status = decoder_.start();
    if (!status.isOk()) {
        stateMachine_.fail(status.message());
        return status;
    }

    status = stateMachine_.apply(session::SessionEvent::negotiationComplete);
    if (!status.isOk()) {
        return status;
    }

    logInfo("client session reached streaming state");
    return Status::ok();
}

Status SessionClient::disconnect() {
    auto status = stateMachine_.apply(session::SessionEvent::stopRequested);
    if (!status.isOk()) {
        return status;
    }

    decoder_.stop();
    renderer_.close();
    stateMachine_.apply(session::SessionEvent::stopped);
    logInfo("client session stopped");
    return Status::ok();
}

session::SessionState SessionClient::state() const {
    return stateMachine_.state();
}

}  // namespace led::client
