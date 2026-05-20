#include "led/host/encoder.h"

#include "led/common/logger.h"
#include "led/transport/h264_annex_b.h"

#include <algorithm>
#include <cstring>
#include <string>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <codecapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <objbase.h>
#include <wmcodecdsp.h>
#include <wrl/client.h>
#endif

namespace led::host {

namespace {

#if defined(_WIN32)
std::string hresultText(HRESULT hr, const char* operation) {
    return std::string(operation) + " failed with HRESULT 0x" + std::to_string(static_cast<unsigned long>(hr));
}

void setVideoTypeCommon(IMFMediaType* type, const protocol::VideoMode& mode, const GUID& subtype) {
    type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    type->SetGUID(MF_MT_SUBTYPE, subtype);
    MFSetAttributeSize(type, MF_MT_FRAME_SIZE, mode.resolution.width, mode.resolution.height);
    MFSetAttributeRatio(type, MF_MT_FRAME_RATE, mode.resolution.refreshRate == 0 ? 60 : mode.resolution.refreshRate, 1);
    MFSetAttributeRatio(type, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
}

std::uint8_t clampByte(int value) {
    return static_cast<std::uint8_t>(std::max(0, std::min(255, value)));
}

std::uint8_t rgbToY(std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    return clampByte(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
}

std::uint8_t rgbToU(std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    return clampByte(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
}

std::uint8_t rgbToV(std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    return clampByte(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
}

std::vector<std::uint8_t> bgraToNv12(const CapturedFrame& frame) {
    const auto width = static_cast<std::size_t>(frame.width);
    const auto height = static_cast<std::size_t>(frame.height);
    std::vector<std::uint8_t> nv12(width * height + width * ((height + 1) / 2), 128);
    auto* yPlane = nv12.data();
    auto* uvPlane = nv12.data() + width * height;

    for (std::size_t y = 0; y < height; y += 2) {
        const auto* row0 = frame.bgra.data() + y * width * 4;
        const auto* row1 = y + 1 < height ? row0 + width * 4 : row0;
        auto* yRow0 = yPlane + y * width;
        auto* yRow1 = y + 1 < height ? yRow0 + width : yRow0;
        auto* uvRow = uvPlane + (y / 2) * width;
        for (std::size_t x = 0; x < width; x += 2) {
            const auto x1 = x + 1 < width ? x + 1 : x;
            const auto* p00 = row0 + x * 4;
            const auto* p01 = row0 + x1 * 4;
            const auto* p10 = row1 + x * 4;
            const auto* p11 = row1 + x1 * 4;

            const int b00 = p00[0];
            const int g00 = p00[1];
            const int r00 = p00[2];
            const int b01 = p01[0];
            const int g01 = p01[1];
            const int r01 = p01[2];
            const int b10 = p10[0];
            const int g10 = p10[1];
            const int r10 = p10[2];
            const int b11 = p11[0];
            const int g11 = p11[1];
            const int r11 = p11[2];

            yRow0[x] = rgbToY(static_cast<std::uint8_t>(r00), static_cast<std::uint8_t>(g00), static_cast<std::uint8_t>(b00));
            if (x + 1 < width) {
                yRow0[x + 1] = rgbToY(static_cast<std::uint8_t>(r01), static_cast<std::uint8_t>(g01), static_cast<std::uint8_t>(b01));
            }
            if (y + 1 < height) {
                yRow1[x] = rgbToY(static_cast<std::uint8_t>(r10), static_cast<std::uint8_t>(g10), static_cast<std::uint8_t>(b10));
                if (x + 1 < width) {
                    yRow1[x + 1] = rgbToY(static_cast<std::uint8_t>(r11), static_cast<std::uint8_t>(g11), static_cast<std::uint8_t>(b11));
                }
            }

            const auto r = static_cast<std::uint8_t>((r00 + r01 + r10 + r11 + 2) / 4);
            const auto g = static_cast<std::uint8_t>((g00 + g01 + g10 + g11 + 2) / 4);
            const auto b = static_cast<std::uint8_t>((b00 + b01 + b10 + b11 + 2) / 4);
            uvRow[x] = rgbToU(r, g, b);
            if (x + 1 < width) {
                uvRow[x + 1] = rgbToV(r, g, b);
            }
        }
    }
    return nv12;
}

bool looksLikeStartCode(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    return (offset + 3 <= bytes.size() &&
            bytes[offset] == 0x00 &&
            bytes[offset + 1] == 0x00 &&
            bytes[offset + 2] == 0x01) ||
        (offset + 4 <= bytes.size() &&
         bytes[offset] == 0x00 &&
         bytes[offset + 1] == 0x00 &&
         bytes[offset + 2] == 0x00 &&
         bytes[offset + 3] == 0x01);
}

std::uint32_t readBigEndianU32(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    return (static_cast<std::uint32_t>(bytes[offset]) << 24) |
        (static_cast<std::uint32_t>(bytes[offset + 1]) << 16) |
        (static_cast<std::uint32_t>(bytes[offset + 2]) << 8) |
        static_cast<std::uint32_t>(bytes[offset + 3]);
}

std::vector<std::vector<std::uint8_t>> normalizeH264Output(const std::vector<std::uint8_t>& bytes) {
    auto annexBUnits = transport::splitAnnexB(bytes);
    if (!annexBUnits.empty()) {
        std::vector<std::vector<std::uint8_t>> units;
        units.reserve(annexBUnits.size());
        for (auto& unit : annexBUnits) {
            units.push_back(std::move(unit.bytes));
        }
        return units;
    }

    std::vector<std::vector<std::uint8_t>> units;
    std::size_t offset = 0;
    while (offset + 4 <= bytes.size()) {
        const auto length = readBigEndianU32(bytes, offset);
        offset += 4;
        if (length == 0 || offset + length > bytes.size()) {
            units.clear();
            break;
        }
        units.emplace_back(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                           bytes.begin() + static_cast<std::ptrdiff_t>(offset + length));
        offset += length;
    }

    if (!units.empty() && offset == bytes.size()) {
        return units;
    }

    if (!bytes.empty() && !looksLikeStartCode(bytes, 0)) {
        return {bytes};
    }
    return {};
}

std::vector<std::vector<std::uint8_t>> parseAvcDecoderConfigurationRecord(const std::uint8_t* bytes, std::size_t size) {
    std::vector<std::vector<std::uint8_t>> units;
    if (bytes == nullptr || size < 7 || bytes[0] != 1) {
        return units;
    }

    std::size_t offset = 5;
    const auto spsCount = bytes[offset++] & 0x1F;
    for (std::uint8_t index = 0; index < spsCount; ++index) {
        if (offset + 2 > size) {
            return {};
        }
        const auto length = (static_cast<std::size_t>(bytes[offset]) << 8) | bytes[offset + 1];
        offset += 2;
        if (length == 0 || offset + length > size) {
            return {};
        }
        units.emplace_back(bytes + offset, bytes + offset + length);
        offset += length;
    }

    if (offset >= size) {
        return units;
    }
    const auto ppsCount = bytes[offset++];
    for (std::uint8_t index = 0; index < ppsCount; ++index) {
        if (offset + 2 > size) {
            return {};
        }
        const auto length = (static_cast<std::size_t>(bytes[offset]) << 8) | bytes[offset + 1];
        offset += 2;
        if (length == 0 || offset + length > size) {
            return {};
        }
        units.emplace_back(bytes + offset, bytes + offset + length);
        offset += length;
    }
    return units;
}

std::vector<std::vector<std::uint8_t>> parseSequenceHeaderBlob(const std::uint8_t* bytes, std::size_t size) {
    auto units = parseAvcDecoderConfigurationRecord(bytes, size);
    if (!units.empty()) {
        return units;
    }
    if (bytes != nullptr && size > 0) {
        std::vector<std::uint8_t> blob(bytes, bytes + size);
        units = normalizeH264Output(blob);
    }
    return units;
}

bool hasNalType(const std::vector<std::vector<std::uint8_t>>& units, unsigned int expectedType) {
    for (const auto& unit : units) {
        if (!unit.empty() && (unit.front() & 0x1F) == expectedType) {
            return true;
        }
    }
    return false;
}

void setCodecBool(ICodecAPI* codecApi, const GUID& key, bool enabled) {
    VARIANT value{};
    value.vt = VT_BOOL;
    value.boolVal = enabled ? VARIANT_TRUE : VARIANT_FALSE;
    codecApi->SetValue(&key, &value);
}

void setCodecU32(ICodecAPI* codecApi, const GUID& key, std::uint32_t number) {
    VARIANT value{};
    value.vt = VT_UI4;
    value.ulVal = number;
    codecApi->SetValue(&key, &value);
}

HRESULT createSoftwareH264Encoder(IMFTransform** transform) {
    return CoCreateInstance(CLSID_CMSH264EncoderMFT, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(transform));
}

HRESULT createHardwareH264Encoder(IMFTransform** transform) {
    *transform = nullptr;

    MFT_REGISTER_TYPE_INFO inputInfo{};
    inputInfo.guidMajorType = MFMediaType_Video;
    inputInfo.guidSubtype = MFVideoFormat_NV12;
    MFT_REGISTER_TYPE_INFO outputInfo{};
    outputInfo.guidMajorType = MFMediaType_Video;
    outputInfo.guidSubtype = MFVideoFormat_H264;

    IMFActivate** activations = nullptr;
    UINT32 activationCount = 0;
    HRESULT hr = MFTEnumEx(
        MFT_CATEGORY_VIDEO_ENCODER,
        MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
        &inputInfo,
        &outputInfo,
        &activations,
        &activationCount);
    if (FAILED(hr) || activationCount == 0 || activations == nullptr) {
        if (activations != nullptr) {
            CoTaskMemFree(activations);
        }
        return FAILED(hr) ? hr : MF_E_TOPO_CODEC_NOT_FOUND;
    }

    HRESULT activateHr = MF_E_TOPO_CODEC_NOT_FOUND;
    for (UINT32 index = 0; index < activationCount && *transform == nullptr; ++index) {
        activateHr = activations[index]->ActivateObject(IID_PPV_ARGS(transform));
    }
    for (UINT32 index = 0; index < activationCount; ++index) {
        activations[index]->Release();
    }
    CoTaskMemFree(activations);
    return *transform != nullptr ? S_OK : activateHr;
}

void unlockAsyncTransformIfNeeded(IMFTransform* transform) {
    Microsoft::WRL::ComPtr<IMFAttributes> attributes;
    if (transform != nullptr && SUCCEEDED(transform->GetAttributes(&attributes)) && attributes != nullptr) {
        attributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
    }
}
#endif

}  // namespace

Status Encoder::configure(const protocol::VideoMode& mode) {
    if (mode.codec != protocol::Codec::h264) {
        return Status::invalidArgument("MVP encoder only accepts H.264 mode");
    }

    mode_ = mode;
    logInfo("host encoder configured for H.264 low-latency placeholder");
    return Status::ok();
}

Status Encoder::start() {
    running_ = true;
#if defined(_WIN32)
    auto status = startMediaFoundation();
    if (!status.isOk()) {
        logWarn("Media Foundation encoder unavailable, falling back to placeholder H.264 NALs: " + status.message());
        mediaFoundationStarted_ = false;
    }
#endif
    logInfo(std::string("host encoder started with backend ") + backendName());
    return Status::ok();
}

Status Encoder::stop() {
#if defined(_WIN32)
    stopMediaFoundation();
#endif
    running_ = false;
    logInfo("host encoder stopped");
    return Status::ok();
}

Status Encoder::requestIdr() {
    if (!running_) {
        return Status::invalidState("cannot request IDR while encoder is stopped");
    }
    logInfo("host encoder IDR requested");
    return Status::ok();
}

EncodedFrame Encoder::encode(const CapturedFrame& frame) const {
#if defined(_WIN32)
    if (mediaFoundationStarted_ && !frame.bgra.empty()) {
        return encodeMediaFoundation(frame);
    }
#endif
    return encodePlaceholder(frame);
}

const protocol::VideoMode& Encoder::mode() const {
    return mode_;
}

const char* Encoder::backendName() const {
#if defined(_WIN32)
    if (mediaFoundationStarted_ && hardwareMediaFoundation_) {
        return "media-foundation-h264-hardware";
    }
    return mediaFoundationStarted_ ? "media-foundation-h264-software" : "placeholder-h264";
#else
    return "placeholder-h264";
#endif
}

EncodedFrame Encoder::encodePlaceholder(const CapturedFrame& frame) const {
    EncodedFrame encoded;
    encoded.frameId = frame.frameId;
    encoded.timestampUs = frame.timestampUs;
    encoded.keyFrame = frame.frameId <= 1 || frame.frameId % 60 == 0;
    encoded.payload.assign(3000, static_cast<std::uint8_t>(frame.frameId & 0xFF));
    encoded.payload.front() = encoded.keyFrame ? 0x65 : 0x41;
    encoded.nalUnits.push_back(encoded.payload);
    return encoded;
}

#if defined(_WIN32)
Status Encoder::startMediaFoundation() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (SUCCEEDED(hr)) {
        comInitialized_ = true;
    } else if (hr != RPC_E_CHANGED_MODE) {
        return Status::unavailable(hresultText(hr, "CoInitializeEx"));
    }

    hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    if (FAILED(hr)) {
        if (comInitialized_) {
            CoUninitialize();
            comInitialized_ = false;
        }
        return Status::unavailable(hresultText(hr, "MFStartup"));
    }

    IMFTransform* transform = nullptr;
    bool hardwareTransform = false;
    hr = createSoftwareH264Encoder(&transform);
    if (FAILED(hr) || transform == nullptr) {
        MFShutdown();
        if (comInitialized_) {
            CoUninitialize();
            comInitialized_ = false;
        }
        return Status::unavailable(hresultText(hr, "CoCreateInstance(CMSH264EncoderMFT)"));
    }
    unlockAsyncTransformIfNeeded(transform);

    Microsoft::WRL::ComPtr<IMFMediaType> outputType;
    hr = MFCreateMediaType(&outputType);
    if (FAILED(hr)) {
        transform->Release();
        MFShutdown();
        if (comInitialized_) {
            CoUninitialize();
            comInitialized_ = false;
        }
        return Status::unavailable(hresultText(hr, "MFCreateMediaType(output)"));
    }
    setVideoTypeCommon(outputType.Get(), mode_, MFVideoFormat_H264);
    outputType->SetUINT32(MF_MT_AVG_BITRATE, mode_.bitrateKbps * 1000);
    hr = transform->SetOutputType(0, outputType.Get(), 0);
    if (FAILED(hr)) {
        transform->Release();
        MFShutdown();
        if (comInitialized_) {
            CoUninitialize();
            comInitialized_ = false;
        }
        return Status::unavailable(hresultText(hr, "SetOutputType(H264)"));
    }

    Microsoft::WRL::ComPtr<IMFMediaType> inputType;
    hr = MFCreateMediaType(&inputType);
    if (FAILED(hr)) {
        transform->Release();
        MFShutdown();
        if (comInitialized_) {
            CoUninitialize();
            comInitialized_ = false;
        }
        return Status::unavailable(hresultText(hr, "MFCreateMediaType(input)"));
    }
    setVideoTypeCommon(inputType.Get(), mode_, MFVideoFormat_NV12);
    inputType->SetUINT32(MF_MT_DEFAULT_STRIDE, mode_.resolution.width);
    hr = transform->SetInputType(0, inputType.Get(), 0);
    if (FAILED(hr)) {
        transform->Release();
        MFShutdown();
        if (comInitialized_) {
            CoUninitialize();
            comInitialized_ = false;
        }
        return Status::unavailable(hresultText(hr, "SetInputType(NV12)"));
    }

    Microsoft::WRL::ComPtr<ICodecAPI> codecApi;
    if (SUCCEEDED(transform->QueryInterface(IID_PPV_ARGS(&codecApi)))) {
        const auto fps = mode_.resolution.refreshRate == 0 ? 60 : mode_.resolution.refreshRate;
        setCodecBool(codecApi.Get(), CODECAPI_AVLowLatencyMode, true);
        setCodecBool(codecApi.Get(), CODECAPI_AVEncCommonRealTime, true);
        setCodecU32(codecApi.Get(), CODECAPI_AVEncMPVDefaultBPictureCount, 0);
        setCodecU32(codecApi.Get(), CODECAPI_AVEncMPVGOPSize, fps);
    }

    transform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    transform_ = transform;
    mediaFoundationStarted_ = true;
    hardwareMediaFoundation_ = hardwareTransform;
    return Status::ok();
}

void Encoder::stopMediaFoundation() {
    if (transform_ != nullptr) {
        auto* transform = static_cast<IMFTransform*>(transform_);
        transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        transform->Release();
        transform_ = nullptr;
    }
    if (mediaFoundationStarted_) {
        MFShutdown();
    }
    if (comInitialized_) {
        CoUninitialize();
        comInitialized_ = false;
    }
    mediaFoundationStarted_ = false;
    hardwareMediaFoundation_ = false;
    parameterSets_.clear();
}

EncodedFrame Encoder::encodeMediaFoundation(const CapturedFrame& frame) const {
    EncodedFrame encoded;
    encoded.frameId = frame.frameId;
    encoded.timestampUs = frame.timestampUs;
    encoded.keyFrame = frame.frameId <= 1 || frame.frameId % 60 == 0;

    auto* transform = static_cast<IMFTransform*>(transform_);
    if (transform == nullptr) {
        return encodePlaceholder(frame);
    }

    const auto nv12 = bgraToNv12(frame);
    Microsoft::WRL::ComPtr<IMFMediaBuffer> inputBuffer;
    HRESULT hr = MFCreateMemoryBuffer(static_cast<DWORD>(nv12.size()), &inputBuffer);
    if (FAILED(hr)) {
        return encodePlaceholder(frame);
    }

    BYTE* destination = nullptr;
    DWORD maxLength = 0;
    hr = inputBuffer->Lock(&destination, &maxLength, nullptr);
    if (FAILED(hr) || maxLength < nv12.size()) {
        return encodePlaceholder(frame);
    }
    std::memcpy(destination, nv12.data(), nv12.size());
    inputBuffer->Unlock();
    inputBuffer->SetCurrentLength(static_cast<DWORD>(nv12.size()));

    Microsoft::WRL::ComPtr<IMFSample> inputSample;
    hr = MFCreateSample(&inputSample);
    if (FAILED(hr)) {
        return encodePlaceholder(frame);
    }
    inputSample->AddBuffer(inputBuffer.Get());
    inputSample->SetSampleTime(static_cast<LONGLONG>(frame.timestampUs * 10));
    const auto fps = mode_.resolution.refreshRate == 0 ? 60 : mode_.resolution.refreshRate;
    inputSample->SetSampleDuration(static_cast<LONGLONG>(10000000ULL / fps));

    hr = transform->ProcessInput(0, inputSample.Get(), 0);
    if (FAILED(hr)) {
        return encodePlaceholder(frame);
    }

    MFT_OUTPUT_STREAM_INFO streamInfo{};
    hr = transform->GetOutputStreamInfo(0, &streamInfo);
    if (FAILED(hr)) {
        return encoded;
    }

    for (;;) {
        Microsoft::WRL::ComPtr<IMFSample> outputSample;
        Microsoft::WRL::ComPtr<IMFMediaBuffer> outputBuffer;
        if ((streamInfo.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) == 0) {
            hr = MFCreateSample(&outputSample);
            if (FAILED(hr)) {
                break;
            }
            const auto outputSize = std::max<DWORD>(streamInfo.cbSize, 1024 * 1024);
            hr = MFCreateMemoryBuffer(outputSize, &outputBuffer);
            if (FAILED(hr)) {
                break;
            }
            outputSample->AddBuffer(outputBuffer.Get());
        }

        MFT_OUTPUT_DATA_BUFFER output{};
        output.dwStreamID = 0;
        output.pSample = outputSample.Get();
        DWORD status = 0;
        hr = transform->ProcessOutput(0, 1, &output, &status);
        if (output.pEvents != nullptr) {
            output.pEvents->Release();
        }
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            break;
        }
        if (FAILED(hr)) {
            break;
        }

        Microsoft::WRL::ComPtr<IMFMediaType> currentOutputType;
        if (SUCCEEDED(transform->GetOutputCurrentType(0, &currentOutputType)) && currentOutputType != nullptr) {
            UINT32 sequenceHeaderSize = 0;
            UINT8* sequenceHeader = nullptr;
            if (SUCCEEDED(currentOutputType->GetAllocatedBlob(
                    MF_MT_MPEG_SEQUENCE_HEADER,
                    &sequenceHeader,
                    &sequenceHeaderSize)) &&
                sequenceHeader != nullptr &&
                sequenceHeaderSize > 0) {
                auto units = parseSequenceHeaderBlob(sequenceHeader, sequenceHeaderSize);
                if (!units.empty()) {
                    parameterSets_ = std::move(units);
                }
                CoTaskMemFree(sequenceHeader);
            }
        }

        IMFMediaBuffer* contiguous = nullptr;
        hr = output.pSample->ConvertToContiguousBuffer(&contiguous);
        if (FAILED(hr) || contiguous == nullptr) {
            break;
        }
        BYTE* bytes = nullptr;
        DWORD currentLength = 0;
        hr = contiguous->Lock(&bytes, nullptr, &currentLength);
        if (SUCCEEDED(hr) && currentLength > 0) {
            const auto oldSize = encoded.payload.size();
            encoded.payload.resize(oldSize + currentLength);
            std::memcpy(encoded.payload.data() + oldSize, bytes, currentLength);
            contiguous->Unlock();
        }
        contiguous->Release();
    }

    encoded.nalUnits = normalizeH264Output(encoded.payload);
    if (encoded.nalUnits.empty() && !encoded.payload.empty()) {
        encoded.nalUnits.push_back(encoded.payload);
    }
    const bool hasSps = hasNalType(encoded.nalUnits, 7);
    if ((encoded.keyFrame || hasNalType(encoded.nalUnits, 5)) && !hasSps && !parameterSets_.empty()) {
        encoded.nalUnits.insert(encoded.nalUnits.begin(), parameterSets_.begin(), parameterSets_.end());
        encoded.payload.clear();
        for (const auto& unit : encoded.nalUnits) {
            encoded.payload.push_back(0x00);
            encoded.payload.push_back(0x00);
            encoded.payload.push_back(0x00);
            encoded.payload.push_back(0x01);
            encoded.payload.insert(encoded.payload.end(), unit.begin(), unit.end());
        }
    }
    return encoded;
}
#endif

}  // namespace led::host
