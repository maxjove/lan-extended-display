#include "led/host/session_manager.h"

#include "led/common/logger.h"

namespace led::host {

Status SessionManager::start(const protocol::VideoMode& requestedMode) {
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

    status = displayManager_.createVirtualDisplay(requestedMode.resolution);
    if (!status.isOk()) {
        stateMachine_.fail(status.message());
        return status;
    }

    status = encoder_.configure(requestedMode);
    if (!status.isOk()) {
        stateMachine_.fail(status.message());
        return status;
    }

    const auto& display = displayManager_.virtualDisplay();
    status = captureEngine_.startRegion(requestedMode.resolution, display.originX, display.originY);
    if (!status.isOk()) {
        stateMachine_.fail(status.message());
        return status;
    }

    status = encoder_.start();
    if (!status.isOk()) {
        stateMachine_.fail(status.message());
        return status;
    }

    status = stateMachine_.apply(session::SessionEvent::negotiationComplete);
    if (!status.isOk()) {
        return status;
    }

    logInfo("host session reached streaming state");
    return Status::ok();
}

Status SessionManager::stop() {
    auto status = stateMachine_.apply(session::SessionEvent::stopRequested);
    if (!status.isOk()) {
        return status;
    }

    encoder_.stop();
    captureEngine_.stop();
    displayManager_.destroyVirtualDisplay();
    displayManager_.restoreDisplayLayout();

    stateMachine_.apply(session::SessionEvent::stopped);
    logInfo("host session stopped");
    return Status::ok();
}

session::SessionState SessionManager::state() const {
    return stateMachine_.state();
}

const DisplayManager& SessionManager::displayManager() const {
    return displayManager_;
}

}  // namespace led::host
