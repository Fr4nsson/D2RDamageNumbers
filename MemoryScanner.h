#pragma once

#include <windows.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "DamageModel.h"
#include "OverlayConfig.h"
#include "OverlayDebugModel.h"

struct HitRecorderStatValue {
    uint16_t layer = 0;
    uint16_t stat = 0;
    int32_t value = 0;
};

struct HitRecorderSample {
    uint64_t id = 0;
    std::chrono::steady_clock::time_point capturedAt;
    uintptr_t currentGame = 0;
    uint32_t targetUnitType = UINT32_MAX;
    uint32_t targetUnitId = UINT32_MAX;
    uintptr_t targetServerUnit = 0;
    uintptr_t targetClientUnit = 0;
    uintptr_t playerServerUnit = 0;
    uintptr_t playerClientUnit = 0;
    int hp = -1;
    int maxHp = -1;
    std::vector<uint8_t> targetServerSnapshot;
    std::vector<uint8_t> targetClientSnapshot;
    std::vector<uint8_t> playerServerSnapshot;
    std::vector<uint8_t> playerClientSnapshot;
    std::vector<uint8_t> targetServerExtendedSnapshot;
    std::vector<uint8_t> targetClientExtendedSnapshot;
    std::vector<uint8_t> playerServerExtendedSnapshot;
    std::vector<uint8_t> playerClientExtendedSnapshot;
    uintptr_t targetStatsListEx = 0;
    uintptr_t playerStatsListEx = 0;
    std::vector<uint8_t> targetStatsListSnapshot;
    std::vector<uint8_t> playerStatsListSnapshot;
    std::vector<uint8_t> targetBaseStatEntrySnapshot;
    std::vector<uint8_t> targetExtraStatEntrySnapshot;
    std::vector<uint8_t> playerBaseStatEntrySnapshot;
    std::vector<uint8_t> playerExtraStatEntrySnapshot;
    std::vector<HitRecorderStatValue> targetBaseStats;
    std::vector<HitRecorderStatValue> targetExtraStats;
    std::vector<HitRecorderStatValue> playerBaseStats;
    std::vector<HitRecorderStatValue> playerExtraStats;
};

struct HitRecorderDamage {
    uint64_t sampleId = 0;
    std::chrono::steady_clock::time_point capturedAt;
    int amount = 0;
    int oldHp = -1;
    int newHp = -1;
    int maxHp = -1;
    uint32_t targetUnitType = UINT32_MAX;
    uint32_t targetUnitId = UINT32_MAX;
};

#pragma pack(push, 1)
struct D2MouseHoverMemory {
    uint8_t isHovered;
    uint8_t playerMoving;
    uint8_t unknown2;
    uint8_t unknown3;
    uint32_t hoveredUnitType;
    uint32_t hoveredUnitId;
};
#pragma pack(pop)

struct D2StatArrayMemory {
    uintptr_t firstStat = 0;
    uint64_t size = 0;
};

struct D2StatValueMemory {
    uint16_t layer = 0;
    uint16_t stat = 0;
    int32_t value = 0;
};

struct TrackedMonsterHp {
    uint32_t unitId = UINT32_MAX;
    uintptr_t serverUnit = 0;
    uintptr_t clientUnit = 0;
    int hp = -1;
    int maxHp = -1;
    uint64_t seenGeneration = 0;
};

struct ServerUnitEntry {
    uint32_t unitType = UINT32_MAX;
    uint32_t unitId = UINT32_MAX;
    uintptr_t serverUnit = 0;
};

struct PositionProbeCandidate {
    std::wstring type;
    size_t offset = 0;
    int x = 0;
    int y = 0;
    int distance = 0;
};

struct PositionDebugCandidate {
    PositionDebugMarker marker;
    int distance = 0;
};

struct WorldProjectionSample {
    float dx = 0.0f;
    float dy = 0.0f;
    float screenX = 0.0f;
    float screenY = 0.0f;
    std::chrono::steady_clock::time_point capturedAt;
};

struct WorldProjectionModel {
    bool ready = false;
    float ax = 0.0f;
    float bx = 0.0f;
    float cx = 0.0f;
    float ay = 0.0f;
    float by = 0.0f;
    float cy = 0.0f;
    float rmsError = 0.0f;
    size_t sampleCount = 0;
};

enum class ScreenAnchorSourceKind {
    TargetClient,
    TargetServer,
    TargetClientChild,
    TargetServerChild
};

enum class ScreenAnchorValueKind {
    Int32Pair,
    UInt32Pair,
    UInt16Pair,
    FloatPair
};

enum class WorldCoordSourceKind {
    ClientUnit,
    ServerUnit,
    ClientChild,
    ServerChild
};

enum class WorldCoordValueKind {
    Int32Pair,
    UInt32Pair,
    UInt16Pair,
    FloatPair
};

struct ScreenAnchorDescriptor {
    ScreenAnchorSourceKind sourceKind = ScreenAnchorSourceKind::TargetClient;
    ScreenAnchorValueKind valueKind = ScreenAnchorValueKind::Int32Pair;
    size_t pointerOffset = 0;
    size_t valueOffset = 0;
};

struct WorldCoordDescriptor {
    WorldCoordSourceKind sourceKind = WorldCoordSourceKind::ClientUnit;
    WorldCoordValueKind valueKind = WorldCoordValueKind::Int32Pair;
    size_t pointerOffset = 0;
    size_t valueOffset = 0;
};

struct ScreenAnchorSample {
    ScreenAnchorDescriptor descriptor;
    float x = 0.0f;
    float y = 0.0f;
    float error = 0.0f;
};

struct WorldCoordSample {
    WorldCoordDescriptor descriptor;
    float dx = 0.0f;
    float dy = 0.0f;
    float screenX = 0.0f;
    float screenY = 0.0f;
};

struct ScreenAnchorCandidate {
    ScreenAnchorDescriptor descriptor;
    size_t sampleCount = 0;
    double totalError = 0.0;
    float averageError = 0.0f;
    float lastX = 0.0f;
    float lastY = 0.0f;
    float lastError = 0.0f;
    float minX = 0.0f;
    float maxX = 0.0f;
    float minY = 0.0f;
    float maxY = 0.0f;
    std::vector<uint64_t> unitKeys;
    std::vector<uint64_t> failedUnitKeys;
    std::vector<uint64_t> farUnitKeys;
    std::chrono::steady_clock::time_point lastSeen;
};

struct ScreenAnchorModel {
    bool ready = false;
    ScreenAnchorDescriptor descriptor;
    size_t sampleCount = 0;
    float averageError = 0.0f;
    float lastX = 0.0f;
    float lastY = 0.0f;
    float lastError = 0.0f;
    size_t distinctUnitCount = 0;
    float movementSpan = 0.0f;
    size_t failedUnitCount = 0;
    size_t farUnitCount = 0;
};

struct WorldCoordCandidate {
    WorldCoordDescriptor descriptor;
    std::vector<WorldProjectionSample> samples;
    std::vector<uint64_t> unitKeys;
    WorldProjectionModel model;
    float minDx = 0.0f;
    float maxDx = 0.0f;
    float minDy = 0.0f;
    float maxDy = 0.0f;
    std::chrono::steady_clock::time_point lastSeen;
};

struct WorldCoordModel {
    bool ready = false;
    WorldCoordDescriptor descriptor;
    WorldProjectionModel projection;
    size_t distinctUnitCount = 0;
    float movementSpan = 0.0f;
};

struct MemorySource {
    DWORD processId = 0;
    HANDLE process = nullptr;
    uintptr_t moduleBase = 0;
    DWORD moduleSize = 0;
    std::wstring moduleName;
    std::wstring processPath;
    uintptr_t mouseHoverAddress = 0;
    uintptr_t currentSinglePlayerGameAddress = 0;
    uintptr_t unitHashTableAddress = 0;
    uintptr_t currentSinglePlayerGame = 0;
    uintptr_t hoveredUnitAddress = 0;
    uintptr_t hoveredClientUnitAddress = 0;
    D2MouseHoverMemory hover{};
    int hoveredHp = -1;
    int hoveredMaxHp = -1;
    uint32_t trackedUnitType = UINT32_MAX;
    uint32_t trackedUnitId = UINT32_MAX;
    uintptr_t trackedGame = 0;
    uintptr_t trackedServerUnitAddress = 0;
    uintptr_t trackedClientUnitAddress = 0;
    int trackedHp = -1;
    std::vector<uint8_t> trackedServerUnitSnapshot;
    std::vector<uint8_t> trackedClientUnitSnapshot;
    uintptr_t trackedPlayerGame = 0;
    uint32_t trackedPlayerUnitId = UINT32_MAX;
    uintptr_t trackedPlayerUnitAddress = 0;
    uintptr_t trackedPlayerClientUnitAddress = 0;
    std::vector<uint8_t> trackedPlayerUnitSnapshot;
    std::vector<uint8_t> trackedPlayerClientUnitSnapshot;
    uintptr_t trackedMonsterGame = 0;
    float gamePatternRetrySeconds = 0.0f;
    uint64_t monsterScanGeneration = 0;
    size_t monsterScanCount = 0;
    size_t monsterTrackedCount = 0;
    std::vector<TrackedMonsterHp> trackedMonsters;
    int lastDamageAmount = 0;
    DamageKind lastDamageKind = DamageKind::Normal;
    std::wstring lastClientUnitDiffs;
    size_t lastModuleBytesRead = 0;
    size_t mouseHoverPatternMatches = 0;
    size_t currentGamePatternMatches = 0;
    size_t unitHashPatternMatches = 0;
    std::wstring mouseHoverPatternName;
    std::wstring currentGamePatternName;
    std::wstring unitHashPatternName;
    std::wstring hpStatus = L"not ready";
    std::wstring patternDiagnostics;
    std::wstring currentGameCandidateDiagnostics;
    bool connected = false;
    bool patternsReady = false;
    bool hoverReady = false;
    bool hpReady = false;
    std::wstring status = L"not connected";
    float reconnectSeconds = 0.0f;
    float pollSeconds = 0.0f;
    float patternRetrySeconds = 0.0f;
};

struct MemoryScannerCallbacks {
    void (*appendLogBlock)(const std::wstring& text) = nullptr;
    void (*queueDamageEvent)(const DamageEvent& event) = nullptr;
    bool (*tryGetCursorInGameRect)(float& x, float& y) = nullptr;
    void (*showStatusNotice)(const wchar_t* message) = nullptr;
    std::wstring (*buildMemoryLogText)() = nullptr;
};

struct MemoryScannerFrameContext {
    HWND gameWnd = nullptr;
    RECT gameRect{};
    bool hasGameRect = false;
    OverlayConfig* config = nullptr;
};

extern MemorySource g_memorySource;
extern std::vector<PositionDebugMarker> g_positionDebugMarkers;
extern bool g_worldProjectionCalibrated;
extern std::vector<WorldProjectionSample> g_worldProjectionSamples;
extern WorldProjectionModel g_worldProjectionModel;
extern std::vector<ScreenAnchorCandidate> g_screenAnchorCandidates;
extern ScreenAnchorModel g_screenAnchorModel;
extern size_t g_screenAnchorHoverSamples;
extern std::vector<WorldCoordCandidate> g_worldCoordCandidates;
extern WorldCoordModel g_worldCoordModel;
extern size_t g_worldCoordHoverSamples;
extern std::vector<HitRecorderSample> g_hitRecorderSamples;
extern std::vector<HitRecorderDamage> g_hitRecorderDamages;
extern HitRecorderDamage g_lastHitRecorderDamage;

void ConfigureMemoryScannerCallbacks(const MemoryScannerCallbacks& callbacks);
void SetMemoryScannerFrameContext(const MemoryScannerFrameContext& context);
void PollMemorySource(const MemoryScannerFrameContext& context, float dt);
void CloseMemorySource();
int RunMemoryScanOnce(HWND gameWnd, OverlayConfig& config);
void WritePositionProbeMarker();
void WriteHitRecorderMarker();

std::wstring FormatHex(uintptr_t value);
bool IsLikelyRemotePointer(uintptr_t address);
const wchar_t* DamageKindName(DamageKind damageKind);
float GetScreenDistance(float ax, float ay, float bx, float by);
bool TryReadPlayerWorldPosition(uintptr_t currentGame, WorldPosition& position);
bool ProjectWorldPositionRelativeToPlayer(
    const WorldPosition& targetPosition,
    const WorldPosition& playerPosition,
    bool useCalibration,
    float& screenX,
    float& screenY
);
bool IsLargeProjectionOutlier(const DamageNumber& number, float projectedX, float projectedY);
bool TryProjectUnitToScreenFromPlayer(
    uintptr_t currentGame,
    const WorldPosition& playerPosition,
    uint32_t targetUnitType,
    uint32_t targetUnitId,
    uintptr_t targetServerUnit,
    uintptr_t targetClientUnit,
    float& screenX,
    float& screenY,
    WorldPosition* targetWorldPosition = nullptr,
    bool* hasTargetWorldPosition = nullptr
);
bool TryProjectUnitToScreen(
    uint32_t targetUnitType,
    uint32_t targetUnitId,
    uintptr_t targetServerUnit,
    uintptr_t targetClientUnit,
    float& screenX,
    float& screenY,
    WorldPosition* targetWorldPosition = nullptr,
    bool* hasTargetWorldPosition = nullptr
);
std::wstring FormatWorldCoordDescriptor(const WorldCoordDescriptor& descriptor);
std::wstring FormatTopWorldCoordCandidates(size_t maxCandidates);
std::wstring FormatScreenAnchorDescriptor(const ScreenAnchorDescriptor& descriptor);
std::wstring FormatTopScreenAnchorCandidates(size_t maxCandidates);
