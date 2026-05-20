#include "led/transport/udp_socket.h"

#include <cstring>
#include <utility>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace led::transport {

namespace {

#if defined(_WIN32)
using Socklen = int;
#else
using Socklen = socklen_t;
#endif

#if defined(_WIN32)
class WinsockRuntime {
public:
    WinsockRuntime() {
        WSADATA data{};
        WSAStartup(MAKEWORD(2, 2), &data);
    }

    ~WinsockRuntime() {
        WSACleanup();
    }
};

void ensureWinsock() {
    static WinsockRuntime runtime;
    (void)runtime;
}

std::string lastSocketError(const char* operation) {
    return std::string(operation) + " failed with WSA error " + std::to_string(WSAGetLastError());
}
#else
void ensureWinsock() {}

std::string lastSocketError(const char* operation) {
    return std::string(operation) + " failed: " + std::strerror(errno);
}
#endif

Status fillSockaddr(const UdpEndpoint& endpoint, sockaddr_in& address) {
    std::memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(endpoint.port);
    if (inet_pton(AF_INET, endpoint.address.c_str(), &address.sin_addr) != 1) {
        return Status::invalidArgument("invalid IPv4 address: " + endpoint.address);
    }
    return Status::ok();
}

std::string sockaddrToString(const sockaddr_in& address) {
    char buffer[INET_ADDRSTRLEN]{};
    const auto* result = inet_ntop(AF_INET, &address.sin_addr, buffer, sizeof(buffer));
    return result != nullptr ? std::string(result) : std::string("0.0.0.0");
}

}  // namespace

UdpSocket::~UdpSocket() {
    close();
}

UdpSocket::UdpSocket(UdpSocket&& other) noexcept
    : socket_(std::exchange(other.socket_, kInvalidSocket)),
      localPort_(std::exchange(other.localPort_, std::uint16_t{0})) {}

UdpSocket& UdpSocket::operator=(UdpSocket&& other) noexcept {
    if (this != &other) {
        close();
        socket_ = std::exchange(other.socket_, kInvalidSocket);
        localPort_ = std::exchange(other.localPort_, std::uint16_t{0});
    }
    return *this;
}

Status UdpSocket::open() {
    if (isOpen()) {
        return Status::ok();
    }

    ensureWinsock();
    const auto created = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
#if defined(_WIN32)
    if (created == INVALID_SOCKET) {
        return Status::unavailable(lastSocketError("socket"));
    }
    socket_ = static_cast<NativeSocket>(created);
#else
    if (created < 0) {
        return Status::unavailable(lastSocketError("socket"));
    }
    socket_ = created;
#endif
    return Status::ok();
}

Status UdpSocket::bind(std::uint16_t port, const std::string& address) {
    auto status = open();
    if (!status.isOk()) {
        return status;
    }

    sockaddr_in bindAddress{};
    status = fillSockaddr(UdpEndpoint{address, port}, bindAddress);
    if (!status.isOk()) {
        return status;
    }

#if defined(_WIN32)
    const auto nativeSocket = static_cast<SOCKET>(socket_);
#else
    const auto nativeSocket = socket_;
#endif

    if (::bind(nativeSocket, reinterpret_cast<sockaddr*>(&bindAddress), sizeof(bindAddress)) != 0) {
        return Status::unavailable(lastSocketError("bind"));
    }

    sockaddr_in actualAddress{};
    Socklen actualLength = sizeof(actualAddress);
    if (::getsockname(nativeSocket, reinterpret_cast<sockaddr*>(&actualAddress), &actualLength) == 0) {
        localPort_ = ntohs(actualAddress.sin_port);
    } else {
        localPort_ = port;
    }
    return Status::ok();
}

Status UdpSocket::setReceiveTimeoutMs(std::uint32_t timeoutMs) {
    if (!isOpen()) {
        return Status::invalidState("cannot set timeout on a closed UDP socket");
    }

#if defined(_WIN32)
    const auto nativeSocket = static_cast<SOCKET>(socket_);
    const DWORD timeout = timeoutMs;
    if (setsockopt(nativeSocket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout)) != 0) {
        return Status::unavailable(lastSocketError("setsockopt"));
    }
#else
    timeval timeout{};
    timeout.tv_sec = static_cast<time_t>(timeoutMs / 1000);
    timeout.tv_usec = static_cast<suseconds_t>((timeoutMs % 1000) * 1000);
    if (setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0) {
        return Status::unavailable(lastSocketError("setsockopt"));
    }
#endif
    return Status::ok();
}

Status UdpSocket::sendTo(const std::vector<std::uint8_t>& bytes, const UdpEndpoint& endpoint) const {
    if (!isOpen()) {
        return Status::invalidState("cannot send on a closed UDP socket");
    }

    sockaddr_in target{};
    auto status = fillSockaddr(endpoint, target);
    if (!status.isOk()) {
        return status;
    }

    const auto sent = ::sendto(
#if defined(_WIN32)
        static_cast<SOCKET>(socket_),
#else
        socket_,
#endif
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<int>(bytes.size()),
        0,
        reinterpret_cast<const sockaddr*>(&target),
        sizeof(target));
    if (sent < 0 || static_cast<std::size_t>(sent) != bytes.size()) {
        return Status::unavailable(lastSocketError("sendto"));
    }
    return Status::ok();
}

Status UdpSocket::receiveFrom(std::size_t maxBytes, std::vector<std::uint8_t>& bytes, UdpEndpoint& endpoint) const {
    if (!isOpen()) {
        return Status::invalidState("cannot receive on a closed UDP socket");
    }

    bytes.assign(maxBytes, 0);
    sockaddr_in source{};
    Socklen sourceLength = sizeof(source);
    const auto received = ::recvfrom(
#if defined(_WIN32)
        static_cast<SOCKET>(socket_),
#else
        socket_,
#endif
        reinterpret_cast<char*>(bytes.data()),
        static_cast<int>(bytes.size()),
        0,
        reinterpret_cast<sockaddr*>(&source),
        &sourceLength);
    if (received < 0) {
        bytes.clear();
        return Status::unavailable(lastSocketError("recvfrom"));
    }

    bytes.resize(static_cast<std::size_t>(received));
    endpoint.address = sockaddrToString(source);
    endpoint.port = ntohs(source.sin_port);
    return Status::ok();
}

void UdpSocket::close() {
    if (!isOpen()) {
        return;
    }

#if defined(_WIN32)
    closesocket(static_cast<SOCKET>(socket_));
#else
    ::close(socket_);
#endif
    socket_ = kInvalidSocket;
    localPort_ = 0;
}

bool UdpSocket::isOpen() const {
    return socket_ != kInvalidSocket;
}

std::uint16_t UdpSocket::localPort() const {
    return localPort_;
}

}  // namespace led::transport
