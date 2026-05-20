#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <shellapi.h>

#include <array>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <thread>

namespace {

constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kHostExitedMessage = WM_APP + 2;
constexpr UINT_PTR kTrayIconId = 1;
constexpr int kMenuStart = 1001;
constexpr int kMenuStop = 1002;
constexpr int kMenuExit = 1003;
constexpr UINT_PTR kMonitorTimerId = 2001;
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

bool installVirtualDisplay(HWND window) {
    return runElevatedPowerShellScript(window, driverScriptPath(L"install-driver.ps1"));
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
        L" --serve-live-capture 17660 17670 17691 0 60 sendinput 20000 1920 1080";

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
    logTray(L"host process started pid=%lu", process.dwProcessId);
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
        std::thread([waitHandle, window]() {
            WaitForSingleObject(waitHandle, INFINITE);
            CloseHandle(waitHandle);
            PostMessageW(window, kHostExitedMessage, 0, 0);
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

void startExtendedDisplay(HWND window) {
    logTray(L"start command received");
    showBalloon(window, L"Starting extended display", L"Preparing the virtual display. Linux will connect automatically.");
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
    logTray(L"start command completed");
    showBalloon(window, L"Extended display started", L"Waiting for the Linux client to attach.");
}

void stopExtendedDisplay(HWND window) {
    logTray(L"stop command received");
    showBalloon(window, L"Stopping extended display", L"Stopping capture and removing the virtual display.");
    stopHost();
    removeVirtualDisplayIfInstalled(window);
    logTray(L"stop command completed");
    showBalloon(window, L"Extended display stopped", L"The virtual display removal was requested.");
}

void addTrayIcon(HWND window) {
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = window;
    data.uID = kTrayIconId;
    data.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    data.uCallbackMessage = kTrayMessage;
    data.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
    lstrcpynW(data.szTip, L"LAN Extended Display", ARRAYSIZE(data.szTip));
    Shell_NotifyIconW(NIM_ADD, &data);
}

void removeTrayIcon(HWND window) {
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = window;
    data.uID = kTrayIconId;
    Shell_NotifyIconW(NIM_DELETE, &data);
}

void showTrayMenu(HWND window) {
    const bool running = hostStillRunning();
    const bool busy = running || g_virtualDisplayInstalled;

    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | (busy ? MF_GRAYED : 0), kMenuStart, L"Start extended display");
    AppendMenuW(menu, MF_STRING | (busy ? 0 : MF_GRAYED), kMenuStop, L"Stop extended display");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
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
        hostStillRunning();
        if (g_virtualDisplayInstalled) {
            showBalloon(window, L"Extended display ended", L"Host stopped. Removing the virtual display.");
            removeVirtualDisplayIfInstalled(window);
        }
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case kMenuStart:
            startExtendedDisplay(window);
            return 0;
        case kMenuStop:
            stopExtendedDisplay(window);
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
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    logTray(L"tray process starting");
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = windowProc;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = kWindowClassName;
    windowClass.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
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
