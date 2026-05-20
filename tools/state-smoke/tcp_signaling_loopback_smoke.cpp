#include "led/protocol/control_messages.h"
#include "led/transport/tcp_socket.h"

#include <iostream>
#include <string>
#include <thread>

int main() {
    led::transport::TcpListener listener;
    auto status = listener.bindAndListen(0, "127.0.0.1");
    if (!status.isOk()) {
        std::cerr << status.message() << '\n';
        return 1;
    }

    std::string serverError;
    std::thread server([&listener, &serverError]() {
        led::transport::TcpStream stream;
        auto status = listener.accept(stream);
        if (!status.isOk()) {
            serverError = status.message();
            return;
        }

        std::string line;
        status = stream.readLine(line);
        if (!status.isOk()) {
            serverError = status.message();
            return;
        }

        led::protocol::ClientHello hello;
        status = led::protocol::parseClientHello(line, hello);
        if (!status.isOk()) {
            serverError = status.message();
            return;
        }

        led::protocol::SessionOffer offer;
        offer.videoMode = led::protocol::defaultStandardMode();
        offer.rtpPort = 17670;
        offer.sessionToken = "local-token";
        status = stream.sendLine(led::protocol::serializeSessionOffer(offer));
        if (!status.isOk()) {
            serverError = status.message();
        }
    });

    led::transport::TcpStream client;
    status = client.connect(led::transport::UdpEndpoint{"127.0.0.1", listener.localPort()});
    if (!status.isOk()) {
        std::cerr << status.message() << '\n';
        server.join();
        return 1;
    }

    led::protocol::ClientHello hello;
    hello.device.deviceId = "client-001";
    hello.device.name = "arm64-client";
    hello.device.platform = "linux-arm64";
    hello.device.version = "0.1.0";

    status = client.sendLine(led::protocol::serializeClientHello(hello));
    if (!status.isOk()) {
        std::cerr << status.message() << '\n';
        server.join();
        return 1;
    }

    std::string line;
    status = client.readLine(line);
    if (!status.isOk()) {
        std::cerr << status.message() << '\n';
        server.join();
        return 1;
    }

    server.join();
    if (!serverError.empty()) {
        std::cerr << serverError << '\n';
        return 1;
    }

    led::protocol::SessionOffer offer;
    status = led::protocol::parseSessionOffer(line, offer);
    if (!status.isOk()) {
        std::cerr << status.message() << '\n';
        return 1;
    }

    if (offer.videoMode.resolution.width != 1920 ||
        offer.videoMode.resolution.height != 1080 ||
        offer.videoMode.codec != led::protocol::Codec::h264 ||
        offer.rtpPort != 17670 ||
        offer.sessionToken != "local-token") {
        std::cerr << "parsed session offer mismatch\n";
        return 1;
    }

    std::cout << "TCP signaling loopback negotiated "
              << offer.videoMode.resolution.width << 'x'
              << offer.videoMode.resolution.height << '@'
              << offer.videoMode.resolution.refreshRate
              << " RTP:" << offer.rtpPort << '\n';
    return 0;
}
