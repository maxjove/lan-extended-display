#include "led/host/display_manager.h"

#include "led/common/logger.h"

#include <algorithm>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

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

#if defined(_WIN32)
namespace {

bool textContains(const std::string& text, const char* needle) {
    return text.find(needle) != std::string::npos;
}

bool isLedVirtualDisplayDevice(const char* deviceName) {
    DISPLAY_DEVICEA adapter{};
    adapter.cb = sizeof(adapter);
    if (EnumDisplayDevicesA(deviceName, 0, &adapter, 0)) {
        const std::string text =
            std::string(adapter.DeviceName) + " " +
            adapter.DeviceString + " " +
            adapter.DeviceID + " " +
            adapter.DeviceKey;
        if (textContains(text, "LED LAN Virtual Display") || textContains(text, "ROOT\\DISPLAY")) {
            return textContains(text, "LED LAN Virtual Display");
        }
    }

    for (DWORD index = 0;; ++index) {
        DISPLAY_DEVICEA display{};
        display.cb = sizeof(display);
        if (!EnumDisplayDevicesA(nullptr, index, &display, 0)) {
            break;
        }
        if (std::string(display.DeviceName) != deviceName) {
            continue;
        }
        const std::string text =
            std::string(display.DeviceString) + " " +
            display.DeviceID + " " +
            display.DeviceKey;
        return textContains(text, "LED LAN Virtual Display");
    }
    return false;
}

BOOL CALLBACK collectMonitorInfo(HMONITOR monitor, HDC, LPRECT, LPARAM userData) {
    auto* displays = reinterpret_cast<std::vector<PhysicalDisplayInfo>*>(userData);
    MONITORINFOEXA info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoA(monitor, &info)) {
        return TRUE;
    }

    const auto width = std::max(0L, info.rcMonitor.right - info.rcMonitor.left);
    const auto height = std::max(0L, info.rcMonitor.bottom - info.rcMonitor.top);
    displays->push_back(PhysicalDisplayInfo{
        info.szDevice,
        info.rcMonitor.left,
        info.rcMonitor.top,
        static_cast<std::uint32_t>(width),
        static_cast<std::uint32_t>(height),
        (info.dwFlags & MONITORINFOF_PRIMARY) != 0,
        isLedVirtualDisplayDevice(info.szDevice),
    });
    return TRUE;
}

}  // namespace
#endif

Status DisplayManager::createVirtualDisplay(const protocol::Resolution& resolution) {
    for (int attempt = 0; attempt < 50; ++attempt) {
        const auto displays = enumerateDisplays();
        auto best = displays.end();
        for (auto it = displays.begin(); it != displays.end(); ++it) {
            if (!it->ledVirtual || it->primary || it->width != resolution.width || it->height != resolution.height) {
                continue;
            }
            if (best == displays.end() || it->originX > best->originX ||
                (it->originX == best->originX && it->originY > best->originY)) {
                best = it;
            }
        }

        if (best != displays.end()) {
            virtualDisplay_.deviceName = best->deviceName;
            virtualDisplay_.resolution = resolution;
            virtualDisplay_.originX = best->originX;
            virtualDisplay_.originY = best->originY;
            virtualDisplay_.dpiScale = 1.0;
            virtualDisplay_.active = true;
            logInfo(
                "host virtual display attached to active monitor " + virtualDisplay_.deviceName +
                " at " + std::to_string(virtualDisplay_.originX) + "," +
                std::to_string(virtualDisplay_.originY));
            return Status::ok();
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    return Status::unavailable(
        "LED IddCx virtual monitor was not found in the active Windows display topology");
}

Status DisplayManager::destroyVirtualDisplay() {
    virtualDisplay_.active = false;
    logInfo("host virtual display detached");
    return Status::ok();
}

Status DisplayManager::restoreDisplayLayout() {
    logInfo("host display layout restore completed");
    return Status::ok();
}

const VirtualDisplayInfo& DisplayManager::virtualDisplay() const {
    return virtualDisplay_;
}

DesktopBounds DisplayManager::currentDesktopBounds() {
#if defined(_WIN32)
    return DesktopBounds{
        GetSystemMetrics(SM_XVIRTUALSCREEN),
        GetSystemMetrics(SM_YVIRTUALSCREEN),
        GetSystemMetrics(SM_CXVIRTUALSCREEN),
        GetSystemMetrics(SM_CYVIRTUALSCREEN),
    };
#else
    return DesktopBounds{0, 0, 1920, 1080};
#endif
}

std::vector<PhysicalDisplayInfo> DisplayManager::enumerateDisplays() {
    std::vector<PhysicalDisplayInfo> displays;
#if defined(_WIN32)
    EnumDisplayMonitors(nullptr, nullptr, collectMonitorInfo, reinterpret_cast<LPARAM>(&displays));
#else
    displays.push_back(PhysicalDisplayInfo{"fallback", 0, 0, 1920, 1080, true, false});
#endif
    return displays;
}

}  // namespace led::host
