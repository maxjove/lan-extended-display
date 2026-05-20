#include "led/transport/h264_rtp_packetizer.h"

#include <cstdint>
#include <iostream>
#include <vector>

int main() {
    led::transport::H264RtpPacketizer packetizer(0x11223344, 100);

    std::vector<std::uint8_t> nal(3000, 0x55);
    nal.front() = 0x65;

    const auto packets = packetizer.packetizeNal(nal, 90000, 1200);
    if (packets.size() != 3) {
        std::cerr << "expected 3 RTP packets, got " << packets.size() << '\n';
        return 1;
    }

    if (!packets.back().marker) {
        std::cerr << "last packet must carry RTP marker bit\n";
        return 1;
    }

    if (packets.front().payload.size() < 2 || packets.front().payload[0] != 0x7C) {
        std::cerr << "first packet is not FU-A for IDR NAL\n";
        return 1;
    }

    std::cout << "packetized " << nal.size() << " bytes into "
              << packets.size() << " RTP packets\n";
    return 0;
}
