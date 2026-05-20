#include "led/client/discovery.h"

namespace led::client {

std::vector<protocol::DeviceInfo> Discovery::scanHosts() const {
    protocol::DeviceInfo placeholder;
    placeholder.deviceId = "manual-host";
    placeholder.name = "Manual Windows Host";
    placeholder.platform = "windows";
    placeholder.version = "0.1.0";
    placeholder.supportedVideoModes = protocol::defaultVideoModes();
    return {placeholder};
}

}  // namespace led::client
