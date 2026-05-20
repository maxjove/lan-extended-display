#include "led/host/cursor_suppressor.h"

#include <array>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef OEMRESOURCE
#define OEMRESOURCE
#endif
#include <windows.h>
#endif

namespace led::host {

#if defined(_WIN32)
namespace {

constexpr std::array<DWORD, 13> kSystemCursorIds{
    OCR_NORMAL,
    OCR_IBEAM,
    OCR_WAIT,
    OCR_CROSS,
    OCR_UP,
    OCR_SIZENWSE,
    OCR_SIZENESW,
    OCR_SIZEWE,
    OCR_SIZENS,
    OCR_SIZEALL,
    OCR_NO,
    OCR_HAND,
    OCR_APPSTARTING,
};

HCURSOR makeBlankCursor() {
    const BYTE andMask[] = {0xFF};
    const BYTE xorMask[] = {0x00};
    return CreateCursor(GetModuleHandle(nullptr), 0, 0, 1, 1, andMask, xorMask);
}

}  // namespace
#endif

Status CursorSuppressor::hideSystemCursor() {
#if defined(_WIN32)
    if (hidden_) {
        return Status::ok();
    }
    for (const auto cursorId : kSystemCursorIds) {
        HCURSOR cursor = makeBlankCursor();
        if (cursor == nullptr) {
            restoreSystemCursor();
            return Status::unavailable("failed to create blank Windows cursor");
        }
        if (!SetSystemCursor(cursor, cursorId)) {
            DestroyCursor(cursor);
            restoreSystemCursor();
            return Status::unavailable("failed to replace Windows system cursor");
        }
    }
    hidden_ = true;
    return Status::ok();
#else
    return Status::ok();
#endif
}

Status CursorSuppressor::restoreSystemCursor() {
#if defined(_WIN32)
    if (!SystemParametersInfoW(SPI_SETCURSORS, 0, nullptr, 0)) {
        return Status::unavailable("failed to restore Windows system cursors");
    }
    hidden_ = false;
#endif
    return Status::ok();
}

CursorSuppressor::~CursorSuppressor() {
    (void)restoreSystemCursor();
}

}  // namespace led::host
