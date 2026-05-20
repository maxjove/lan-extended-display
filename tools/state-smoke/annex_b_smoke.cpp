#include "led/transport/h264_annex_b.h"

#include <cstdint>
#include <iostream>
#include <vector>

int main() {
    const std::vector<std::uint8_t> bytes{
        0x00, 0x00, 0x00, 0x01, 0x67, 0x11, 0x22,
        0x00, 0x00, 0x01, 0x68, 0x33,
        0x00, 0x00, 0x00, 0x01, 0x65, 0x44, 0x55, 0x66,
    };

    const auto units = led::transport::splitAnnexB(bytes);
    if (units.size() != 3) {
        std::cerr << "expected 3 NAL units, got " << units.size() << '\n';
        return 1;
    }
    if (led::transport::h264NalType(units[0].bytes) != 7 ||
        led::transport::h264NalType(units[1].bytes) != 8 ||
        led::transport::h264NalType(units[2].bytes) != 5) {
        std::cerr << "unexpected NAL type sequence\n";
        return 1;
    }

    std::cout << "Annex B split NAL units=" << units.size() << '\n';
    return 0;
}
