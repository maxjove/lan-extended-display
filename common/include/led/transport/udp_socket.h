#pragma once

#include "led/common/status.h"

#include <cstdint>
#include <string>
#include <vector>

namespace led::transport {

struct UdpEndpoint {
    std::string address{"127.0.0.1"};
    std::uint16_t port{0};
};

class UdpSocket {
public:
    UdpSocket() = default;
    ~UdpSocket();

    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    UdpSocket(UdpSocket&& other) noexcept;
    UdpSocket& operator=(UdpSocket&& other) noexcept;

    Status open();
    Status bind(std::uint16_t port, const std::string& address = "0.0.0.0");
    Status setReceiveTimeoutMs(std::uint32_t timeoutMs);
    Status setSendBufferBytes(std::uint32_t bytes);
    Status sendTo(const std::vector<std::uint8_t>& bytes, const UdpEndpoint& endpoint) const;
    Status receiveFrom(std::size_t maxBytes, std::vector<std::uint8_t>& bytes, UdpEndpoint& endpoint) const;
    void close();

    [[nodiscard]] bool isOpen() const;
    [[nodiscard]] std::uint16_t localPort() const;

private:
#if defined(_WIN32)
    using NativeSocket = std::uintptr_t;
    static constexpr NativeSocket kInvalidSocket = ~NativeSocket{0};
#else
    using NativeSocket = int;
    static constexpr NativeSocket kInvalidSocket = -1;
#endif

    NativeSocket socket_{kInvalidSocket};
    std::uint16_t localPort_{0};
};

}  // namespace led::transport
