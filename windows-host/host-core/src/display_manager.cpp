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
    });
    return TRUE;
}

}  // namespace
#endif

Status DisplayManager::createVirtualDisplay(const protocol::Resolution& resolution) {
    for (int attempt = 0; attempt < 20; ++attempt) {
        const auto displays = enumerateDisplays();
        auto best = displays.end();
        for (auto it = displays.begin(); it != displays.end(); ++it) {
            if (it->primary || it->width != resolution.width || it->height != resolution.height) {
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

    const auto bounds = currentDesktopBounds();
    virtualDisplay_.deviceName = "placeholder";
    virtualDisplay_.resolution = resolution;
    virtualDisplay_.originX = bounds.left + bounds.width;
    virtualDisplay_.originY = bounds.top;
    virtualDisplay_.dpiScale = 1.0;
    virtualDisplay_.active = true;
    logInfo(
        "host virtual display placeholder activated at " +
        std::to_string(virtualDisplay_.originX) + "," +
        std::to_string(virtualDisplay_.originY) +
        "; LED IddCx monitor was not found");
    return Status::ok();
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
    displays.push_back(PhysicalDisplayInfo{"fallback", 0, 0, 1920, 1080, true});
#endif
    return displays;
}

}  // namespace led::host
