// punch-windows.cpp : Windows tray wrapper for punchd.

#include "framework.h"
#include "punch-windows.h"

#include "punch-api.h"
#include "punch-util.h"

#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cwctype>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#define MAX_LOADSTRING 100

constexpr UINT WM_TRAYICON = WM_APP + 1;
constexpr UINT WM_STATUS_REFRESHED = WM_APP + 2;
constexpr UINT WM_RELAY_MENU_REFRESHED = WM_APP + 3;
constexpr UINT WM_LOG_LINE = WM_APP + 4;
constexpr UINT WM_LOG_STATUS = WM_APP + 5;
constexpr UINT_PTR IDT_STATUS_REFRESH = 1;
constexpr UINT STATUS_REFRESH_CONNECTED_MS = 1000;
constexpr UINT STATUS_REFRESH_DISCONNECTED_MS = 2000;

constexpr UINT IDM_TRAY_START = 40001;
constexpr UINT IDM_TRAY_STOP = 40002;
constexpr UINT IDM_TRAY_EXIT = 40004;
constexpr UINT IDM_TRAY_CONFIG = 40005;
constexpr UINT IDM_TRAY_LOGS = 40006;
constexpr UINT IDM_TRAY_STATUS_BASE = 41000;
constexpr UINT IDM_RELAY_SELECT_TOGGLE = 42000;
constexpr UINT IDM_RELAY_GROUP_BASE = 42100;
constexpr UINT IDM_RELAY_RELAY_BASE = 43100;

HINSTANCE hInst;
WCHAR szTitle[MAX_LOADSTRING];
WCHAR szWindowClass[MAX_LOADSTRING];

enum class RelayCommandType {
    Group,
    Relay,
    GroupSelectMode,
};

struct RelayCommand {
    RelayCommandType type;
    std::wstring group;
    std::wstring relay;
};

PunchStatus g_status;
AppConfig g_appConfig;
bool g_starting = false;
bool g_stopping = false;
HICON g_trayIcon = nullptr;
HMENU g_openTrayMenu = nullptr;
bool g_openTrayMenuHasLastError = false;
std::vector<RelayCommand> g_relayCommands;
RelayMenuState g_relayMenuState;
bool g_hasRelayMenuState = false;
std::mutex g_relayMenuMutex;
std::atomic_bool g_statusRefreshInProgress = false;
std::atomic_bool g_relayMenuRefreshInProgress = false;

ATOM MyRegisterClass(HINSTANCE hInstance);
BOOL InitInstance(HINSTANCE, int);
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
INT_PTR CALLBACK About(HWND, UINT, WPARAM, LPARAM);
INT_PTR CALLBACK ConfigDialog(HWND, UINT, WPARAM, LPARAM);
INT_PTR CALLBACK LogsDialog(HWND, UINT, WPARAM, LPARAM);

std::atomic_bool g_logStreamStop = false;
std::thread g_logStreamThread;

void LoadAppConfig() {
    std::wstring path = ConfigPath();
    wchar_t buffer[1024]{};
    GetPrivateProfileStringW(L"api", L"address", L"", buffer, static_cast<DWORD>(ARRAYSIZE(buffer)), path.c_str());
    std::wstring address = buffer;
    if (address.empty()) {
        address = GetEnvString(L"PUNCH_API_ADDR");
    }
    g_appConfig.apiAddress = NormalizeAPIAddress(address);

    buffer[0] = L'\0';
    GetPrivateProfileStringW(L"api", L"secret", L"", buffer, static_cast<DWORD>(ARRAYSIZE(buffer)), path.c_str());
    g_appConfig.apiSecret = buffer;
    if (g_appConfig.apiSecret.empty()) {
        g_appConfig.apiSecret = GetEnvString(L"PUNCH_API_TOKEN");
    }
}

bool SaveAppConfig() {
    std::wstring path = ConfigPath();
    return WritePrivateProfileStringW(L"api", L"address", g_appConfig.apiAddress.c_str(), path.c_str()) &&
        WritePrivateProfileStringW(L"api", L"secret", g_appConfig.apiSecret.c_str(), path.c_str());
}

void EnableDpiAwareness() {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        using SetProcessDpiAwarenessContextProc = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
        auto setContext = reinterpret_cast<SetProcessDpiAwarenessContextProc>(
            GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
        if (setContext && setContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
            return;
        }
    }
    SetProcessDPIAware();
}

UINT GetWindowDpi(HWND hWnd) {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32 && hWnd) {
        using GetDpiForWindowProc = UINT(WINAPI*)(HWND);
        auto getDpiForWindow = reinterpret_cast<GetDpiForWindowProc>(
            GetProcAddress(user32, "GetDpiForWindow"));
        if (getDpiForWindow) {
            UINT dpi = getDpiForWindow(hWnd);
            if (dpi > 0) {
                return dpi;
            }
        }
    }

    HDC dc = GetDC(nullptr);
    if (!dc) {
        return 96;
    }
    UINT dpi = static_cast<UINT>(GetDeviceCaps(dc, LOGPIXELSX));
    ReleaseDC(nullptr, dc);
    return dpi > 0 ? dpi : 96;
}

int GetSystemMetricForDpi(int metric, UINT dpi) {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        using GetSystemMetricsForDpiProc = int(WINAPI*)(int, UINT);
        auto getSystemMetricsForDpi = reinterpret_cast<GetSystemMetricsForDpiProc>(
            GetProcAddress(user32, "GetSystemMetricsForDpi"));
        if (getSystemMetricsForDpi) {
            int value = getSystemMetricsForDpi(metric, dpi);
            if (value > 0) {
                return value;
            }
        }
    }
    return MulDiv(GetSystemMetrics(metric), static_cast<int>(dpi), 96);
}

HICON LoadIconForDpi(HINSTANCE instance, UINT resourceID, int metric, HWND hWnd) {
    UINT dpi = GetWindowDpi(hWnd);
    int size = GetSystemMetricForDpi(metric, dpi);
    if (size < 16) {
        size = 16;
    }
    HICON icon = reinterpret_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(resourceID), IMAGE_ICON, size, size, LR_DEFAULTCOLOR));
    if (icon) {
        return icon;
    }
    HICON shared = LoadIconW(instance, MAKEINTRESOURCEW(resourceID));
    return shared ? CopyIcon(shared) : nullptr;
}

void DestroyTrayIconHandle() {
    if (g_trayIcon) {
        DestroyIcon(g_trayIcon);
        g_trayIcon = nullptr;
    }
}

std::wstring DefaultAPIAddress() {
    return NormalizeAPIAddress(g_appConfig.apiAddress);
}

std::wstring APIToken() {
    return g_appConfig.apiSecret;
}

RelayMenuState CurrentRelayMenuState() {
    std::lock_guard<std::mutex> lock(g_relayMenuMutex);
    if (g_hasRelayMenuState) {
        return g_relayMenuState;
    }

    RelayMenuState state;
    state.lastError = g_relayMenuRefreshInProgress ? L"Loading relays..." : L"Relay data not loaded";
    return state;
}

void ClearRelayMenuState() {
    std::lock_guard<std::mutex> lock(g_relayMenuMutex);
    g_relayMenuState = RelayMenuState{};
    g_hasRelayMenuState = false;
}

void ApplyStatus(HWND hWnd, const PunchStatus& next) {
    g_status = next;
    if (g_status.apiReachable) {
        g_starting = false;
        g_stopping = false;
    }

    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hWnd;
    nid.uID = IDI_PUNCHWINDOWS;
    nid.uFlags = NIF_TIP;
    std::wstring tip = L"Punch: " + g_status.state;
    wcsncpy_s(nid.szTip, tip.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void RefreshStatus(HWND hWnd) {
    ApplyStatus(hWnd, FetchStatus(DefaultAPIAddress(), APIToken()));
}

UINT StatusRefreshInterval() {
    return g_status.apiReachable ? STATUS_REFRESH_CONNECTED_MS : STATUS_REFRESH_DISCONNECTED_MS;
}

void StartAsyncStatusRefresh(HWND hWnd) {
    if (g_statusRefreshInProgress.exchange(true)) {
        return;
    }

    std::wstring api = DefaultAPIAddress();
    std::wstring token = APIToken();
    std::thread([hWnd, api, token]() {
        PunchStatus* next = new PunchStatus(FetchStatus(api, token));
        if (!PostMessageW(hWnd, WM_STATUS_REFRESHED, 0, reinterpret_cast<LPARAM>(next))) {
            delete next;
            g_statusRefreshInProgress = false;
        }
    }).detach();
}

void StartAsyncRelayMenuRefresh(HWND hWnd) {
    if (g_relayMenuRefreshInProgress.exchange(true)) {
        return;
    }

    std::wstring api = DefaultAPIAddress();
    std::wstring token = APIToken();
    std::thread([hWnd, api, token]() {
        RelayMenuState* next = new RelayMenuState(FetchRelayMenuState(api, token));
        if (!PostMessageW(hWnd, WM_RELAY_MENU_REFRESHED, 0, reinterpret_cast<LPARAM>(next))) {
            delete next;
            g_relayMenuRefreshInProgress = false;
        }
    }).detach();
}

bool ShellExecuteElevated(HWND hWnd, const std::wstring& file, const std::wstring& params, bool wait) {
    std::wstring appDir = GetAppDirectory();
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = wait ? SEE_MASK_NOCLOSEPROCESS : 0;
    sei.hwnd = hWnd;
    sei.lpVerb = L"runas";
    sei.lpFile = file.c_str();
    sei.lpParameters = params.empty() ? nullptr : params.c_str();
    sei.lpDirectory = appDir.c_str();
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei)) {
        return false;
    }
    if (wait && sei.hProcess) {
        WaitForSingleObject(sei.hProcess, 10000);
        CloseHandle(sei.hProcess);
    }
    return true;
}

void StartPunchd(HWND hWnd) {
    std::wstring punchd = ResolveToolPath(L"PUNCHD_PATH", L"punchd.exe");
    if (!FileExists(punchd)) {
        MessageBoxW(hWnd, (L"Cannot find punchd.exe.\n\nPlace it next to punch-windows.exe or set PUNCHD_PATH.\nExpected: " + punchd).c_str(),
            L"Punch", MB_ICONERROR);
        return;
    }
    if (!ShellExecuteElevated(hWnd, punchd, L"", false)) {
        MessageBoxW(hWnd, L"Failed to start punchd.exe with administrative privileges.", L"Punch", MB_ICONERROR);
        return;
    }
    g_starting = true;
    g_status.state = L"Starting";
    SetTimer(hWnd, IDT_STATUS_REFRESH, 500, nullptr);
}

void StopPunchd(HWND hWnd) {
    std::wstring error;
    if (HttpPost(DefaultAPIAddress() + L"/api/shutdown", APIToken(), 202, error)) {
        g_stopping = true;
        g_status.state = L"Stopping";
        SetTimer(hWnd, IDT_STATUS_REFRESH, 500, nullptr);
        return;
    }

    std::wstring taskkill = JoinPath(GetEnvString(L"WINDIR"), L"System32\\taskkill.exe");
    if (!FileExists(taskkill)) {
        taskkill = L"taskkill.exe";
    }
    if (!ShellExecuteElevated(hWnd, taskkill, L"/IM punchd.exe /T /F", true)) {
        MessageBoxW(hWnd, L"Failed to stop punchd.exe with administrative privileges.", L"Punch", MB_ICONERROR);
        return;
    }
    g_stopping = true;
    g_status.state = L"Stopping";
    SetTimer(hWnd, IDT_STATUS_REFRESH, 500, nullptr);
}

void ShowAPIError(HWND hWnd, const std::wstring& action, const std::wstring& error) {
    MessageBoxW(hWnd, (action + L" failed.\n\n" + error).c_str(), L"Punch", MB_ICONERROR);
}

void PostLogStatus(HWND hDlg, const std::wstring& text) {
    auto* copy = new std::wstring(text);
    if (!PostMessageW(hDlg, WM_LOG_STATUS, 0, reinterpret_cast<LPARAM>(copy))) {
        delete copy;
    }
}

void StartLogStream(HWND hDlg) {
    g_logStreamStop = false;
    std::wstring api = DefaultAPIAddress();
    std::wstring token = APIToken();
    g_logStreamThread = std::thread([hDlg, api, token]() {
        uint64_t since = 0;
        bool connected = false;
        while (!g_logStreamStop.load()) {
            std::wstring error;
            LogBatch batch = FetchLogs(api, token, since, error);
            if (!error.empty()) {
                connected = false;
                PostLogStatus(hDlg, L"Log refresh failed: " + error);
            } else {
                if (!connected) {
                    PostLogStatus(hDlg, L"Connected");
                    connected = true;
                }
                since = batch.nextSeq;
            }
            for (const LogEntry& entry : batch.entries) {
                if (entry.seq > since) {
                    since = entry.seq;
                }
                const std::wstring& line = entry.line;
                auto* copy = new std::wstring(line);
                if (!PostMessageW(hDlg, WM_LOG_LINE, 0, reinterpret_cast<LPARAM>(copy))) {
                    delete copy;
                }
            }

            for (int i = 0; i < 5 && !g_logStreamStop.load(); ++i) {
                Sleep(100);
            }
        }
    });
}

void StopLogStream() {
    g_logStreamStop = true;
    if (g_logStreamThread.joinable()) {
        g_logStreamThread.join();
    }
}

void WaitForPunchdExit(DWORD timeoutMs) {
    DWORD elapsed = 0;
    while (elapsed < timeoutMs && IsPunchdProcessRunning()) {
        Sleep(250);
        elapsed += 250;
    }
}

void ApplyConfig(HWND hWnd, const AppConfig& next, bool updatePunchdSettings) {
    std::wstring oldAPI = DefaultAPIAddress();
    std::wstring oldToken = APIToken();
    bool wasConnected = g_status.apiReachable;
    AppConfig normalized = next;
    normalized.apiAddress = NormalizeAPIAddress(normalized.apiAddress);

    if (!wasConnected || !updatePunchdSettings) {
        g_appConfig = normalized;
        if (!SaveAppConfig()) {
            MessageBoxW(hWnd, (L"Failed to save config file:\n\n" + ConfigPath()).c_str(), L"Punch", MB_ICONERROR);
            return;
        }
        ClearRelayMenuState();
        StartAsyncStatusRefresh(hWnd);
        StartAsyncRelayMenuRefresh(hWnd);
        SetTimer(hWnd, IDT_STATUS_REFRESH, StatusRefreshInterval(), nullptr);
        return;
    }

    std::wstring error;
    if (!HttpPutConfigValue(oldAPI, oldToken, L"api.listen", APIListenFromAddress(normalized.apiAddress), error)) {
        ShowAPIError(hWnd, L"Updating API address", error);
        return;
    }
    if (!HttpPutConfigValue(oldAPI, oldToken, L"api.secret", normalized.apiSecret, error)) {
        ShowAPIError(hWnd, L"Updating API secret", error);
        return;
    }
    g_appConfig = normalized;
    if (!SaveAppConfig()) {
        MessageBoxW(hWnd, (L"Failed to save config file:\n\n" + ConfigPath()).c_str(), L"Punch", MB_ICONERROR);
        return;
    }
    ClearRelayMenuState();
    if (!HttpPost(oldAPI + L"/api/shutdown", oldToken, 202, error)) {
        ShowAPIError(hWnd, L"Restarting punchd", error);
        return;
    }

    g_stopping = true;
    g_status.state = L"Restarting";
    WaitForPunchdExit(10000);
    StartPunchd(hWnd);
    SetTimer(hWnd, IDT_STATUS_REFRESH, 500, nullptr);
}

void SetRelaySelectMode(HWND hWnd, const std::wstring& mode) {
    std::wstring error;
    if (!HttpPutConfigValue(DefaultAPIAddress(), APIToken(), L"relay.select", mode, error)) {
        ShowAPIError(hWnd, L"Changing relay selection mode", error);
        return;
    }
    StartAsyncStatusRefresh(hWnd);
    StartAsyncRelayMenuRefresh(hWnd);
}

void SelectRelayGroup(HWND hWnd, const std::wstring& group) {
    std::wstring error;
    if (!HttpPost(DefaultAPIAddress() + L"/api/relaygroups/" + UrlEscapePathSegment(group) + L"/select",
        APIToken(), 200, error)) {
        ShowAPIError(hWnd, L"Selecting relay group", error);
        return;
    }
    StartAsyncStatusRefresh(hWnd);
    StartAsyncRelayMenuRefresh(hWnd);
}

void SelectRelay(HWND hWnd, const std::wstring& group, const std::wstring& relay) {
    std::wstring error;
    if (!HttpPost(DefaultAPIAddress() + L"/api/relays/" + UrlEscapePathSegment(relay) + L"/select?group=" + UrlEscapePathSegment(group),
        APIToken(), 200, error)) {
        ShowAPIError(hWnd, L"Selecting relay", error);
        return;
    }
    StartAsyncStatusRefresh(hWnd);
    StartAsyncRelayMenuRefresh(hWnd);
}

void SetRelayGroupSelectMode(HWND hWnd, const std::wstring& group, const std::wstring& mode) {
    std::wstring token = APIToken();
    std::wstring groupPath = UrlEscapePathSegment(group);
    std::string body;
    std::wstring error;
    if (!HttpGet(DefaultAPIAddress() + L"/api/relaygroups/" + groupPath, token, body, error)) {
        ShowAPIError(hWnd, L"Loading relay group", error);
        return;
    }

    std::string config = JsonObject(body, "config");
    if (config.empty()) {
        ShowAPIError(hWnd, L"Changing relay group selection mode", L"API response did not include relay group config");
        return;
    }

    std::string nextConfig = SetJsonStringField(config, "select", WideToUtf8(mode));
    std::string ignored;
    if (!HttpRequest(L"PUT", DefaultAPIAddress() + L"/api/relaygroups/" + groupPath, token, nextConfig, 200, ignored, error)) {
        ShowAPIError(hWnd, L"Changing relay group selection mode", error);
        return;
    }
    StartAsyncStatusRefresh(hWnd);
    StartAsyncRelayMenuRefresh(hWnd);
}

void AddDisabledMenuItem(HMENU menu, UINT id, const std::wstring& text) {
    AppendMenuW(menu, MF_STRING | MF_GRAYED, id, text.c_str());
}

void UpdateDisabledMenuItem(HMENU menu, UINT id, const std::wstring& text) {
    ModifyMenuW(menu, id, MF_BYCOMMAND | MF_STRING | MF_GRAYED, id, text.c_str());
}

void UpdateOpenTrayStatusMenu() {
    if (!g_openTrayMenu) {
        return;
    }

    UpdateDisabledMenuItem(g_openTrayMenu, IDM_TRAY_STATUS_BASE + 0, L"State: " + g_status.state);
    UpdateDisabledMenuItem(g_openTrayMenu, IDM_TRAY_STATUS_BASE + 1, L"Version: " + g_status.version);
    UpdateDisabledMenuItem(g_openTrayMenu, IDM_TRAY_STATUS_BASE + 2, L"Uptime: " + g_status.uptime);
    UpdateDisabledMenuItem(g_openTrayMenu, IDM_TRAY_STATUS_BASE + 3, L"Relay Group: " + g_status.relayGroup);
    UpdateDisabledMenuItem(g_openTrayMenu, IDM_TRAY_STATUS_BASE + 4, L"Relay: " + g_status.relay);
    UpdateDisabledMenuItem(g_openTrayMenu, IDM_TRAY_STATUS_BASE + 5, L"Relay Status: " + g_status.relayStatus);
    UpdateDisabledMenuItem(g_openTrayMenu, IDM_TRAY_STATUS_BASE + 6, L"Download: " + g_status.downloadTotal + L" total, " + g_status.downloadSpeed);
    UpdateDisabledMenuItem(g_openTrayMenu, IDM_TRAY_STATUS_BASE + 7, L"Upload: " + g_status.uploadTotal + L" total, " + g_status.uploadSpeed);
    UpdateDisabledMenuItem(g_openTrayMenu, IDM_TRAY_STATUS_BASE + 8, L"Internet Check: " + g_status.internetCheck);
    UpdateDisabledMenuItem(g_openTrayMenu, IDM_TRAY_STATUS_BASE + 9, L"Active Relay Check: " + g_status.relayCheck);

    if (!g_status.lastError.empty()) {
        std::wstring text = L"Last Error: " + g_status.lastError;
        if (g_openTrayMenuHasLastError) {
            UpdateDisabledMenuItem(g_openTrayMenu, IDM_TRAY_STATUS_BASE + 10, text);
        } else {
            InsertMenuW(g_openTrayMenu, 10, MF_BYPOSITION | MF_STRING | MF_GRAYED, IDM_TRAY_STATUS_BASE + 10, text.c_str());
            g_openTrayMenuHasLastError = true;
        }
    } else if (g_openTrayMenuHasLastError) {
        DeleteMenu(g_openTrayMenu, IDM_TRAY_STATUS_BASE + 10, MF_BYCOMMAND);
        g_openTrayMenuHasLastError = false;
    }
}

void AddRelayInfoItems(HMENU menu, const RelayGroupInfo& group) {
    std::wstring label = group.selected ? L"Selected Group: Yes" : L"Selected Group: No";
    AddDisabledMenuItem(menu, 0, label);
    AddDisabledMenuItem(menu, 0, L"Group Select: " + (group.select.empty() ? L"-" : group.select));
    AddDisabledMenuItem(menu, 0, L"Relays: " + std::to_wstring(group.relayCount));
    AddDisabledMenuItem(menu, 0, L"Selected Relay: " + (group.currentRelay.empty() ? L"-" : RelayShortName(group.currentRelay, group.name)));
    AddDisabledMenuItem(menu, 0, L"Health: " + FormatHealthText(group.currentStatus, group.currentLatency, group.currentTCPConnectLatency, group.error));
}

void AddRelaysSubmenu(HMENU parent, const RelayMenuState& state) {
    HMENU relaysMenu = CreatePopupMenu();
    if (!state.apiReachable) {
        AddDisabledMenuItem(relaysMenu, 0, L"Unavailable: " + (state.lastError.empty() ? L"API unavailable" : state.lastError));
        AppendMenuW(parent, MF_POPUP | MF_GRAYED, reinterpret_cast<UINT_PTR>(relaysMenu), L"Relays");
        return;
    }

    std::wstring mode = state.selectMode;
    std::transform(mode.begin(), mode.end(), mode.begin(), [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    AddDisabledMenuItem(relaysMenu, 0, std::wstring(L"Select: ") + (mode == L"manual" ? L"Manual" : L"Auto"));
    AppendMenuW(relaysMenu, MF_STRING, IDM_RELAY_SELECT_TOGGLE, mode == L"manual" ? L"Change to auto" : L"Change to manual");
    AppendMenuW(relaysMenu, MF_SEPARATOR, 0, nullptr);

    g_relayCommands.clear();
    if (state.groups.empty()) {
        AddDisabledMenuItem(relaysMenu, 0, L"No relay groups");
        AppendMenuW(parent, MF_POPUP, reinterpret_cast<UINT_PTR>(relaysMenu), L"Relays");
        return;
    }

    for (size_t i = 0; i < state.groups.size(); ++i) {
        const RelayGroupInfo& group = state.groups[i];
        HMENU groupMenu = CreatePopupMenu();
        AddRelayInfoItems(groupMenu, group);

        std::wstring groupMode = group.select;
        std::transform(groupMode.begin(), groupMode.end(), groupMode.begin(), [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
        UINT groupModeCommand = IDM_RELAY_GROUP_BASE + static_cast<UINT>(g_relayCommands.size());
        g_relayCommands.push_back({ RelayCommandType::GroupSelectMode, group.name, groupMode == L"manual" ? L"auto" : L"manual" });
        AppendMenuW(groupMenu, MF_STRING, groupModeCommand, groupMode == L"manual" ? L"Change group to auto" : L"Change group to manual");
        AppendMenuW(groupMenu, MF_SEPARATOR, 0, nullptr);

        UINT groupFlags = MF_STRING;
        if (mode != L"manual" || group.selected) {
            groupFlags |= MF_GRAYED;
        }
        UINT groupCommand = IDM_RELAY_GROUP_BASE + static_cast<UINT>(g_relayCommands.size());
        g_relayCommands.push_back({ RelayCommandType::Group, group.name, L"" });
        AppendMenuW(groupMenu, groupFlags, groupCommand, L"Select this group");
        AppendMenuW(groupMenu, MF_SEPARATOR, 0, nullptr);

        bool anyRelay = false;
        for (const RelayInfo& relay : state.relays) {
            if (relay.group != group.name) {
                continue;
            }
            anyRelay = true;
            std::wstring shortName = RelayShortName(relay.name, relay.group);
            std::wstring item = shortName + L" - " + FormatHealthText(relay.status, relay.latency, relay.tcpConnectLatency, relay.error);
            UINT relayFlags = MF_STRING | (relay.selected ? MF_CHECKED : MF_UNCHECKED);
            if (groupMode != L"manual" || relay.selected) {
                relayFlags |= MF_GRAYED;
            }
            UINT relayCommand = IDM_RELAY_RELAY_BASE + static_cast<UINT>(g_relayCommands.size());
            g_relayCommands.push_back({ RelayCommandType::Relay, group.name, shortName });
            AppendMenuW(groupMenu, relayFlags, relayCommand, item.c_str());
        }
        if (!anyRelay) {
            AddDisabledMenuItem(groupMenu, 0, L"No relays");
        }

        AppendMenuW(relaysMenu, MF_POPUP | (group.selected ? MF_CHECKED : MF_UNCHECKED),
            reinterpret_cast<UINT_PTR>(groupMenu), group.name.c_str());
        if (i + 1 < state.groups.size()) {
            AppendMenuW(relaysMenu, MF_SEPARATOR, 0, nullptr);
        }
    }

    AppendMenuW(parent, MF_POPUP, reinterpret_cast<UINT_PTR>(relaysMenu), L"Relays");
}

void ShowTrayMenu(HWND hWnd) {
    StartAsyncStatusRefresh(hWnd);
    StartAsyncRelayMenuRefresh(hWnd);
    RelayMenuState relayState = CurrentRelayMenuState();

    HMENU menu = CreatePopupMenu();
    AddDisabledMenuItem(menu, IDM_TRAY_STATUS_BASE + 0, L"State: " + g_status.state);
    AddDisabledMenuItem(menu, IDM_TRAY_STATUS_BASE + 1, L"Version: " + g_status.version);
    AddDisabledMenuItem(menu, IDM_TRAY_STATUS_BASE + 2, L"Uptime: " + g_status.uptime);
    AddDisabledMenuItem(menu, IDM_TRAY_STATUS_BASE + 3, L"Relay Group: " + g_status.relayGroup);
    AddDisabledMenuItem(menu, IDM_TRAY_STATUS_BASE + 4, L"Relay: " + g_status.relay);
    AddDisabledMenuItem(menu, IDM_TRAY_STATUS_BASE + 5, L"Relay Status: " + g_status.relayStatus);
    AddDisabledMenuItem(menu, IDM_TRAY_STATUS_BASE + 6, L"Download: " + g_status.downloadTotal + L" total, " + g_status.downloadSpeed);
    AddDisabledMenuItem(menu, IDM_TRAY_STATUS_BASE + 7, L"Upload: " + g_status.uploadTotal + L" total, " + g_status.uploadSpeed);
    AddDisabledMenuItem(menu, IDM_TRAY_STATUS_BASE + 8, L"Internet Check: " + g_status.internetCheck);
    AddDisabledMenuItem(menu, IDM_TRAY_STATUS_BASE + 9, L"Active Relay Check: " + g_status.relayCheck);
    if (!g_status.lastError.empty()) {
        AddDisabledMenuItem(menu, IDM_TRAY_STATUS_BASE + 10, L"Last Error: " + g_status.lastError);
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AddRelaysSubmenu(menu, relayState);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    UINT startFlags = MF_STRING;
    if (g_status.processRunning || g_starting) {
        startFlags |= MF_GRAYED;
    }
    AppendMenuW(menu, startFlags, IDM_TRAY_START, L"Start punchd as Administrator");

    UINT stopFlags = MF_STRING;
    if (!g_status.processRunning || g_stopping) {
        stopFlags |= MF_GRAYED;
    }
    AppendMenuW(menu, stopFlags, IDM_TRAY_STOP, L"Stop punchd as Administrator");

    AppendMenuW(menu, MF_STRING, IDM_TRAY_LOGS, L"Logs");
    AppendMenuW(menu, MF_STRING, IDM_TRAY_CONFIG, L"Config");
    AppendMenuW(menu, MF_STRING, IDM_TRAY_EXIT, L"Exit");

    POINT pt{};
    GetCursorPos(&pt);
    SetForegroundWindow(hWnd);
    g_openTrayMenu = menu;
    g_openTrayMenuHasLastError = !g_status.lastError.empty();
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_LEFTALIGN, pt.x, pt.y, 0, hWnd, nullptr);
    g_openTrayMenu = nullptr;
    g_openTrayMenuHasLastError = false;
    DestroyMenu(menu);
}

void AddTrayIcon(HWND hWnd) {
    DestroyTrayIconHandle();
    g_trayIcon = LoadIconForDpi(hInst, IDI_SMALL, SM_CXSMICON, hWnd);

    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hWnd;
    nid.uID = IDI_PUNCHWINDOWS;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon = g_trayIcon;
    wcsncpy_s(nid.szTip, L"Punch", _TRUNCATE);
    Shell_NotifyIconW(NIM_ADD, &nid);
}

void UpdateTrayIcon(HWND hWnd) {
    DestroyTrayIconHandle();
    g_trayIcon = LoadIconForDpi(hInst, IDI_SMALL, SM_CXSMICON, hWnd);

    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hWnd;
    nid.uID = IDI_PUNCHWINDOWS;
    nid.uFlags = NIF_ICON;
    nid.hIcon = g_trayIcon;
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void RemoveTrayIcon(HWND hWnd) {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hWnd;
    nid.uID = IDI_PUNCHWINDOWS;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    DestroyTrayIconHandle();
}

int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
    _In_opt_ HINSTANCE hPrevInstance,
    _In_ LPWSTR lpCmdLine,
    _In_ int nCmdShow) {
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);
    UNREFERENCED_PARAMETER(nCmdShow);

    EnableDpiAwareness();
    LoadAppConfig();

    LoadStringW(hInstance, IDS_APP_TITLE, szTitle, MAX_LOADSTRING);
    LoadStringW(hInstance, IDC_PUNCHWINDOWS, szWindowClass, MAX_LOADSTRING);
    MyRegisterClass(hInstance);

    if (!InitInstance(hInstance, SW_HIDE)) {
        return FALSE;
    }

    HACCEL hAccelTable = LoadAccelerators(hInstance, MAKEINTRESOURCE(IDC_PUNCHWINDOWS));
    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        if (!TranslateAccelerator(msg.hwnd, hAccelTable, &msg)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }
    return static_cast<int>(msg.wParam);
}

ATOM MyRegisterClass(HINSTANCE hInstance) {
    WNDCLASSEXW wcex{};
    wcex.cbSize = sizeof(WNDCLASSEX);
    wcex.lpfnWndProc = WndProc;
    wcex.hInstance = hInstance;
    wcex.hIcon = LoadIconForDpi(hInstance, IDI_PUNCHWINDOWS, SM_CXICON, nullptr);
    wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wcex.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wcex.lpszClassName = szWindowClass;
    wcex.hIconSm = LoadIconForDpi(wcex.hInstance, IDI_SMALL, SM_CXSMICON, nullptr);
    return RegisterClassExW(&wcex);
}

BOOL InitInstance(HINSTANCE hInstance, int nCmdShow) {
    hInst = hInstance;
    HWND hWnd = CreateWindowW(szWindowClass, szTitle, WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, 0, CW_USEDEFAULT, 0, nullptr, nullptr, hInstance, nullptr);
    if (!hWnd) {
        return FALSE;
    }

    ShowWindow(hWnd, nCmdShow);
    AddTrayIcon(hWnd);
    StartAsyncStatusRefresh(hWnd);
    StartAsyncRelayMenuRefresh(hWnd);
    SetTimer(hWnd, IDT_STATUS_REFRESH, StatusRefreshInterval(), nullptr);
    return TRUE;
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_TRAYICON:
        if (LOWORD(lParam) == WM_RBUTTONUP || LOWORD(lParam) == WM_LBUTTONUP) {
            ShowTrayMenu(hWnd);
        }
        break;
    case WM_TIMER:
        if (wParam == IDT_STATUS_REFRESH) {
            StartAsyncStatusRefresh(hWnd);
            SetTimer(hWnd, IDT_STATUS_REFRESH, StatusRefreshInterval(), nullptr);
        }
        break;
    case WM_STATUS_REFRESHED: {
        PunchStatus* next = reinterpret_cast<PunchStatus*>(lParam);
        if (next) {
            ApplyStatus(hWnd, *next);
            delete next;
        }
        g_statusRefreshInProgress = false;
        UpdateOpenTrayStatusMenu();
        SetTimer(hWnd, IDT_STATUS_REFRESH, StatusRefreshInterval(), nullptr);
        break;
    }
    case WM_RELAY_MENU_REFRESHED: {
        RelayMenuState* next = reinterpret_cast<RelayMenuState*>(lParam);
        if (next) {
            {
                std::lock_guard<std::mutex> lock(g_relayMenuMutex);
                g_relayMenuState = *next;
                g_hasRelayMenuState = true;
            }
            delete next;
        }
        g_relayMenuRefreshInProgress = false;
        break;
    }
    case WM_DPICHANGED:
        UpdateTrayIcon(hWnd);
        break;
    case WM_COMMAND: {
        int wmId = LOWORD(wParam);
        switch (wmId) {
        case IDM_TRAY_START:
            StartPunchd(hWnd);
            break;
        case IDM_TRAY_STOP:
            StopPunchd(hWnd);
            break;
        case IDM_TRAY_CONFIG:
            DialogBox(hInst, MAKEINTRESOURCE(IDD_CONFIGBOX), hWnd, ConfigDialog);
            break;
        case IDM_TRAY_LOGS:
            DialogBox(hInst, MAKEINTRESOURCE(IDD_LOGSBOX), hWnd, LogsDialog);
            break;
        case IDM_RELAY_SELECT_TOGGLE: {
            RelayMenuState state = CurrentRelayMenuState();
            std::wstring mode = state.selectMode;
            std::transform(mode.begin(), mode.end(), mode.begin(), [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
            SetRelaySelectMode(hWnd, mode == L"manual" ? L"auto" : L"manual");
            break;
        }
        case IDM_TRAY_EXIT:
        case IDM_EXIT:
            DestroyWindow(hWnd);
            break;
        default:
            if (wmId >= static_cast<int>(IDM_RELAY_GROUP_BASE) && wmId < static_cast<int>(IDM_RELAY_GROUP_BASE + 1000)) {
                size_t index = static_cast<size_t>(wmId - IDM_RELAY_GROUP_BASE);
                if (index < g_relayCommands.size() && g_relayCommands[index].type == RelayCommandType::Group) {
                    SelectRelayGroup(hWnd, g_relayCommands[index].group);
                    break;
                }
                if (index < g_relayCommands.size() && g_relayCommands[index].type == RelayCommandType::GroupSelectMode) {
                    SetRelayGroupSelectMode(hWnd, g_relayCommands[index].group, g_relayCommands[index].relay);
                    break;
                }
            }
            if (wmId >= static_cast<int>(IDM_RELAY_RELAY_BASE) && wmId < static_cast<int>(IDM_RELAY_RELAY_BASE + 1000)) {
                size_t index = static_cast<size_t>(wmId - IDM_RELAY_RELAY_BASE);
                if (index < g_relayCommands.size() && g_relayCommands[index].type == RelayCommandType::Relay) {
                    SelectRelay(hWnd, g_relayCommands[index].group, g_relayCommands[index].relay);
                    break;
                }
            }
            return DefWindowProc(hWnd, message, wParam, lParam);
        }
        break;
    }
    case WM_DESTROY:
        KillTimer(hWnd, IDT_STATUS_REFRESH);
        RemoveTrayIcon(hWnd);
        PostQuitMessage(0);
        break;
    default:
        return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

INT_PTR CALLBACK About(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam) {
    UNREFERENCED_PARAMETER(lParam);
    switch (message) {
    case WM_INITDIALOG:
        return TRUE;
    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL) {
            EndDialog(hDlg, LOWORD(wParam));
            return TRUE;
        }
        break;
    }
    return FALSE;
}

std::wstring GetDialogText(HWND hDlg, int controlID) {
    int len = GetWindowTextLengthW(GetDlgItem(hDlg, controlID));
    std::wstring text(static_cast<size_t>(len + 1), L'\0');
    if (len > 0) {
        GetDlgItemTextW(hDlg, controlID, text.data(), len + 1);
    }
    text.resize(static_cast<size_t>(len));
    return text;
}

INT_PTR CALLBACK ConfigDialog(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam) {
    UNREFERENCED_PARAMETER(lParam);
    switch (message) {
    case WM_INITDIALOG:
        SetDlgItemTextW(hDlg, IDC_API_ADDRESS, g_appConfig.apiAddress.c_str());
        SetDlgItemTextW(hDlg, IDC_API_SECRET, g_appConfig.apiSecret.c_str());
        CheckDlgButton(hDlg, IDC_UPDATE_PUNCHD_SETTINGS, BST_UNCHECKED);
        EnableWindow(GetDlgItem(hDlg, IDC_UPDATE_PUNCHD_SETTINGS), g_status.apiReachable ? TRUE : FALSE);
        return TRUE;
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDOK: {
            AppConfig next;
            next.apiAddress = GetDialogText(hDlg, IDC_API_ADDRESS);
            next.apiSecret = GetDialogText(hDlg, IDC_API_SECRET);
            bool updatePunchdSettings = IsDlgButtonChecked(hDlg, IDC_UPDATE_PUNCHD_SETTINGS) == BST_CHECKED;
            EndDialog(hDlg, IDOK);
            ApplyConfig(GetParent(hDlg), next, updatePunchdSettings);
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(hDlg, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

std::wstring JsonValueField(const std::string& json) {
    std::string field = "\"value\"";
    size_t pos = json.find(field);
    if (pos == std::string::npos) {
        return L"";
    }
    pos = json.find(':', pos);
    if (pos == std::string::npos) {
        return L"";
    }
    ++pos;
    while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) {
        ++pos;
    }
    if (pos >= json.size() || json[pos] != '"') {
        return L"";
    }
    ++pos;
    std::string value;
    bool escape = false;
    for (; pos < json.size(); ++pos) {
        char ch = json[pos];
        if (escape) {
            value.push_back(ch);
            escape = false;
            continue;
        }
        if (ch == '\\') {
            escape = true;
            continue;
        }
        if (ch == '"') {
            break;
        }
        value.push_back(ch);
    }
    return Utf8ToWide(value);
}

void AppendLogLine(HWND hDlg, const std::wstring& line) {
    HWND edit = GetDlgItem(hDlg, IDC_LOG_OUTPUT);
    int len = GetWindowTextLengthW(edit);
    if (len > 900000) {
        SendMessageW(edit, EM_SETSEL, 0, 200000);
        SendMessageW(edit, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(L""));
        len = GetWindowTextLengthW(edit);
    }
    std::wstring text = line + L"\r\n";
    SendMessageW(edit, EM_SETSEL, len, len);
    SendMessageW(edit, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(text.c_str()));
    SendMessageW(edit, WM_HSCROLL, SB_LEFT, 0);
    SendMessageW(edit, WM_VSCROLL, SB_BOTTOM, 0);
}

std::wstring ComboLogLevel(HWND combo) {
    int index = static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0));
    switch (index) {
    case 0:
        return L"debug";
    case 2:
        return L"warn";
    case 3:
        return L"error";
    default:
        return L"info";
    }
}

void SetComboLogLevel(HWND combo, const std::wstring& level) {
    int index = 1;
    if (level == L"debug") {
        index = 0;
    } else if (level == L"warn") {
        index = 2;
    } else if (level == L"error") {
        index = 3;
    }
    SendMessageW(combo, CB_SETCURSEL, index, 0);
}

void ResizeLogsDialog(HWND hDlg, int width, int height) {
    RECT dlu{ 0, 0, 10, 10 };
    MapDialogRect(hDlg, &dlu);
    int marginX = dlu.right;
    int marginY = dlu.bottom;

    RECT statusRect{ 150, 14, 400, 28 };
    RECT outputRect{ 10, 34, 510, 286 };
    RECT buttonRect{ 0, 0, 50, 14 };
    MapDialogRect(hDlg, &statusRect);
    MapDialogRect(hDlg, &outputRect);
    MapDialogRect(hDlg, &buttonRect);

    int statusX = statusRect.left;
    int statusY = statusRect.top;
    int outputX = outputRect.left;
    int outputY = outputRect.top;
    int buttonW = buttonRect.right;
    int buttonH = buttonRect.bottom;

    MoveWindow(GetDlgItem(hDlg, IDC_LOG_STATUS), statusX, statusY, (std::max)(100, width - statusX - marginX), statusRect.bottom - statusRect.top, TRUE);
    MoveWindow(GetDlgItem(hDlg, IDC_LOG_OUTPUT), outputX, outputY, (std::max)(120, width - outputX - marginX), (std::max)(80, height - outputY - buttonH - marginY * 2), TRUE);
    MoveWindow(GetDlgItem(hDlg, IDCANCEL), (std::max)(marginX, width - buttonW - marginX), (std::max)(marginY, height - buttonH - marginY), buttonW, buttonH, TRUE);
}

INT_PTR CALLBACK LogsDialog(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam) {
    UNREFERENCED_PARAMETER(lParam);
    switch (message) {
    case WM_INITDIALOG: {
        StopLogStream();
        HWND combo = GetDlgItem(hDlg, IDC_LOG_LEVEL);
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Debug"));
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Info"));
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Warn"));
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Error"));
        SetComboLogLevel(combo, L"info");

        std::string body;
        std::wstring error;
        if (HttpGet(DefaultAPIAddress() + L"/api/config?key=system.log_level", APIToken(), body, error)) {
            SetComboLogLevel(combo, JsonValueField(body));
        } else {
            SetDlgItemTextW(hDlg, IDC_LOG_STATUS, (L"Cannot read log level: " + error).c_str());
        }

        HWND edit = GetDlgItem(hDlg, IDC_LOG_OUTPUT);
        SendMessageW(edit, EM_SETLIMITTEXT, 1048576, 0);
        SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
        StartLogStream(hDlg);
        return TRUE;
    }
    case WM_SIZE:
        ResizeLogsDialog(hDlg, LOWORD(lParam), HIWORD(lParam));
        return TRUE;
    case WM_LOG_LINE: {
        std::wstring* line = reinterpret_cast<std::wstring*>(lParam);
        if (line) {
            AppendLogLine(hDlg, *line);
            delete line;
        }
        return TRUE;
    }
    case WM_LOG_STATUS: {
        std::wstring* status = reinterpret_cast<std::wstring*>(lParam);
        if (status) {
            SetDlgItemTextW(hDlg, IDC_LOG_STATUS, status->c_str());
            delete status;
        }
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == IDC_LOG_LEVEL && HIWORD(wParam) == CBN_SELCHANGE) {
            HWND combo = GetDlgItem(hDlg, IDC_LOG_LEVEL);
            std::wstring level = ComboLogLevel(combo);
            SetDlgItemTextW(hDlg, IDC_LOG_STATUS, L"Changing verbosity...");
            std::wstring api = DefaultAPIAddress();
            std::wstring token = APIToken();
            std::thread([hDlg, api, token, level]() {
                std::wstring error;
                if (HttpPutConfigValue(api, token, L"system.log_level", level, error)) {
                    PostLogStatus(hDlg, L"Verbosity: " + level);
                } else {
                    PostLogStatus(hDlg, L"Verbosity change failed: " + error);
                }
            }).detach();
            return TRUE;
        }
        if (LOWORD(wParam) == IDCANCEL) {
            StopLogStream();
            EndDialog(hDlg, IDCANCEL);
            return TRUE;
        }
        break;
    case WM_CLOSE:
        StopLogStream();
        EndDialog(hDlg, IDCANCEL);
        return TRUE;
    }
    return FALSE;
}

