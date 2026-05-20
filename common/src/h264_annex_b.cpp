#include "led/transport/h264_annex_b.h"

#include <fstream>
#include <string>
#include <utility>

namespace led::transport {

namespace {

std::size_t startCodeLengthAt(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    if (offset + 3 <= bytes.size() &&
        bytes[offset] == 0x00 &&
        bytes[offset + 1] == 0x00 &&
        bytes[offset + 2] == 0x01) {
        return 3;
    }

    if (offset + 4 <= bytes.size() &&
        bytes[offset] == 0x00 &&
        bytes[offset + 1] == 0x00 &&
        bytes[offset + 2] == 0x00 &&
        bytes[offset + 3] == 0x01) {
        return 4;
    }

    return 0;
}

std::size_t findStartCode(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    while (offset + 3 <= bytes.size()) {
        if (startCodeLengthAt(bytes, offset) != 0) {
            return offset;
        }
        ++offset;
    }
    return bytes.size();
}

}  // namespace

std::vector<AnnexBNalUnit> splitAnnexB(const std::vector<std::uint8_t>& bytes) {
    std::vector<AnnexBNalUnit> units;
    auto start = findStartCode(bytes, 0);
    while (start < bytes.size()) {
        const auto prefixLength = startCodeLengthAt(bytes, start);
        const auto nalStart = start + prefixLength;
        const auto nextStart = findStartCode(bytes, nalStart);
        if (nalStart < nextStart) {
            AnnexBNalUnit unit;
            unit.offset = nalStart;
            unit.bytes.assign(
                bytes.begin() + static_cast<std::ptrdiff_t>(nalStart),
                bytes.begin() + static_cast<std::ptrdiff_t>(nextStart));
            units.push_back(std::move(unit));
        }
        start = nextStart;
    }
    return units;
}

Status readBinaryFile(const char* path, std::vector<std::uint8_t>& bytes) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return Status::unavailable(std::string("failed to open file: ") + path);
    }

    file.seekg(0, std::ios::end);
    const auto size = file.tellg();
    if (size < 0) {
        return Status::unavailable(std::string("failed to get file size: ") + path);
    }
    file.seekg(0, std::ios::beg);

    bytes.assign(static_cast<std::size_t>(size), 0);
    if (!bytes.empty()) {
        file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!file) {
            return Status::unavailable(std::string("failed to read file: ") + path);
        }
    }
    return Status::ok();
}

unsigned int h264NalType(const std::vector<std::uint8_t>& nalUnit) {
    if (nalUnit.empty()) {
        return 0;
    }
    return static_cast<unsigned int>(nalUnit.front() & 0x1F);
}

}  // namespace led::transport
