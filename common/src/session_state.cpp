#include "led/session/session_state.h"

#include <utility>

namespace led::session {

const char* toString(SessionState state) {
    switch (state) {
    case SessionState::idle:
        return "idle";
    case SessionState::discovering:
        return "discovering";
    case SessionState::pairing:
        return "pairing";
    case SessionState::paired:
        return "paired";
    case SessionState::negotiating:
        return "negotiating";
    case SessionState::streaming:
        return "streaming";
    case SessionState::stopping:
        return "stopping";
    case SessionState::error:
        return "error";
    }
    return "unknown";
}

const char* toString(SessionEvent event) {
    switch (event) {
    case SessionEvent::startDiscovery:
        return "start_discovery";
    case SessionEvent::hostSelected:
        return "host_selected";
    case SessionEvent::pairingAccepted:
        return "pairing_accepted";
    case SessionEvent::pairingRejected:
        return "pairing_rejected";
    case SessionEvent::negotiationStarted:
        return "negotiation_started";
    case SessionEvent::negotiationComplete:
        return "negotiation_complete";
    case SessionEvent::streamStarted:
        return "stream_started";
    case SessionEvent::stopRequested:
        return "stop_requested";
    case SessionEvent::stopped:
        return "stopped";
    case SessionEvent::failure:
        return "failure";
    }
    return "unknown";
}

SessionState SessionStateMachine::state() const {
    return state_;
}

const std::string& SessionStateMachine::lastError() const {
    return lastError_;
}

Status SessionStateMachine::apply(SessionEvent event) {
    if (event == SessionEvent::failure) {
        fail("session failure event received");
        return Status::ok();
    }

    if (event == SessionEvent::pairingRejected) {
        state_ = SessionState::idle;
        lastError_ = "pairing rejected";
        return Status::ok();
    }

    if (event == SessionEvent::stopRequested) {
        if (state_ == SessionState::idle) {
            return Status::invalidState("cannot stop an idle session");
        }
        state_ = SessionState::stopping;
        return Status::ok();
    }

    if (event == SessionEvent::stopped) {
        reset();
        return Status::ok();
    }

    switch (state_) {
    case SessionState::idle:
        if (event == SessionEvent::startDiscovery) {
            state_ = SessionState::discovering;
            return Status::ok();
        }
        break;
    case SessionState::discovering:
        if (event == SessionEvent::hostSelected) {
            state_ = SessionState::pairing;
            return Status::ok();
        }
        break;
    case SessionState::pairing:
        if (event == SessionEvent::pairingAccepted) {
            state_ = SessionState::paired;
            return Status::ok();
        }
        break;
    case SessionState::paired:
        if (event == SessionEvent::negotiationStarted) {
            state_ = SessionState::negotiating;
            return Status::ok();
        }
        break;
    case SessionState::negotiating:
        if (event == SessionEvent::negotiationComplete || event == SessionEvent::streamStarted) {
            state_ = SessionState::streaming;
            return Status::ok();
        }
        break;
    case SessionState::streaming:
    case SessionState::stopping:
    case SessionState::error:
        break;
    }

    return Status::invalidState(
        std::string("invalid transition from ") + toString(state_) + " with event " + toString(event));
}

void SessionStateMachine::fail(std::string message) {
    state_ = SessionState::error;
    lastError_ = std::move(message);
}

void SessionStateMachine::reset() {
    state_ = SessionState::idle;
    lastError_.clear();
}

}  // namespace led::session
