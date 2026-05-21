#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kHostExitedMessage = WM_APP + 2;
constexpr UINT_PTR kTrayIconId = 1;
constexpr int kMenuStart = 1001;
constexpr int kMenuStop = 1002;
constexpr int kMenuExit = 1003;
constexpr int kMenuManualIp = 1004;
constexpr int kMenuRefreshDevices = 1005;
constexpr int kMenuOpenLog = 1006;
constexpr int kMenuInstallFirewall = 1007;
constexpr int kMenuQuality45 = 1010;
constexpr int kMenuQuality55 = 1011;
constexpr int kMenuQuality65 = 1012;
constexpr int kMenuDeviceBase = 2000;
constexpr int kMaxDeviceMenuItems = 64;
constexpr UINT_PTR kMonitorTimerId = 2001;
constexpr std::uint16_t kDiscoveryPort = 17659;
constexpr const wchar_t* kWindowClassName = L"LanExtendedDisplayTrayWindow";
constexpr const wchar_t* kStopEventName = L"Local\\LanExtendedDisplayHostStop";
constexpr const wchar_t* kDriverActiveMonitorEventName = L"Global\\LanExtendedDisplayActiveMonitor";
constexpr const wchar_t* kDriverCreateMonitorEventName = L"Global\\LanExtendedDisplayCreateMonitor";
constexpr const wchar_t* kDriverDestroyMonitorEventName = L"Global\\LanExtendedDisplayDestroyMonitor";

struct HostProcess {
    PROCESS_INFORMATION process{};
    bool started{false};
};

HostProcess g_host;
HANDLE g_hostStdout{nullptr};
HANDLE g_hostStderr{nullptr};
HANDLE g_activeMonitorEvent{nullptr};
bool g_virtualDisplayInstalled{false};
std::atomic_bool g_discoveryStop{false};
std::thread g_discoveryThread;
std::mutex g_devicesMutex;
HICON g_trayIcon{nullptr};
int g_jpegQuality = 55;
std::uint64_t g_hostGeneration = 0;

struct ClientDevice {
    std::string address;
    std::string name;
    std::string status;
    DWORD lastSeenTick{0};
};

std::vector<ClientDevice> g_devices;
std::string g_selectedClientIp;

class WinsockRuntime {
public:
    WinsockRuntime() {
        WSADATA data{};
        WSAStartup(MAKEWORD(2, 2), &data);
    }

    ~WinsockRuntime() {
        WSACleanup();
    }
};

void ensureWinsock() {
    static WinsockRuntime runtime;
    (void)runtime;
}

void logTray(const wchar_t* format, ...) {
    wchar_t directory[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, directory, ARRAYSIZE(directory)) == 0) {
        return;
    }
    std::wstring path(directory);
    const auto slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        path.resize(slash + 1);
    } else {
        path.clear();
    }
    path += L"led_host_tray.log";

    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"a, ccs=UTF-8") != 0 || file == nullptr) {
        return;
    }

    SYSTEMTIME now{};
    GetLocalTime(&now);
    fwprintf(
        file,
        L"%04u-%02u-%02u %02u:%02u:%02u.%03u ",
        now.wYear,
        now.wMonth,
        now.wDay,
        now.wHour,
        now.wMinute,
        now.wSecond,
        now.wMilliseconds);

    va_list args;
    va_start(args, format);
    vfwprintf(file, format, args);
    va_end(args);
    fwprintf(file, L"\n");
    fclose(file);
}

HICON createTrayIcon() {
    constexpr int kSize = 32;
    HDC screen = GetDC(nullptr);
    if (screen == nullptr) {
        return LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
    }

    HDC dc = CreateCompatibleDC(screen);
    HBITMAP colorBitmap = CreateCompatibleBitmap(screen, kSize, kSize);
    HBITMAP maskBitmap = CreateBitmap(kSize, kSize, 1, 1, nullptr);
    ReleaseDC(nullptr, screen);
    if (dc == nullptr || colorBitmap == nullptr || maskBitmap == nullptr) {
        if (dc != nullptr) {
            DeleteDC(dc);
        }
        if (colorBitmap != nullptr) {
            DeleteObject(colorBitmap);
        }
        if (maskBitmap != nullptr) {
            DeleteObject(maskBitmap);
        }
        return LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
    }

    HDC maskDc = CreateCompatibleDC(nullptr);
    if (maskDc != nullptr) {
        auto* oldMask = SelectObject(maskDc, maskBitmap);
        PatBlt(maskDc, 0, 0, kSize, kSize, BLACKNESS);
        SelectObject(maskDc, oldMask);
        DeleteDC(maskDc);
    }

    auto* oldBitmap = SelectObject(dc, colorBitmap);
    HBRUSH background = CreateSolidBrush(RGB(18, 24, 28));
    RECT full{0, 0, kSize, kSize};
    FillRect(dc, &full, background);
    DeleteObject(background);

    HBRUSH screenBrush = CreateSolidBrush(RGB(43, 174, 190));
    HBRUSH standBrush = CreateSolidBrush(RGB(238, 244, 246));
    HPEN borderPen = CreatePen(PS_SOLID, 2, RGB(238, 244, 246));
    HPEN glowPen = CreatePen(PS_SOLID, 1, RGB(122, 226, 236));

    auto* oldBrush = SelectObject(dc, screenBrush);
    auto* oldPen = SelectObject(dc, borderPen);
    RoundRect(dc, 5, 7, 27, 22, 4, 4);
    SelectObject(dc, glowPen);
    MoveToEx(dc, 8, 10, nullptr);
    LineTo(dc, 24, 10);
    MoveToEx(dc, 8, 13, nullptr);
    LineTo(dc, 20, 13);

    SelectObject(dc, standBrush);
    SelectObject(dc, borderPen);
    Rectangle(dc, 14, 22, 18, 25);
    Rectangle(dc, 10, 25, 22, 28);

    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldBitmap);
    DeleteObject(glowPen);
    DeleteObject(borderPen);
    DeleteObject(screenBrush);
    DeleteObject(standBrush);
    DeleteDC(dc);

    ICONINFO iconInfo{};
    iconInfo.fIcon = TRUE;
    iconInfo.hbmColor = colorBitmap;
    iconInfo.hbmMask = maskBitmap;
    HICON icon = CreateIconIndirect(&iconInfo);
    DeleteObject(colorBitmap);
    DeleteObject(maskBitmap);
    return icon != nullptr ? icon : LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
}

HICON trayIcon() {
    if (g_trayIcon == nullptr) {
        g_trayIcon = createTrayIcon();
    }
    return g_trayIcon;
}

std::string wideToUtf8(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) {
        return {};
    }
    std::string result(static_cast<std::size_t>(size - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, result.data(), size, nullptr, nullptr);
    return result;
}

std::wstring utf8ToWide(const std::string& value) {
    if (value.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
    if (size <= 1) {
        return {};
    }
    std::wstring result(static_cast<std::size_t>(size - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, result.data(), size);
    return result;
}

std::string trimAscii(const std::string& value) {
    const auto begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return {};
    }
    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(begin, end - begin + 1);
}

std::string localComputerName() {
    char name[MAX_COMPUTERNAME_LENGTH + 1]{};
    DWORD size = ARRAYSIZE(name);
    if (GetComputerNameA(name, &size)) {
        return std::string(name, size);
    }
    return "windows-host";
}

std::string parseField(const std::string& message, const std::string& key) {
    const std::string marker = key + "=";
    const auto begin = message.find(marker);
    if (begin == std::string::npos) {
        return {};
    }
    const auto valueBegin = begin + marker.size();
    const auto end = message.find(';', valueBegin);
    return message.substr(valueBegin, end == std::string::npos ? std::string::npos : end - valueBegin);
}

bool isValidIpv4(const std::string& address) {
    sockaddr_in parsed{};
    return inet_pton(AF_INET, address.c_str(), &parsed.sin_addr) == 1;
}

std::string socketAddressToString(const sockaddr_in& address) {
    char buffer[INET_ADDRSTRLEN]{};
    const auto* result = inet_ntop(AF_INET, &address.sin_addr, buffer, sizeof(buffer));
    return result != nullptr ? std::string(result) : std::string("0.0.0.0");
}

std::string localAddressForTarget(const std::string& targetIp) {
    ensureWinsock();
    SOCKET socketHandle = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socketHandle == INVALID_SOCKET) {
        return {};
    }

    sockaddr_in target{};
    target.sin_family = AF_INET;
    target.sin_port = htons(kDiscoveryPort);
    inet_pton(AF_INET, targetIp.c_str(), &target.sin_addr);
    ::connect(socketHandle, reinterpret_cast<const sockaddr*>(&target), sizeof(target));

    sockaddr_in local{};
    int localLength = sizeof(local);
    std::string result;
    if (getsockname(socketHandle, reinterpret_cast<sockaddr*>(&local), &localLength) == 0) {
        result = socketAddressToString(local);
    }
    closesocket(socketHandle);
    return result;
}

bool sendUdpMessage(const std::string& targetIp, std::uint16_t port, const std::string& message, bool broadcast = false) {
    ensureWinsock();
    SOCKET socketHandle = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socketHandle == INVALID_SOCKET) {
        return false;
    }
    if (broadcast) {
        BOOL enabled = TRUE;
        setsockopt(socketHandle, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&enabled), sizeof(enabled));
    }

    sockaddr_in target{};
    target.sin_family = AF_INET;
    target.sin_port = htons(port);
    if (inet_pton(AF_INET, targetIp.c_str(), &target.sin_addr) != 1) {
        closesocket(socketHandle);
        return false;
    }
    const int sent = sendto(
        socketHandle,
        message.data(),
        static_cast<int>(message.size()),
        0,
        reinterpret_cast<const sockaddr*>(&target),
        sizeof(target));
    closesocket(socketHandle);
    return sent == static_cast<int>(message.size());
}

void rememberDevice(const std::string& address, const std::string& name, const std::string& status) {
    if (!isValidIpv4(address)) {
        return;
    }
    std::scoped_lock lock(g_devicesMutex);
    const DWORD now = GetTickCount();
    auto found = std::find_if(g_devices.begin(), g_devices.end(), [&](const ClientDevice& device) {
        return device.address == address;
    });
    if (found == g_devices.end()) {
        g_devices.push_back(ClientDevice{address, name.empty() ? address : name, status.empty() ? "ready" : status, now});
        logTray(
            L"discovered linux client %ls name=%ls status=%ls",
            utf8ToWide(address).c_str(),
            utf8ToWide(name).c_str(),
            utf8ToWide(status).c_str());
    } else {
        found->name = name.empty() ? address : name;
        found->status = status.empty() ? "ready" : status;
        found->lastSeenTick = now;
    }
}

void sendDiscoveryProbe() {
    std::ostringstream message;
    message << "LED_DISCOVER_V1;name=" << localComputerName() << ";";
    sendUdpMessage("255.255.255.255", kDiscoveryPort, message.str(), true);
}

void discoveryLoop() {
    ensureWinsock();
    SOCKET socketHandle = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socketHandle == INVALID_SOCKET) {
        logTray(L"discovery socket create failed: %d", WSAGetLastError());
        return;
    }

    BOOL enabled = TRUE;
    setsockopt(socketHandle, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&enabled), sizeof(enabled));
    setsockopt(socketHandle, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&enabled), sizeof(enabled));
    DWORD timeout = 1000;
    setsockopt(socketHandle, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));

    sockaddr_in bindAddress{};
    bindAddress.sin_family = AF_INET;
    bindAddress.sin_port = htons(kDiscoveryPort);
    bindAddress.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(socketHandle, reinterpret_cast<sockaddr*>(&bindAddress), sizeof(bindAddress)) != 0) {
        logTray(L"discovery bind failed: %d", WSAGetLastError());
        closesocket(socketHandle);
        return;
    }
    logTray(L"discovery listening on udp %u", kDiscoveryPort);

    DWORD lastProbe = 0;
    while (!g_discoveryStop.load()) {
        const DWORD now = GetTickCount();
        if (lastProbe == 0 || static_cast<LONG>(now - lastProbe) >= 3000) {
            sendDiscoveryProbe();
            lastProbe = now;
        }

        char buffer[1024]{};
        sockaddr_in source{};
        int sourceLength = sizeof(source);
        const int received = recvfrom(
            socketHandle,
            buffer,
            static_cast<int>(sizeof(buffer) - 1),
            0,
            reinterpret_cast<sockaddr*>(&source),
            &sourceLength);
        if (received <= 0) {
            continue;
        }
        buffer[received] = '\0';
        std::string message(buffer, static_cast<std::size_t>(received));
        if (message.rfind("LED_CLIENT_V1", 0) == 0) {
            rememberDevice(
                socketAddressToString(source),
                parseField(message, "name"),
                parseField(message, "status"));
        }
    }

    closesocket(socketHandle);
}

void startDiscovery() {
    if (g_discoveryThread.joinable()) {
        return;
    }
    g_discoveryStop = false;
    g_discoveryThread = std::thread(discoveryLoop);
}

void stopDiscovery() {
    g_discoveryStop = true;
    if (g_discoveryThread.joinable()) {
        g_discoveryThread.join();
    }
}

std::vector<ClientDevice> onlineDevices() {
    std::scoped_lock lock(g_devicesMutex);
    const DWORD now = GetTickCount();
    g_devices.erase(
        std::remove_if(g_devices.begin(), g_devices.end(), [&](const ClientDevice& device) {
            return static_cast<LONG>(now - device.lastSeenTick) > 15000;
        }),
        g_devices.end());
    std::sort(g_devices.begin(), g_devices.end(), [](const ClientDevice& left, const ClientDevice& right) {
        return left.address < right.address;
    });
    return g_devices;
}

std::wstring quote(const std::wstring& value) {
    return L"\"" + value + L"\"";
}

std::wstring executableDirectory() {
    std::array<wchar_t, MAX_PATH> path{};
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    std::wstring text(path.data(), length);
    const auto slash = text.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : text.substr(0, slash);
}

std::wstring parentDirectory(const std::wstring& path) {
    const auto slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos || slash == 0) {
        return path;
    }
    return path.substr(0, slash);
}

std::wstring repoRootFromExecutableDirectory() {
    auto root = executableDirectory();
    root = parentDirectory(root);
    root = parentDirectory(root);
    root = parentDirectory(root);
    return root;
}

bool fileExists(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::wstring driverScriptPath(const wchar_t* scriptName) {
    return repoRootFromExecutableDirectory() +
        L"\\windows-host\\driver\\idd-virtual-display\\scripts\\" + scriptName;
}

void showBalloon(HWND window, const wchar_t* title, const wchar_t* message) {
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = window;
    data.uID = kTrayIconId;
    data.uFlags = NIF_INFO;
    data.dwInfoFlags = NIIF_INFO;
    lstrcpynW(data.szInfoTitle, title, ARRAYSIZE(data.szInfoTitle));
    lstrcpynW(data.szInfo, message, ARRAYSIZE(data.szInfo));
    Shell_NotifyIconW(NIM_MODIFY, &data);
}

bool runElevatedPowerShellScript(HWND window, const std::wstring& scriptPath) {
    if (!fileExists(scriptPath)) {
        MessageBoxW(window, (L"Script not found:\n" + scriptPath).c_str(), L"LAN Extended Display", MB_ICONERROR);
        return false;
    }

    std::wstring parameters = L"-NoProfile -ExecutionPolicy Bypass -File " + quote(scriptPath);
    SHELLEXECUTEINFOW execute{};
    execute.cbSize = sizeof(execute);
    execute.fMask = SEE_MASK_NOCLOSEPROCESS;
    execute.hwnd = window;
    execute.lpVerb = L"runas";
    execute.lpFile = L"powershell.exe";
    execute.lpParameters = parameters.c_str();
    execute.nShow = SW_HIDE;
    if (!ShellExecuteExW(&execute)) {
        MessageBoxW(window, L"Administrator permission was not granted.", L"LAN Extended Display", MB_ICONERROR);
        return false;
    }

    WaitForSingleObject(execute.hProcess, INFINITE);
    DWORD exitCode = 1;
    GetExitCodeProcess(execute.hProcess, &exitCode);
    CloseHandle(execute.hProcess);
    if (exitCode != 0) {
        MessageBoxW(window, L"Virtual display driver command failed. Run the driver script manually to see detailed output.", L"LAN Extended Display", MB_ICONERROR);
        return false;
    }
    return true;
}

std::wstring trayConfigPath() {
    return executableDirectory() + L"\\led_host_tray.ini";
}

int normalizeJpegQuality(int quality) {
    if (quality == 45 || quality == 55 || quality == 65) {
        return quality;
    }
    return 55;
}

void loadTraySettings() {
    g_jpegQuality = normalizeJpegQuality(GetPrivateProfileIntW(
        L"video",
        L"jpeg_quality",
        55,
        trayConfigPath().c_str()));
}

void saveTraySettings() {
    wchar_t value[16]{};
    swprintf_s(value, L"%d", normalizeJpegQuality(g_jpegQuality));
    WritePrivateProfileStringW(L"video", L"jpeg_quality", value, trayConfigPath().c_str());
}

bool installVirtualDisplay(HWND window) {
    return runElevatedPowerShellScript(window, driverScriptPath(L"install-driver.ps1"));
}

bool installFirewallRules(HWND window) {
    const std::wstring script =
        L"$ErrorActionPreference='Stop';"
        L"Get-NetFirewallRule -DisplayName 'led_host_tray' -ErrorAction SilentlyContinue | "
        L"  Where-Object { $_.Direction -eq 'Inbound' -and $_.Action -eq 'Block' } | "
        L"  Disable-NetFirewallRule | Out-Null;"
        L"Get-NetFirewallRule -DisplayName 'led_host_app' -ErrorAction SilentlyContinue | "
        L"  Where-Object { $_.Direction -eq 'Inbound' -and $_.Action -eq 'Block' } | "
        L"  Disable-NetFirewallRule | Out-Null;"
        L"$rules=@("
        L"@{Name='LAN Extended Display Discovery';Protocol='UDP';Port='17659'},"
        L"@{Name='LAN Extended Display Control';Protocol='TCP';Port='17660'},"
        L"@{Name='LAN Extended Display Video';Protocol='UDP';Port='17670'},"
        L"@{Name='LAN Extended Display Input';Protocol='UDP';Port='17691'},"
        L"@{Name='LAN Extended Display Telemetry';Protocol='UDP';Port='17692'}"
        L");"
        L"foreach($rule in $rules){"
        L"  if(-not (Get-NetFirewallRule -DisplayName $rule.Name -ErrorAction SilentlyContinue)){"
        L"    New-NetFirewallRule -DisplayName $rule.Name -Direction Inbound -Action Allow -Profile Private,Domain -Protocol $rule.Protocol -LocalPort $rule.Port | Out-Null"
        L"  }"
        L"}";
    const std::wstring parameters =
        L"-NoProfile -ExecutionPolicy Bypass -Command " + quote(script);

    SHELLEXECUTEINFOW execute{};
    execute.cbSize = sizeof(execute);
    execute.fMask = SEE_MASK_NOCLOSEPROCESS;
    execute.hwnd = window;
    execute.lpVerb = L"runas";
    execute.lpFile = L"powershell.exe";
    execute.lpParameters = parameters.c_str();
    execute.nShow = SW_HIDE;
    if (!ShellExecuteExW(&execute)) {
        return false;
    }
    WaitForSingleObject(execute.hProcess, INFINITE);
    DWORD exitCode = 1;
    GetExitCodeProcess(execute.hProcess, &exitCode);
    CloseHandle(execute.hProcess);
    logTray(L"install firewall rules exit=%lu", exitCode);
    return exitCode == 0;
}

void notifyClientToConnect(const std::string& clientIp) {
    if (clientIp.empty()) {
        return;
    }
    const std::string hostIp = localAddressForTarget(clientIp);
    if (hostIp.empty()) {
        logTray(L"cannot resolve local address for client %ls", utf8ToWide(clientIp).c_str());
        return;
    }

    std::ostringstream message;
    message << "LED_CONNECT_V1;host=" << hostIp << ";control=17660;input=17691;";
    const bool ok = sendUdpMessage(clientIp, kDiscoveryPort, message.str());
    logTray(
        L"send connect command to %ls through host %ls: %ls",
        utf8ToWide(clientIp).c_str(),
        utf8ToWide(hostIp).c_str(),
        ok ? L"ok" : L"failed");
}

bool ensureActiveMonitorEvent() {
    if (g_activeMonitorEvent != nullptr) {
        return true;
    }
    g_activeMonitorEvent = OpenEventW(EVENT_MODIFY_STATE, FALSE, kDriverActiveMonitorEventName);
    logTray(L"open active monitor event: %ls", g_activeMonitorEvent != nullptr ? L"ok" : L"failed");
    return g_activeMonitorEvent != nullptr;
}

bool signalDriverEvent(const wchar_t* eventName) {
    HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE, eventName);
    if (event == nullptr) {
        logTray(L"open driver event failed: %ls gle=%lu", eventName, GetLastError());
        return false;
    }
    const BOOL ok = SetEvent(event);
    CloseHandle(event);
    logTray(L"signal driver event %ls: %ls", eventName, ok ? L"ok" : L"failed");
    return ok != FALSE;
}

bool waitForDriverControlEvents(int timeoutMs) {
    const DWORD deadline = GetTickCount() + static_cast<DWORD>(timeoutMs);
    do {
        HANDLE createEvent = OpenEventW(EVENT_MODIFY_STATE, FALSE, kDriverCreateMonitorEventName);
        HANDLE destroyEvent = OpenEventW(EVENT_MODIFY_STATE, FALSE, kDriverDestroyMonitorEventName);
        if (createEvent != nullptr) {
            CloseHandle(createEvent);
        }
        if (destroyEvent != nullptr) {
            CloseHandle(destroyEvent);
        }
        if (createEvent != nullptr && destroyEvent != nullptr) {
            return true;
        }
        Sleep(100);
    } while (static_cast<LONG>(deadline - GetTickCount()) > 0);
    return false;
}

bool createVirtualMonitor(HWND window) {
    logTray(L"create virtual monitor requested");
    ensureActiveMonitorEvent();
    if (g_activeMonitorEvent != nullptr) {
        SetEvent(g_activeMonitorEvent);
        logTray(L"active monitor event set");
    }
    if (signalDriverEvent(kDriverCreateMonitorEventName)) {
        return true;
    }
    if (!installVirtualDisplay(window)) {
        return false;
    }
    if (!waitForDriverControlEvents(5000)) {
        MessageBoxW(window, L"LED virtual display driver control channel was not found.", L"LAN Extended Display", MB_ICONERROR);
        return false;
    }
    ensureActiveMonitorEvent();
    if (g_activeMonitorEvent != nullptr) {
        SetEvent(g_activeMonitorEvent);
    }
    if (!signalDriverEvent(kDriverCreateMonitorEventName)) {
        MessageBoxW(window, L"Cannot create the virtual monitor through the LED driver.", L"LAN Extended Display", MB_ICONERROR);
        return false;
    }
    return true;
}

bool destroyVirtualMonitor(HWND window) {
    logTray(L"destroy virtual monitor requested");
    if (ensureActiveMonitorEvent()) {
        ResetEvent(g_activeMonitorEvent);
        logTray(L"active monitor event reset");
    }
    if (signalDriverEvent(kDriverDestroyMonitorEventName)) {
        if (g_activeMonitorEvent != nullptr) {
            CloseHandle(g_activeMonitorEvent);
            g_activeMonitorEvent = nullptr;
        }
        return true;
    }
    const bool ok = runElevatedPowerShellScript(window, driverScriptPath(L"remove-virtual-display.ps1"));
    if (g_activeMonitorEvent != nullptr) {
        CloseHandle(g_activeMonitorEvent);
        g_activeMonitorEvent = nullptr;
    }
    return ok;
}

void removeVirtualDisplayIfInstalled(HWND window) {
    if (!g_virtualDisplayInstalled) {
        return;
    }
    destroyVirtualMonitor(window);
    g_virtualDisplayInstalled = false;
}

bool hostStillRunning() {
    if (!g_host.started || g_host.process.hProcess == nullptr) {
        return false;
    }
    const DWORD wait = WaitForSingleObject(g_host.process.hProcess, 0);
    if (wait == WAIT_TIMEOUT) {
        return true;
    }
    CloseHandle(g_host.process.hThread);
    CloseHandle(g_host.process.hProcess);
    g_host = {};
    if (g_hostStdout != nullptr) {
        CloseHandle(g_hostStdout);
        g_hostStdout = nullptr;
    }
    if (g_hostStderr != nullptr) {
        CloseHandle(g_hostStderr);
        g_hostStderr = nullptr;
    }
    return false;
}

void setStopEvent() {
    HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE, kStopEventName);
    if (event != nullptr) {
        SetEvent(event);
        CloseHandle(event);
    }
}

bool startHost(HWND window) {
    if (hostStillRunning()) {
        logTray(L"host already running");
        return true;
    }

    const auto hostExe = executableDirectory() + L"\\led_host_app.exe";
    std::wstring command =
        quote(hostExe) +
        L" --serve-mjpeg-capture 17660 17670 0 60 " +
        std::to_wstring(normalizeJpegQuality(g_jpegQuality)) +
        L" 1920 1080 17691 sendinput";

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
    startup.wShowWindow = SW_HIDE;

    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;
    const auto directory = executableDirectory();
    g_hostStdout = CreateFileW(
        (directory + L"\\led_host_app.out.log").c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ,
        &security,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    g_hostStderr = CreateFileW(
        (directory + L"\\led_host_app.err.log").c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ,
        &security,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (g_hostStdout == INVALID_HANDLE_VALUE) {
        g_hostStdout = nullptr;
    }
    if (g_hostStderr == INVALID_HANDLE_VALUE) {
        g_hostStderr = nullptr;
    }
    startup.hStdOutput = g_hostStdout;
    startup.hStdError = g_hostStderr;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION process{};
    if (!CreateProcessW(
            nullptr,
            command.data(),
            nullptr,
            nullptr,
            TRUE,
            CREATE_NO_WINDOW,
            nullptr,
            directory.c_str(),
            &startup,
            &process)) {
        if (g_hostStdout != nullptr) {
            CloseHandle(g_hostStdout);
        }
        if (g_hostStderr != nullptr) {
            CloseHandle(g_hostStderr);
        }
        g_hostStdout = nullptr;
        g_hostStderr = nullptr;
        logTray(L"CreateProcess failed gle=%lu", GetLastError());
        MessageBoxW(nullptr, L"Cannot start led_host_app.exe. Keep tray and host executables in the same directory.", L"LAN Extended Display", MB_ICONERROR);
        return false;
    }

    g_host.process = process;
    g_host.started = true;
    const auto hostGeneration = ++g_hostGeneration;
    logTray(L"host process started pid=%lu jpeg_quality=%d", process.dwProcessId, normalizeJpegQuality(g_jpegQuality));
    if (WaitForSingleObject(g_host.process.hProcess, 1200) != WAIT_TIMEOUT) {
        DWORD exitCode = 1;
        GetExitCodeProcess(g_host.process.hProcess, &exitCode);
        logTray(L"host exited during startup exit=%lu", exitCode);
        hostStillRunning();
        MessageBoxW(nullptr, L"Host exited during startup. The LED virtual display was not found; check led_host_app.err.log.", L"LAN Extended Display", MB_ICONERROR);
        return false;
    }
    HANDLE waitHandle = nullptr;
    if (DuplicateHandle(
            GetCurrentProcess(),
            g_host.process.hProcess,
            GetCurrentProcess(),
            &waitHandle,
            SYNCHRONIZE,
            FALSE,
            0)) {
        std::thread([waitHandle, window, hostGeneration]() {
            WaitForSingleObject(waitHandle, INFINITE);
            CloseHandle(waitHandle);
            PostMessageW(window, kHostExitedMessage, static_cast<WPARAM>(hostGeneration), 0);
        }).detach();
    }
    return true;
}

void stopHost() {
    setStopEvent();
    if (!g_host.started || g_host.process.hProcess == nullptr) {
        return;
    }
    if (WaitForSingleObject(g_host.process.hProcess, 5000) == WAIT_TIMEOUT) {
        TerminateProcess(g_host.process.hProcess, 1);
        WaitForSingleObject(g_host.process.hProcess, 2000);
    }
    hostStillRunning();
}

void startExtendedDisplay(HWND window, const std::string& clientIp = {}) {
    const std::string targetIp = clientIp.empty() ? g_selectedClientIp : clientIp;
    logTray(L"start command received target=%ls", utf8ToWide(targetIp).c_str());
    if (!targetIp.empty()) {
        g_selectedClientIp = targetIp;
    }
    showBalloon(window, L"Starting extended display", L"Preparing the virtual display and client connection.");
    if (!createVirtualMonitor(window)) {
        logTray(L"create virtual monitor failed");
        return;
    }
    g_virtualDisplayInstalled = true;
    if (!startHost(window)) {
        logTray(L"start host failed; removing virtual monitor");
        removeVirtualDisplayIfInstalled(window);
        return;
    }
    notifyClientToConnect(targetIp);
    logTray(L"start command completed");
    showBalloon(window, L"Extended display started", targetIp.empty() ? L"Waiting for a Linux client to attach." : L"Linux client was asked to connect.");
}

void stopExtendedDisplay(HWND window) {
    logTray(L"stop command received");
    showBalloon(window, L"Stopping extended display", L"Stopping capture and removing the virtual display.");
    stopHost();
    removeVirtualDisplayIfInstalled(window);
    logTray(L"stop command completed");
    showBalloon(window, L"Extended display stopped", L"The virtual display removal was requested.");
}

void setJpegQuality(HWND window, int quality) {
    g_jpegQuality = normalizeJpegQuality(quality);
    saveTraySettings();
    const bool running = hostStillRunning() || g_virtualDisplayInstalled;
    logTray(L"jpeg quality set to %d running=%ls", g_jpegQuality, running ? L"true" : L"false");
    if (!running) {
        showBalloon(window, L"Quality set", L"The selected quality will be used for the next start.");
        return;
    }

    const std::string targetIp = g_selectedClientIp;
    showBalloon(window, L"Applying quality", L"Restarting the current display session with the new quality.");
    stopExtendedDisplay(window);
    Sleep(500);
    startExtendedDisplay(window, targetIp);
}

void addTrayIcon(HWND window) {
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = window;
    data.uID = kTrayIconId;
    data.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    data.uCallbackMessage = kTrayMessage;
    data.hIcon = trayIcon();
    lstrcpynW(data.szTip, L"LAN Extended Display", ARRAYSIZE(data.szTip));
    Shell_NotifyIconW(NIM_ADD, &data);
}

void removeTrayIcon(HWND window) {
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = window;
    data.uID = kTrayIconId;
    Shell_NotifyIconW(NIM_DELETE, &data);
    if (g_trayIcon != nullptr) {
        DestroyIcon(g_trayIcon);
        g_trayIcon = nullptr;
    }
}

std::wstring promptForIp(HWND owner) {
    struct PromptState {
        HWND edit{nullptr};
        bool done{false};
        bool accepted{false};
        std::wstring value;
    } state;

    const wchar_t* className = L"LanExtendedDisplayIpPrompt";
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = [](HWND window, UINT message, WPARAM wParam, LPARAM lParam) -> LRESULT {
        auto* prompt = reinterpret_cast<PromptState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        switch (message) {
        case WM_CREATE: {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            prompt = reinterpret_cast<PromptState*>(create->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(prompt));
            CreateWindowExW(0, L"STATIC", L"Linux client IP:", WS_CHILD | WS_VISIBLE, 14, 18, 220, 22, window, nullptr, nullptr, nullptr);
            prompt->edit = CreateWindowExW(
                WS_EX_CLIENTEDGE,
                L"EDIT",
                L"",
                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                14,
                44,
                270,
                24,
                window,
                reinterpret_cast<HMENU>(1),
                nullptr,
                nullptr);
            CreateWindowExW(0, L"BUTTON", L"Start", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 98, 84, 82, 28, window, reinterpret_cast<HMENU>(IDOK), nullptr, nullptr);
            CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE, 194, 84, 82, 28, window, reinterpret_cast<HMENU>(IDCANCEL), nullptr, nullptr);
            SendMessageW(prompt->edit, EM_LIMITTEXT, 63, 0);
            SetFocus(prompt->edit);
            return 0;
        }
        case WM_COMMAND:
            if (LOWORD(wParam) == IDOK && prompt != nullptr) {
                wchar_t buffer[128]{};
                GetWindowTextW(prompt->edit, buffer, ARRAYSIZE(buffer));
                prompt->value = buffer;
                prompt->accepted = true;
                prompt->done = true;
                DestroyWindow(window);
                return 0;
            }
            if (LOWORD(wParam) == IDCANCEL && prompt != nullptr) {
                prompt->done = true;
                DestroyWindow(window);
                return 0;
            }
            break;
        case WM_CLOSE:
            if (prompt != nullptr) {
                prompt->done = true;
            }
            DestroyWindow(window);
            return 0;
        default:
            break;
        }
        return DefWindowProcW(window, message, wParam, lParam);
    };
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = className;
    windowClass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    RegisterClassW(&windowClass);

    HWND dialog = CreateWindowExW(
        WS_EX_DLGMODALFRAME | WS_EX_TOPMOST,
        className,
        L"Start by IP",
        WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        310,
        160,
        owner,
        nullptr,
        GetModuleHandleW(nullptr),
        &state);
    if (dialog == nullptr) {
        return {};
    }

    EnableWindow(owner, FALSE);
    ShowWindow(dialog, SW_SHOW);
    MSG message{};
    while (!state.done && GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(dialog, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);
    return state.accepted ? state.value : std::wstring{};
}

std::wstring deviceLabel(const ClientDevice& device) {
    std::wstring label = utf8ToWide(device.address);
    if (!device.name.empty() && device.name != device.address) {
        label += L"  ";
        label += utf8ToWide(device.name);
    }
    if (!device.status.empty()) {
        label += L"  [";
        label += utf8ToWide(device.status);
        label += L"]";
    }
    if (device.address == g_selectedClientIp) {
        label += L"  *";
    }
    return label;
}

void showTrayMenu(HWND window) {
    const bool running = hostStillRunning();
    const bool busy = running || g_virtualDisplayInstalled;
    const auto devices = onlineDevices();

    HMENU menu = CreatePopupMenu();
    std::wstring status = busy ? L"Status: running" : L"Status: stopped";
    status += L"  Quality: ";
    status += std::to_wstring(normalizeJpegQuality(g_jpegQuality));
    if (!g_selectedClientIp.empty()) {
        status += L"  Target: ";
        status += utf8ToWide(g_selectedClientIp);
    }
    AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, status.c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (busy ? MF_GRAYED : 0), kMenuStart, L"Start default");
    AppendMenuW(menu, MF_STRING | (busy ? MF_GRAYED : 0), kMenuManualIp, L"Start by IP...");
    HMENU devicesMenu = CreatePopupMenu();
    if (devices.empty()) {
        AppendMenuW(devicesMenu, MF_STRING | MF_GRAYED, 0, L"No Linux clients found");
    } else {
        const int count = (std::min)(static_cast<int>(devices.size()), kMaxDeviceMenuItems);
        for (int index = 0; index < count; ++index) {
            const std::wstring label = deviceLabel(devices[static_cast<std::size_t>(index)]);
            AppendMenuW(devicesMenu, MF_STRING | (busy ? MF_GRAYED : 0), kMenuDeviceBase + index, label.c_str());
        }
    }
    AppendMenuW(menu, MF_POPUP | (busy ? MF_GRAYED : 0), reinterpret_cast<UINT_PTR>(devicesMenu), L"Start discovered client");
    AppendMenuW(menu, MF_STRING, kMenuRefreshDevices, L"Refresh clients");
    HMENU qualityMenu = CreatePopupMenu();
    const int quality = normalizeJpegQuality(g_jpegQuality);
    AppendMenuW(qualityMenu, MF_STRING | (quality == 45 ? MF_CHECKED : 0), kMenuQuality45, L"Fast 45");
    AppendMenuW(qualityMenu, MF_STRING | (quality == 55 ? MF_CHECKED : 0), kMenuQuality55, L"Balanced 55");
    AppendMenuW(qualityMenu, MF_STRING | (quality == 65 ? MF_CHECKED : 0), kMenuQuality65, L"Sharp 65");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(qualityMenu), L"Quality");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (busy ? 0 : MF_GRAYED), kMenuStop, L"Stop extended display");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuInstallFirewall, L"Install firewall rules");
    AppendMenuW(menu, MF_STRING, kMenuOpenLog, L"Open log");
    AppendMenuW(menu, MF_STRING, kMenuExit, L"Exit");

    POINT cursor{};
    GetCursorPos(&cursor);
    SetForegroundWindow(window);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, cursor.x, cursor.y, 0, window, nullptr);
    DestroyMenu(menu);
}

LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE:
        addTrayIcon(window);
        startDiscovery();
        SetTimer(window, kMonitorTimerId, 2000, nullptr);
        return 0;
    case WM_TIMER:
        if (wParam == kMonitorTimerId && g_virtualDisplayInstalled && !hostStillRunning()) {
            showBalloon(window, L"Extended display ended", L"Host stopped unexpectedly. Removing the virtual display.");
            removeVirtualDisplayIfInstalled(window);
            return 0;
        }
        break;
    case kHostExitedMessage:
        if (static_cast<std::uint64_t>(wParam) != g_hostGeneration) {
            logTray(
                L"ignored stale host exit message generation=%llu current=%llu",
                static_cast<unsigned long long>(wParam),
                static_cast<unsigned long long>(g_hostGeneration));
            return 0;
        }
        hostStillRunning();
        if (g_virtualDisplayInstalled) {
            showBalloon(window, L"Extended display ended", L"Host stopped. Removing the virtual display.");
            removeVirtualDisplayIfInstalled(window);
        }
        return 0;
    case WM_COMMAND:
        if (LOWORD(wParam) >= kMenuDeviceBase && LOWORD(wParam) < kMenuDeviceBase + kMaxDeviceMenuItems) {
            const auto devices = onlineDevices();
            const int index = static_cast<int>(LOWORD(wParam) - kMenuDeviceBase);
            if (index >= 0 && index < static_cast<int>(devices.size())) {
                startExtendedDisplay(window, devices[static_cast<std::size_t>(index)].address);
            }
            return 0;
        }
        switch (LOWORD(wParam)) {
        case kMenuStart:
            startExtendedDisplay(window);
            return 0;
        case kMenuManualIp: {
            const std::string ip = trimAscii(wideToUtf8(promptForIp(window)));
            if (ip.empty()) {
                return 0;
            }
            if (!isValidIpv4(ip)) {
                MessageBoxW(window, L"Please enter a valid IPv4 address.", L"LAN Extended Display", MB_ICONERROR);
                return 0;
            }
            rememberDevice(ip, ip, "manual");
            startExtendedDisplay(window, ip);
            return 0;
        }
        case kMenuRefreshDevices:
            sendDiscoveryProbe();
            showBalloon(window, L"Scanning", L"Looking for Linux extended display clients on the LAN.");
            return 0;
        case kMenuQuality45:
            setJpegQuality(window, 45);
            return 0;
        case kMenuQuality55:
            setJpegQuality(window, 55);
            return 0;
        case kMenuQuality65:
            setJpegQuality(window, 65);
            return 0;
        case kMenuInstallFirewall:
            if (installFirewallRules(window)) {
                showBalloon(window, L"Firewall rules installed", L"Discovery and streaming ports are allowed on private networks.");
                sendDiscoveryProbe();
            } else {
                MessageBoxW(window, L"Firewall rule installation failed or was cancelled.", L"LAN Extended Display", MB_ICONERROR);
            }
            return 0;
        case kMenuStop:
            stopExtendedDisplay(window);
            return 0;
        case kMenuOpenLog:
            ShellExecuteW(window, L"open", (executableDirectory() + L"\\led_host_tray.log").c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            return 0;
        case kMenuExit:
            DestroyWindow(window);
            return 0;
        default:
            break;
        }
        break;
    case kTrayMessage:
        if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU) {
            showTrayMenu(window);
            return 0;
        }
        if (lParam == WM_LBUTTONDBLCLK) {
            if (hostStillRunning() || g_virtualDisplayInstalled) {
                stopExtendedDisplay(window);
            } else {
                startExtendedDisplay(window);
            }
            return 0;
        }
        break;
    case WM_DESTROY:
        KillTimer(window, kMonitorTimerId);
        removeTrayIcon(window);
        stopHost();
        removeVirtualDisplayIfInstalled(window);
        stopDiscovery();
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    loadTraySettings();
    logTray(L"tray process starting");
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = windowProc;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = kWindowClassName;
    windowClass.hIcon = trayIcon();
    RegisterClassW(&windowClass);

    HWND window = CreateWindowExW(
        0,
        kWindowClassName,
        L"LAN Extended Display",
        0,
        0,
        0,
        0,
        0,
        nullptr,
        nullptr,
        instance,
        nullptr);
    if (window == nullptr) {
        return 1;
    }

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}
