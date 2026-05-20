#pragma once

#include "led/common/status.h"
#include "led/protocol/messages.h"

#include <cstdint>
#include <string>
#include <vector>

namespace led::host {

struct DesktopBounds {
    int left{0};
    int top{0};
    int width{0};
    int height{0};
};

struct PhysicalDisplayInfo {
    std::string deviceName;
    int originX{0};
    int originY{0};
    std::uint32_t width{0};
    std::uint32_t height{0};
    bool primary{false};
    bool ledVirtual{false};
};

struct VirtualDisplayInfo {
    std::string deviceName;
    protocol::Resolution resolution{};
    int originX{0};
    int originY{0};
    double dpiScale{1.0};
    bool active{false};
};

class DisplayManager {
public:
    Status createVirtualDisplay(const protocol::Resolution& resolution);
    Status destroyVirtualDisplay();
    Status restoreDisplayLayout();

    [[nodiscard]] const VirtualDisplayInfo& virtualDisplay() const;
    [[nodiscard]] static DesktopBounds currentDesktopBounds();
    [[nodiscard]] static std::vector<PhysicalDisplayInfo> enumerateDisplays();

private:
    VirtualDisplayInfo virtualDisplay_{};
};

}  // namespace led::host
