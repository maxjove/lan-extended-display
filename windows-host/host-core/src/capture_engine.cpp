#include "led/host/capture_engine.h"

#include "led/common/logger.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <string>
#include <utility>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11.h>
#include <dxgi1_2.h>
#include <windows.h>
#include <wrl/client.h>
#endif

namespace led::host {

#if defined(_WIN32)
namespace {

std::string hresultText(HRESULT hr, const char* operation) {
    return std::string(operation) + " failed with HRESULT 0x" + std::to_string(static_cast<unsigned long>(hr));
}

void releaseCom(void*& pointer) {
    if (pointer != nullptr) {
        static_cast<IUnknown*>(pointer)->Release();
        pointer = nullptr;
    }
}

void scaleBgra(
    const std::uint8_t* source,
    std::uint32_t sourceWidth,
    std::uint32_t sourceHeight,
    std::uint32_t sourceStride,
    std::uint8_t* destination,
    std::uint32_t destinationWidth,
    std::uint32_t destinationHeight) {
    if (source == nullptr || destination == nullptr || sourceWidth == 0 || sourceHeight == 0 ||
        destinationWidth == 0 || destinationHeight == 0) {
        return;
    }

    const auto destinationStride = static_cast<std::size_t>(destinationWidth) * 4;
    if (sourceWidth == destinationWidth && sourceHeight == destinationHeight) {
        for (std::uint32_t y = 0; y < destinationHeight; ++y) {
            std::memcpy(
                destination + static_cast<std::size_t>(y) * destinationStride,
                source + static_cast<std::size_t>(y) * sourceStride,
                destinationStride);
        }
        return;
    }

    for (std::uint32_t y = 0; y < destinationHeight; ++y) {
        const auto sourceY = static_cast<std::uint32_t>(
            (static_cast<std::uint64_t>(y) * sourceHeight) / destinationHeight);
        const auto* sourceRow = source + static_cast<std::size_t>(sourceY) * sourceStride;
        auto* destinationRow = destination + static_cast<std::size_t>(y) * destinationStride;
        for (std::uint32_t x = 0; x < destinationWidth; ++x) {
            const auto sourceX = static_cast<std::uint32_t>(
                (static_cast<std::uint64_t>(x) * sourceWidth) / destinationWidth);
            std::memcpy(destinationRow + static_cast<std::size_t>(x) * 4,
                        sourceRow + static_cast<std::size_t>(sourceX) * 4,
                        4);
        }
    }
}

Status startDxgiCapture(
    CaptureEngine& engine,
    const protocol::Resolution& resolution,
    bool captureRegion,
    int regionOriginX,
    int regionOriginY) {
    Microsoft::WRL::ComPtr<IDXGIAdapter1> selectedAdapter;
    Microsoft::WRL::ComPtr<IDXGIOutput> selectedOutput;

    if (captureRegion) {
        Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
        HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
        if (FAILED(hr)) {
            return Status::unavailable(hresultText(hr, "CreateDXGIFactory1"));
        }

        for (UINT adapterIndex = 0; selectedOutput == nullptr; ++adapterIndex) {
            Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
            hr = factory->EnumAdapters1(adapterIndex, &adapter);
            if (hr == DXGI_ERROR_NOT_FOUND) {
                break;
            }
            if (FAILED(hr)) {
                return Status::unavailable(hresultText(hr, "EnumAdapters1"));
            }

            for (UINT outputIndex = 0; selectedOutput == nullptr; ++outputIndex) {
                Microsoft::WRL::ComPtr<IDXGIOutput> output;
                hr = adapter->EnumOutputs(outputIndex, &output);
                if (hr == DXGI_ERROR_NOT_FOUND) {
                    break;
                }
                if (FAILED(hr)) {
                    return Status::unavailable(hresultText(hr, "EnumOutputs"));
                }

                DXGI_OUTPUT_DESC desc{};
                hr = output->GetDesc(&desc);
                if (FAILED(hr)) {
                    return Status::unavailable(hresultText(hr, "GetDesc"));
                }

                const auto left = desc.DesktopCoordinates.left;
                const auto top = desc.DesktopCoordinates.top;
                const auto right = desc.DesktopCoordinates.right;
                const auto bottom = desc.DesktopCoordinates.bottom;
                const bool originMatches = left == regionOriginX && top == regionOriginY;
                const bool containsOrigin =
                    regionOriginX >= left && regionOriginX < right && regionOriginY >= top && regionOriginY < bottom;
                if (originMatches || containsOrigin) {
                    selectedAdapter = adapter;
                    selectedOutput = output;
                }
            }
        }

        if (selectedOutput == nullptr || selectedAdapter == nullptr) {
            return Status::unavailable("DXGI output for requested display region was not found");
        }
    }

    D3D_FEATURE_LEVEL featureLevel{};
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    HRESULT hr = D3D11CreateDevice(
        selectedAdapter.Get(),
        selectedAdapter != nullptr ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        nullptr,
        0,
        D3D11_SDK_VERSION,
        &device,
        &featureLevel,
        &context);
    if (FAILED(hr)) {
        return Status::unavailable(hresultText(hr, "D3D11CreateDevice"));
    }

    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
    hr = device->QueryInterface(IID_PPV_ARGS(&dxgiDevice));
    if (FAILED(hr)) {
        context->Release();
        device->Release();
        return Status::unavailable(hresultText(hr, "QueryInterface(IDXGIDevice)"));
    }

    if (selectedOutput == nullptr) {
        Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
        hr = dxgiDevice->GetAdapter(&adapter);
        if (FAILED(hr)) {
            context->Release();
            device->Release();
            return Status::unavailable(hresultText(hr, "GetAdapter"));
        }

        hr = adapter->EnumOutputs(0, &selectedOutput);
        if (FAILED(hr)) {
            context->Release();
            device->Release();
            return Status::unavailable(hresultText(hr, "EnumOutputs(0)"));
        }
    }

    DXGI_OUTPUT_DESC outputDesc{};
    selectedOutput->GetDesc(&outputDesc);
    const auto sourceWidth = static_cast<std::uint32_t>(
        std::max<LONG>(1, outputDesc.DesktopCoordinates.right - outputDesc.DesktopCoordinates.left));
    const auto sourceHeight = static_cast<std::uint32_t>(
        std::max<LONG>(1, outputDesc.DesktopCoordinates.bottom - outputDesc.DesktopCoordinates.top));

    Microsoft::WRL::ComPtr<IDXGIOutput1> output1;
    hr = selectedOutput.As(&output1);
    if (FAILED(hr)) {
        context->Release();
        device->Release();
        return Status::unavailable(hresultText(hr, "QueryInterface(IDXGIOutput1)"));
    }

    IDXGIOutputDuplication* duplication = nullptr;
    hr = output1->DuplicateOutput(device, &duplication);
    if (FAILED(hr)) {
        context->Release();
        device->Release();
        return Status::unavailable(hresultText(hr, "DuplicateOutput"));
    }

    D3D11_TEXTURE2D_DESC stagingDesc{};
    stagingDesc.Width = sourceWidth;
    stagingDesc.Height = sourceHeight;
    stagingDesc.MipLevels = 1;
    stagingDesc.ArraySize = 1;
    stagingDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    stagingDesc.SampleDesc.Count = 1;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    ID3D11Texture2D* stagingTexture = nullptr;
    hr = device->CreateTexture2D(&stagingDesc, nullptr, &stagingTexture);
    if (FAILED(hr)) {
        duplication->Release();
        context->Release();
        device->Release();
        return Status::unavailable(hresultText(hr, "CreateTexture2D(staging)"));
    }

    engine.d3dDevice_ = device;
    engine.d3dContext_ = context;
    engine.dxgiDuplication_ = duplication;
    engine.stagingTexture_ = stagingTexture;
    engine.sourceWidth_ = sourceWidth;
    engine.sourceHeight_ = sourceHeight;
    engine.sourceOriginX_ = outputDesc.DesktopCoordinates.left;
    engine.sourceOriginY_ = outputDesc.DesktopCoordinates.top;
    engine.dxgiCapture_ = true;
    (void)resolution;
    return Status::ok();
}

void stopDxgiCapture(CaptureEngine& engine) {
    releaseCom(engine.stagingTexture_);
    releaseCom(engine.dxgiDuplication_);
    releaseCom(engine.d3dContext_);
    releaseCom(engine.d3dDevice_);
    engine.sourceWidth_ = 0;
    engine.sourceHeight_ = 0;
    engine.dxgiCapture_ = false;
}

Status startGdiCapture(CaptureEngine& engine, const protocol::Resolution& resolution) {
    if (!engine.sourceDeviceName_.empty()) {
        engine.screenDc_ = CreateDCA("DISPLAY", engine.sourceDeviceName_.c_str(), nullptr, nullptr);
        engine.screenDcCreated_ = true;
        if (engine.screenDc_ == nullptr) {
            logWarn("GDI display DC unavailable for " + engine.sourceDeviceName_ + ", falling back to desktop DC");
            engine.screenDc_ = GetDC(nullptr);
            engine.screenDcCreated_ = false;
        }
    } else {
        engine.screenDc_ = GetDC(nullptr);
        engine.screenDcCreated_ = false;
    }
    if (engine.screenDc_ == nullptr) {
        return Status::unavailable("failed to acquire screen DC");
    }

    engine.memoryDc_ = CreateCompatibleDC(static_cast<HDC>(engine.screenDc_));
    if (engine.memoryDc_ == nullptr) {
        if (engine.screenDcCreated_) {
            DeleteDC(static_cast<HDC>(engine.screenDc_));
        } else {
            ReleaseDC(nullptr, static_cast<HDC>(engine.screenDc_));
        }
        engine.screenDc_ = nullptr;
        engine.screenDcCreated_ = false;
        return Status::unavailable("failed to create capture memory DC");
    }

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = static_cast<LONG>(resolution.width);
    info.bmiHeader.biHeight = -static_cast<LONG>(resolution.height);
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    engine.bitmap_ = CreateDIBSection(
        static_cast<HDC>(engine.screenDc_),
        &info,
        DIB_RGB_COLORS,
        &engine.bitmapBits_,
        nullptr,
        0);
    if (engine.bitmap_ == nullptr || engine.bitmapBits_ == nullptr) {
        DeleteDC(static_cast<HDC>(engine.memoryDc_));
        if (engine.screenDcCreated_) {
            DeleteDC(static_cast<HDC>(engine.screenDc_));
        } else {
            ReleaseDC(nullptr, static_cast<HDC>(engine.screenDc_));
        }
        engine.memoryDc_ = nullptr;
        engine.screenDc_ = nullptr;
        engine.screenDcCreated_ = false;
        engine.bitmapBits_ = nullptr;
        return Status::unavailable("failed to create capture DIB section");
    }

    SelectObject(static_cast<HDC>(engine.memoryDc_), static_cast<HBITMAP>(engine.bitmap_));
    SetStretchBltMode(static_cast<HDC>(engine.memoryDc_), HALFTONE);
    return Status::ok();
}

void stopGdiCapture(CaptureEngine& engine) {
    if (engine.bitmap_ != nullptr) {
        DeleteObject(static_cast<HBITMAP>(engine.bitmap_));
        engine.bitmap_ = nullptr;
        engine.bitmapBits_ = nullptr;
    }
    if (engine.memoryDc_ != nullptr) {
        DeleteDC(static_cast<HDC>(engine.memoryDc_));
        engine.memoryDc_ = nullptr;
    }
    if (engine.screenDc_ != nullptr) {
        if (engine.screenDcCreated_) {
            DeleteDC(static_cast<HDC>(engine.screenDc_));
        } else {
            ReleaseDC(nullptr, static_cast<HDC>(engine.screenDc_));
        }
        engine.screenDc_ = nullptr;
        engine.screenDcCreated_ = false;
    }
}

Status captureGdiFrame(CaptureEngine& engine, CapturedFrame& frame) {
    if (engine.screenDc_ == nullptr || engine.memoryDc_ == nullptr || engine.bitmapBits_ == nullptr) {
        return Status::invalidState("GDI capture backend is not initialized");
    }

    const bool displayDcCapture = !engine.sourceDeviceName_.empty() && engine.screenDcCreated_;
    const auto sourceX = displayDcCapture ? 0 : (engine.captureRegion_ ? engine.sourceOriginX_ : GetSystemMetrics(SM_XVIRTUALSCREEN));
    const auto sourceY = displayDcCapture ? 0 : (engine.captureRegion_ ? engine.sourceOriginY_ : GetSystemMetrics(SM_YVIRTUALSCREEN));
    const auto sourceWidth = engine.captureRegion_ ? static_cast<int>(engine.sourceWidth_) : std::max(1, GetSystemMetrics(SM_CXVIRTUALSCREEN));
    const auto sourceHeight = engine.captureRegion_ ? static_cast<int>(engine.sourceHeight_) : std::max(1, GetSystemMetrics(SM_CYVIRTUALSCREEN));

    const BOOL copied = StretchBlt(
        static_cast<HDC>(engine.memoryDc_),
        0,
        0,
        static_cast<int>(frame.width),
        static_cast<int>(frame.height),
        static_cast<HDC>(engine.screenDc_),
        sourceX,
        sourceY,
        sourceWidth,
        sourceHeight,
        SRCCOPY | CAPTUREBLT);
    if (!copied) {
        return Status::unavailable("GDI desktop capture failed");
    }

    const auto byteCount = static_cast<std::size_t>(frame.width) * frame.height * 4;
    frame.bgra.resize(byteCount);
    std::memcpy(frame.bgra.data(), engine.bitmapBits_, byteCount);
    engine.capturedRealFrame_ = true;
    return Status::ok();
}

Status captureDxgiFrame(CaptureEngine& engine, CapturedFrame& frame) {
    auto* duplication = static_cast<IDXGIOutputDuplication*>(engine.dxgiDuplication_);
    auto* context = static_cast<ID3D11DeviceContext*>(engine.d3dContext_);
    auto* stagingTexture = static_cast<ID3D11Texture2D*>(engine.stagingTexture_);
    if (duplication == nullptr || context == nullptr || stagingTexture == nullptr) {
        return Status::invalidState("DXGI capture backend is not initialized");
    }

    DXGI_OUTDUPL_FRAME_INFO frameInfo{};
    IDXGIResource* desktopResource = nullptr;
    HRESULT hr = duplication->AcquireNextFrame(1, &frameInfo, &desktopResource);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        if (!engine.capturedRealFrame_) {
            if (engine.memoryDc_ == nullptr) {
                auto gdiStatus = startGdiCapture(engine, protocol::Resolution{frame.width, frame.height, 0});
                if (!gdiStatus.isOk()) {
                    return gdiStatus;
                }
            }
            return captureGdiFrame(engine, frame);
        }
        frame = engine.latestFrame_;
        return Status::ok();
    }
    if (hr == DXGI_ERROR_ACCESS_LOST) {
        return Status::unavailable("DXGI_ACCESS_LOST");
    }
    if (FAILED(hr)) {
        return Status::unavailable(hresultText(hr, "AcquireNextFrame"));
    }

    Microsoft::WRL::ComPtr<ID3D11Texture2D> desktopTexture;
    hr = desktopResource->QueryInterface(IID_PPV_ARGS(&desktopTexture));
    desktopResource->Release();
    if (FAILED(hr)) {
        duplication->ReleaseFrame();
        return Status::unavailable(hresultText(hr, "QueryInterface(ID3D11Texture2D)"));
    }

    context->CopyResource(stagingTexture, desktopTexture.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    hr = context->Map(stagingTexture, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        duplication->ReleaseFrame();
        return Status::unavailable(hresultText(hr, "Map(stagingTexture)"));
    }

    frame.bgra.resize(static_cast<std::size_t>(frame.width) * frame.height * 4);
    scaleBgra(
        static_cast<const std::uint8_t*>(mapped.pData),
        engine.sourceWidth_,
        engine.sourceHeight_,
        mapped.RowPitch,
        frame.bgra.data(),
        frame.width,
        frame.height);

    context->Unmap(stagingTexture, 0);
    duplication->ReleaseFrame();
    engine.capturedRealFrame_ = true;
    return Status::ok();
}

}  // namespace
#endif

namespace {

Status startCapture(CaptureEngine& engine, const protocol::Resolution& resolution, bool captureRegion, int originX, int originY, std::string deviceName) {
    if (resolution.width == 0 || resolution.height == 0) {
        return Status::invalidArgument("capture resolution must be non-zero");
    }

    engine.running_ = true;
    engine.captureRegion_ = captureRegion;
    engine.capturedRealFrame_ = false;
    engine.sourceOriginX_ = originX;
    engine.sourceOriginY_ = originY;
    engine.sourceDeviceName_ = std::move(deviceName);
    engine.sourceWidth_ = resolution.width;
    engine.sourceHeight_ = resolution.height;
    engine.latestFrame_ = CapturedFrame{};
    engine.latestFrame_.width = resolution.width;
    engine.latestFrame_.height = resolution.height;
    engine.latestFrame_.bgra.assign(static_cast<std::size_t>(resolution.width) * resolution.height * 4, 0);
    engine.startTime_ = std::chrono::steady_clock::now();

#if defined(_WIN32)
    auto dxgiStatus = startDxgiCapture(engine, resolution, captureRegion, originX, originY);
    if (dxgiStatus.isOk()) {
        logInfo("host capture engine started with DXGI Desktop Duplication backend");
        return Status::ok();
    }
    logWarn("DXGI Desktop Duplication unavailable, falling back to GDI capture: " + dxgiStatus.message());
    auto gdiStatus = startGdiCapture(engine, resolution);
    if (!gdiStatus.isOk()) {
        engine.running_ = false;
        return gdiStatus;
    }
    logInfo("host capture engine started with GDI desktop capture backend");
#else
    logInfo("host capture engine started with placeholder D3D11 path");
#endif
    return Status::ok();
}

}  // namespace

Status CaptureEngine::start(const protocol::Resolution& resolution) {
    return startCapture(*this, resolution, false, 0, 0, {});
}

Status CaptureEngine::startRegion(const protocol::Resolution& resolution, int originX, int originY, std::string deviceName) {
    return startCapture(*this, resolution, true, originX, originY, std::move(deviceName));
}

Status CaptureEngine::captureNextFrame(CapturedFrame& frame) {
    if (!running_) {
        return Status::invalidState("cannot capture frame while capture engine is stopped");
    }

    const auto elapsed = std::chrono::steady_clock::now() - startTime_;
    latestFrame_.frameId += 1;
    latestFrame_.timestampUs = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count());

#if defined(_WIN32)
    if (dxgiCapture_) {
        auto status = captureDxgiFrame(*this, latestFrame_);
        if (!status.isOk()) {
            logWarn("DXGI capture failed, trying to reinitialize Desktop Duplication: " + status.message());
            const auto restartOriginX = sourceOriginX_;
            const auto restartOriginY = sourceOriginY_;
            stopDxgiCapture(*this);
            auto restartStatus = startDxgiCapture(
                *this,
                protocol::Resolution{latestFrame_.width, latestFrame_.height, 0},
                captureRegion_,
                restartOriginX,
                restartOriginY);
            if (restartStatus.isOk()) {
                status = captureDxgiFrame(*this, latestFrame_);
                if (status.isOk()) {
                    frame = latestFrame_;
                    return Status::ok();
                }
            }
            const auto dxgiFailure = restartStatus.isOk() ? status.message() : restartStatus.message();
            logWarn("DXGI reinitialize failed, switching to GDI capture: " + dxgiFailure);
            auto gdiStatus = startGdiCapture(*this, protocol::Resolution{
                latestFrame_.width,
                latestFrame_.height,
                0,
            });
            if (!gdiStatus.isOk()) {
                return gdiStatus;
            }
            logInfo("host capture engine switched to GDI desktop capture backend");
        } else {
            frame = latestFrame_;
            return Status::ok();
        }
    }

    auto gdiStatus = captureGdiFrame(*this, latestFrame_);
    if (!gdiStatus.isOk()) {
        return gdiStatus;
    }
#endif

    frame = latestFrame_;
    return Status::ok();
}

Status CaptureEngine::stop() {
    running_ = false;
#if defined(_WIN32)
    stopDxgiCapture(*this);
    stopGdiCapture(*this);
#endif
    logInfo("host capture engine stopped");
    return Status::ok();
}

bool CaptureEngine::isRunning() const {
    return running_;
}

CapturedFrame CaptureEngine::latestFrame() const {
    return latestFrame_;
}

}  // namespace led::host
