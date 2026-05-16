#pragma once

#include <windows.h>

#include <string>

struct OverlayConfig {
    std::wstring fontFile = L"diablo4.ttf";
    std::wstring fontFace = L"PT Serif";

    int fontSize = 38;
    int critFontSize = 48;
    int fontWeight = 500;
    int critFontWeight = 1000;
    int outlineThickness = 1;
    int shadowOffsetX = 0;
    int shadowOffsetY = 0;
    int maxActiveNumbers = 160;
    int burstCount = 8;
    int renderFps = 120;
    int coalesceMaxDamage = 999999;
    int coalesceWindowMs = 500;
    int stackLanes = 7;

    float lifetimeSeconds = 0.85f;
    float critLifetimeSeconds = 0.95f;
    float fadeStart = 0.75f;
    float popStartScale = 0.01f;
    float popOvershootScale = 1.75f;
    float popInSeconds = 0.08f;
    float popSettleSeconds = 0.12f;
    float floatSpeed = 45.0f;
    float horizontalDrift = 0.0f;
    float spawnYOffset = 0.0f;
    float burstRadius = 32.0f;
    float followResponse = 72.0f;
    float followSnapDistance = 180.0f;
    float followMaxSpeed = 2400.0f;
    float coalesceRefreshLifetime = 0.52f;
    float coalescePulseScale = 1.24f;
    float coalescePulseSeconds = 0.13f;
    float coalesceTickPopSeconds = 0.70f;
    float coalesceTickPopScale = 0.60f;
    float coalesceTickPopDistance = 64.0f;
    float coalesceTickPopMergeOffsetY = -28.0f;
    float stackLaneWidth = 40.0f;
    float stackVerticalStep = 24.0f;
    float stackReuseSeconds = 0.60f;
    float stackMaxYOffset = 96.0f;
    float screenAnchorMaxCursorDistance = 320.0f;
    float screenAnchorMaxAverageError = 110.0f;
    float screenAnchorMinMovementSpan = 80.0f;
    float worldCoordMaxRmsError = 90.0f;
    float worldCoordMinMovementSpan = 120.0f;
    float worldCoordScreenOffsetX = 0.0f;
    float worldCoordScreenOffsetY = 0.0f;
    float dpsNumberXPercent = 2.0f;
    float dpsNumberYPercent = 98.0f;
    float dpsRollingWindowSeconds = 5.0f;

    bool testMode = false;
    bool showDebugStatus = false;
    bool showDpsNumber = true;
    bool logEnabled = false;
    bool hitRecorderEnabled = false;
    bool trackAllMonsters = true;
    bool showPositionCandidates = false;
    bool useWorldPositions = true;
    bool followDamageTargets = true;
    bool learnWorldProjection = true;
    bool useScreenAnchorCandidates = false;
    bool useWorldCoordCandidates = false;
    bool coalesceSmallDamage = true;
    bool coalesceTickPop = true;
    bool useStackLanes = true;

    float memoryPollSeconds = 0.02f;
    float worldAnchorX = 0.50f;
    float worldAnchorY = 0.52f;
    float worldTileWidth = 16.0f;
    float worldTileHeight = 8.0f;
    int worldProjectionMinSamples = 8;
    int screenAnchorMinSamples = 16;
    int screenAnchorMinDistinctUnits = 2;
    int worldCoordMinDistinctUnits = 2;

    int positionCandidateHotkey = VK_F5;
    int positionProbeHotkey = VK_F6;
    int hitMarkerHotkey = VK_F7;
    int normalHotkey = VK_F8;
    int critHotkey = VK_F9;
    int burstHotkey = VK_F11;

    COLORREF normalColor = RGB(235, 235, 225);
    COLORREF criticalColor = RGB(255, 214, 70);
    COLORREF outlineColor = RGB(42, 28, 8);
    COLORREF shadowColor = RGB(40, 28, 6);
};

std::wstring GetConfigPath();
std::wstring GetConfigDirectory();
std::wstring GetLogPath();
std::wstring ResolveConfigRelativePath(const std::wstring& path);

bool EnsureDefaultConfigFileExists();
void LoadOverlayConfig(OverlayConfig& config);
bool WriteConfigBool(const wchar_t* section, const wchar_t* key, bool value);
bool WriteConfigFloat(const wchar_t* section, const wchar_t* key, float value);
