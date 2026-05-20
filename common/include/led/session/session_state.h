#pragma once

#include "led/common/status.h"

#include <string>

namespace led::session {

enum class SessionState {
    idle,
    discovering,
    pairing,
    paired,
    negotiating,
    streaming,
    stopping,
    error
};

enum class SessionEvent {
    startDiscovery,
    hostSelected,
    pairingAccepted,
    pairingRejected,
    negotiationStarted,
    negotiationComplete,
    streamStarted,
    stopRequested,
    stopped,
    failure
};

[[nodiscard]] const char* toString(SessionState state);
[[nodiscard]] const char* toString(SessionEvent event);

class SessionStateMachine {
public:
    [[nodiscard]] SessionState state() const;
    [[nodiscard]] const std::string& lastError() const;

    Status apply(SessionEvent event);
    void fail(std::string message);
    void reset();

private:
    SessionState state_{SessionState::idle};
    std::string lastError_;
};

}  // namespace led::session
