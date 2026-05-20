#pragma once

#include "led/client/decoder.h"
#include "led/client/renderer.h"
#include "led/common/status.h"
#include "led/protocol/messages.h"
#include "led/session/session_state.h"

namespace led::client {

class SessionClient {
public:
    Status connectToHost(const protocol::DeviceInfo& host, const protocol::VideoMode& mode);
    Status disconnect();

    [[nodiscard]] session::SessionState state() const;

private:
    session::SessionStateMachine stateMachine_;
    Decoder decoder_;
    Renderer renderer_;
};

}  // namespace led::client
