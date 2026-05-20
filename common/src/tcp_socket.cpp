#include "led/transport/tcp_socket.h"

#include <array>
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
#include <unistd.h>
#endif

namespace led::transport {

namespace {

#if defined(_WIN32)
using Socklen = int;

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

void ensureSockets() {
    static WinsockRuntime runtime;
    (void)runtime;
}

std::string lastSocketError(const char* operation) {
    return std::string(operation) + " failed with WSA error " + std::to_string(WSAGetLastError());
}
#else
using Socklen = socklen_t;

void ensureSockets() {}

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

UdpEndpoint endpointFromSockaddr(const sockaddr_in& address) {
    std::array<char, INET_ADDRSTRLEN> buffer{};
    const auto* result = inet_ntop(AF_INET, &address.sin_addr, buffer.data(), static_cast<Socklen>(buffer.size()));
    return UdpEndpoint{
        result != nullptr ? std::string(result) : std::string("0.0.0.0"),
        ntohs(address.sin_port),
    };
}

#if defined(_WIN32)
void closeNativeSocket(std::uintptr_t socket) {
    closesocket(static_cast<SOCKET>(socket));
}
#else
void closeNativeSocket(int socket) {
    ::close(socket);
}
#endif

}  // namespace

TcpStream::~TcpStream() {
    close();
}

TcpStream::TcpStream(TcpStream&& other) noexcept
    : socket_(std::exchange(other.socket_, kInvalidSocket)),
      peerEndpoint_(std::exchange(other.peerEndpoint_, UdpEndpoint{})) {}

TcpStream& TcpStream::operator=(TcpStream&& other) noexcept {
    if (this != &other) {
        close();
        socket_ = std::exchange(other.socket_, kInvalidSocket);
        peerEndpoint_ = std::exchange(other.peerEndpoint_, UdpEndpoint{});
    }
    return *this;
}

TcpStream::TcpStream(NativeSocket socket, UdpEndpoint peerEndpoint)
    : socket_(socket), peerEndpoint_(std::move(peerEndpoint)) {}

Status TcpStream::connect(const UdpEndpoint& endpoint) {
    close();
    ensureSockets();

    const auto created = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#if defined(_WIN32)
    if (created == INVALID_SOCKET) {
        return Status::unavailable(lastSocketError("socket"));
    }
    socket_ = static_cast<NativeSocket>(created);
    const auto nativeSocket = static_cast<SOCKET>(socket_);
#else
    if (created < 0) {
        return Status::unavailable(lastSocketError("socket"));
    }
    socket_ = created;
    const auto nativeSocket = socket_;
#endif

    sockaddr_in target{};
    auto status = fillSockaddr(endpoint, target);
    if (!status.isOk()) {
        close();
        return status;
    }

    if (::connect(nativeSocket, reinterpret_cast<sockaddr*>(&target), sizeof(target)) != 0) {
        const auto error = lastSocketError("connect");
        close();
        return Status::unavailable(error);
    }
    peerEndpoint_ = endpoint;
    return Status::ok();
}

Status TcpStream::sendLine(const std::string& line) const {
    if (!isOpen()) {
        return Status::invalidState("cannot send on a closed TCP stream");
    }

    std::string withNewline = line;
    withNewline.push_back('\n');
    std::size_t sentTotal = 0;
    while (sentTotal < withNewline.size()) {
#if defined(_WIN32)
        const auto nativeSocket = static_cast<SOCKET>(socket_);
#else
        const auto nativeSocket = socket_;
#endif
        const auto sent = ::send(
            nativeSocket,
            withNewline.data() + static_cast<std::ptrdiff_t>(sentTotal),
            static_cast<int>(withNewline.size() - sentTotal),
            0);
        if (sent <= 0) {
            return Status::unavailable(lastSocketError("send"));
        }
        sentTotal += static_cast<std::size_t>(sent);
    }
    return Status::ok();
}

Status TcpStream::readLine(std::string& line, std::size_t maxBytes) const {
    if (!isOpen()) {
        return Status::invalidState("cannot read from a closed TCP stream");
    }

    line.clear();
    while (line.size() < maxBytes) {
        char ch = '\0';
#if defined(_WIN32)
        const auto nativeSocket = static_cast<SOCKET>(socket_);
#else
        const auto nativeSocket = socket_;
#endif
        const auto received = ::recv(nativeSocket, &ch, 1, 0);
        if (received <= 0) {
            return Status::unavailable(lastSocketError("recv"));
        }
        if (ch == '\n') {
            return Status::ok();
        }
        if (ch != '\r') {
            line.push_back(ch);
        }
    }
    return Status::invalidArgument("TCP line exceeded maximum length");
}

void TcpStream::close() {
    if (!isOpen()) {
        return;
    }
    closeNativeSocket(socket_);
    socket_ = kInvalidSocket;
    peerEndpoint_ = {};
}

bool TcpStream::isOpen() const {
    return socket_ != kInvalidSocket;
}

const UdpEndpoint& TcpStream::peerEndpoint() const {
    return peerEndpoint_;
}

TcpListener::~TcpListener() {
    close();
}

Status TcpListener::bindAndListen(std::uint16_t port, const std::string& address, int backlog) {
    close();
    ensureSockets();

    const auto created = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#if defined(_WIN32)
    if (created == INVALID_SOCKET) {
        return Status::unavailable(lastSocketError("socket"));
    }
    socket_ = static_cast<NativeSocket>(created);
    const auto nativeSocket = static_cast<SOCKET>(socket_);
#else
    if (created < 0) {
        return Status::unavailable(lastSocketError("socket"));
    }
    socket_ = created;
    const auto nativeSocket = socket_;
#endif

    sockaddr_in bindAddress{};
    auto status = fillSockaddr(UdpEndpoint{address, port}, bindAddress);
    if (!status.isOk()) {
        close();
        return status;
    }

    int reuse = 1;
    setsockopt(nativeSocket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    if (::bind(nativeSocket, reinterpret_cast<sockaddr*>(&bindAddress), sizeof(bindAddress)) != 0) {
        const auto error = lastSocketError("bind");
        close();
        return Status::unavailable(error);
    }

    if (::listen(nativeSocket, backlog) != 0) {
        const auto error = lastSocketError("listen");
        close();
        return Status::unavailable(error);
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

Status TcpListener::accept(TcpStream& stream) const {
    if (!isOpen()) {
        return Status::invalidState("cannot accept on a closed TCP listener");
    }

#if defined(_WIN32)
    const auto nativeSocket = static_cast<SOCKET>(socket_);
    sockaddr_in peerAddress{};
    Socklen peerLength = sizeof(peerAddress);
    const auto accepted = ::accept(nativeSocket, reinterpret_cast<sockaddr*>(&peerAddress), &peerLength);
    if (accepted == INVALID_SOCKET) {
        return Status::unavailable(lastSocketError("accept"));
    }
    stream = TcpStream(static_cast<TcpStream::NativeSocket>(accepted), endpointFromSockaddr(peerAddress));
#else
    sockaddr_in peerAddress{};
    Socklen peerLength = sizeof(peerAddress);
    const auto accepted = ::accept(socket_, reinterpret_cast<sockaddr*>(&peerAddress), &peerLength);
    if (accepted < 0) {
        return Status::unavailable(lastSocketError("accept"));
    }
    stream = TcpStream(accepted, endpointFromSockaddr(peerAddress));
#endif
    return Status::ok();
}

void TcpListener::close() {
    if (!isOpen()) {
        return;
    }
    closeNativeSocket(socket_);
    socket_ = kInvalidSocket;
    localPort_ = 0;
}

bool TcpListener::isOpen() const {
    return socket_ != kInvalidSocket;
}

std::uint16_t TcpListener::localPort() const {
    return localPort_;
}

}  // namespace led::transport
