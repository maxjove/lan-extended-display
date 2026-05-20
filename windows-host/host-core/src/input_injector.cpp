#include "led/host/input_injector.h"

#include "led/common/logger.h"

#include <algorithm>
#include <cmath>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace led::host {

namespace {

double clampNormalized(double value) {
    if (!std::isfinite(value)) {
        return 0.0;
    }
    return std::clamp(value, 0.0, 1.0);
}

#if defined(_WIN32)
struct VirtualDesktopBounds {
    int left{0};
    int top{0};
    int width{1};
    int height{1};
};

VirtualDesktopBounds currentVirtualDesktopBounds() {
    VirtualDesktopBounds bounds;
    bounds.left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    bounds.top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    bounds.width = std::max(1, GetSystemMetrics(SM_CXVIRTUALSCREEN));
    bounds.height = std::max(1, GetSystemMetrics(SM_CYVIRTUALSCREEN));
    return bounds;
}

LONG toAbsoluteCoordinate(int value, int origin, int span) {
    if (span <= 1) {
        return 0;
    }
    const auto normalized = static_cast<double>(value - origin) * 65535.0 / static_cast<double>(span - 1);
    return static_cast<LONG>(std::clamp(std::lround(normalized), 0L, 65535L));
}

Status sendMouseInput(DWORD flags, LONG dx, LONG dy, DWORD mouseData = 0) {
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dx = dx;
    input.mi.dy = dy;
    input.mi.mouseData = mouseData;
    input.mi.dwFlags = flags;
    if (SendInput(1, &input, sizeof(INPUT)) != 1) {
        return Status::unavailable("SendInput mouse event failed");
    }
    return Status::ok();
}

Status sendAbsoluteMouseButton(DWORD buttonFlag, LONG dx, LONG dy) {
    INPUT inputs[2]{};
    inputs[0].type = INPUT_MOUSE;
    inputs[0].mi.dx = dx;
    inputs[0].mi.dy = dy;
    inputs[0].mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;

    inputs[1].type = INPUT_MOUSE;
    inputs[1].mi.dwFlags = buttonFlag;

    if (SendInput(2, inputs, sizeof(INPUT)) != 2) {
        return Status::unavailable("SendInput absolute mouse button event failed");
    }
    return Status::ok();
}

Status sendKeyInput(WORD virtualKey, bool down) {
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = virtualKey;
    input.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
    if (SendInput(1, &input, sizeof(INPUT)) != 1) {
        return Status::unavailable("SendInput key event failed");
    }
    return Status::ok();
}
#endif

}  // namespace

InputInjector::InputInjector(const DisplayManager& displayManager, InputInjectionBackend backend)
    : displayManager_(displayManager),
      backend_(backend) {}

Status InputInjector::inject(const protocol::InputEvent& event) {
    if (!displayManager_.virtualDisplay().active) {
        return Status::invalidState("cannot inject input without an active virtual display");
    }

    if (backend_ == InputInjectionBackend::dryRun) {
        return injectDryRun(event);
    }
    return injectSendInput(event);
}

const char* InputInjector::backendName() const {
    return toString(backend_);
}

Status InputInjector::injectDryRun(const protocol::InputEvent& event) const {
    logDebug(std::string("host input dry-run accepted ") + protocol::toString(event.kind));
    return Status::ok();
}

Status InputInjector::injectSendInput(const protocol::InputEvent& event) const {
#if defined(_WIN32)
    const auto& display = displayManager_.virtualDisplay();
    const auto bounds = currentVirtualDesktopBounds();
    const auto x = display.originX + static_cast<int>(
        std::lround(clampNormalized(event.normalizedX) * static_cast<double>(display.resolution.width - 1)));
    const auto y = display.originY + static_cast<int>(
        std::lround(clampNormalized(event.normalizedY) * static_cast<double>(display.resolution.height - 1)));
    const auto absoluteX = toAbsoluteCoordinate(x, bounds.left, bounds.width);
    const auto absoluteY = toAbsoluteCoordinate(y, bounds.top, bounds.height);

    switch (event.kind) {
    case protocol::InputKind::pointerMove:
        return sendMouseInput(MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK, absoluteX, absoluteY);
    case protocol::InputKind::pointerButton: {
        const bool down = event.value1 != 0;
        DWORD flag = 0;
        if (event.value0 == 1) {
            flag = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
        } else if (event.value0 == 2) {
            flag = down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
        } else if (event.value0 == 3) {
            flag = down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
        } else {
            return Status::invalidArgument("unsupported pointer button id");
        }
        return sendAbsoluteMouseButton(flag, absoluteX, absoluteY);
    }
    case protocol::InputKind::wheel:
        return sendMouseInput(MOUSEEVENTF_WHEEL, 0, 0, static_cast<DWORD>(event.value0));
    case protocol::InputKind::key:
        if (event.value0 <= 0 || event.value0 > 255) {
            return Status::invalidArgument("key event requires a Windows virtual-key code in value0");
        }
        return sendKeyInput(static_cast<WORD>(event.value0), event.value1 != 0);
    case protocol::InputKind::touch:
        return Status::unavailable("SendInput touch injection is not implemented yet");
    }
    return Status::invalidArgument("unknown input event kind");
#else
    (void)event;
    return Status::unavailable("SendInput backend is only available on Windows");
#endif
}

bool parseInputInjectionBackend(const char* value, InputInjectionBackend& backend) {
    const std::string text = value != nullptr ? value : "";
    if (text == "dry-run" || text == "dryrun" || text.empty()) {
        backend = InputInjectionBackend::dryRun;
        return true;
    }
    if (text == "sendinput" || text == "inject") {
        backend = InputInjectionBackend::sendInput;
        return true;
    }
    return false;
}

const char* toString(InputInjectionBackend backend) {
    switch (backend) {
    case InputInjectionBackend::dryRun:
        return "dry-run";
    case InputInjectionBackend::sendInput:
        return "sendinput";
    }
    return "unknown";
}

}  // namespace led::host
