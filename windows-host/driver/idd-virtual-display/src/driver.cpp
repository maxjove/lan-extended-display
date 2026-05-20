#include <windows.h>
#include <wdf.h>
#include <IddCx.h>
#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_6.h>
#include <initguid.h>

namespace {

constexpr UINT kModeCount = 3;
constexpr UINT64 kPipelineRate1080p60 = 1920ull * 1080ull * 60ull;
constexpr UINT64 kPipelineRate720p60 = 1280ull * 720ull * 60ull;
constexpr UINT64 kPipelineRate1080p30 = 1920ull * 1080ull * 30ull;
constexpr PCWSTR kActiveMonitorEventName = L"Global\\LanExtendedDisplayActiveMonitor";
constexpr PCWSTR kCreateMonitorEventName = L"Global\\LanExtendedDisplayCreateMonitor";
constexpr PCWSTR kDestroyMonitorEventName = L"Global\\LanExtendedDisplayDestroyMonitor";

const GUID kMonitorContainerId =
    {0x7d3e54b4, 0x8117, 0x4f35, {0x9e, 0xb9, 0xa1, 0x71, 0x12, 0xb0, 0x44, 0x61}};

struct SwapChainPump {
    IDDCX_SWAPCHAIN swapChain = nullptr;
    HANDLE surfaceEvent = nullptr;
    HANDLE stopEvent = nullptr;
    HANDLE thread = nullptr;
    DWORD threadId = 0;
    ID3D11Device* d3dDevice = nullptr;
    IDXGIDevice* dxgiDevice = nullptr;
};

struct ControlChannel {
    HANDLE activeEvent = nullptr;
    HANDLE createEvent = nullptr;
    HANDLE destroyEvent = nullptr;
    HANDLE stopEvent = nullptr;
    HANDLE thread = nullptr;
};

struct DeviceState {
    WDFDEVICE wdfDevice = nullptr;
    IDDCX_ADAPTER adapter = nullptr;
    IDDCX_MONITOR monitor = nullptr;
    SwapChainPump pump;
    ControlChannel control;
};

DeviceState g_state;
CRITICAL_SECTION g_stateLock;
INIT_ONCE g_lockInitOnce = INIT_ONCE_STATIC_INIT;

BOOL CALLBACK InitLockOnce(PINIT_ONCE, PVOID, PVOID*) {
    InitializeCriticalSection(&g_stateLock);
    return TRUE;
}

void EnsureLock() {
    InitOnceExecuteOnce(&g_lockInitOnce, InitLockOnce, nullptr, nullptr);
}

void DebugLog(PCWSTR message) {
    OutputDebugStringW(L"[led_idd] ");
    OutputDebugStringW(message);
    OutputDebugStringW(L"\n");

    CreateDirectoryW(L"C:\\ProgramData\\lan-extended-display", nullptr);

    HANDLE file = CreateFileW(
        L"C:\\ProgramData\\lan-extended-display\\led_idd.log",
        FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }

    SYSTEMTIME now = {};
    GetLocalTime(&now);

    wchar_t line[512] = {};
    const int count = wsprintfW(
        line,
        L"%04u-%02u-%02u %02u:%02u:%02u.%03u %s\r\n",
        now.wYear,
        now.wMonth,
        now.wDay,
        now.wHour,
        now.wMinute,
        now.wSecond,
        now.wMilliseconds,
        message);
    if (count > 0) {
        DWORD written = 0;
        WriteFile(file, line, static_cast<DWORD>(count * sizeof(wchar_t)), &written, nullptr);
    }
    CloseHandle(file);
}

void DebugLogStatus(PCWSTR operation, NTSTATUS status) {
    wchar_t message[256] = {};
    wsprintfW(message, L"%s status=0x%08X", operation, static_cast<unsigned long>(status));
    DebugLog(message);
}

void DebugLogHresult(PCWSTR operation, HRESULT hr) {
    wchar_t message[256] = {};
    wsprintfW(message, L"%s hr=0x%08X", operation, static_cast<unsigned long>(hr));
    DebugLog(message);
}

DISPLAYCONFIG_VIDEO_SIGNAL_INFO MakeSignalInfo(UINT width, UINT height, UINT refreshHz, bool monitorMode) {
    DISPLAYCONFIG_VIDEO_SIGNAL_INFO info = {};
    info.activeSize.cx = width;
    info.activeSize.cy = height;
    info.totalSize.cx = width;
    info.totalSize.cy = height;
    info.vSyncFreq.Numerator = refreshHz;
    info.vSyncFreq.Denominator = 1;
    info.hSyncFreq.Numerator = refreshHz * height;
    info.hSyncFreq.Denominator = 1;
    info.pixelRate = static_cast<UINT64>(width) * height * refreshHz;
    info.AdditionalSignalInfo.videoStandard = 255;
    info.AdditionalSignalInfo.vSyncFreqDivider = monitorMode ? 0 : 1;
    info.scanLineOrdering = DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;
    return info;
}

IDDCX_MONITOR_MODE MakeMonitorMode(UINT width, UINT height, UINT refreshHz, IDDCX_MONITOR_MODE_ORIGIN origin) {
    IDDCX_MONITOR_MODE mode = {};
    mode.Size = sizeof(mode);
    mode.Origin = origin;
    mode.MonitorVideoSignalInfo = MakeSignalInfo(width, height, refreshHz, true);
    return mode;
}

IDDCX_TARGET_MODE MakeTargetMode(UINT width, UINT height, UINT refreshHz, UINT64 rate) {
    IDDCX_TARGET_MODE mode = {};
    mode.Size = sizeof(mode);
    mode.TargetVideoSignalInfo.targetVideoSignalInfo = MakeSignalInfo(width, height, refreshHz, false);
    mode.RequiredBandwidth = rate;
    return mode;
}

void FillMonitorModes(
    IDDCX_MONITOR_MODE* modes,
    UINT capacity,
    IDDCX_MONITOR_MODE_ORIGIN origin,
    UINT* outCount,
    UINT* preferredIndex) {
    IDDCX_MONITOR_MODE localModes[kModeCount] = {
        MakeMonitorMode(1920, 1080, 60, origin),
        MakeMonitorMode(1280, 720, 60, origin),
        MakeMonitorMode(1920, 1080, 30, origin),
    };

    const UINT copyCount = capacity < kModeCount ? capacity : kModeCount;
    for (UINT i = 0; i < copyCount; ++i) {
        modes[i] = localModes[i];
    }

    *outCount = kModeCount;
    *preferredIndex = 0;
}

void FillTargetModes(IDDCX_TARGET_MODE* modes, UINT capacity, UINT* outCount) {
    IDDCX_TARGET_MODE localModes[kModeCount] = {
        MakeTargetMode(1920, 1080, 60, kPipelineRate1080p60),
        MakeTargetMode(1280, 720, 60, kPipelineRate720p60),
        MakeTargetMode(1920, 1080, 30, kPipelineRate1080p30),
    };

    const UINT copyCount = capacity < kModeCount ? capacity : kModeCount;
    for (UINT i = 0; i < copyCount; ++i) {
        modes[i] = localModes[i];
    }

    *outCount = kModeCount;
}

DWORD WINAPI SwapChainThreadProc(void* context) {
    auto* pump = static_cast<SwapChainPump*>(context);
    IDDCX_SWAPCHAIN swapChain = pump->swapChain;

    IDARG_IN_SWAPCHAINSETDEVICE setDevice = {};
    setDevice.pDevice = pump->dxgiDevice;
    HRESULT hr = IddCxSwapChainSetDevice(swapChain, &setDevice);
    if (FAILED(hr)) {
        DebugLogHresult(L"IddCxSwapChainSetDevice failed", hr);
        WdfObjectDelete(reinterpret_cast<WDFOBJECT>(swapChain));
        return 0;
    }
    DebugLog(L"Swap-chain device set");

    HANDLE waitHandles[2] = {pump->surfaceEvent, pump->stopEvent};

    for (;;) {
        if (WaitForSingleObject(pump->stopEvent, 0) == WAIT_OBJECT_0) {
            break;
        }

        IDARG_OUT_RELEASEANDACQUIREBUFFER buffer = {};
        buffer.MetaData.Size = sizeof(buffer.MetaData);
        const HRESULT acquireResult = IddCxSwapChainReleaseAndAcquireBuffer(swapChain, &buffer);
        if (acquireResult == E_PENDING) {
            const DWORD wait = WaitForMultipleObjects(2, waitHandles, FALSE, 16);
            if (wait == WAIT_OBJECT_0 + 1) {
                break;
            }
            continue;
        }
        if (SUCCEEDED(acquireResult)) {
            if (buffer.MetaData.pSurface != nullptr) {
                buffer.MetaData.pSurface->Release();
            }
            IddCxSwapChainFinishedProcessingFrame(swapChain);
            continue;
        }

        DebugLogHresult(L"IddCxSwapChainReleaseAndAcquireBuffer failed", acquireResult);
        break;
    }

    WdfObjectDelete(reinterpret_cast<WDFOBJECT>(swapChain));
    return 0;
}

void StopSwapChainPump(SwapChainPump* pump) {
    if (pump->stopEvent != nullptr) {
        SetEvent(pump->stopEvent);
    }

    if (pump->thread != nullptr && pump->threadId != GetCurrentThreadId()) {
        const DWORD wait = WaitForSingleObject(pump->thread, 5000);
        if (wait == WAIT_TIMEOUT) {
            DebugLog(L"Timed out waiting for swap-chain thread to stop");
            return;
        }
    }

    if (pump->thread != nullptr) {
        CloseHandle(pump->thread);
        pump->thread = nullptr;
        pump->threadId = 0;
    }

    if (pump->stopEvent != nullptr) {
        CloseHandle(pump->stopEvent);
        pump->stopEvent = nullptr;
    }

    if (pump->dxgiDevice != nullptr) {
        pump->dxgiDevice->Release();
        pump->dxgiDevice = nullptr;
    }

    if (pump->d3dDevice != nullptr) {
        pump->d3dDevice->Release();
        pump->d3dDevice = nullptr;
    }

    pump->surfaceEvent = nullptr;
    pump->swapChain = nullptr;
}

NTSTATUS StartSwapChainPump(SwapChainPump* pump, const IDARG_IN_SETSWAPCHAIN* args) {
    StopSwapChainPump(pump);

    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;

    IDXGIFactory4* factory = nullptr;
    HRESULT hr = CreateDXGIFactory2(0, __uuidof(IDXGIFactory4), reinterpret_cast<void**>(&factory));
    if (FAILED(hr) || factory == nullptr) {
        DebugLogHresult(L"CreateDXGIFactory2 failed", hr);
        return STATUS_UNSUCCESSFUL;
    }

    IDXGIAdapter1* adapter = nullptr;
    hr = factory->EnumAdapterByLuid(args->RenderAdapterLuid, __uuidof(IDXGIAdapter1), reinterpret_cast<void**>(&adapter));
    if (FAILED(hr) || adapter == nullptr) {
        factory->Release();
        DebugLogHresult(L"EnumAdapterByLuid failed", hr);
        return STATUS_UNSUCCESSFUL;
    }

    D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_0;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    hr = D3D11CreateDevice(
        adapter,
        D3D_DRIVER_TYPE_UNKNOWN,
        nullptr,
        flags,
        nullptr,
        0,
        D3D11_SDK_VERSION,
        &device,
        &featureLevel,
        &context);

    if (context != nullptr) {
        context->Release();
    }

    if (FAILED(hr) || device == nullptr) {
        adapter->Release();
        factory->Release();
        DebugLogHresult(L"D3D11CreateDevice failed", hr);
        return STATUS_UNSUCCESSFUL;
    }

    IDXGIDevice* dxgiDevice = nullptr;
    hr = device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDevice));
    if (FAILED(hr) || dxgiDevice == nullptr) {
        device->Release();
        adapter->Release();
        factory->Release();
        DebugLogHresult(L"IDXGIDevice query failed", hr);
        return STATUS_UNSUCCESSFUL;
    }

    adapter->Release();
    factory->Release();

    pump->swapChain = args->hSwapChain;
    pump->surfaceEvent = args->hNextSurfaceAvailable;
    pump->d3dDevice = device;
    pump->dxgiDevice = dxgiDevice;
    pump->stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (pump->stopEvent == nullptr) {
        StopSwapChainPump(pump);
        return STATUS_UNSUCCESSFUL;
    }

    pump->thread = CreateThread(nullptr, 0, SwapChainThreadProc, pump, 0, &pump->threadId);
    if (pump->thread == nullptr) {
        StopSwapChainPump(pump);
        return STATUS_UNSUCCESSFUL;
    }

    return STATUS_SUCCESS;
}

NTSTATUS CreateVirtualMonitor() {
    if (g_state.monitor != nullptr) {
        return STATUS_SUCCESS;
    }
    if (g_state.adapter == nullptr) {
        return STATUS_DEVICE_NOT_READY;
    }

    IDDCX_MONITOR_DESCRIPTION description = {};
    description.Size = sizeof(description);
    description.Type = IDDCX_MONITOR_DESCRIPTION_TYPE_EDID;
    description.DataSize = 0;
    description.pData = nullptr;

    IDDCX_MONITOR_INFO monitorInfo = {};
    monitorInfo.Size = sizeof(monitorInfo);
    monitorInfo.MonitorType = DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HDMI;
    monitorInfo.ConnectorIndex = 0;
    monitorInfo.MonitorDescription = description;
    monitorInfo.MonitorContainerId = kMonitorContainerId;

    IDARG_IN_MONITORCREATE createIn = {};
    createIn.pMonitorInfo = &monitorInfo;

    IDARG_OUT_MONITORCREATE createOut = {};
    NTSTATUS status = IddCxMonitorCreate(g_state.adapter, &createIn, &createOut);
    if (!NT_SUCCESS(status)) {
        DebugLogStatus(L"IddCxMonitorCreate failed", status);
        return status;
    }

    IDARG_OUT_MONITORARRIVAL arrivalOut = {};
    status = IddCxMonitorArrival(createOut.MonitorObject, &arrivalOut);
    if (!NT_SUCCESS(status)) {
        DebugLogStatus(L"IddCxMonitorArrival failed", status);
        return status;
    }

    g_state.monitor = createOut.MonitorObject;
    DebugLog(L"Virtual monitor arrived");
    return STATUS_SUCCESS;
}

NTSTATUS DestroyVirtualMonitor() {
    EnsureLock();
    EnterCriticalSection(&g_stateLock);
    const IDDCX_MONITOR monitor = g_state.monitor;
    if (monitor == nullptr) {
        LeaveCriticalSection(&g_stateLock);
        return STATUS_SUCCESS;
    }
    g_state.monitor = nullptr;
    LeaveCriticalSection(&g_stateLock);

    StopSwapChainPump(&g_state.pump);
    const NTSTATUS status = IddCxMonitorDeparture(monitor);
    if (!NT_SUCCESS(status)) {
        DebugLogStatus(L"IddCxMonitorDeparture failed", status);
        return status;
    }

    DebugLog(L"Virtual monitor departed");
    return STATUS_SUCCESS;
}

NTSTATUS RetryCreateVirtualMonitor() {
    NTSTATUS status = STATUS_DEVICE_NOT_READY;
    for (int attempt = 0; attempt < 50; ++attempt) {
        EnsureLock();
        EnterCriticalSection(&g_stateLock);
        status = CreateVirtualMonitor();
        LeaveCriticalSection(&g_stateLock);
        if (NT_SUCCESS(status)) {
            return status;
        }
        Sleep(100);
    }
    return status;
}

NTSTATUS RequestDestroyVirtualMonitor() {
    return DestroyVirtualMonitor();
}

bool ActiveMonitorRequested() {
    return g_state.control.activeEvent != nullptr &&
        WaitForSingleObject(g_state.control.activeEvent, 0) == WAIT_OBJECT_0;
}

DWORD WINAPI ControlThreadProc(void*) {
    DebugLog(L"Control thread started");
    HANDLE waitHandles[3] = {
        g_state.control.stopEvent,
        g_state.control.createEvent,
        g_state.control.destroyEvent,
    };

    for (;;) {
        const DWORD wait = WaitForMultipleObjects(3, waitHandles, FALSE, INFINITE);
        if (wait == WAIT_OBJECT_0) {
            break;
        }
        if (wait == WAIT_OBJECT_0 + 1) {
            DebugLog(L"Create monitor requested");
            if (g_state.control.activeEvent != nullptr) {
                SetEvent(g_state.control.activeEvent);
            }
            const NTSTATUS status = RetryCreateVirtualMonitor();
            if (!NT_SUCCESS(status)) {
                DebugLogStatus(L"Create monitor request failed", status);
            }
            continue;
        }
        if (wait == WAIT_OBJECT_0 + 2) {
            DebugLog(L"Destroy monitor requested");
            if (g_state.control.activeEvent != nullptr) {
                ResetEvent(g_state.control.activeEvent);
            }
            const NTSTATUS status = RequestDestroyVirtualMonitor();
            if (!NT_SUCCESS(status)) {
                DebugLogStatus(L"Destroy monitor request failed", status);
            }
            continue;
        }
    }

    DebugLog(L"Control thread stopped");
    return 0;
}

bool CreateWorldWritableEvent(PCWSTR name, bool manualReset, HANDLE* outHandle) {
    SECURITY_DESCRIPTOR descriptor{};
    if (!InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION)) {
        return false;
    }
    if (!SetSecurityDescriptorDacl(&descriptor, TRUE, nullptr, FALSE)) {
        return false;
    }

    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.lpSecurityDescriptor = &descriptor;
    attributes.bInheritHandle = FALSE;

    *outHandle = CreateEventW(&attributes, manualReset, FALSE, name);
    return *outHandle != nullptr;
}

NTSTATUS StartControlChannel(ControlChannel* control) {
    if (control->thread != nullptr) {
        return STATUS_SUCCESS;
    }

    if (!CreateWorldWritableEvent(kActiveMonitorEventName, true, &control->activeEvent) ||
        !CreateWorldWritableEvent(kCreateMonitorEventName, false, &control->createEvent) ||
        !CreateWorldWritableEvent(kDestroyMonitorEventName, false, &control->destroyEvent)) {
        DebugLog(L"Create control events failed");
        return STATUS_UNSUCCESSFUL;
    }

    control->stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (control->stopEvent == nullptr) {
        DebugLog(L"Create control stop event failed");
        return STATUS_UNSUCCESSFUL;
    }

    control->thread = CreateThread(nullptr, 0, ControlThreadProc, nullptr, 0, nullptr);
    if (control->thread == nullptr) {
        DebugLog(L"Create control thread failed");
        return STATUS_UNSUCCESSFUL;
    }

    return STATUS_SUCCESS;
}

void StopControlChannel(ControlChannel* control) {
    if (control->stopEvent != nullptr) {
        SetEvent(control->stopEvent);
    }

    if (control->thread != nullptr) {
        WaitForSingleObject(control->thread, 5000);
        CloseHandle(control->thread);
        control->thread = nullptr;
    }

    if (control->stopEvent != nullptr) {
        CloseHandle(control->stopEvent);
        control->stopEvent = nullptr;
    }
    if (control->createEvent != nullptr) {
        CloseHandle(control->createEvent);
        control->createEvent = nullptr;
    }
    if (control->destroyEvent != nullptr) {
        CloseHandle(control->destroyEvent);
        control->destroyEvent = nullptr;
    }
    if (control->activeEvent != nullptr) {
        CloseHandle(control->activeEvent);
        control->activeEvent = nullptr;
    }
}

NTSTATUS NTAPI EvtIddAdapterInitFinished(
    IDDCX_ADAPTER adapter,
    const IDARG_IN_ADAPTER_INIT_FINISHED* args) {
    UNREFERENCED_PARAMETER(adapter);

    if (!NT_SUCCESS(args->AdapterInitStatus)) {
        DebugLogStatus(L"Adapter init failed", args->AdapterInitStatus);
        return args->AdapterInitStatus;
    }

    EnsureLock();
    EnterCriticalSection(&g_stateLock);
    g_state.adapter = adapter;
    NTSTATUS status = STATUS_SUCCESS;
    if (ActiveMonitorRequested()) {
        DebugLog(L"Active monitor requested during adapter init");
        status = CreateVirtualMonitor();
    }
    LeaveCriticalSection(&g_stateLock);
    DebugLog(L"Adapter initialized");
    return status;
}

NTSTATUS NTAPI EvtIddAdapterCommitModes(
    IDDCX_ADAPTER adapter,
    const IDARG_IN_COMMITMODES* args) {
    UNREFERENCED_PARAMETER(adapter);
    UNREFERENCED_PARAMETER(args);
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI EvtIddGetDefaultDescriptionModes(
    IDDCX_MONITOR monitor,
    const IDARG_IN_GETDEFAULTDESCRIPTIONMODES* args,
    IDARG_OUT_GETDEFAULTDESCRIPTIONMODES* outArgs) {
    UNREFERENCED_PARAMETER(monitor);
    UINT preferred = 0;
    outArgs->DefaultMonitorModeBufferOutputCount = kModeCount;
    outArgs->PreferredMonitorModeIdx = preferred;

    if (args->DefaultMonitorModeBufferInputCount < kModeCount) {
        return args->DefaultMonitorModeBufferInputCount == 0 ? STATUS_SUCCESS : STATUS_BUFFER_TOO_SMALL;
    }

    UINT count = 0;
    FillMonitorModes(
        args->pDefaultMonitorModes,
        args->DefaultMonitorModeBufferInputCount,
        IDDCX_MONITOR_MODE_ORIGIN_DRIVER,
        &count,
        &preferred);
    outArgs->DefaultMonitorModeBufferOutputCount = count;
    outArgs->PreferredMonitorModeIdx = preferred;
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI EvtIddParseMonitorDescription(
    const IDARG_IN_PARSEMONITORDESCRIPTION* args,
    IDARG_OUT_PARSEMONITORDESCRIPTION* outArgs) {
    UINT preferred = 0;
    outArgs->MonitorModeBufferOutputCount = kModeCount;
    outArgs->PreferredMonitorModeIdx = preferred;

    if (args->MonitorModeBufferInputCount < kModeCount) {
        return args->MonitorModeBufferInputCount == 0 ? STATUS_SUCCESS : STATUS_BUFFER_TOO_SMALL;
    }

    UINT count = 0;
    FillMonitorModes(
        args->pMonitorModes,
        args->MonitorModeBufferInputCount,
        IDDCX_MONITOR_MODE_ORIGIN_MONITORDESCRIPTOR,
        &count,
        &preferred);
    outArgs->MonitorModeBufferOutputCount = count;
    outArgs->PreferredMonitorModeIdx = preferred;
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI EvtIddQueryTargetModes(
    IDDCX_MONITOR monitor,
    const IDARG_IN_QUERYTARGETMODES* args,
    IDARG_OUT_QUERYTARGETMODES* outArgs) {
    UNREFERENCED_PARAMETER(monitor);
    outArgs->TargetModeBufferOutputCount = kModeCount;

    if (args->TargetModeBufferInputCount < kModeCount) {
        return args->TargetModeBufferInputCount == 0 ? STATUS_SUCCESS : STATUS_BUFFER_TOO_SMALL;
    }

    UINT count = 0;
    FillTargetModes(args->pTargetModes, args->TargetModeBufferInputCount, &count);
    outArgs->TargetModeBufferOutputCount = count;
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI EvtIddAssignSwapChain(
    IDDCX_MONITOR monitor,
    const IDARG_IN_SETSWAPCHAIN* args) {
    UNREFERENCED_PARAMETER(monitor);
    DebugLog(L"Assign swap-chain requested");
    EnsureLock();
    EnterCriticalSection(&g_stateLock);
    const NTSTATUS status = StartSwapChainPump(&g_state.pump, args);
    LeaveCriticalSection(&g_stateLock);
    return status;
}

NTSTATUS NTAPI EvtIddUnassignSwapChain(IDDCX_MONITOR monitor) {
    UNREFERENCED_PARAMETER(monitor);
    DebugLog(L"Unassign swap-chain requested");
    StopSwapChainPump(&g_state.pump);
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI EvtDeviceD0Entry(WDFDEVICE device, WDF_POWER_DEVICE_STATE previousState) {
    UNREFERENCED_PARAMETER(previousState);
    EnsureLock();

    EnterCriticalSection(&g_stateLock);
    if (g_state.adapter != nullptr) {
        LeaveCriticalSection(&g_stateLock);
        return STATUS_SUCCESS;
    }
    LeaveCriticalSection(&g_stateLock);

    IDDCX_ENDPOINT_VERSION hardwareVersion = {};
    hardwareVersion.Size = sizeof(hardwareVersion);
    hardwareVersion.MajorVer = 0;
    hardwareVersion.MinorVer = 1;

    IDDCX_ENDPOINT_VERSION firmwareVersion = hardwareVersion;

    IDDCX_ADAPTER_CAPS caps = {};
    caps.Size = sizeof(caps);
    caps.Flags = IDDCX_ADAPTER_FLAGS_USE_SMALLEST_MODE;
    caps.MaxDisplayPipelineRate = kPipelineRate1080p60 + kPipelineRate720p60;
    caps.MaxMonitorsSupported = 1;
    caps.StaticDesktopReencodeFrameCount = 2;
    caps.EndPointDiagnostics.Size = sizeof(caps.EndPointDiagnostics);
    caps.EndPointDiagnostics.TransmissionType = IDDCX_TRANSMISSION_TYPE_OTHER;
    caps.EndPointDiagnostics.pEndPointFriendlyName = L"LAN Extended Display";
    caps.EndPointDiagnostics.pEndPointModelName = L"LED Virtual Display";
    caps.EndPointDiagnostics.pEndPointManufacturerName = L"lan-extended-display";
    caps.EndPointDiagnostics.pHardwareVersion = &hardwareVersion;
    caps.EndPointDiagnostics.pFirmwareVersion = &firmwareVersion;
    caps.EndPointDiagnostics.GammaSupport = IDDCX_FEATURE_IMPLEMENTATION_NONE;

    IDARG_IN_ADAPTER_INIT adapterInit = {};
    adapterInit.WdfDevice = device;
    adapterInit.pCaps = &caps;

    IDARG_OUT_ADAPTER_INIT adapterOut = {};
    NTSTATUS status = IddCxAdapterInitAsync(&adapterInit, &adapterOut);
    if (!NT_SUCCESS(status)) {
        DebugLogStatus(L"IddCxAdapterInitAsync failed", status);
        return status;
    }

    EnterCriticalSection(&g_stateLock);
    g_state.wdfDevice = device;
    g_state.adapter = adapterOut.AdapterObject;
    LeaveCriticalSection(&g_stateLock);

    DebugLog(L"Device added");
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI EvtDriverDeviceAdd(WDFDRIVER driver, PWDFDEVICE_INIT deviceInit) {
    UNREFERENCED_PARAMETER(driver);
    EnsureLock();

    IDD_CX_CLIENT_CONFIG iddConfig;
    IDD_CX_CLIENT_CONFIG_INIT(&iddConfig);
    iddConfig.EvtIddCxAdapterInitFinished = EvtIddAdapterInitFinished;
    iddConfig.EvtIddCxAdapterCommitModes = EvtIddAdapterCommitModes;
    iddConfig.EvtIddCxParseMonitorDescription = EvtIddParseMonitorDescription;
    iddConfig.EvtIddCxMonitorGetDefaultDescriptionModes = EvtIddGetDefaultDescriptionModes;
    iddConfig.EvtIddCxMonitorQueryTargetModes = EvtIddQueryTargetModes;
    iddConfig.EvtIddCxMonitorAssignSwapChain = EvtIddAssignSwapChain;
    iddConfig.EvtIddCxMonitorUnassignSwapChain = EvtIddUnassignSwapChain;

    NTSTATUS status = IddCxDeviceInitConfig(deviceInit, &iddConfig);
    if (!NT_SUCCESS(status)) {
        DebugLogStatus(L"IddCxDeviceInitConfig failed", status);
        return status;
    }

    WDF_PNPPOWER_EVENT_CALLBACKS pnpPowerCallbacks;
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpPowerCallbacks);
    pnpPowerCallbacks.EvtDeviceD0Entry = EvtDeviceD0Entry;
    WdfDeviceInitSetPnpPowerEventCallbacks(deviceInit, &pnpPowerCallbacks);

    WDF_OBJECT_ATTRIBUTES deviceAttributes;
    WDF_OBJECT_ATTRIBUTES_INIT(&deviceAttributes);

    WDFDEVICE device = nullptr;
    status = WdfDeviceCreate(&deviceInit, &deviceAttributes, &device);
    if (!NT_SUCCESS(status)) {
        DebugLogStatus(L"WdfDeviceCreate failed", status);
        return status;
    }

    status = IddCxDeviceInitialize(device);
    if (!NT_SUCCESS(status)) {
        DebugLogStatus(L"IddCxDeviceInitialize failed", status);
        return status;
    }

    EnterCriticalSection(&g_stateLock);
    g_state.wdfDevice = device;
    LeaveCriticalSection(&g_stateLock);

    status = StartControlChannel(&g_state.control);
    if (!NT_SUCCESS(status)) {
        DebugLogStatus(L"StartControlChannel failed", status);
        return status;
    }

    DebugLog(L"Device created");
    return STATUS_SUCCESS;
}

void NTAPI EvtDriverUnload(WDFDRIVER driver) {
    UNREFERENCED_PARAMETER(driver);
    DestroyVirtualMonitor();
    StopSwapChainPump(&g_state.pump);
    StopControlChannel(&g_state.control);
    DebugLog(L"Driver unload");
}

}  // namespace

extern "C" DRIVER_INITIALIZE DriverEntry;

extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT driverObject, PUNICODE_STRING registryPath) {
    EnsureLock();

    WDF_DRIVER_CONFIG config;
    WDF_DRIVER_CONFIG_INIT(&config, EvtDriverDeviceAdd);
    config.EvtDriverUnload = EvtDriverUnload;

    NTSTATUS status = WdfDriverCreate(driverObject, registryPath, WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
    if (!NT_SUCCESS(status)) {
        DebugLogStatus(L"WdfDriverCreate failed", status);
        return status;
    }

    DebugLog(L"DriverEntry complete");
    return STATUS_SUCCESS;
}
