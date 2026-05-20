#pragma once

#include "led/common/status.h"
#include "led/host/capture_engine.h"
#include "led/host/display_manager.h"
#include "led/host/encoder.h"
#include "led/session/session_state.h"

namespace led::host {

class SessionManager {
public:
    Status start(const protocol::VideoMode& requestedMode);
    Status stop();

    [[nodiscard]] session::SessionState state() const;
    [[nodiscard]] const DisplayManager& displayManager() const;

private:
    session::SessionStateMachine stateMachine_;
    DisplayManager displayManager_;
    CaptureEngine captureEngine_;
    Encoder encoder_;
};

}  // namespace led::host
