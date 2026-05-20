#pragma once

#include "led/common/status.h"

#include <cstdint>
#include <vector>

namespace led::transport {

struct AnnexBNalUnit {
    std::size_t offset{0};
    std::vector<std::uint8_t> bytes;
};

[[nodiscard]] std::vector<AnnexBNalUnit> splitAnnexB(const std::vector<std::uint8_t>& bytes);
[[nodiscard]] Status readBinaryFile(const char* path, std::vector<std::uint8_t>& bytes);
[[nodiscard]] unsigned int h264NalType(const std::vector<std::uint8_t>& nalUnit);

}  // namespace led::transport
