#include "led/common/logger.h"
#include "led/session/session_state.h"

#include <array>
#include <iostream>

int main() {
    led::session::SessionStateMachine machine;

    constexpr std::array flow{
        led::session::SessionEvent::startDiscovery,
        led::session::SessionEvent::hostSelected,
        led::session::SessionEvent::pairingAccepted,
        led::session::SessionEvent::negotiationStarted,
        led::session::SessionEvent::negotiationComplete,
    };

    for (const auto event : flow) {
        const auto status = machine.apply(event);
        if (!status.isOk()) {
            led::logError(status.message());
            return 1;
        }
        std::cout << led::session::toString(event)
                  << " -> " << led::session::toString(machine.state()) << '\n';
    }

    if (machine.state() != led::session::SessionState::streaming) {
        led::logError("state machine did not reach streaming");
        return 1;
    }

    const auto stopStatus = machine.apply(led::session::SessionEvent::stopRequested);
    if (!stopStatus.isOk()) {
        led::logError(stopStatus.message());
        return 1;
    }

    machine.apply(led::session::SessionEvent::stopped);
    std::cout << "final -> " << led::session::toString(machine.state()) << '\n';
    return machine.state() == led::session::SessionState::idle ? 0 : 1;
}
