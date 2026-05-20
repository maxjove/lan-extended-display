#include "led/transport/h264_rtp_depacketizer.h"
#include "led/transport/h264_rtp_packetizer.h"
#include "led/transport/rtp_codec.h"
#include "led/transport/udp_socket.h"

#include <cstdint>
#include <iostream>
#include <utility>
#include <vector>

int main() {
    led::transport::UdpSocket receiver;
    auto status = receiver.bind(0, "127.0.0.1");
    if (!status.isOk()) {
        std::cerr << status.message() << '\n';
        return 1;
    }

    status = receiver.setReceiveTimeoutMs(1000);
    if (!status.isOk()) {
        std::cerr << status.message() << '\n';
        return 1;
    }

    led::transport::UdpSocket sender;
    status = sender.open();
    if (!status.isOk()) {
        std::cerr << status.message() << '\n';
        return 1;
    }

    led::transport::H264RtpPacketizer packetizer(0x12345678, 42);
    std::vector<std::uint8_t> nal(3000, 0x33);
    nal.front() = 0x65;
    const auto packets = packetizer.packetizeNal(nal, 90000, 1200);
    if (packets.size() < 2) {
        std::cerr << "expected fragmented RTP packets for large NAL\n";
        return 1;
    }

    for (const auto& packet : packets) {
        const auto bytes = led::transport::serializeRtpPacket(packet);
        status = sender.sendTo(bytes, led::transport::UdpEndpoint{"127.0.0.1", receiver.localPort()});
        if (!status.isOk()) {
            std::cerr << status.message() << '\n';
            return 1;
        }
    }

    led::transport::H264RtpDepacketizer depacketizer;
    led::transport::UdpEndpoint source;
    std::vector<std::uint8_t> reassembled;
    while (reassembled.empty()) {
        std::vector<std::uint8_t> received;
        status = receiver.receiveFrom(2048, received, source);
        if (!status.isOk()) {
            std::cerr << status.message() << '\n';
            return 1;
        }

        led::transport::RtpPacket parsed;
        status = led::transport::parseRtpPacket(received, parsed);
        if (!status.isOk()) {
            std::cerr << status.message() << '\n';
            return 1;
        }

        led::transport::ReassembledNal nalOut;
        status = depacketizer.pushPacket(parsed, nalOut);
        if (!status.isOk()) {
            std::cerr << status.message() << '\n';
            return 1;
        }

        reassembled = std::move(nalOut.nalUnit);
    }

    if (reassembled != nal) {
        std::cerr << "reassembled NAL does not match original\n";
        return 1;
    }

    std::cout << "UDP/RTP loopback reassembled " << reassembled.size()
              << " payload bytes from " << source.address << ':' << source.port << '\n';
    return 0;
}
