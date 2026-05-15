#include "OverlayConfig.h"

#include <cwchar>
#include <cstdlib>

static int ClampIntLocal(int value, int minValue, int maxValue) {
    if (value < minValue) return minValue;
    if (value > maxValue) return maxValue;
    return value;
}

static float ClampFloatLocal(float value, float minValue, float maxValue) {
    if (value < minValue) return minValue;
    if (value > maxValue) return maxValue;
    return value;
}

std::wstring GetConfigPath() {
    wchar_t modulePath[MAX_PATH]{};
    DWORD length = GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return L"D2RDamageNumbers.ini";
    }

    std::wstring path = modulePath;
    size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
        return L"D2RDamageNumbers.ini";
    }

    return path.substr(0, slash + 1) + L"D2RDamageNumbers.ini";
}

std::wstring GetConfigDirectory() {
    std::wstring configPath = GetConfigPath();
    size_t slash = configPath.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
        return L"";
    }

    return configPath.substr(0, slash + 1);
}

std::wstring GetLogPath() {
    return GetConfigDirectory() + L"D2RDamageNumbers.log";
}

static bool IsAbsolutePath(const std::wstring& path) {
    if (path.length() >= 2 && path[1] == L':') return true;
    if (path.length() >= 2 && (path[0] == L'\\' || path[0] == L'/') && (path[1] == L'\\' || path[1] == L'/')) return true;
    return false;
}

std::wstring ResolveConfigRelativePath(const std::wstring& path) {
    if (path.empty() || IsAbsolutePath(path)) {
        return path;
    }

    return GetConfigDirectory() + path;
}

static int ReadConfigInt(const wchar_t* section, const wchar_t* key, int defaultValue, int minValue, int maxValue, const std::wstring& configPath) {
    int value = GetPrivateProfileIntW(section, key, defaultValue, configPath.c_str());
    return ClampIntLocal(value, minValue, maxValue);
}

static float ReadConfigFloat(const wchar_t* section, const wchar_t* key, float defaultValue, float minValue, float maxValue, const std::wstring& configPath) {
    wchar_t buffer[64]{};
    swprintf_s(buffer, L"%.3f", defaultValue);

    wchar_t value[64]{};
    GetPrivateProfileStringW(section, key, buffer, value, 64, configPath.c_str());

    wchar_t* end = nullptr;
    float parsed = wcstof(value, &end);
    if (end == value) {
        parsed = defaultValue;
    }

    return ClampFloatLocal(parsed, minValue, maxValue);
}

static std::wstring ReadConfigString(const wchar_t* section, const wchar_t* key, const std::wstring& defaultValue, const std::wstring& configPath) {
    wchar_t value[MAX_PATH]{};
    GetPrivateProfileStringW(section, key, defaultValue.c_str(), value, MAX_PATH, configPath.c_str());

    if (value[0] == L'\0') {
        return defaultValue;
    }

    return value;
}

static bool ReadConfigBool(const wchar_t* section, const wchar_t* key, bool defaultValue, const std::wstring& configPath) {
    return ReadConfigInt(section, key, defaultValue ? 1 : 0, 0, 1, configPath) != 0;
}

bool WriteConfigBool(const wchar_t* section, const wchar_t* key, bool value) {
    const std::wstring configPath = GetConfigPath();
    const wchar_t* text = value ? L"1" : L"0";
    const bool wroteValue = WritePrivateProfileStringW(section, key, text, configPath.c_str()) != FALSE;
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, configPath.c_str());
    return wroteValue;
}

bool WriteConfigFloat(const wchar_t* section, const wchar_t* key, float value) {
    const std::wstring configPath = GetConfigPath();
    wchar_t text[64]{};
    swprintf_s(text, L"%.1f", static_cast<double>(value));
    const bool wroteValue = WritePrivateProfileStringW(section, key, text, configPath.c_str()) != FALSE;
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, configPath.c_str());
    return wroteValue;
}

static bool TryParseHexColor(const wchar_t* value, COLORREF& color) {
    if (!value || !*value) return false;

    const wchar_t* p = value;
    if (*p == L'#') {
        ++p;
    }

    if (wcslen(p) != 6) return false;

    wchar_t* end = nullptr;
    unsigned long rgb = wcstoul(p, &end, 16);
    if (!end || *end != L'\0') return false;

    color = RGB((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff);
    return true;
}

static bool TryParseCsvColor(const wchar_t* value, COLORREF& color) {
    int r = 0;
    int g = 0;
    int b = 0;
    if (swscanf_s(value, L"%d,%d,%d", &r, &g, &b) != 3) return false;

    color = RGB(ClampIntLocal(r, 0, 255), ClampIntLocal(g, 0, 255), ClampIntLocal(b, 0, 255));
    return true;
}

static COLORREF ReadConfigColor(const wchar_t* section, const wchar_t* key, COLORREF defaultValue, const std::wstring& configPath) {
    wchar_t defaultText[16]{};
    swprintf_s(
        defaultText,
        L"#%02X%02X%02X",
        GetRValue(defaultValue),
        GetGValue(defaultValue),
        GetBValue(defaultValue)
    );

    wchar_t value[64]{};
    GetPrivateProfileStringW(section, key, defaultText, value, 64, configPath.c_str());

    COLORREF parsed = defaultValue;
    if (TryParseHexColor(value, parsed) || TryParseCsvColor(value, parsed)) {
        return parsed;
    }

    return defaultValue;
}

static int ParseHotkeyName(const wchar_t* value, int defaultValue) {
    if (!value || !*value) return defaultValue;

    if ((value[0] == L'F' || value[0] == L'f') && value[1] != L'\0') {
        wchar_t* end = nullptr;
        long functionKey = wcstol(value + 1, &end, 10);
        if (end && *end == L'\0' && functionKey >= 1 && functionKey <= 24) {
            return VK_F1 + static_cast<int>(functionKey) - 1;
        }
    }

    if ((value[0] == L'0') && (value[1] == L'x' || value[1] == L'X')) {
        wchar_t* end = nullptr;
        long virtualKey = wcstol(value + 2, &end, 16);
        if (end && *end == L'\0' && virtualKey > 0 && virtualKey <= 0xff) {
            return static_cast<int>(virtualKey);
        }
    }

    if (wcslen(value) == 1) {
        wchar_t ch = value[0];
        if (ch >= L'a' && ch <= L'z') {
            ch = ch - L'a' + L'A';
        }
        if ((ch >= L'A' && ch <= L'Z') || (ch >= L'0' && ch <= L'9')) {
            return static_cast<int>(ch);
        }
    }

    return defaultValue;
}

static int ReadConfigHotkey(const wchar_t* section, const wchar_t* key, int defaultValue, const std::wstring& configPath) {
    wchar_t value[32]{};
    GetPrivateProfileStringW(section, key, L"", value, 32, configPath.c_str());
    return ParseHotkeyName(value, defaultValue);
}

void LoadOverlayConfig(OverlayConfig& config) {
    const std::wstring configPath = GetConfigPath();

    config.fontFile = ReadConfigString(L"Overlay", L"FontFile", config.fontFile, configPath);
    config.fontFace = ReadConfigString(L"Overlay", L"FontFace", config.fontFace, configPath);
    config.fontSize = ReadConfigInt(L"Overlay", L"FontSize", config.fontSize, 12, 96, configPath);
    config.critFontSize = ReadConfigInt(L"Overlay", L"CritFontSize", config.critFontSize, 12, 128, configPath);
    config.fontWeight = ReadConfigInt(L"Overlay", L"FontWeight", config.fontWeight, 100, 1000, configPath);
    config.critFontWeight = ReadConfigInt(L"Overlay", L"CritFontWeight", config.critFontWeight, 100, 1000, configPath);
    config.outlineThickness = ReadConfigInt(L"Overlay", L"OutlineThickness", config.outlineThickness, 0, 8, configPath);
    config.shadowOffsetX = ReadConfigInt(L"Overlay", L"ShadowOffsetX", config.shadowOffsetX, -12, 12, configPath);
    config.shadowOffsetY = ReadConfigInt(L"Overlay", L"ShadowOffsetY", config.shadowOffsetY, -12, 12, configPath);
    config.maxActiveNumbers = ReadConfigInt(L"Overlay", L"MaxActiveNumbers", config.maxActiveNumbers, 1, 500, configPath);
    config.showDpsNumber = ReadConfigBool(L"Overlay", L"ShowDpsNumber", config.showDpsNumber, configPath);
    config.dpsNumberXPercent = ReadConfigFloat(L"Overlay", L"DpsNumberXPercent", config.dpsNumberXPercent, 0.0f, 100.0f, configPath);
    config.dpsNumberYPercent = ReadConfigFloat(L"Overlay", L"DpsNumberYPercent", config.dpsNumberYPercent, 0.0f, 100.0f, configPath);
    config.dpsRollingWindowSeconds = ReadConfigFloat(L"Overlay", L"DpsRollingWindowSeconds", config.dpsRollingWindowSeconds, 0.25f, 60.0f, configPath);
    config.renderFps = ReadConfigInt(L"Overlay", L"RenderFps", config.renderFps, 30, 240, configPath);
    config.coalesceSmallDamage = ReadConfigBool(L"Overlay", L"CoalesceSmallDamage", config.coalesceSmallDamage, configPath);
    config.coalesceMaxDamage = ReadConfigInt(L"Overlay", L"CoalesceMaxDamage", config.coalesceMaxDamage, 1, 1000000, configPath);
    config.coalesceWindowMs = ReadConfigInt(L"Overlay", L"CoalesceWindowMs", config.coalesceWindowMs, 50, 5000, configPath);
    config.coalesceRefreshLifetime = ReadConfigFloat(L"Overlay", L"CoalesceRefreshLifetime", config.coalesceRefreshLifetime, 0.05f, 5.0f, configPath);
    config.coalescePulseScale = ReadConfigFloat(L"Overlay", L"CoalescePulseScale", config.coalescePulseScale, 1.0f, 2.0f, configPath);
    config.coalescePulseSeconds = ReadConfigFloat(L"Overlay", L"CoalescePulseSeconds", config.coalescePulseSeconds, 0.01f, 0.50f, configPath);
    config.coalesceTickPop = ReadConfigBool(L"Overlay", L"CoalesceTickPop", config.coalesceTickPop, configPath);
    config.coalesceTickPopSeconds = ReadConfigFloat(L"Overlay", L"CoalesceTickPopSeconds", config.coalesceTickPopSeconds, 0.05f, 0.75f, configPath);
    config.coalesceTickPopScale = ReadConfigFloat(L"Overlay", L"CoalesceTickPopScale", config.coalesceTickPopScale, 0.25f, 1.25f, configPath);
    config.coalesceTickPopDistance = ReadConfigFloat(L"Overlay", L"CoalesceTickPopDistance", config.coalesceTickPopDistance, 0.0f, 120.0f, configPath);
    config.coalesceTickPopMergeOffsetY = ReadConfigFloat(L"Overlay", L"CoalesceTickPopMergeOffsetY", config.coalesceTickPopMergeOffsetY, -120.0f, 120.0f, configPath);
    config.useStackLanes = ReadConfigBool(L"Overlay", L"UseStackLanes", config.useStackLanes, configPath);
    config.stackLanes = ReadConfigInt(L"Overlay", L"StackLanes", config.stackLanes, 1, 9, configPath);
    config.stackLaneWidth = ReadConfigFloat(L"Overlay", L"StackLaneWidth", config.stackLaneWidth, 0.0f, 120.0f, configPath);
    config.stackVerticalStep = ReadConfigFloat(L"Overlay", L"StackVerticalStep", config.stackVerticalStep, 0.0f, 80.0f, configPath);
    config.stackReuseSeconds = ReadConfigFloat(L"Overlay", L"StackReuseSeconds", config.stackReuseSeconds, 0.05f, 3.0f, configPath);
    config.stackMaxYOffset = ReadConfigFloat(L"Overlay", L"StackMaxYOffset", config.stackMaxYOffset, 0.0f, 240.0f, configPath);
    config.lifetimeSeconds = ReadConfigFloat(L"Overlay", L"LifetimeSeconds", config.lifetimeSeconds, 0.10f, 5.0f, configPath);
    config.critLifetimeSeconds = ReadConfigFloat(L"Overlay", L"CritLifetimeSeconds", config.critLifetimeSeconds, 0.10f, 5.0f, configPath);
    config.fadeStart = ReadConfigFloat(L"Overlay", L"FadeStart", config.fadeStart, 0.0f, 0.95f, configPath);
    config.popStartScale = ReadConfigFloat(L"Overlay", L"PopStartScale", config.popStartScale, 0.10f, 1.00f, configPath);
    config.popOvershootScale = ReadConfigFloat(L"Overlay", L"PopOvershootScale", config.popOvershootScale, 1.00f, 2.00f, configPath);
    config.popInSeconds = ReadConfigFloat(L"Overlay", L"PopInSeconds", config.popInSeconds, 0.01f, 0.50f, configPath);
    config.popSettleSeconds = ReadConfigFloat(L"Overlay", L"PopSettleSeconds", config.popSettleSeconds, 0.01f, 0.50f, configPath);
    config.floatSpeed = ReadConfigFloat(L"Overlay", L"FloatSpeed", config.floatSpeed, 0.0f, 400.0f, configPath);
    config.horizontalDrift = ReadConfigFloat(L"Overlay", L"HorizontalDrift", config.horizontalDrift, 0.0f, 200.0f, configPath);
    config.spawnYOffset = ReadConfigFloat(L"Overlay", L"SpawnYOffset", config.spawnYOffset, -300.0f, 300.0f, configPath);
    config.followResponse = ReadConfigFloat(L"Overlay", L"FollowResponse", config.followResponse, 1.0f, 240.0f, configPath);
    config.followSnapDistance = ReadConfigFloat(L"Overlay", L"FollowSnapDistance", config.followSnapDistance, 1.0f, 1000.0f, configPath);
    config.followMaxSpeed = ReadConfigFloat(L"Overlay", L"FollowMaxSpeed", config.followMaxSpeed, 60.0f, 12000.0f, configPath);

    config.testMode = ReadConfigBool(L"Test", L"Enabled", config.testMode, configPath);
    config.showDebugStatus = ReadConfigBool(L"Debug", L"ShowStatus", config.showDebugStatus, configPath);
    config.showPositionCandidates = ReadConfigBool(L"Debug", L"ShowPositionCandidates", config.showPositionCandidates, configPath);
    config.positionCandidateHotkey = ReadConfigHotkey(L"Debug", L"PositionCandidateHotkey", config.positionCandidateHotkey, configPath);
    config.normalHotkey = ReadConfigHotkey(L"Test", L"NormalHotkey", config.normalHotkey, configPath);
    config.critHotkey = ReadConfigHotkey(L"Test", L"CritHotkey", config.critHotkey, configPath);
    config.burstHotkey = ReadConfigHotkey(L"Test", L"BurstHotkey", config.burstHotkey, configPath);
    config.burstCount = ReadConfigInt(L"Test", L"BurstCount", config.burstCount, 1, 40, configPath);
    config.burstRadius = ReadConfigFloat(L"Test", L"BurstRadius", config.burstRadius, 0.0f, 300.0f, configPath);

    config.memoryPollSeconds = ReadConfigFloat(L"Memory", L"PollSeconds", config.memoryPollSeconds, 0.005f, 0.50f, configPath);
    config.trackAllMonsters = ReadConfigBool(L"Memory", L"TrackAllMonsters", config.trackAllMonsters, configPath);
    config.useWorldPositions = ReadConfigBool(L"Memory", L"UseWorldPositions", config.useWorldPositions, configPath);
    config.followDamageTargets = ReadConfigBool(L"Memory", L"FollowDamageTargets", config.followDamageTargets, configPath);
    config.useScreenAnchorCandidates = ReadConfigBool(L"Memory", L"UseScreenAnchorCandidates", config.useScreenAnchorCandidates, configPath);
    config.useWorldCoordCandidates = ReadConfigBool(L"Memory", L"UseWorldCoordCandidates", config.useWorldCoordCandidates, configPath);
    config.learnWorldProjection = ReadConfigBool(L"Memory", L"LearnWorldProjection", config.learnWorldProjection, configPath);
    config.worldAnchorX = ReadConfigFloat(L"Memory", L"WorldAnchorX", config.worldAnchorX, 0.0f, 1.0f, configPath);
    config.worldAnchorY = ReadConfigFloat(L"Memory", L"WorldAnchorY", config.worldAnchorY, 0.0f, 1.0f, configPath);
    config.worldTileWidth = ReadConfigFloat(L"Memory", L"WorldTileWidth", config.worldTileWidth, 1.0f, 80.0f, configPath);
    config.worldTileHeight = ReadConfigFloat(L"Memory", L"WorldTileHeight", config.worldTileHeight, 1.0f, 80.0f, configPath);
    config.worldProjectionMinSamples = ReadConfigInt(L"Memory", L"WorldProjectionMinSamples", config.worldProjectionMinSamples, 3, 40, configPath);
    config.worldCoordMinDistinctUnits = ReadConfigInt(L"Memory", L"WorldCoordMinDistinctUnits", config.worldCoordMinDistinctUnits, 1, 10, configPath);
    config.worldCoordMaxRmsError = ReadConfigFloat(L"Memory", L"WorldCoordMaxRmsError", config.worldCoordMaxRmsError, 5.0f, 500.0f, configPath);
    config.worldCoordMinMovementSpan = ReadConfigFloat(L"Memory", L"WorldCoordMinMovementSpan", config.worldCoordMinMovementSpan, 0.0f, 1000.0f, configPath);
    config.worldCoordScreenOffsetX = ReadConfigFloat(L"Memory", L"WorldCoordScreenOffsetX", config.worldCoordScreenOffsetX, -400.0f, 400.0f, configPath);
    config.worldCoordScreenOffsetY = ReadConfigFloat(L"Memory", L"WorldCoordScreenOffsetY", config.worldCoordScreenOffsetY, -400.0f, 400.0f, configPath);
    config.screenAnchorMinSamples = ReadConfigInt(L"Memory", L"ScreenAnchorMinSamples", config.screenAnchorMinSamples, 4, 80, configPath);
    config.screenAnchorMinDistinctUnits = ReadConfigInt(L"Memory", L"ScreenAnchorMinDistinctUnits", config.screenAnchorMinDistinctUnits, 1, 10, configPath);
    config.screenAnchorMaxCursorDistance = ReadConfigFloat(L"Memory", L"ScreenAnchorMaxCursorDistance", config.screenAnchorMaxCursorDistance, 20.0f, 800.0f, configPath);
    config.screenAnchorMaxAverageError = ReadConfigFloat(L"Memory", L"ScreenAnchorMaxAverageError", config.screenAnchorMaxAverageError, 5.0f, 500.0f, configPath);
    config.screenAnchorMinMovementSpan = ReadConfigFloat(L"Memory", L"ScreenAnchorMinMovementSpan", config.screenAnchorMinMovementSpan, 0.0f, 1000.0f, configPath);
    config.positionProbeHotkey = ReadConfigHotkey(L"Memory", L"PositionProbeHotkey", config.positionProbeHotkey, configPath);
    config.hitRecorderEnabled = ReadConfigBool(L"Memory", L"HitRecorder", config.hitRecorderEnabled, configPath);
    config.hitMarkerHotkey = ReadConfigHotkey(L"Memory", L"MarkerHotkey", config.hitMarkerHotkey, configPath);
    config.logEnabled = ReadConfigBool(L"Log", L"Enabled", config.logEnabled, configPath);

    config.normalColor = ReadConfigColor(
        L"Colors",
        L"Normal",
        ReadConfigColor(L"Colors", L"Physical", config.normalColor, configPath),
        configPath
    );
    config.criticalColor = ReadConfigColor(L"Colors", L"Critical", config.criticalColor, configPath);
    config.outlineColor = ReadConfigColor(L"Colors", L"Outline", config.outlineColor, configPath);
    config.shadowColor = ReadConfigColor(L"Colors", L"Shadow", config.shadowColor, configPath);
}
