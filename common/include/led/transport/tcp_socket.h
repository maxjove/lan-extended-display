#pragma once

#include "led/common/status.h"
#include "led/transport/udp_socket.h"

#include <cstdint>
#include <string>

namespace led::transport {

class TcpStream {
public:
    TcpStream() = default;
    ~TcpStream();

    TcpStream(const TcpStream&) = delete;
    TcpStream& operator=(const TcpStream&) = delete;

    TcpStream(TcpStream&& other) noexcept;
    TcpStream& operator=(TcpStream&& other) noexcept;

    Status connect(const UdpEndpoint& endpoint);
    Status sendLine(const std::string& line) const;
    Status readLine(std::string& line, std::size_t maxBytes = 4096) const;
    void close();

    [[nodiscard]] bool isOpen() const;
    [[nodiscard]] const UdpEndpoint& peerEndpoint() const;

private:
    friend class TcpListener;

#if defined(_WIN32)
    using NativeSocket = std::uintptr_t;
    static constexpr NativeSocket kInvalidSocket = ~NativeSocket{0};
#else
    using NativeSocket = int;
    static constexpr NativeSocket kInvalidSocket = -1;
#endif

    explicit TcpStream(NativeSocket socket, UdpEndpoint peerEndpoint = {});

    NativeSocket socket_{kInvalidSocket};
    UdpEndpoint peerEndpoint_{};
};

class TcpListener {
public:
    TcpListener() = default;
    ~TcpListener();

    TcpListener(const TcpListener&) = delete;
    TcpListener& operator=(const TcpListener&) = delete;

    Status bindAndListen(std::uint16_t port, const std::string& address = "0.0.0.0", int backlog = 1);
    Status setAcceptTimeoutMs(int timeoutMs);
    Status accept(TcpStream& stream) const;
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
    int acceptTimeoutMs_{0};
};

}  // namespace led::transport
