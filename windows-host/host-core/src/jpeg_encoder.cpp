#include "led/host/jpeg_encoder.h"

#if defined(_WIN32)
#include <windows.h>
#include <gdiplus.h>
#include <objbase.h>
#include <wincodec.h>
#include <wrl/client.h>
#endif

#if defined(LED_HAS_TURBOJPEG)
#include <turbojpeg.h>
#endif

#include <algorithm>
#include <cstddef>
#include <cwchar>
#include <memory>
#include <string>
#include <vector>

namespace led::host {

#if defined(LED_HAS_TURBOJPEG)
namespace {

class TurboJpegCompressor {
public:
    TurboJpegCompressor() : handle_(tjInitCompress()) {}
    ~TurboJpegCompressor() {
        if (handle_ != nullptr) {
            tjDestroy(handle_);
        }
    }

    TurboJpegCompressor(const TurboJpegCompressor&) = delete;
    TurboJpegCompressor& operator=(const TurboJpegCompressor&) = delete;

    [[nodiscard]] tjhandle get() const { return handle_; }

private:
    tjhandle handle_{nullptr};
};

struct TurboJpegScratchBuffer {
    std::unique_ptr<unsigned char[]> bytes;
    unsigned long capacity{0};

    [[nodiscard]] unsigned char* ensure(unsigned long required) {
        if (required > capacity) {
            bytes = std::make_unique<unsigned char[]>(required);
            capacity = required;
        }
        return bytes.get();
    }
};

Status encodeBgraJpegRectTurbo(
    const CapturedFrame& frame,
    std::uint32_t x,
    std::uint32_t y,
    std::uint32_t width,
    std::uint32_t height,
    float quality,
    EncodedJpegFrame& encoded) {
    thread_local TurboJpegCompressor compressor;
    tjhandle handle = compressor.get();
    if (handle == nullptr) {
        return Status::unavailable("TurboJPEG compressor initialization failed");
    }

    const auto qualityPercent = static_cast<int>(std::clamp(quality, 0.1F, 1.0F) * 100.0F);
    const auto maxBytes = tjBufSize(static_cast<int>(width), static_cast<int>(height), TJSAMP_444);
    if (maxBytes == 0) {
        return Status::unavailable("TurboJPEG buffer sizing failed");
    }

    thread_local TurboJpegScratchBuffer scratch;
    auto* jpegBuffer = scratch.ensure(static_cast<unsigned long>(maxBytes));
    unsigned long jpegBytes = scratch.capacity;
    const auto* source = frame.bgra.data() + (static_cast<std::size_t>(y) * frame.width + x) * 4;
    const int result = tjCompress2(
        handle,
        const_cast<unsigned char*>(source),
        static_cast<int>(width),
        static_cast<int>(frame.width * 4),
        static_cast<int>(height),
        TJPF_BGRA,
        &jpegBuffer,
        &jpegBytes,
        TJSAMP_444,
        qualityPercent,
        TJFLAG_FASTDCT | TJFLAG_NOREALLOC);
    if (result != 0) {
        const std::string error = tjGetErrorStr2(handle) != nullptr ? tjGetErrorStr2(handle) : "unknown TurboJPEG error";
        return Status::unavailable("TurboJPEG encode failed: " + error);
    }

    encoded.jpegBytes.assign(jpegBuffer, jpegBuffer + jpegBytes);
    encoded.frameId = frame.frameId;
    encoded.timestampUs = frame.timestampUs;
    encoded.width = width;
    encoded.height = height;
    return Status::ok();
}

}  // namespace
#endif

#if defined(_WIN32)
namespace {

class GdiplusRuntime {
public:
    GdiplusRuntime() {
        Gdiplus::GdiplusStartupInput startupInput;
        status_ = Gdiplus::GdiplusStartup(&token_, &startupInput, nullptr);
    }

    ~GdiplusRuntime() {
        if (status_ == Gdiplus::Ok) {
            Gdiplus::GdiplusShutdown(token_);
        }
    }

    GdiplusRuntime(const GdiplusRuntime&) = delete;
    GdiplusRuntime& operator=(const GdiplusRuntime&) = delete;

    [[nodiscard]] bool isOk() const { return status_ == Gdiplus::Ok; }

private:
    ULONG_PTR token_{0};
    Gdiplus::Status status_{Gdiplus::GenericError};
};

GdiplusRuntime& gdiplusRuntime() {
    static GdiplusRuntime runtime;
    return runtime;
}

std::string hresultText(HRESULT hr, const char* operation) {
    return std::string(operation) + " failed with HRESULT 0x" + std::to_string(static_cast<unsigned long>(hr));
}

led::Status jpegEncoderClsid(CLSID& clsid) {
    UINT count = 0;
    UINT bytes = 0;
    Gdiplus::GetImageEncodersSize(&count, &bytes);
    if (count == 0 || bytes == 0) {
        return Status::unavailable("GDI+ returned no image encoders");
    }
    std::vector<std::uint8_t> storage(bytes);
    auto* encoders = reinterpret_cast<Gdiplus::ImageCodecInfo*>(storage.data());
    if (Gdiplus::GetImageEncoders(count, bytes, encoders) != Gdiplus::Ok) {
        return Status::unavailable("GDI+ GetImageEncoders failed");
    }
    for (UINT index = 0; index < count; ++index) {
        if (std::wcscmp(encoders[index].MimeType, L"image/jpeg") == 0) {
            clsid = encoders[index].Clsid;
            return Status::ok();
        }
    }
    return Status::unavailable("GDI+ JPEG encoder was not found");
}

Status readStreamBytes(IStream* stream, std::vector<std::uint8_t>& bytes) {
    STATSTG stat{};
    HRESULT hr = stream->Stat(&stat, STATFLAG_NONAME);
    if (FAILED(hr) || stat.cbSize.QuadPart <= 0) {
        return Status::unavailable(FAILED(hr) ? hresultText(hr, "IStream::Stat") : "JPEG stream is empty");
    }
    LARGE_INTEGER zero{};
    hr = stream->Seek(zero, STREAM_SEEK_SET, nullptr);
    if (FAILED(hr)) {
        return Status::unavailable(hresultText(hr, "IStream::Seek"));
    }
    bytes.assign(static_cast<std::size_t>(stat.cbSize.QuadPart), 0);
    ULONG read = 0;
    hr = stream->Read(bytes.data(), static_cast<ULONG>(bytes.size()), &read);
    if (FAILED(hr) || read != bytes.size()) {
        return Status::unavailable(FAILED(hr) ? hresultText(hr, "IStream::Read") : "short JPEG stream read");
    }
    return Status::ok();
}

Status encodeBgraJpegRectWic(
    const CapturedFrame& frame,
    std::uint32_t x,
    std::uint32_t y,
    std::uint32_t width,
    std::uint32_t height,
    float quality,
    EncodedJpegFrame& encoded) {
    Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(
        CLSID_WICImagingFactory,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        return Status::unavailable(hresultText(hr, "CoCreateInstance(IWICImagingFactory)"));
    }

    Microsoft::WRL::ComPtr<IStream> stream;
    hr = CreateStreamOnHGlobal(nullptr, TRUE, &stream);
    if (FAILED(hr)) {
        return Status::unavailable(hresultText(hr, "CreateStreamOnHGlobal"));
    }

    Microsoft::WRL::ComPtr<IWICBitmapEncoder> encoder;
    hr = factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &encoder);
    if (FAILED(hr)) {
        return Status::unavailable(hresultText(hr, "IWICImagingFactory::CreateEncoder"));
    }
    hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    if (FAILED(hr)) {
        return Status::unavailable(hresultText(hr, "IWICBitmapEncoder::Initialize"));
    }

    Microsoft::WRL::ComPtr<IWICBitmapFrameEncode> frameEncode;
    Microsoft::WRL::ComPtr<IPropertyBag2> options;
    hr = encoder->CreateNewFrame(&frameEncode, &options);
    if (FAILED(hr)) {
        return Status::unavailable(hresultText(hr, "IWICBitmapEncoder::CreateNewFrame"));
    }

    PROPBAG2 optionNames[2]{};
    optionNames[0].pstrName = const_cast<LPOLESTR>(L"ImageQuality");
    optionNames[1].pstrName = const_cast<LPOLESTR>(L"JpegYCrCbSubsampling");
    VARIANT optionValues[2]{};
    VariantInit(&optionValues[0]);
    VariantInit(&optionValues[1]);
    optionValues[0].vt = VT_R4;
    optionValues[0].fltVal = std::clamp(quality, 0.1F, 1.0F);
    optionValues[1].vt = VT_UI1;
    optionValues[1].bVal = static_cast<BYTE>(WICJpegYCrCbSubsampling444);
    hr = options->Write(2, optionNames, optionValues);
    VariantClear(&optionValues[0]);
    VariantClear(&optionValues[1]);
    if (FAILED(hr)) {
        return Status::unavailable(hresultText(hr, "IPropertyBag2::Write"));
    }

    hr = frameEncode->Initialize(options.Get());
    if (FAILED(hr)) {
        return Status::unavailable(hresultText(hr, "IWICBitmapFrameEncode::Initialize"));
    }
    hr = frameEncode->SetSize(width, height);
    if (FAILED(hr)) {
        return Status::unavailable(hresultText(hr, "IWICBitmapFrameEncode::SetSize"));
    }
    WICPixelFormatGUID pixelFormat = GUID_WICPixelFormat24bppBGR;
    hr = frameEncode->SetPixelFormat(&pixelFormat);
    if (FAILED(hr)) {
        return Status::unavailable(hresultText(hr, "IWICBitmapFrameEncode::SetPixelFormat"));
    }

    Microsoft::WRL::ComPtr<IWICBitmap> bitmap;
    const auto sourceStride = frame.width * 4;
    const auto sourceBytes = (height - 1) * sourceStride + width * 4;
    auto* source = const_cast<BYTE*>(reinterpret_cast<const BYTE*>(
        frame.bgra.data() + (static_cast<std::size_t>(y) * frame.width + x) * 4));
    hr = factory->CreateBitmapFromMemory(
        width,
        height,
        GUID_WICPixelFormat32bppBGRA,
        sourceStride,
        sourceBytes,
        source,
        &bitmap);
    if (FAILED(hr)) {
        return Status::unavailable(hresultText(hr, "IWICImagingFactory::CreateBitmapFromMemory"));
    }
    hr = frameEncode->WriteSource(bitmap.Get(), nullptr);
    if (FAILED(hr)) {
        return Status::unavailable(hresultText(hr, "IWICBitmapFrameEncode::WriteSource"));
    }
    hr = frameEncode->Commit();
    if (FAILED(hr)) {
        return Status::unavailable(hresultText(hr, "IWICBitmapFrameEncode::Commit"));
    }
    hr = encoder->Commit();
    if (FAILED(hr)) {
        return Status::unavailable(hresultText(hr, "IWICBitmapEncoder::Commit"));
    }

    auto status = readStreamBytes(stream.Get(), encoded.jpegBytes);
    if (!status.isOk()) {
        return status;
    }
    encoded.frameId = frame.frameId;
    encoded.timestampUs = frame.timestampUs;
    encoded.width = width;
    encoded.height = height;
    return Status::ok();
}

}  // namespace
#endif

Status encodeBgraJpegRect(
    const CapturedFrame& frame,
    std::uint32_t x,
    std::uint32_t y,
    std::uint32_t width,
    std::uint32_t height,
    float quality,
    EncodedJpegFrame& encoded) {
    encoded.frameId = 0;
    encoded.timestampUs = 0;
    encoded.width = 0;
    encoded.height = 0;
    encoded.jpegBytes.clear();
    if (frame.bgra.empty() || frame.width == 0 || frame.height == 0 || width == 0 || height == 0) {
        return Status::invalidArgument("cannot JPEG encode an empty frame");
    }
    if (x >= frame.width || y >= frame.height || x + width > frame.width || y + height > frame.height) {
        return Status::invalidArgument("JPEG encode rect is outside the frame");
    }
#if !defined(_WIN32)
    (void)x;
    (void)y;
    (void)width;
    (void)height;
    (void)quality;
    return Status::unavailable("JPEG encoding is only implemented on Windows host");
#else
#if defined(LED_HAS_TURBOJPEG)
    auto turboStatus = encodeBgraJpegRectTurbo(frame, x, y, width, height, quality, encoded);
    if (turboStatus.isOk()) {
        return Status::ok();
    }
#endif

    const HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool coInitialized = SUCCEEDED(coHr);
    if (FAILED(coHr) && coHr != RPC_E_CHANGED_MODE) {
        return Status::unavailable(hresultText(coHr, "CoInitializeEx"));
    }

    auto status = encodeBgraJpegRectWic(frame, x, y, width, height, quality, encoded);
    if (status.isOk()) {
        if (coInitialized) {
            CoUninitialize();
        }
        return Status::ok();
    }

    if (!gdiplusRuntime().isOk()) {
        if (coInitialized) {
            CoUninitialize();
        }
        return Status::unavailable("GDI+ startup failed");
    }

    CLSID clsid{};
    status = jpegEncoderClsid(clsid);
    if (!status.isOk()) {
        if (coInitialized) {
            CoUninitialize();
        }
        return status;
    }

    Microsoft::WRL::ComPtr<IStream> stream;
    HRESULT hr = CreateStreamOnHGlobal(nullptr, TRUE, &stream);
    if (FAILED(hr)) {
        if (coInitialized) {
            CoUninitialize();
        }
        return Status::unavailable(hresultText(hr, "CreateStreamOnHGlobal"));
    }

    auto qualityValue = static_cast<ULONG>(std::clamp(quality, 0.1F, 1.0F) * 100.0F);
    Gdiplus::EncoderParameters parameters;
    parameters.Count = 1;
    parameters.Parameter[0].Guid = Gdiplus::EncoderQuality;
    parameters.Parameter[0].Type = Gdiplus::EncoderParameterValueTypeLong;
    parameters.Parameter[0].NumberOfValues = 1;
    parameters.Parameter[0].Value = &qualityValue;

    Gdiplus::Bitmap bitmap(
        static_cast<INT>(width),
        static_cast<INT>(height),
        static_cast<INT>(frame.width * 4),
        PixelFormat32bppARGB,
        const_cast<BYTE*>(reinterpret_cast<const BYTE*>(
            frame.bgra.data() + (static_cast<std::size_t>(y) * frame.width + x) * 4)));
    const auto saveStatus = bitmap.Save(stream.Get(), &clsid, &parameters);
    if (saveStatus != Gdiplus::Ok) {
        if (coInitialized) {
            CoUninitialize();
        }
        return Status::unavailable("GDI+ JPEG save failed with status " + std::to_string(static_cast<int>(saveStatus)));
    }

    status = readStreamBytes(stream.Get(), encoded.jpegBytes);
    if (!status.isOk()) {
        if (coInitialized) {
            CoUninitialize();
        }
        return status;
    }

    encoded.frameId = frame.frameId;
    encoded.timestampUs = frame.timestampUs;
    encoded.width = width;
    encoded.height = height;
    if (coInitialized) {
        CoUninitialize();
    }
    return Status::ok();
#endif
}

Status encodeBgraJpeg(const CapturedFrame& frame, float quality, EncodedJpegFrame& encoded) {
    return encodeBgraJpegRect(frame, 0, 0, frame.width, frame.height, quality, encoded);
}

}  // namespace led::host
