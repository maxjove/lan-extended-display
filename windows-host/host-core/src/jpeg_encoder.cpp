#include "led/host/jpeg_encoder.h"

#if defined(_WIN32)
#include <windows.h>
#include <gdiplus.h>
#include <objbase.h>
#include <wrl/client.h>
#endif

#include <algorithm>
#include <cstddef>
#include <cwchar>
#include <string>
#include <vector>

namespace led::host {

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
    encoded = {};
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
    const HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool coInitialized = SUCCEEDED(coHr);
    if (FAILED(coHr) && coHr != RPC_E_CHANGED_MODE) {
        return Status::unavailable(hresultText(coHr, "CoInitializeEx"));
    }

    if (!gdiplusRuntime().isOk()) {
        if (coInitialized) {
            CoUninitialize();
        }
        return Status::unavailable("GDI+ startup failed");
    }

    CLSID clsid{};
    auto status = jpegEncoderClsid(clsid);
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

    STATSTG stat{};
    hr = stream->Stat(&stat, STATFLAG_NONAME);
    if (FAILED(hr) || stat.cbSize.QuadPart <= 0) {
        if (coInitialized) {
            CoUninitialize();
        }
        return Status::unavailable(FAILED(hr) ? hresultText(hr, "IStream::Stat") : "JPEG stream is empty");
    }
    LARGE_INTEGER zero{};
    stream->Seek(zero, STREAM_SEEK_SET, nullptr);
    encoded.jpegBytes.assign(static_cast<std::size_t>(stat.cbSize.QuadPart), 0);
    ULONG read = 0;
    hr = stream->Read(encoded.jpegBytes.data(), static_cast<ULONG>(encoded.jpegBytes.size()), &read);
    if (FAILED(hr) || read != encoded.jpegBytes.size()) {
        if (coInitialized) {
            CoUninitialize();
        }
        return Status::unavailable(FAILED(hr) ? hresultText(hr, "IStream::Read") : "short JPEG stream read");
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
