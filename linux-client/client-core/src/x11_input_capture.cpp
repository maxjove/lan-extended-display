#include "led/client/x11_input_capture.h"

#include "led/client/input_capture.h"
#include "led/client/input_sender.h"
#include "led/client/renderer.h"
#include "led/protocol/messages.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>

#if defined(LED_HAS_X11)
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <sys/select.h>
#ifdef Status
#undef Status
#endif
#endif

namespace led::client {

#if defined(LED_HAS_X11)
namespace {

double clampNormalized(double value) {
    return std::max(0.0, std::min(1.0, value));
}

std::int32_t keysymToWindowsVirtualKey(KeySym key) {
    if (key >= XK_a && key <= XK_z) {
        return static_cast<std::int32_t>('A' + (key - XK_a));
    }
    if (key >= XK_A && key <= XK_Z) {
        return static_cast<std::int32_t>('A' + (key - XK_A));
    }
    if (key >= XK_0 && key <= XK_9) {
        return static_cast<std::int32_t>('0' + (key - XK_0));
    }
    if (key >= XK_F1 && key <= XK_F12) {
        return static_cast<std::int32_t>(0x70 + (key - XK_F1));
    }

    switch (key) {
    case XK_BackSpace:
        return 0x08;
    case XK_Tab:
        return 0x09;
    case XK_Return:
    case XK_KP_Enter:
        return 0x0D;
    case XK_Escape:
        return 0x1B;
    case XK_space:
        return 0x20;
    case XK_Left:
        return 0x25;
    case XK_Up:
        return 0x26;
    case XK_Right:
        return 0x27;
    case XK_Down:
        return 0x28;
    case XK_Delete:
        return 0x2E;
    case XK_Shift_L:
    case XK_Shift_R:
        return 0x10;
    case XK_Control_L:
    case XK_Control_R:
        return 0x11;
    case XK_Alt_L:
    case XK_Alt_R:
    case XK_Meta_L:
    case XK_Meta_R:
        return 0x12;
    default:
        return 0;
    }
}

bool shouldStop(const std::atomic_bool* stopRequested) {
    return stopRequested != nullptr && stopRequested->load();
}

bool windowNameEquals(Display* display, Window window, const char* expectedName) {
    char* name = nullptr;
    const auto ok = XFetchName(display, window, &name) != 0 && name != nullptr;
    const bool matches = ok && std::strcmp(name, expectedName) == 0;
    if (name != nullptr) {
        XFree(name);
    }
    return matches;
}

Window findWindowByName(Display* display, Window root, const char* expectedName) {
    if (windowNameEquals(display, root, expectedName)) {
        return root;
    }

    Window returnedRoot = 0;
    Window returnedParent = 0;
    Window* children = nullptr;
    unsigned int childCount = 0;
    if (XQueryTree(display, root, &returnedRoot, &returnedParent, &children, &childCount) == 0) {
        return 0;
    }

    Window found = 0;
    for (unsigned int index = 0; index < childCount && found == 0; ++index) {
        found = findWindowByName(display, children[index], expectedName);
    }
    if (children != nullptr) {
        XFree(children);
    }
    return found;
}

Window waitForWindowByName(Display* display, Window root, const char* expectedName) {
    for (int attempt = 0; attempt < 50; ++attempt) {
        const auto window = findWindowByName(display, root, expectedName);
        if (window != 0) {
            return window;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return 0;
}

Cursor createBlankCursor(Display* display, Window window) {
    static char empty[] = {0};
    XColor color{};
    Pixmap pixmap = XCreateBitmapFromData(display, window, empty, 1, 1);
    Cursor cursor = XCreatePixmapCursor(display, pixmap, pixmap, &color, &color, 0, 0);
    XFreePixmap(display, pixmap);
    return cursor;
}

double normalizedCoordinate(int value, int span) {
    return clampNormalized(static_cast<double>(value) / static_cast<double>(std::max(1, span - 1)));
}

}  // namespace
#endif

bool X11InputCapture::available() {
#if defined(LED_HAS_X11)
    return true;
#else
    return false;
#endif
}

led::Status X11InputCapture::run(
    const X11InputCaptureOptions& options,
    const std::atomic_bool* stopRequested,
    X11InputCaptureStats& stats) {
#if !defined(LED_HAS_X11)
    (void)options;
    (void)stopRequested;
    (void)stats;
    return led::Status::unavailable("X11 input capture is not compiled in");
#else
    stats = {};

    InputSender sender;
    auto status = sender.open(options.hostAddress, options.inputPort);
    if (!status.isOk()) {
        return status;
    }

    Display* display = XOpenDisplay(nullptr);
    if (display == nullptr) {
        sender.close();
        return led::Status::unavailable("failed to open X11 display");
    }

    const int screen = DefaultScreen(display);
    const Window root = RootWindow(display, screen);
    Window target = waitForWindowByName(display, root, "LAN Extended Display");
    if (target == 0) {
        target = root;
    }

    XWindowAttributes attributes{};
    XGetWindowAttributes(display, target, &attributes);
    const int width = std::max(1, attributes.width);
    const int height = std::max(1, attributes.height);

    const unsigned int pointerEventMask = PointerMotionMask | ButtonPressMask | ButtonReleaseMask;
    XSelectInput(display, target, pointerEventMask | KeyPressMask | KeyReleaseMask | StructureNotifyMask);
    Cursor blankCursor = None;
    if (target != root && options.hideLocalCursor) {
        blankCursor = createBlankCursor(display, target);
        XDefineCursor(display, target, blankCursor);
    }

    bool pointerGrabbed = false;
    bool keyboardGrabbed = false;
    if (options.grabPointer) {
        pointerGrabbed = XGrabPointer(
            display,
            target,
            False,
            pointerEventMask,
            GrabModeAsync,
            GrabModeAsync,
            target == root ? None : target,
            blankCursor == None ? None : blankCursor,
            CurrentTime) == GrabSuccess;
    }
    if (options.grabKeyboard) {
        keyboardGrabbed = XGrabKeyboard(display, target, False, GrabModeAsync, GrabModeAsync, CurrentTime) == GrabSuccess;
    }

    if (options.grabPointer && !pointerGrabbed && options.grabKeyboard && !keyboardGrabbed) {
        if (blankCursor != None) {
            XUndefineCursor(display, target);
            XFreeCursor(display, blankCursor);
        }
        XCloseDisplay(display);
        sender.close();
        return led::Status::unavailable("failed to grab X11 pointer and keyboard");
    }

    InputCapture capture;
    auto lastEvent = std::chrono::steady_clock::now();
    auto sendInputEvent = [&](const protocol::InputEvent& inputEvent) -> led::Status {
        auto sendStatus = sender.send(inputEvent);
        if (sendStatus.isOk()) {
            ++stats.eventsSent;
            lastEvent = std::chrono::steady_clock::now();
        }
        return sendStatus;
    };
    for (;;) {
        if (shouldStop(stopRequested)) {
            break;
        }
        if (options.maxEvents != 0 && stats.eventsSent >= options.maxEvents) {
            break;
        }
        if (options.idleTimeoutMs != 0) {
            const auto idle = std::chrono::steady_clock::now() - lastEvent;
            if (std::chrono::duration_cast<std::chrono::milliseconds>(idle).count() >= options.idleTimeoutMs) {
                break;
            }
        }

        bool hasPendingMotion = false;
        protocol::InputEvent pendingMotion;
        while (XPending(display) > 0) {
            XEvent event{};
            XNextEvent(display, &event);

            protocol::InputEvent inputEvent;
            bool hasInput = false;
            switch (event.type) {
            case MotionNotify: {
                if (!options.forwardPointerEvents) {
                    break;
                }
                const auto x = normalizedCoordinate(event.xmotion.x, width);
                const auto y = normalizedCoordinate(event.xmotion.y, height);
                if (options.renderer != nullptr) {
                    options.renderer->updateLocalCursor(x, y);
                }
                pendingMotion = capture.pointerMove(x, y);
                hasPendingMotion = true;
                ++stats.pointerMoves;
                break;
            }
            case ButtonPress:
            case ButtonRelease: {
                if (!options.forwardPointerEvents) {
                    break;
                }
                if (hasPendingMotion) {
                    status = sendInputEvent(pendingMotion);
                    if (!status.isOk()) {
                        if (pointerGrabbed) {
                            XUngrabPointer(display, CurrentTime);
                        }
                        if (keyboardGrabbed) {
                            XUngrabKeyboard(display, CurrentTime);
                        }
                        if (blankCursor != None) {
                            XUndefineCursor(display, target);
                            XFreeCursor(display, blankCursor);
                        }
                        XCloseDisplay(display);
                        sender.close();
                        return status;
                    }
                    hasPendingMotion = false;
                }
                const auto button = event.xbutton.button;
                if (button == Button4 || button == Button5) {
                    if (event.type == ButtonPress) {
                        inputEvent = capture.wheel(button == Button4 ? 120 : -120);
                        ++stats.wheelEvents;
                        hasInput = true;
                    }
                    break;
                }
                const auto x = normalizedCoordinate(event.xbutton.x, width);
                const auto y = normalizedCoordinate(event.xbutton.y, height);
                if (options.renderer != nullptr) {
                    options.renderer->updateLocalCursor(x, y);
                }
                inputEvent = capture.pointerButton(x, y, static_cast<std::int32_t>(button), event.type == ButtonPress);
                ++stats.pointerButtons;
                hasInput = true;
                break;
            }
            case KeyPress:
            case KeyRelease: {
                if (!options.forwardKeyboardEvents) {
                    break;
                }
                if (hasPendingMotion) {
                    status = sendInputEvent(pendingMotion);
                    if (!status.isOk()) {
                        if (pointerGrabbed) {
                            XUngrabPointer(display, CurrentTime);
                        }
                        if (keyboardGrabbed) {
                            XUngrabKeyboard(display, CurrentTime);
                        }
                        if (blankCursor != None) {
                            XUndefineCursor(display, target);
                            XFreeCursor(display, blankCursor);
                        }
                        XCloseDisplay(display);
                        sender.close();
                        return status;
                    }
                    hasPendingMotion = false;
                }
                const auto key = XLookupKeysym(&event.xkey, 0);
                const auto virtualKey = keysymToWindowsVirtualKey(key);
                if (virtualKey != 0) {
                    inputEvent = capture.key(virtualKey, event.type == KeyPress);
                    ++stats.keyEvents;
                    hasInput = true;
                }
                break;
            }
            default:
                break;
            }

            if (!hasInput) {
                ++stats.ignoredEvents;
                continue;
            }

            status = sendInputEvent(inputEvent);
            if (!status.isOk()) {
                if (pointerGrabbed) {
                    XUngrabPointer(display, CurrentTime);
                }
                if (keyboardGrabbed) {
                    XUngrabKeyboard(display, CurrentTime);
                }
                if (blankCursor != None) {
                    XUndefineCursor(display, target);
                    XFreeCursor(display, blankCursor);
                }
                XCloseDisplay(display);
                sender.close();
                return status;
            }
        }

        if (hasPendingMotion) {
            status = sendInputEvent(pendingMotion);
            if (!status.isOk()) {
                if (pointerGrabbed) {
                    XUngrabPointer(display, CurrentTime);
                }
                if (keyboardGrabbed) {
                    XUngrabKeyboard(display, CurrentTime);
                }
                if (blankCursor != None) {
                    XUndefineCursor(display, target);
                    XFreeCursor(display, blankCursor);
                }
                XCloseDisplay(display);
                sender.close();
                return status;
            }
        }

        const int connection = ConnectionNumber(display);
        fd_set descriptors;
        FD_ZERO(&descriptors);
        FD_SET(connection, &descriptors);

        timeval timeout{};
        timeout.tv_sec = 0;
        timeout.tv_usec = 1000;
        (void)select(connection + 1, &descriptors, nullptr, nullptr, &timeout);
    }

    if (pointerGrabbed) {
        XUngrabPointer(display, CurrentTime);
    }
    if (keyboardGrabbed) {
        XUngrabKeyboard(display, CurrentTime);
    }
    if (blankCursor != None) {
        XUndefineCursor(display, target);
        XFreeCursor(display, blankCursor);
    }
    XCloseDisplay(display);
    sender.close();
    return led::Status::ok();
#endif
}

}  // namespace led::client
