#include "TrayController.h"

#include <shellapi.h>

#include "Resource.h"

namespace TrayController {
namespace {

constexpr UINT kTrayIconId = 1;
constexpr UINT kToggleTestModeCommand = 40001;
constexpr UINT kToggleDebugStatusCommand = 40002;
constexpr UINT kReloadConfigCommand = 40003;
constexpr UINT kOpenConfigCommand = 40004;
constexpr UINT kTestBurstCommand = 40005;
constexpr UINT kExitCommand = 40006;
constexpr UINT kOpenLogCommand = 40007;
constexpr UINT kToggleLogEnabledCommand = 40008;
constexpr UINT kTogglePositionCandidatesCommand = 40009;

bool g_trayIconAdded = false;

std::wstring BuildTrayTooltip(const Context& context) {
    std::wstring tooltip = L"D2R Damage Numbers\nD2R: ";
    tooltip += context.hasGameRect ? L"found" : L"missing";
    tooltip += L"\nMemory: ";
    tooltip += context.memoryConnected ? L"connected" : L"disconnected";

    if (context.config) {
        tooltip += L"\nTest: ";
        tooltip += context.config->testMode ? L"on" : L"off";
        tooltip += L"\nLog: ";
        tooltip += context.config->logEnabled ? L"on" : L"off";
    }

    return tooltip;
}

void ShowStatus(const Callbacks& callbacks, const wchar_t* message) {
    if (callbacks.showStatusNotice) {
        callbacks.showStatusNotice(message);
    }
}

void AppendLog(const Callbacks& callbacks, const std::wstring& text) {
    if (callbacks.appendLogBlock) {
        callbacks.appendLogBlock(text);
    }
}

void OpenConfigFile(HWND hwnd, const Callbacks& callbacks) {
    const std::wstring configPath = GetConfigPath();
    HINSTANCE result = ShellExecuteW(hwnd, L"open", configPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);

    if (reinterpret_cast<INT_PTR>(result) <= 32) {
        ShowStatus(callbacks, L"Could not open config");
        return;
    }

    ShowStatus(callbacks, L"Config opened");
}

void OpenLogFile(HWND hwnd, const Callbacks& callbacks) {
    AppendLog(callbacks, L"Log opened from tray menu");

    const std::wstring logPath = GetLogPath();
    HINSTANCE result = ShellExecuteW(hwnd, L"open", logPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);

    if (reinterpret_cast<INT_PTR>(result) <= 32) {
        ShowStatus(callbacks, L"Could not open log");
        return;
    }

    ShowStatus(callbacks, L"Log opened");
}

void ShowTrayMenu(HWND hwnd, const Context& context) {
    if (!context.config) return;

    HMENU menu = CreatePopupMenu();
    if (!menu) return;

    const OverlayConfig& config = *context.config;
    const std::wstring d2rStatus = context.hasGameRect ? L"D2R window: Found" : L"D2R window: Missing";
    const std::wstring memoryStatus = L"Memory: " + context.memoryStatus;
    const std::wstring activeNumbers = L"Active numbers: " + std::to_wstring(context.activeNumberCount);

    AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, d2rStatus.c_str());
    AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, memoryStatus.c_str());
    AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, activeNumbers.c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (config.testMode ? MF_CHECKED : MF_UNCHECKED), kToggleTestModeCommand, L"Test Mode");
    AppendMenuW(menu, MF_STRING | (config.showDebugStatus ? MF_CHECKED : MF_UNCHECKED), kToggleDebugStatusCommand, L"Show Debug Status");
    AppendMenuW(menu, MF_STRING | (config.showPositionCandidates ? MF_CHECKED : MF_UNCHECKED), kTogglePositionCandidatesCommand, L"Show Position Candidates");
    AppendMenuW(menu, MF_STRING | (config.logEnabled ? MF_CHECKED : MF_UNCHECKED), kToggleLogEnabledCommand, L"Write Log");
    AppendMenuW(menu, MF_STRING, kReloadConfigCommand, L"Reload Config");
    AppendMenuW(menu, MF_STRING, kOpenConfigCommand, L"Open Config");
    AppendMenuW(menu, MF_STRING, kOpenLogCommand, L"Open Log");
    AppendMenuW(menu, context.hasGameRect ? MF_STRING : MF_STRING | MF_GRAYED, kTestBurstCommand, L"Spawn Test Burst");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kExitCommand, L"Exit");

    POINT pt{};
    GetCursorPos(&pt);

    SetForegroundWindow(hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_LEFTALIGN, pt.x, pt.y, 0, hwnd, nullptr);
    PostMessageW(hwnd, WM_NULL, 0, 0);

    DestroyMenu(menu);
}

void ToggleDebugStatus(HWND hwnd, Context& context, const Callbacks& callbacks) {
    if (!context.config) return;

    context.config->showDebugStatus = !context.config->showDebugStatus;
    const bool saved = WriteConfigBool(L"Debug", L"ShowStatus", context.config->showDebugStatus);
    ShowStatus(callbacks, saved
        ? (context.config->showDebugStatus ? L"Debug status shown and saved" : L"Debug status hidden and saved")
        : L"Debug status changed but not saved");
    UpdateTrayIconTooltip(hwnd, context);
}

}

void AddTrayIcon(HWND hwnd, const Context& context) {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = kTrayIconId;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = kTrayIconMessage;
    nid.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_D2RDAMAGENUMBERS));

    const std::wstring tooltip = BuildTrayTooltip(context);
    wcsncpy_s(nid.szTip, ARRAYSIZE(nid.szTip), tooltip.c_str(), _TRUNCATE);

    g_trayIconAdded = Shell_NotifyIconW(NIM_ADD, &nid) == TRUE;

    if (g_trayIconAdded) {
        nid.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &nid);
    }
}

void RemoveTrayIcon(HWND hwnd) {
    if (!g_trayIconAdded) return;

    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = kTrayIconId;

    Shell_NotifyIconW(NIM_DELETE, &nid);
    g_trayIconAdded = false;
}

void UpdateTrayIconTooltip(HWND hwnd, const Context& context) {
    if (!g_trayIconAdded || !hwnd) return;

    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = kTrayIconId;
    nid.uFlags = NIF_TIP;

    const std::wstring tooltip = BuildTrayTooltip(context);
    wcsncpy_s(nid.szTip, ARRAYSIZE(nid.szTip), tooltip.c_str(), _TRUNCATE);

    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

bool HandleTrayIconMessage(HWND hwnd, LPARAM lParam, Context& context, const Callbacks& callbacks) {
    if (LOWORD(lParam) == WM_RBUTTONUP || LOWORD(lParam) == WM_CONTEXTMENU) {
        ShowTrayMenu(hwnd, context);
        return true;
    }

    if (LOWORD(lParam) == WM_LBUTTONDBLCLK) {
        ToggleDebugStatus(hwnd, context, callbacks);
        return true;
    }

    return true;
}

bool HandleCommand(HWND hwnd, UINT commandId, Context& context, const Callbacks& callbacks) {
    if (!context.config) return false;

    OverlayConfig& config = *context.config;
    switch (commandId) {
    case kToggleTestModeCommand:
        config.testMode = !config.testMode;
        {
            const bool saved = WriteConfigBool(L"Test", L"Enabled", config.testMode);
            ShowStatus(callbacks, saved
                ? (config.testMode ? L"Test mode enabled and saved" : L"Test mode disabled and saved")
                : L"Test mode changed but not saved");
        }
        UpdateTrayIconTooltip(hwnd, context);
        return true;

    case kToggleDebugStatusCommand:
        ToggleDebugStatus(hwnd, context, callbacks);
        return true;

    case kTogglePositionCandidatesCommand:
        config.showPositionCandidates = !config.showPositionCandidates;
        if (!config.showPositionCandidates && context.positionDebugMarkers) {
            context.positionDebugMarkers->clear();
        }
        {
            const bool saved = WriteConfigBool(L"Debug", L"ShowPositionCandidates", config.showPositionCandidates);
            ShowStatus(callbacks, saved
                ? (config.showPositionCandidates ? L"Position candidates shown and saved" : L"Position candidates hidden and saved")
                : L"Position candidates changed but not saved");
        }
        return true;

    case kToggleLogEnabledCommand:
        config.logEnabled = !config.logEnabled;
        {
            const bool saved = WriteConfigBool(L"Log", L"Enabled", config.logEnabled);
            if (config.logEnabled) {
                AppendLog(callbacks, L"Log writing enabled from tray menu");
            }
            ShowStatus(callbacks, saved
                ? (config.logEnabled ? L"Log writing enabled and saved" : L"Log writing disabled and saved")
                : L"Log writing changed but not saved");
        }
        UpdateTrayIconTooltip(hwnd, context);
        return true;

    case kReloadConfigCommand:
        if (callbacks.reloadConfig) {
            callbacks.reloadConfig();
        }
        if (callbacks.restartFrameTimer) {
            callbacks.restartFrameTimer(hwnd);
        }
        if (callbacks.trimActiveNumbers) {
            callbacks.trimActiveNumbers();
        }
        ShowStatus(callbacks, L"Config reloaded");
        UpdateTrayIconTooltip(hwnd, context);
        return true;

    case kOpenConfigCommand:
        OpenConfigFile(hwnd, callbacks);
        return true;

    case kOpenLogCommand:
        OpenLogFile(hwnd, callbacks);
        return true;

    case kTestBurstCommand:
        if (callbacks.queueTestBurstAtGameCenter) {
            callbacks.queueTestBurstAtGameCenter();
        }
        ShowStatus(callbacks, context.hasGameRect ? L"Test burst spawned" : L"D2R window missing");
        return true;

    case kExitCommand:
        DestroyWindow(hwnd);
        return true;

    default:
        return false;
    }
}

}
