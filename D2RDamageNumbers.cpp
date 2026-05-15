#include <windows.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <cstdint>
#include <vector>
#include <string>
#include <chrono>
#include <algorithm>
#include <cmath>
#include <climits>
#include <cwchar>
#include <cstdlib>
#include <cstring>

#include "DamageNumberSystem.h"
#include "DamageModel.h"
#include "D2RMemoryLayout.h"
#include "DpsTracker.h"
#include "FrameTimer.h"
#include "MemoryScanner.h"
#include "OverlayDebugModel.h"
#include "OverlayConfig.h"
#include "OverlayRenderer.h"
#include "Resource.h"
#include "TrayController.h"

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "psapi.lib")

static HWND g_overlayWnd = nullptr;
static HWND g_gameWnd = nullptr;
static RECT g_gameRect{};
static bool g_hasGameRect = false;
static OverlayConfig g_config;
static std::chrono::steady_clock::time_point g_lastFrame;
static std::chrono::steady_clock::time_point g_statusNoticeUntil;
static std::wstring g_statusNotice;
static float g_trayTooltipRefreshSeconds = 0.0f;
static float g_memoryLogRefreshSeconds = 0.0f;
static float g_smoothedFrameSeconds = 0.0f;
static std::wstring g_lastLoggedMemoryText;

static std::wstring BuildMemoryLogText();
static void AppendLogBlock(const std::wstring& text);
static void LogMemoryStatusIfChanged();
static void QueueDamageEvent(const DamageEvent& event);
static bool TryGetCursorInGameRect(float& x, float& y);
static void ShowStatusNotice(const wchar_t* message);
static bool IsStatusNoticeActive();
static std::wstring BuildDebugStatusText();
static void LoadConfig();
static void RestartFrameTimer(HWND hwnd);
static void QueueTestBurstAtGameCenter();

static int ClampInt(int value, int minValue, int maxValue) {
    return max(minValue, min(maxValue, value));
}

static float ClampFloat(float value, float minValue, float maxValue) {
    return max(minValue, min(maxValue, value));
}

static std::string WideToUtf8(const std::wstring& text) {
    if (text.empty()) return std::string();

    const int length = WideCharToMultiByte(
        CP_UTF8,
        0,
        text.c_str(),
        static_cast<int>(text.size()),
        nullptr,
        0,
        nullptr,
        nullptr
    );

    if (length <= 0) return std::string();

    std::string output(length, '\0');
    WideCharToMultiByte(
        CP_UTF8,
        0,
        text.c_str(),
        static_cast<int>(text.size()),
        &output[0],
        length,
        nullptr,
        nullptr
    );

    return output;
}

static std::wstring FormatLogTimestamp() {
    SYSTEMTIME time{};
    GetLocalTime(&time);

    wchar_t text[64]{};
    swprintf_s(
        text,
        L"%04u-%02u-%02u %02u:%02u:%02u.%03u",
        time.wYear,
        time.wMonth,
        time.wDay,
        time.wHour,
        time.wMinute,
        time.wSecond,
        time.wMilliseconds
    );

    return text;
}

static void AppendLogBlock(const std::wstring& text) {
    if (!g_config.logEnabled) return;

    const std::wstring logPath = GetLogPath();
    HANDLE file = CreateFileW(
        logPath.c_str(),
        FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );

    if (file == INVALID_HANDLE_VALUE) return;

    std::wstring block = FormatLogTimestamp();
    block += L"\r\n";
    block += text;
    block += L"\r\n\r\n";

    const std::string utf8 = WideToUtf8(block);
    if (!utf8.empty()) {
        DWORD bytesWritten = 0;
        WriteFile(file, utf8.data(), static_cast<DWORD>(utf8.size()), &bytesWritten, nullptr);
    }

    CloseHandle(file);
}

static std::wstring FormatPatternValue(uintptr_t address, size_t matches, const std::wstring& patternName) {
    std::wstring text = address != 0 ? FormatHex(address) : std::to_wstring(matches);

    if (!patternName.empty()) {
        text += L" (";
        text += patternName;
        text += L")";
    }

    return text;
}

static std::wstring BuildMemoryLogText() {
    std::wstring text = L"D2R: ";
    text += g_hasGameRect ? L"found" : L"missing";
    text += L"\nMemory: ";
    text += g_memorySource.connected ? L"connected" : L"disconnected";
    text += L" (";
    text += g_memorySource.status;
    text += L")";

    if (g_memorySource.connected) {
        text += L"\nPID: ";
        text += std::to_wstring(g_memorySource.processId);

        if (!g_memorySource.processPath.empty()) {
            text += L"\nProcess: ";
            text += g_memorySource.processPath;
        }

        text += L"\nModule: ";
        text += g_memorySource.moduleName.empty() ? L"(unknown)" : g_memorySource.moduleName;
        text += L" ";
        text += FormatHex(g_memorySource.moduleBase);
        text += L" size=";
        text += std::to_wstring(g_memorySource.moduleSize);
        text += L" read=";
        text += std::to_wstring(g_memorySource.lastModuleBytesRead);

        text += L"\nPatterns: hover=";
        text += FormatPatternValue(
            g_memorySource.mouseHoverAddress,
            g_memorySource.mouseHoverPatternMatches,
            g_memorySource.mouseHoverPatternName
        );
        text += L" game=";
        text += FormatPatternValue(
            g_memorySource.currentSinglePlayerGameAddress,
            g_memorySource.currentGamePatternMatches,
            g_memorySource.currentGamePatternName
        );
        text += L" unit=";
        text += FormatPatternValue(
            g_memorySource.unitHashTableAddress,
            g_memorySource.unitHashPatternMatches,
            g_memorySource.unitHashPatternName
        );
    }

    if (g_memorySource.hoverReady) {
        text += L"\nHover: ";
        text += g_memorySource.hover.isHovered ? L"yes" : L"no";
        text += L" type=";
        text += std::to_wstring(g_memorySource.hover.hoveredUnitType);
        text += L" id=";
        text += std::to_wstring(g_memorySource.hover.hoveredUnitId);
        text += L" currentGame=";
        text += FormatHex(g_memorySource.currentSinglePlayerGame);
    }

    text += L"\nHP: ";
    if (g_memorySource.hpReady) {
        text += std::to_wstring(g_memorySource.hoveredHp);
        text += L"/";
        text += std::to_wstring(g_memorySource.hoveredMaxHp);
        text += L" serverUnit=";
        text += FormatHex(g_memorySource.hoveredUnitAddress);
        text += L" clientUnit=";
        text += FormatHex(g_memorySource.hoveredClientUnitAddress);
    }
    else {
        text += g_memorySource.hpStatus;
    }
    text += L"\nMonster tracking: ";
    text += g_config.trackAllMonsters ? L"all" : L"hover";
    text += L" scanned=";
    text += std::to_wstring(g_memorySource.monsterScanCount);
    text += L" tracked=";
    text += std::to_wstring(g_memorySource.monsterTrackedCount);

    if (g_memorySource.lastDamageAmount > 0) {
        text += L"\nLast damage: ";
        text += std::to_wstring(g_memorySource.lastDamageAmount);
        text += L" ";
        text += DamageKindName(g_memorySource.lastDamageKind);
        text += L" critSource=none";
    }

    if (!g_memorySource.patternDiagnostics.empty()) {
        text += L"\n";
        text += g_memorySource.patternDiagnostics;
    }

    if (!g_memorySource.currentGameCandidateDiagnostics.empty()) {
        text += L"\n";
        text += g_memorySource.currentGameCandidateDiagnostics;
    }

    return text;
}

static void LogMemoryStatusIfChanged() {
    const std::wstring text = BuildMemoryLogText();
    if (text == g_lastLoggedMemoryText) return;

    g_lastLoggedMemoryText = text;
    AppendLogBlock(text);
}

static void LoadConfig() {
    LoadOverlayConfig(g_config);
    ReloadOverlayRendererConfig(g_config);
}

static MemoryScannerFrameContext BuildMemoryScannerFrameContext() {
    MemoryScannerFrameContext context{};
    context.gameWnd = g_gameWnd;
    context.gameRect = g_gameRect;
    context.hasGameRect = g_hasGameRect;
    context.config = &g_config;
    return context;
}

static void ConfigureMemoryScannerIntegration() {
    MemoryScannerCallbacks callbacks{};
    callbacks.appendLogBlock = AppendLogBlock;
    callbacks.queueDamageEvent = QueueDamageEvent;
    callbacks.tryGetCursorInGameRect = TryGetCursorInGameRect;
    callbacks.showStatusNotice = ShowStatusNotice;
    callbacks.buildMemoryLogText = BuildMemoryLogText;
    ConfigureMemoryScannerCallbacks(callbacks);
}

static TrayController::Context BuildTrayControllerContext() {
    TrayController::Context context{};
    context.config = &g_config;
    context.positionDebugMarkers = &g_positionDebugMarkers;
    context.hasGameRect = g_hasGameRect;
    context.memoryConnected = g_memorySource.connected;
    context.memoryStatus = g_memorySource.status;
    context.activeNumberCount = DamageNumberSystem::ActiveCount();
    return context;
}

static void TrimDamageNumbersForConfig() {
    DamageNumberSystem::TrimActiveNumbers(g_config);
}

static TrayController::Callbacks BuildTrayControllerCallbacks() {
    TrayController::Callbacks callbacks{};
    callbacks.appendLogBlock = AppendLogBlock;
    callbacks.showStatusNotice = ShowStatusNotice;
    callbacks.reloadConfig = LoadConfig;
    callbacks.restartFrameTimer = RestartFrameTimer;
    callbacks.trimActiveNumbers = TrimDamageNumbersForConfig;
    callbacks.queueTestBurstAtGameCenter = QueueTestBurstAtGameCenter;
    return callbacks;
}

static void RestartFrameTimer(HWND hwnd) {
    FrameTimer::Restart(hwnd, g_config.renderFps);
}

static void QueueDamageEvent(const DamageEvent& event) {
    DamageNumberSystem::QueueDamageEvent(event, g_overlayWnd != nullptr);
}

static bool TryGetCursorInGameRect(float& x, float& y) {
    if (!g_hasGameRect) return false;

    POINT cursor{};
    if (!GetCursorPos(&cursor)) return false;

    const int width = g_gameRect.right - g_gameRect.left;
    const int height = g_gameRect.bottom - g_gameRect.top;

    x = static_cast<float>(cursor.x - g_gameRect.left);
    y = static_cast<float>(cursor.y - g_gameRect.top);

    return x >= 0.0f && y >= 0.0f && x <= width && y <= height;
}

static void QueueTestDamageAtCursor(DamageKind damageKind) {
    float x = 0.0f;
    float y = 0.0f;
    if (!TryGetCursorInGameRect(x, y)) return;

    DamageNumberSystem::QueueTestDamageAt(g_config, g_overlayWnd != nullptr, x, y, damageKind);
}

static void QueueTestBurstAtCursor() {
    float x = 0.0f;
    float y = 0.0f;
    if (!TryGetCursorInGameRect(x, y)) return;

    DamageNumberSystem::QueueTestBurstAt(g_config, g_overlayWnd != nullptr, x, y);
}

static void QueueTestBurstAtGameCenter() {
    if (!g_hasGameRect) return;

    const float width = static_cast<float>(g_gameRect.right - g_gameRect.left);
    const float height = static_cast<float>(g_gameRect.bottom - g_gameRect.top);
    const float x = width * 0.5f;
    const float y = height * 0.45f;

    DamageNumberSystem::QueueTestBurstAt(g_config, g_overlayWnd != nullptr, x, y);
}

static bool WasHotkeyPressed(int virtualKey) {
    return virtualKey > 0 && (GetAsyncKeyState(virtualKey) & 1) != 0;
}

static void HandleTestHotkeys() {
    if (!g_config.testMode) return;

    if (WasHotkeyPressed(g_config.normalHotkey)) {
        QueueTestDamageAtCursor(DamageKind::Normal);
    }

    if (WasHotkeyPressed(g_config.critHotkey)) {
        QueueTestDamageAtCursor(DamageKind::Critical);
    }

    if (WasHotkeyPressed(g_config.burstHotkey)) {
        QueueTestBurstAtCursor();
    }
}

static void HandleHitRecorderHotkeys() {
    if (WasHotkeyPressed(g_config.positionCandidateHotkey)) {
        g_config.showPositionCandidates = !g_config.showPositionCandidates;
        if (!g_config.showPositionCandidates) {
            g_positionDebugMarkers.clear();
        }
        ShowStatusNotice(g_config.showPositionCandidates ? L"Position candidates on" : L"Position candidates off");
    }

    if (WasHotkeyPressed(g_config.positionProbeHotkey)) {
        WritePositionProbeMarker();
    }

    if (WasHotkeyPressed(g_config.hitMarkerHotkey)) {
        WriteHitRecorderMarker();
    }
}

static BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM) {
    if (!IsWindowVisible(hwnd)) return TRUE;

    wchar_t title[512]{};
    GetWindowTextW(hwnd, title, 512);

    std::wstring t = title;

    if (t.find(L"Diablo II: Resurrected") != std::wstring::npos ||
        t.find(L"Diablo II Resurrected") != std::wstring::npos) {
        g_gameWnd = hwnd;
        return FALSE;
    }

    return TRUE;
}

static HWND FindD2RWindow() {
    g_gameWnd = nullptr;
    EnumWindows(EnumWindowsProc, 0);
    return g_gameWnd;
}

static bool HasCommandLineSwitch(const wchar_t* switchName) {
    if (!switchName || switchName[0] == L'\0') return false;

    const wchar_t* commandLine = GetCommandLineW();
    return commandLine && wcsstr(commandLine, switchName) != nullptr;
}

static void UpdateOverlayPosition() {
    HWND gameWnd = FindD2RWindow();

    if (!gameWnd || !IsWindow(gameWnd)) {
        g_hasGameRect = false;
        ShowWindow(g_overlayWnd, SW_HIDE);
        return;
    }

    RECT rc{};
    GetWindowRect(gameWnd, &rc);

    int width = rc.right - rc.left;
    int height = rc.bottom - rc.top;

    if (width <= 0 || height <= 0) {
        g_hasGameRect = false;
        ShowWindow(g_overlayWnd, SW_HIDE);
        return;
    }

    g_gameRect = rc;
    g_hasGameRect = true;

    ShowWindow(g_overlayWnd, SW_SHOWNA);

    SetWindowPos(
        g_overlayWnd,
        HWND_TOPMOST,
        rc.left,
        rc.top,
        width,
        height,
        SWP_NOACTIVATE
    );
}

static bool IsStatusNoticeActive() {
    return !g_statusNotice.empty() && std::chrono::steady_clock::now() < g_statusNoticeUntil;
}

static void ShowStatusNotice(const wchar_t* message) {
    g_statusNotice = message ? message : L"";
    g_statusNoticeUntil = std::chrono::steady_clock::now() + std::chrono::milliseconds(1800);
}

static std::wstring BuildDebugStatusText() {
    std::wstring status = L"D2R: ";
    status += g_hasGameRect ? L"found" : L"missing";
    status += L"\nTest mode: ";
    status += g_config.testMode ? L"on" : L"off";
    status += L"\nNumbers: ";
    status += std::to_wstring(DamageNumberSystem::ActiveCount());
    status += L"\nPending events: ";
    status += std::to_wstring(DamageNumberSystem::PendingCount());
    status += L"\nRender: ";
    status += std::to_wstring(g_config.renderFps);
    status += L" fps";
    if (g_smoothedFrameSeconds > 0.0001f) {
        status += L" actual~";
        status += std::to_wstring(static_cast<int>(std::lround(1.0f / g_smoothedFrameSeconds)));
    }
    status += L"\nLog: ";
    status += g_config.logEnabled ? L"on" : L"off";
    status += L"\nHit recorder: ";
    status += g_config.hitRecorderEnabled ? L"on" : L"off";
    status += L" samples=";
    status += std::to_wstring(g_hitRecorderSamples.size());
    status += L" hits=";
    status += std::to_wstring(g_hitRecorderDamages.size());
    if (g_lastHitRecorderDamage.amount > 0) {
        status += L" last=";
        status += std::to_wstring(g_lastHitRecorderDamage.amount);
        status += L" id=";
        status += std::to_wstring(g_lastHitRecorderDamage.sampleId);
    }
    status += L"\nMonster tracking: ";
    status += g_config.trackAllMonsters ? L"all" : L"hover";
    status += L" scanned=";
    status += std::to_wstring(g_memorySource.monsterScanCount);
    status += L" tracked=";
    status += std::to_wstring(g_memorySource.monsterTrackedCount);
    status += L"\nPos candidates: ";
    status += g_config.showPositionCandidates ? L"on" : L"off";
    status += L" markers=";
    status += std::to_wstring(g_positionDebugMarkers.size());
    status += L"\nWorld pos: ";
    status += g_config.useWorldPositions ? L"on" : L"off";
    status += g_config.followDamageTargets ? L" follow" : L" static";
    if (g_config.learnWorldProjection && g_worldProjectionModel.ready) {
        status += L" learned samples=";
        status += std::to_wstring(g_worldProjectionModel.sampleCount);
        status += L" err=";
        status += std::to_wstring(static_cast<int>(g_worldProjectionModel.rmsError));
    }
    else {
        status += g_worldProjectionCalibrated ? L" calibrated" : L" uncalibrated";
        status += L" samples=";
        status += std::to_wstring(g_worldProjectionSamples.size());
    }
    status += L"\nWorld coord: ";
    if (!g_config.useWorldCoordCandidates) {
        status += L"off";
    }
    else if (!g_config.learnWorldProjection) {
        status += L"off";
    }
    else if (g_worldCoordModel.ready) {
        status += L"best ";
        status += FormatWorldCoordDescriptor(g_worldCoordModel.descriptor);
        status += L" samples=";
        status += std::to_wstring(g_worldCoordModel.projection.sampleCount);
        status += L" units=";
        status += std::to_wstring(g_worldCoordModel.distinctUnitCount);
        status += L" rms=";
        status += std::to_wstring(static_cast<int>(g_worldCoordModel.projection.rmsError));
        status += L" offset=";
        status += std::to_wstring(static_cast<int>(std::lround(g_config.worldCoordScreenOffsetX)));
        status += L",";
        status += std::to_wstring(static_cast<int>(std::lround(g_config.worldCoordScreenOffsetY)));
    }
    else {
        status += L"learning hovers=";
        status += std::to_wstring(g_worldCoordHoverSamples);
        status += L" candidates=";
        status += std::to_wstring(g_worldCoordCandidates.size());
        status += L" top=";
        std::wstring top = FormatTopWorldCoordCandidates(1);
        const size_t newline = top.find(L'\n');
        if (newline != std::wstring::npos) {
            top.resize(newline);
        }
        status += top;
    }
    status += L"\nScreen lock: ";
    if (!g_config.useScreenAnchorCandidates) {
        status += L"off";
    }
    else if (g_screenAnchorModel.ready) {
        status += L"best ";
        status += FormatScreenAnchorDescriptor(g_screenAnchorModel.descriptor);
        status += L" samples=";
        status += std::to_wstring(g_screenAnchorModel.sampleCount);
        status += L" avg=";
        status += std::to_wstring(static_cast<int>(g_screenAnchorModel.averageError));
        status += L" last=";
        status += std::to_wstring(static_cast<int>(g_screenAnchorModel.lastError));
        status += L" xy=";
        status += std::to_wstring(static_cast<int>(std::lround(g_screenAnchorModel.lastX)));
        status += L",";
        status += std::to_wstring(static_cast<int>(std::lround(g_screenAnchorModel.lastY)));
        status += L" units=";
        status += std::to_wstring(g_screenAnchorModel.distinctUnitCount);
        status += L" span=";
        status += std::to_wstring(static_cast<int>(g_screenAnchorModel.movementSpan));
    }
    else {
        status += L"learning hovers=";
        status += std::to_wstring(g_screenAnchorHoverSamples);
        status += L" candidates=";
        status += std::to_wstring(g_screenAnchorCandidates.size());
        status += L" top=";
        std::wstring top = FormatTopScreenAnchorCandidates(1);
        const size_t newline = top.find(L'\n');
        if (newline != std::wstring::npos) {
            top.resize(newline);
        }
        status += top;
    }
    status += L"\nMemory: ";
    status += g_memorySource.connected ? L"connected" : L"disconnected";
    status += L" (";
    status += g_memorySource.status;
    status += L")";

    if (g_memorySource.connected) {
        status += L"\nPID: ";
        status += std::to_wstring(g_memorySource.processId);
        status += L"\nModule: ";
        status += g_memorySource.moduleName.empty() ? L"(unknown)" : g_memorySource.moduleName;
        status += L" ";
        status += FormatHex(g_memorySource.moduleBase);
        status += L" size=";
        status += std::to_wstring(g_memorySource.moduleSize / (1024 * 1024));
        status += L"MB read=";
        status += std::to_wstring(g_memorySource.lastModuleBytesRead / (1024 * 1024));
        status += L"MB";
        status += L"\nPatterns: hover=";
        status += g_memorySource.mouseHoverAddress != 0
            ? FormatHex(g_memorySource.mouseHoverAddress)
            : std::to_wstring(g_memorySource.mouseHoverPatternMatches);
        if (!g_memorySource.mouseHoverPatternName.empty()) {
            status += L" ";
            status += g_memorySource.mouseHoverPatternName;
        }
        status += L" game=";
        status += g_memorySource.currentSinglePlayerGameAddress != 0
            ? FormatHex(g_memorySource.currentSinglePlayerGameAddress)
            : std::to_wstring(g_memorySource.currentGamePatternMatches);
        if (!g_memorySource.currentGamePatternName.empty()) {
            status += L" ";
            status += g_memorySource.currentGamePatternName;
        }
        status += L" unit=";
        status += g_memorySource.unitHashTableAddress != 0
            ? FormatHex(g_memorySource.unitHashTableAddress)
            : std::to_wstring(g_memorySource.unitHashPatternMatches);
        if (!g_memorySource.unitHashPatternName.empty()) {
            status += L" ";
            status += g_memorySource.unitHashPatternName;
        }
    }

    if (g_memorySource.hoverReady) {
        status += L"\nHover: ";
        status += g_memorySource.hover.isHovered ? L"yes" : L"no";
        status += L" type=";
        status += std::to_wstring(g_memorySource.hover.hoveredUnitType);
        status += L" id=";
        status += std::to_wstring(g_memorySource.hover.hoveredUnitId);
    }

    status += L"\nHP: ";
    if (g_memorySource.hpReady) {
        status += std::to_wstring(g_memorySource.hoveredHp);
        status += L"/";
        status += std::to_wstring(g_memorySource.hoveredMaxHp);
    }
    else {
        status += g_memorySource.hpStatus;
    }

    if (g_memorySource.lastDamageAmount > 0) {
        status += L"\nLast hit: ";
        status += std::to_wstring(g_memorySource.lastDamageAmount);
        status += L" ";
        status += DamageKindName(g_memorySource.lastDamageKind);
    }

    if (IsStatusNoticeActive()) {
        status += L"\n";
        status += g_statusNotice;
    }

    return status;
}

static void RunOverlayFrame(HWND hwnd) {
    auto now = std::chrono::steady_clock::now();
    std::chrono::duration<float> elapsed = now - g_lastFrame;
    g_lastFrame = now;

    const float dt = ClampFloat(elapsed.count(), 0.0f, 0.10f);
    if (g_smoothedFrameSeconds <= 0.0f) {
        g_smoothedFrameSeconds = dt;
    }
    else {
        g_smoothedFrameSeconds += (dt - g_smoothedFrameSeconds) * 0.08f;
    }

    UpdateOverlayPosition();
    const MemoryScannerFrameContext memoryContext = BuildMemoryScannerFrameContext();
    PollMemorySource(memoryContext, dt);
    g_memoryLogRefreshSeconds += dt;
    if (g_memoryLogRefreshSeconds >= 0.25f) {
        g_memoryLogRefreshSeconds = 0.0f;
        LogMemoryStatusIfChanged();
    }

    g_trayTooltipRefreshSeconds += dt;
    if (g_trayTooltipRefreshSeconds >= 1.0f) {
        g_trayTooltipRefreshSeconds = 0.0f;
        TrayController::Context trayContext = BuildTrayControllerContext();
        TrayController::UpdateTrayIconTooltip(g_overlayWnd, trayContext);
    }

    HandleHitRecorderHotkeys();
    HandleTestHotkeys();
    DpsTracker::Update(dt, g_config.dpsRollingWindowSeconds);
    DamageNumberSystem::ProcessDamageEvents(g_config);
    DamageNumberSystem::UpdateNumbers(g_config, g_hasGameRect, dt);

    const bool statusNoticeActive = IsStatusNoticeActive();
    const auto& damageNumbers = DamageNumberSystem::Numbers();
    OverlayRenderContext renderContext{};
    renderContext.hwnd = hwnd;
    renderContext.config = &g_config;
    renderContext.numbers = &damageNumbers;
    renderContext.positionDebugMarkers = &g_positionDebugMarkers;
    renderContext.dpsText = DpsTracker::CurrentDpsText(g_config.dpsRollingWindowSeconds);
    renderContext.gameRectReady = g_hasGameRect;
    renderContext.statusNoticeActive = statusNoticeActive;
    if (g_config.showDebugStatus || statusNoticeActive) {
        renderContext.debugStatusText = BuildDebugStatusText();
    }
    RenderOverlay(renderContext);
}

static LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        g_overlayWnd = hwnd;
        g_lastFrame = std::chrono::steady_clock::now();
        {
            TrayController::Context trayContext = BuildTrayControllerContext();
            TrayController::AddTrayIcon(hwnd, trayContext);
        }
        RestartFrameTimer(hwnd);
        return 0;

    case FrameTimer::kFrameTickMessage:
        FrameTimer::MarkFrameMessageHandled();
        RunOverlayFrame(hwnd);
        return 0;

    case WM_TIMER:
        if (FrameTimer::IsFallbackTimer(wParam)) {
            RunOverlayFrame(hwnd);
            return 0;
        }
        break;

    case TrayController::kTrayIconMessage:
    {
        TrayController::Context trayContext = BuildTrayControllerContext();
        const TrayController::Callbacks trayCallbacks = BuildTrayControllerCallbacks();
        TrayController::HandleTrayIconMessage(hwnd, lParam, trayContext, trayCallbacks);
        return 0;
    }

    case WM_COMMAND:
    {
        TrayController::Context trayContext = BuildTrayControllerContext();
        const TrayController::Callbacks trayCallbacks = BuildTrayControllerCallbacks();
        if (TrayController::HandleCommand(hwnd, LOWORD(wParam), trayContext, trayCallbacks)) {
            return 0;
        }
        break;
    }

    case WM_DESTROY:
        AppendLogBlock(L"Overlay stopped");
        FrameTimer::Stop(hwnd);
        TrayController::RemoveTrayIcon(hwnd);
        CloseMemorySource();
        ReleaseOverlayRendererResources();
        FrameTimer::EndTimerResolution();
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int) {
    const wchar_t CLASS_NAME[] = L"D2RDamageNumbersOverlay";

    ConfigureMemoryScannerIntegration();
    LoadConfig();

    if (HasCommandLineSwitch(L"--scan-memory")) {
        return RunMemoryScanOnce(FindD2RWindow(), g_config);
    }

    AppendLogBlock(L"Overlay started");

    WNDCLASSW wc{};
    wc.lpfnWndProc = OverlayWndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);

    RegisterClassW(&wc);

    g_overlayWnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW,
        CLASS_NAME,
        L"D2R Damage Numbers Overlay",
        WS_POPUP,
        100,
        100,
        1280,
        720,
        nullptr,
        nullptr,
        hInstance,
        nullptr
    );

    if (!g_overlayWnd) {
        MessageBoxW(nullptr, L"Failed to create overlay window.", L"Error", MB_ICONERROR);
        return 1;
    }

    MARGINS margins{ -1 };
    DwmExtendFrameIntoClientArea(g_overlayWnd, &margins);

    ShowWindow(g_overlayWnd, SW_SHOWNA);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    return 0;
}


