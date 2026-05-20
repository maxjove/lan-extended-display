#pragma once

#include "led/protocol/messages.h"

#include <vector>

namespace led::client {

class Discovery {
public:
    [[nodiscard]] std::vector<protocol::DeviceInfo> scanHosts() const;
};

}  // namespace led::client
