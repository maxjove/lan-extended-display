#pragma once

#include "led/common/status.h"
#include "led/host/display_manager.h"
#include "led/protocol/messages.h"

namespace led::host {

enum class InputInjectionBackend {
    dryRun,
    sendInput
};

class InputInjector {
public:
    explicit InputInjector(
        const DisplayManager& displayManager,
        InputInjectionBackend backend = InputInjectionBackend::dryRun);

    Status inject(const protocol::InputEvent& event);

    [[nodiscard]] const char* backendName() const;

private:
    Status injectDryRun(const protocol::InputEvent& event) const;
    Status injectSendInput(const protocol::InputEvent& event) const;

    const DisplayManager& displayManager_;
    InputInjectionBackend backend_{InputInjectionBackend::dryRun};
};

[[nodiscard]] bool parseInputInjectionBackend(const char* value, InputInjectionBackend& backend);
[[nodiscard]] const char* toString(InputInjectionBackend backend);

}  // namespace led::host
