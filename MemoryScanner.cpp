// Memory scanner implementation slice.
#include "MemoryScanner.h"

#include <psapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cwchar>

#include "D2RMemoryLayout.h"

MemorySource g_memorySource;
std::vector<PositionDebugMarker> g_positionDebugMarkers;
bool g_worldProjectionCalibrated = false;
std::vector<WorldProjectionSample> g_worldProjectionSamples;
WorldProjectionModel g_worldProjectionModel;
std::vector<ScreenAnchorCandidate> g_screenAnchorCandidates;
ScreenAnchorModel g_screenAnchorModel;
size_t g_screenAnchorHoverSamples = 0;
std::vector<WorldCoordCandidate> g_worldCoordCandidates;
WorldCoordModel g_worldCoordModel;
size_t g_worldCoordHoverSamples = 0;
std::vector<HitRecorderSample> g_hitRecorderSamples;
std::vector<HitRecorderDamage> g_hitRecorderDamages;
HitRecorderDamage g_lastHitRecorderDamage;

namespace {

OverlayConfig g_fallbackConfig;
MemoryScannerCallbacks g_callbacks;
MemoryScannerFrameContext g_frameContext;
float g_worldProjectionBiasX = 0.0f;
float g_worldProjectionBiasY = 0.0f;
std::wstring g_lastLoggedScreenAnchorLabel;
size_t g_worldCoordScreenOffsetCalibrationSamples = 0;
std::wstring g_lastLoggedWorldCoordLabel;
uint64_t g_nextHitRecorderSampleId = 1;

OverlayConfig& ActiveConfig() {
    return g_frameContext.config ? *g_frameContext.config : g_fallbackConfig;
}

float ClampFloat(float value, float minValue, float maxValue) {
    if (value < minValue) return minValue;
    if (value > maxValue) return maxValue;
    return value;
}

void ScannerAppendLogBlock(const std::wstring& text) {
    if (g_callbacks.appendLogBlock) {
        g_callbacks.appendLogBlock(text);
    }
}

void ScannerQueueDamageEvent(const DamageEvent& event) {
    if (g_callbacks.queueDamageEvent) {
        g_callbacks.queueDamageEvent(event);
    }
}

bool ScannerTryGetCursorInGameRect(float& x, float& y) {
    return g_callbacks.tryGetCursorInGameRect && g_callbacks.tryGetCursorInGameRect(x, y);
}

void ScannerShowStatusNotice(const wchar_t* message) {
    if (g_callbacks.showStatusNotice) {
        g_callbacks.showStatusNotice(message);
    }
}

std::wstring ScannerBuildMemoryLogText() {
    return g_callbacks.buildMemoryLogText ? g_callbacks.buildMemoryLogText() : std::wstring();
}

}

#define g_config (ActiveConfig())
#define g_gameWnd (g_frameContext.gameWnd)
#define g_gameRect (g_frameContext.gameRect)
#define g_hasGameRect (g_frameContext.hasGameRect)
#define AppendLogBlock ScannerAppendLogBlock
#define QueueDamageEvent ScannerQueueDamageEvent
#define TryGetCursorInGameRect ScannerTryGetCursorInGameRect
#define ShowStatusNotice ScannerShowStatusNotice
#define BuildMemoryLogText ScannerBuildMemoryLogText
#define CloseMemorySource CloseMemorySourceCore
#define FormatHex FormatHexCore
#define IsLikelyRemotePointer IsLikelyRemotePointerCore
#define DamageKindName DamageKindNameCore
#define GetScreenDistance GetScreenDistanceCore
#define TryReadPlayerWorldPosition TryReadPlayerWorldPositionCore
#define ProjectWorldPositionRelativeToPlayer ProjectWorldPositionRelativeToPlayerCore
#define IsLargeProjectionOutlier IsLargeProjectionOutlierCore
#define TryProjectUnitToScreenFromPlayer TryProjectUnitToScreenFromPlayerCore
#define TryProjectUnitToScreen TryProjectUnitToScreenCore
#define FormatWorldCoordDescriptor FormatWorldCoordDescriptorCore
#define FormatTopWorldCoordCandidates FormatTopWorldCoordCandidatesCore
#define FormatScreenAnchorDescriptor FormatScreenAnchorDescriptorCore
#define FormatTopScreenAnchorCandidates FormatTopScreenAnchorCandidatesCore
#define PollMemorySource PollMemorySourceCore
#define WritePositionProbeMarker WritePositionProbeMarkerCore
#define WriteHitRecorderMarker WriteHitRecorderMarkerCore

static void CloseMemorySource() {
    if (g_memorySource.process) {
        CloseHandle(g_memorySource.process);
    }

    g_memorySource = MemorySource{};
    g_worldProjectionCalibrated = false;
    g_worldProjectionBiasX = 0.0f;
    g_worldProjectionBiasY = 0.0f;
    g_worldProjectionSamples.clear();
    g_worldProjectionModel = WorldProjectionModel{};
    g_screenAnchorCandidates.clear();
    g_screenAnchorModel = ScreenAnchorModel{};
    g_screenAnchorHoverSamples = 0;
    g_lastLoggedScreenAnchorLabel.clear();
    g_worldCoordCandidates.clear();
    g_worldCoordModel = WorldCoordModel{};
    g_worldCoordHoverSamples = 0;
    g_lastLoggedWorldCoordLabel.clear();
}

static std::wstring FormatHex(uintptr_t value) {
    wchar_t text[32]{};
    swprintf_s(text, L"0x%llX", static_cast<unsigned long long>(value));
    return text;
}

static bool ReadRemoteMemory(uintptr_t address, void* buffer, size_t size) {
    if (!g_memorySource.process || address == 0 || !buffer || size == 0) return false;

    SIZE_T bytesRead = 0;
    return ReadProcessMemory(
        g_memorySource.process,
        reinterpret_cast<LPCVOID>(address),
        buffer,
        size,
        &bytesRead
    ) != FALSE && bytesRead == size;
}

static bool ReadRemoteBytes(uintptr_t address, size_t size, std::vector<uint8_t>& bytes) {
    bytes.clear();
    if (size == 0) return true;

    bytes.resize(size);
    if (!ReadRemoteMemory(address, bytes.data(), bytes.size())) {
        bytes.clear();
        return false;
    }

    return true;
}

static bool IsLikelyRemotePointer(uintptr_t address) {
    return address >= 0x10000 && address < 0x0000800000000000ULL;
}

template <typename T>
static bool ReadRemoteValue(uintptr_t address, T& value) {
    return ReadRemoteMemory(address, &value, sizeof(T));
}

static bool IsReadableProtect(DWORD protect) {
    if (protect & PAGE_GUARD) return false;
    if (protect & PAGE_NOACCESS) return false;

    const DWORD baseProtect = protect & 0xff;
    return baseProtect == PAGE_READONLY ||
        baseProtect == PAGE_READWRITE ||
        baseProtect == PAGE_WRITECOPY ||
        baseProtect == PAGE_EXECUTE_READ ||
        baseProtect == PAGE_EXECUTE_READWRITE ||
        baseProtect == PAGE_EXECUTE_WRITECOPY;
}

static bool ReadRemoteBytesBestEffort(uintptr_t address, size_t maxSize, std::vector<uint8_t>& bytes) {
    bytes.clear();
    if (!g_memorySource.process || !IsLikelyRemotePointer(address) || maxSize == 0) return false;

    uintptr_t current = address;
    size_t remaining = maxSize;
    while (remaining > 0) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQueryEx(
            g_memorySource.process,
            reinterpret_cast<LPCVOID>(current),
            &mbi,
            sizeof(mbi)
        ) != sizeof(mbi)) {
            break;
        }

        const uintptr_t regionBase = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        const uintptr_t regionEnd = regionBase + static_cast<uintptr_t>(mbi.RegionSize);
        if (regionEnd <= current || mbi.State != MEM_COMMIT || !IsReadableProtect(mbi.Protect)) {
            break;
        }

        const size_t regionAvailable = static_cast<size_t>(regionEnd - current);
        const size_t chunkSize = regionAvailable < remaining ? regionAvailable : remaining;
        const size_t oldSize = bytes.size();
        bytes.resize(oldSize + chunkSize);

        SIZE_T bytesRead = 0;
        if (ReadProcessMemory(
            g_memorySource.process,
            reinterpret_cast<LPCVOID>(current),
            bytes.data() + oldSize,
            chunkSize,
            &bytesRead
        ) == FALSE || bytesRead == 0) {
            bytes.resize(oldSize);
            break;
        }

        bytes.resize(oldSize + static_cast<size_t>(bytesRead));
        current += static_cast<uintptr_t>(bytesRead);
        remaining -= static_cast<size_t>(bytesRead);

        if (bytesRead < chunkSize) break;
    }

    return !bytes.empty();
}

static bool FindRemoteMainModule(DWORD processId, uintptr_t& moduleBase, DWORD& moduleSize, std::wstring& moduleName) {
    HANDLE snapshot = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 20; ++attempt) {
        snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, processId);
        if (snapshot != INVALID_HANDLE_VALUE) break;
        if (GetLastError() != ERROR_BAD_LENGTH) return false;
        Sleep(50);
    }
    if (snapshot == INVALID_HANDLE_VALUE) return false;

    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);

    bool found = false;
    for (int attempt = 0; attempt < 20; ++attempt) {
        if (Module32FirstW(snapshot, &entry)) {
            moduleBase = reinterpret_cast<uintptr_t>(entry.modBaseAddr);
            moduleSize = entry.modBaseSize;
            moduleName = entry.szModule;
            found = moduleBase != 0 && moduleSize > 0;
            break;
        }

        if (GetLastError() != ERROR_BAD_LENGTH) break;
        Sleep(50);
    }

    CloseHandle(snapshot);
    return found;
}

static bool FindRemoteMainModulePsapi(HANDLE process, uintptr_t& moduleBase, DWORD& moduleSize, std::wstring& moduleName) {
    HMODULE modules[1024]{};
    DWORD bytesNeeded = 0;
    if (!EnumProcessModulesEx(process, modules, sizeof(modules), &bytesNeeded, LIST_MODULES_ALL)) {
        return false;
    }

    const DWORD foundModules = bytesNeeded / sizeof(HMODULE);
    const DWORD moduleCount = foundModules < ARRAYSIZE(modules) ? foundModules : ARRAYSIZE(modules);
    if (moduleCount == 0) return false;

    MODULEINFO moduleInfo{};
    if (!GetModuleInformation(process, modules[0], &moduleInfo, sizeof(moduleInfo))) {
        return false;
    }

    wchar_t baseName[MAX_PATH]{};
    if (GetModuleBaseNameW(process, modules[0], baseName, ARRAYSIZE(baseName)) == 0) {
        wcscpy_s(baseName, L"D2R.exe");
    }

    moduleBase = reinterpret_cast<uintptr_t>(moduleInfo.lpBaseOfDll);
    moduleSize = moduleInfo.SizeOfImage;
    moduleName = baseName;
    return moduleBase != 0 && moduleSize > 0;
}

static bool FindD2RProcessId(DWORD& processId) {
    processId = 0;

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return false;

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);

    bool found = false;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, L"D2R.exe") == 0 ||
                _wcsicmp(entry.szExeFile, L"Diablo II Resurrected.exe") == 0) {
                processId = entry.th32ProcessID;
                found = processId != 0;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);
    return found;
}

static bool AttachMemorySourceProcess(DWORD processId) {
    if (processId == 0) {
        CloseMemorySource();
        g_memorySource.status = L"missing process id";
        return false;
    }

    if (g_memorySource.connected && g_memorySource.processId == processId) {
        return true;
    }

    CloseMemorySource();

    HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, processId);
    if (!process) {
        g_memorySource.status = L"OpenProcess failed";
        return false;
    }

    wchar_t processPath[MAX_PATH]{};
    DWORD processPathSize = ARRAYSIZE(processPath);
    QueryFullProcessImageNameW(process, 0, processPath, &processPathSize);

    uintptr_t moduleBase = 0;
    DWORD moduleSize = 0;
    std::wstring moduleName;
    if (!FindRemoteMainModule(processId, moduleBase, moduleSize, moduleName) &&
        !FindRemoteMainModulePsapi(process, moduleBase, moduleSize, moduleName)) {
        CloseHandle(process);
        g_memorySource.status = L"module lookup failed";
        return false;
    }

    g_memorySource.processId = processId;
    g_memorySource.process = process;
    g_memorySource.moduleBase = moduleBase;
    g_memorySource.moduleSize = moduleSize;
    g_memorySource.moduleName = moduleName;
    g_memorySource.processPath = processPath;
    g_memorySource.connected = true;
    g_memorySource.status = L"connected";
    return true;
}

static bool AttachMemorySource(HWND gameWnd) {
    if (!gameWnd) {
        CloseMemorySource();
        g_memorySource.status = L"waiting for D2R window";
        return false;
    }

    DWORD processId = 0;
    GetWindowThreadProcessId(gameWnd, &processId);
    return AttachMemorySourceProcess(processId);
}

static std::vector<int> ParseIdaPattern(const char* signature) {
    std::vector<int> bytes;
    const char* current = signature;

    while (*current) {
        if (*current == ' ') {
            ++current;
            continue;
        }

        if (*current == '?') {
            bytes.push_back(-1);
            ++current;
            if (*current == '?') {
                ++current;
            }
            continue;
        }

        char* next = nullptr;
        unsigned long value = strtoul(current, &next, 16);
        if (next == current) break;

        bytes.push_back(static_cast<int>(value & 0xff));
        current = next;
    }

    return bytes;
}

static bool ReadRemoteModuleImage(std::vector<uint8_t>& image) {
    if (!g_memorySource.connected || g_memorySource.moduleBase == 0 || g_memorySource.moduleSize == 0) return false;

    g_memorySource.lastModuleBytesRead = 0;
    image.clear();
    image.assign(g_memorySource.moduleSize, 0);

    const uintptr_t moduleStart = g_memorySource.moduleBase;
    const uintptr_t moduleEnd = moduleStart + g_memorySource.moduleSize;
    uintptr_t address = moduleStart;
    size_t totalBytesRead = 0;

    while (address < moduleEnd) {
        MEMORY_BASIC_INFORMATION mbi{};
        SIZE_T querySize = VirtualQueryEx(
            g_memorySource.process,
            reinterpret_cast<LPCVOID>(address),
            &mbi,
            sizeof(mbi)
        );

        if (querySize == 0) {
            address += 0x1000;
            continue;
        }

        const uintptr_t regionStart = max(address, reinterpret_cast<uintptr_t>(mbi.BaseAddress));
        const uintptr_t regionEnd = min(moduleEnd, reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize);

        if (mbi.State == MEM_COMMIT && IsReadableProtect(mbi.Protect) && regionEnd > regionStart) {
            for (uintptr_t page = regionStart; page < regionEnd; page += 0x1000) {
                const uintptr_t pageRemaining = regionEnd - page;
                const size_t pageSize = static_cast<size_t>(pageRemaining < 0x1000 ? pageRemaining : 0x1000);
                const size_t imageOffset = static_cast<size_t>(page - moduleStart);
                SIZE_T bytesRead = 0;

                if (ReadProcessMemory(
                    g_memorySource.process,
                    reinterpret_cast<LPCVOID>(page),
                    image.data() + imageOffset,
                    pageSize,
                    &bytesRead
                ) != FALSE && bytesRead > 0) {
                    totalBytesRead += bytesRead;
                }
            }
        }

        uintptr_t nextAddress = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (nextAddress <= address) {
            nextAddress = address + 0x1000;
        }
        address = nextAddress;
    }

    if (totalBytesRead == 0) {
        image.clear();
        return false;
    }

    g_memorySource.lastModuleBytesRead = totalBytesRead;
    return true;
}

static size_t ScanImagePatternMatches(
    const std::vector<uint8_t>& image,
    const std::vector<int>& pattern,
    size_t maxMatches,
    size_t* firstOffset
) {
    if (firstOffset) *firstOffset = 0;
    if (image.empty() || pattern.empty() || image.size() < pattern.size()) return 0;

    size_t matches = 0;
    for (size_t i = 0; i <= image.size() - pattern.size(); ++i) {
        bool found = true;

        for (size_t j = 0; j < pattern.size(); ++j) {
            if (pattern[j] >= 0 && image[i + j] != static_cast<uint8_t>(pattern[j])) {
                found = false;
                break;
            }
        }

        if (found) {
            if (matches == 0 && firstOffset) {
                *firstOffset = i;
            }

            ++matches;
            if (maxMatches > 0 && matches >= maxMatches) {
                return matches;
            }
        }
    }

    return matches;
}

static std::vector<size_t> ScanImagePatternOffsets(
    const std::vector<uint8_t>& image,
    const std::vector<int>& pattern,
    size_t maxMatches
) {
    std::vector<size_t> offsets;
    if (image.empty() || pattern.empty() || image.size() < pattern.size()) return offsets;

    for (size_t i = 0; i <= image.size() - pattern.size(); ++i) {
        bool found = true;

        for (size_t j = 0; j < pattern.size(); ++j) {
            if (pattern[j] >= 0 && image[i + j] != static_cast<uint8_t>(pattern[j])) {
                found = false;
                break;
            }
        }

        if (found) {
            offsets.push_back(i);
            if (maxMatches > 0 && offsets.size() >= maxMatches) {
                return offsets;
            }
        }
    }

    return offsets;
}

struct RipRelativePattern {
    const wchar_t* name;
    const char* signature;
    int opcodeOffset;
};

struct ModuleRelativePattern {
    const wchar_t* name;
    const char* signature;
    int displacementOffset;
    int addressAdjustment;
};

static bool ResolveRipRelativePatternCandidates(
    const std::vector<uint8_t>& image,
    const RipRelativePattern* candidates,
    size_t candidateCount,
    uintptr_t& address,
    size_t& matchCount,
    std::wstring& matchName
) {
    address = 0;
    matchCount = 0;
    matchName.clear();

    for (size_t i = 0; i < candidateCount; ++i) {
        const std::vector<int> pattern = ParseIdaPattern(candidates[i].signature);
        size_t offset = 0;
        const size_t matches = ScanImagePatternMatches(image, pattern, 16, &offset);

        if (matches > 0) {
            matchCount = matches;
            matchName = candidates[i].name;
        }

        if (matches != 1) {
            continue;
        }

        if (offset + candidates[i].opcodeOffset + sizeof(int32_t) > image.size()) {
            continue;
        }

        int32_t relativeAddress = 0;
        memcpy(&relativeAddress, image.data() + offset + candidates[i].opcodeOffset, sizeof(relativeAddress));

        address = g_memorySource.moduleBase + offset + candidates[i].opcodeOffset + sizeof(int32_t) + relativeAddress;
        return address != 0;
    }

    return false;
}

static uintptr_t GetServerUnitTableBaseForValidation(uintptr_t currentGame, uint32_t unitType) {
    switch (unitType) {
    case 0:
        return currentGame + D2_GAME_UNIT_TABLE_PLAYERS;
    case 1:
        return currentGame + D2_GAME_UNIT_TABLE_MONSTERS;
    case 2:
        return currentGame + D2_GAME_UNIT_TABLE_OBJECTS;
    case 3:
        return currentGame + D2_GAME_UNIT_TABLE_MISSILES;
    case 4:
        return currentGame + D2_GAME_UNIT_TABLE_ITEMS;
    case 5:
        return currentGame + D2_GAME_UNIT_TABLE_TILES;
    default:
        return 0;
    }
}

static size_t CountLikelyServerUnitsForValidation(
    uintptr_t tableBase,
    int bucketCount,
    size_t maxUnits
) {
    if (!IsLikelyRemotePointer(tableBase)) return 0;

    size_t count = 0;
    for (int bucket = 0; bucket < bucketCount && count < maxUnits; ++bucket) {
        uintptr_t current = 0;
        if (!ReadRemoteValue(tableBase + (bucket * sizeof(uintptr_t)), current)) {
            continue;
        }

        int guard = 0;
        while (IsLikelyRemotePointer(current) && guard++ < 64 && count < maxUnits) {
            uint32_t unitId = UINT32_MAX;
            uintptr_t next = 0;

            if (!ReadRemoteValue(current + D2_UNIT_OFFSET_ID, unitId)) {
                break;
            }

            if (unitId != UINT32_MAX) {
                ++count;
            }

            if (!ReadRemoteValue(current + D2_SERVER_UNIT_OFFSET_NEXT, next)) {
                break;
            }

            current = next;
        }
    }

    return count;
}

static int ScoreCurrentGamePointerForValidation(
    uintptr_t currentGame,
    size_t* playerCount,
    size_t* monsterCount,
    size_t* objectCount
) {
    if (playerCount) *playerCount = 0;
    if (monsterCount) *monsterCount = 0;
    if (objectCount) *objectCount = 0;

    if (!IsLikelyRemotePointer(currentGame)) return 0;

    const size_t players = CountLikelyServerUnitsForValidation(
        GetServerUnitTableBaseForValidation(currentGame, 0),
        D2_UNIT_HASH_BUCKETS,
        8
    );
    const size_t monsters = CountLikelyServerUnitsForValidation(
        GetServerUnitTableBaseForValidation(currentGame, D2_UNIT_MONSTER),
        D2_UNIT_HASH_BUCKETS,
        64
    );
    const size_t objects = CountLikelyServerUnitsForValidation(
        GetServerUnitTableBaseForValidation(currentGame, 2),
        D2_UNIT_HASH_BUCKETS,
        32
    );

    if (playerCount) *playerCount = players;
    if (monsterCount) *monsterCount = monsters;
    if (objectCount) *objectCount = objects;

    if (players == 0 && monsters == 0) return 0;
    return static_cast<int>((players * 20) + (monsters * 3) + objects);
}

static bool ResolveValidatedCurrentGamePatternCandidates(
    const std::vector<uint8_t>& image,
    const RipRelativePattern* candidates,
    size_t candidateCount,
    uintptr_t& address,
    size_t& matchCount,
    std::wstring& matchName,
    std::wstring& diagnostics
) {
    address = 0;
    diagnostics.clear();

    uintptr_t bestAddress = 0;
    uintptr_t bestCurrentGame = 0;
    std::wstring bestName;
    int bestScore = 0;
    size_t bestPlayers = 0;
    size_t bestMonsters = 0;
    size_t bestObjects = 0;

    std::wstring text = L"Current game candidate validation:";
    size_t diagnosticLines = 0;

    for (size_t i = 0; i < candidateCount; ++i) {
        const std::vector<int> pattern = ParseIdaPattern(candidates[i].signature);
        const std::vector<size_t> offsets = ScanImagePatternOffsets(image, pattern, 64);
        if (offsets.empty()) continue;

        matchCount = offsets.size();
        matchName = candidates[i].name;

        for (size_t offset : offsets) {
            if (offset + candidates[i].opcodeOffset + sizeof(int32_t) > image.size()) {
                continue;
            }

            int32_t relativeAddress = 0;
            memcpy(&relativeAddress, image.data() + offset + candidates[i].opcodeOffset, sizeof(relativeAddress));

            const uintptr_t candidateAddress =
                g_memorySource.moduleBase +
                offset +
                candidates[i].opcodeOffset +
                sizeof(int32_t) +
                relativeAddress;

            uintptr_t currentGame = 0;
            const bool readValue = ReadRemoteValue(candidateAddress, currentGame);
            size_t players = 0;
            size_t monsters = 0;
            size_t objects = 0;
            const int score = readValue
                ? ScoreCurrentGamePointerForValidation(currentGame, &players, &monsters, &objects)
                : 0;

            if (diagnosticLines < 24) {
                text += L"\n  ";
                text += candidates[i].name ? candidates[i].name : L"?";
                text += L" ";
                text += FormatHex(offset);
                text += L" ptr=";
                text += FormatHex(candidateAddress);
                text += L" value=";
                text += readValue ? FormatHex(currentGame) : L"read-failed";
                text += L" score=";
                text += std::to_wstring(score);
                text += L" p=";
                text += std::to_wstring(players);
                text += L" m=";
                text += std::to_wstring(monsters);
                text += L" o=";
                text += std::to_wstring(objects);
                ++diagnosticLines;
            }

            if (score > bestScore) {
                bestScore = score;
                bestAddress = candidateAddress;
                bestCurrentGame = currentGame;
                bestName = candidates[i].name ? candidates[i].name : L"?";
                bestPlayers = players;
                bestMonsters = monsters;
                bestObjects = objects;
            }
        }
    }

    if (diagnosticLines == 24) {
        text += L"\n  ...";
    }

    if (bestScore > 0) {
        text += L"\n  selected ";
        text += bestName;
        text += L" ptr=";
        text += FormatHex(bestAddress);
        text += L" value=";
        text += FormatHex(bestCurrentGame);
        text += L" score=";
        text += std::to_wstring(bestScore);
        text += L" p=";
        text += std::to_wstring(bestPlayers);
        text += L" m=";
        text += std::to_wstring(bestMonsters);
        text += L" o=";
        text += std::to_wstring(bestObjects);

        address = bestAddress;
        matchName = bestName;
        diagnostics = text;
        return true;
    }

    if (diagnosticLines > 0) {
        diagnostics = text;
    }

    return false;
}

static bool ResolveModuleRelativePatternCandidates(
    const std::vector<uint8_t>& image,
    const ModuleRelativePattern* candidates,
    size_t candidateCount,
    uintptr_t& address,
    size_t& matchCount,
    std::wstring& matchName
) {
    address = 0;

    for (size_t i = 0; i < candidateCount; ++i) {
        const std::vector<int> pattern = ParseIdaPattern(candidates[i].signature);
        size_t offset = 0;
        const size_t matches = ScanImagePatternMatches(image, pattern, 16, &offset);

        if (matches > 0) {
            matchCount = matches;
            matchName = candidates[i].name;
        }

        if (matches != 1) {
            continue;
        }

        if (offset + candidates[i].displacementOffset + sizeof(int32_t) > image.size()) {
            continue;
        }

        int32_t moduleRelativeOffset = 0;
        memcpy(
            &moduleRelativeOffset,
            image.data() + offset + candidates[i].displacementOffset,
            sizeof(moduleRelativeOffset)
        );

        address = g_memorySource.moduleBase +
            static_cast<uintptr_t>(moduleRelativeOffset + candidates[i].addressAdjustment);
        return address != 0;
    }

    return false;
}

static std::wstring FormatPatternOffsetList(const std::vector<size_t>& offsets) {
    std::wstring text = std::to_wstring(offsets.size());

    if (!offsets.empty()) {
        text += L" [";

        const size_t count = offsets.size() < 6 ? offsets.size() : 6;
        for (size_t i = 0; i < count; ++i) {
            if (i > 0) text += L", ";
            text += FormatHex(offsets[i]);
        }

        if (offsets.size() > count) {
            text += L", ...";
        }

        text += L"]";
    }

    return text;
}

static std::wstring BuildPatternDiagnostics(const std::vector<uint8_t>& image) {
    struct PatternDiagnostic {
        const wchar_t* name;
        const char* signature;
    };

    const PatternDiagnostic diagnostics[] = {
        { L"game-v1", "48 89 05 ? ? ? ? 48 85 C0 0F 84 ? ? ? ? 44 8B C7" },
        { L"game-branch", "48 89 05 ? ? ? ? 48 85 C0 0F 84 ? ? ? ?" },
        { L"game-test", "48 89 05 ? ? ? ? 48 85 C0" },
        { L"mouse-v1", "48 8D 3D ? ? ? ? BB ? ? ? ? 48 8B CF E8 ? ? ? ? 48 83 C7 10" },
        { L"mouse-short", "48 8D 3D ? ? ? ? BB ? ? ? ? 48 8B CF E8 ? ? ? ?" },
        { L"mouse-lea", "48 8D 3D ? ? ? ? BB ? ? ? ? 48 8B CF" },
        { L"last-hover-v3", "C6 84 C2 ? ? ? ? ? 48 8B 74 24" },
        { L"unit-hash-v3", "48 03 C7 49 8B 8C C6" },
        { L"old-call", "48 8B CF E8 ? ? ? ? 48 83 C7 10" },
        { L"mov-ebx-rcx-call", "BB ? ? ? ? 48 8B CF E8 ? ? ? ?" },
    };

    std::wstring text = L"Pattern diagnostics:";
    for (const auto& diagnostic : diagnostics) {
        const std::vector<int> pattern = ParseIdaPattern(diagnostic.signature);
        const std::vector<size_t> offsets = ScanImagePatternOffsets(image, pattern, 16);

        text += L"\n  ";
        text += diagnostic.name;
        text += L" = ";
        text += FormatPatternOffsetList(offsets);
    }

    return text;
}

static std::wstring BuildPatternMissStatus(const wchar_t* label, size_t matches, const std::wstring& patternName) {
    std::wstring text = label;
    text += L"=";
    text += std::to_wstring(matches);

    if (!patternName.empty()) {
        text += L" ";
        text += patternName;
    }

    if (matches >= 16) {
        text += L"+";
    }

    return text;
}

static bool ResolveCurrentGameAddressFromImage(
    const std::vector<uint8_t>& image,
    uintptr_t& currentGameAddress,
    size_t& matchCount,
    std::wstring& matchName,
    std::wstring& diagnostics
) {
    const RipRelativePattern currentGamePatterns[] = {
        { L"game-v1", "48 89 05 ? ? ? ? 48 85 C0 0F 84 ? ? ? ? 44 8B C7", 3 },
        { L"game-branch", "48 89 05 ? ? ? ? 48 85 C0 0F 84 ? ? ? ?", 3 },
        { L"game-test", "48 89 05 ? ? ? ? 48 85 C0", 3 },
    };

    diagnostics.clear();

    if (ResolveRipRelativePatternCandidates(
        image,
        currentGamePatterns,
        ARRAYSIZE(currentGamePatterns),
        currentGameAddress,
        matchCount,
        matchName
    )) {
        return true;
    }

    return ResolveValidatedCurrentGamePatternCandidates(
        image,
        currentGamePatterns,
        ARRAYSIZE(currentGamePatterns),
        currentGameAddress,
        matchCount,
        matchName,
        diagnostics
    );
}

static bool InitializeMemoryPatterns() {
    if (!g_memorySource.connected) return false;
    if (g_memorySource.patternsReady) return true;

    std::vector<uint8_t> image;
    if (!ReadRemoteModuleImage(image)) {
        g_memorySource.status = L"module page read failed";
        return false;
    }
    g_memorySource.patternDiagnostics = BuildPatternDiagnostics(image);

    uintptr_t mouseHoverAddress = 0;
    uintptr_t currentGameAddress = 0;

    const RipRelativePattern mouseHoverPatterns[] = {
        { L"mouse-v1", "48 8D 3D ? ? ? ? BB ? ? ? ? 48 8B CF E8 ? ? ? ? 48 83 C7 10", 3 },
        { L"mouse-short", "48 8D 3D ? ? ? ? BB ? ? ? ? 48 8B CF E8 ? ? ? ?", 3 },
        { L"mouse-lea", "48 8D 3D ? ? ? ? BB ? ? ? ? 48 8B CF", 3 },
    };

    const ModuleRelativePattern mouseHoverModulePatterns[] = {
        { L"last-hover-v3", "C6 84 C2 ? ? ? ? ? 48 8B 74 24", 3, -1 },
    };

    const ModuleRelativePattern unitHashPatterns[] = {
        { L"unit-hash-v3", "48 03 C7 49 8B 8C C6", 7, 0 },
    };

    bool foundMouseHover = ResolveModuleRelativePatternCandidates(
        image,
        mouseHoverModulePatterns,
        ARRAYSIZE(mouseHoverModulePatterns),
        mouseHoverAddress,
        g_memorySource.mouseHoverPatternMatches,
        g_memorySource.mouseHoverPatternName
    );

    if (!foundMouseHover) {
        foundMouseHover = ResolveRipRelativePatternCandidates(
            image,
            mouseHoverPatterns,
            ARRAYSIZE(mouseHoverPatterns),
            mouseHoverAddress,
            g_memorySource.mouseHoverPatternMatches,
            g_memorySource.mouseHoverPatternName
        );
    }

    std::wstring currentGameCandidateDiagnostics;
    const bool foundCurrentGame = ResolveCurrentGameAddressFromImage(
        image,
        currentGameAddress,
        g_memorySource.currentGamePatternMatches,
        g_memorySource.currentGamePatternName,
        currentGameCandidateDiagnostics
    );
    g_memorySource.currentGameCandidateDiagnostics = currentGameCandidateDiagnostics;

    uintptr_t unitHashTableAddress = 0;
    const bool foundUnitHashTable = ResolveModuleRelativePatternCandidates(
        image,
        unitHashPatterns,
        ARRAYSIZE(unitHashPatterns),
        unitHashTableAddress,
        g_memorySource.unitHashPatternMatches,
        g_memorySource.unitHashPatternName
    );

    if (!foundMouseHover || !foundUnitHashTable) {
        g_memorySource.status = L"patterns not found ";
        g_memorySource.status += BuildPatternMissStatus(L"hover", g_memorySource.mouseHoverPatternMatches, g_memorySource.mouseHoverPatternName);
        g_memorySource.status += L" ";
        g_memorySource.status += BuildPatternMissStatus(L"game", g_memorySource.currentGamePatternMatches, g_memorySource.currentGamePatternName);
        g_memorySource.status += L" ";
        g_memorySource.status += BuildPatternMissStatus(L"unit", g_memorySource.unitHashPatternMatches, g_memorySource.unitHashPatternName);
        return false;
    }

    g_memorySource.mouseHoverAddress = mouseHoverAddress;
    g_memorySource.currentSinglePlayerGameAddress = foundCurrentGame ? currentGameAddress : 0;
    g_memorySource.unitHashTableAddress = unitHashTableAddress;
    g_memorySource.patternsReady = true;
    g_memorySource.status = foundCurrentGame ? L"patterns ready" : L"hover pattern ready; game pattern missing";
    return true;
}

static bool TryRefreshCurrentGamePatternAddress() {
    std::vector<uint8_t> image;
    if (!ReadRemoteModuleImage(image)) {
        return false;
    }

    g_memorySource.patternDiagnostics = BuildPatternDiagnostics(image);

    uintptr_t currentGameAddress = 0;
    size_t matchCount = 0;
    std::wstring matchName;
    std::wstring diagnostics;
    const bool foundCurrentGame = ResolveCurrentGameAddressFromImage(
        image,
        currentGameAddress,
        matchCount,
        matchName,
        diagnostics
    );

    g_memorySource.currentGamePatternMatches = matchCount;
    g_memorySource.currentGamePatternName = matchName;
    g_memorySource.currentGameCandidateDiagnostics = diagnostics;

    if (!foundCurrentGame) {
        return false;
    }

    g_memorySource.currentSinglePlayerGameAddress = currentGameAddress;
    return true;
}


static void ResetHpTracking(const wchar_t* status) {
    g_memorySource.hoveredUnitAddress = 0;
    g_memorySource.hoveredClientUnitAddress = 0;
    g_memorySource.hoveredHp = -1;
    g_memorySource.hoveredMaxHp = -1;
    g_memorySource.trackedUnitType = UINT32_MAX;
    g_memorySource.trackedUnitId = UINT32_MAX;
    g_memorySource.trackedGame = 0;
    g_memorySource.trackedServerUnitAddress = 0;
    g_memorySource.trackedClientUnitAddress = 0;
    g_memorySource.trackedHp = -1;
    g_memorySource.trackedServerUnitSnapshot.clear();
    g_memorySource.trackedClientUnitSnapshot.clear();
    g_memorySource.hpReady = false;
    g_memorySource.hpStatus = status ? status : L"not ready";
}

static void ClearMonsterHpTracking() {
    g_memorySource.trackedMonsterGame = 0;
    g_memorySource.monsterScanGeneration = 0;
    g_memorySource.monsterScanCount = 0;
    g_memorySource.monsterTrackedCount = 0;
    g_memorySource.trackedMonsters.clear();
}

static bool FindUnitByTypeAndId(uint32_t unitType, uint32_t unitId, uintptr_t& unitAddress) {
    unitAddress = 0;

    if (g_memorySource.unitHashTableAddress == 0 || unitType > 5 || unitId == UINT32_MAX) {
        return false;
    }

    const uintptr_t tableAddress = g_memorySource.unitHashTableAddress +
        (D2_UNIT_HASH_TABLE_STRIDE * static_cast<uintptr_t>(unitType));

    for (int bucket = 0; bucket < D2_UNIT_HASH_BUCKETS; ++bucket) {
        uintptr_t current = 0;
        if (!ReadRemoteValue(tableAddress + (bucket * sizeof(uintptr_t)), current)) {
            continue;
        }

        int guard = 0;
        while (IsLikelyRemotePointer(current) && guard++ < 512) {
            uint32_t remoteUnitType = UINT32_MAX;
            uint32_t remoteUnitId = UINT32_MAX;
            uintptr_t next = 0;

            if (!ReadRemoteValue(current + D2_UNIT_OFFSET_TYPE, remoteUnitType) ||
                !ReadRemoteValue(current + D2_UNIT_OFFSET_ID, remoteUnitId)) {
                break;
            }

            if (remoteUnitType == unitType && remoteUnitId == unitId) {
                unitAddress = current;
                return true;
            }

            if (!ReadRemoteValue(current + D2_UNIT_OFFSET_NEXT, next)) {
                break;
            }

            current = next;
        }
    }

    return false;
}

static bool GetServerUnitTable(uintptr_t currentGame, uint32_t unitType, uint32_t unitId, uintptr_t& tableAddress) {
    if (!IsLikelyRemotePointer(currentGame)) return false;

    switch (unitType) {
    case 0:
        tableAddress = currentGame + D2_GAME_UNIT_TABLE_PLAYERS + ((unitId & 0x7F) * sizeof(uintptr_t));
        return true;
    case 1:
        tableAddress = currentGame + D2_GAME_UNIT_TABLE_MONSTERS + ((unitId & 0x7F) * sizeof(uintptr_t));
        return true;
    case 2:
        tableAddress = currentGame + D2_GAME_UNIT_TABLE_OBJECTS + ((unitId & 0x7F) * sizeof(uintptr_t));
        return true;
    case 3:
        tableAddress = currentGame + D2_GAME_UNIT_TABLE_MISSILES + ((unitId & 0x7F) * sizeof(uintptr_t));
        return true;
    case 4:
        tableAddress = currentGame + D2_GAME_UNIT_TABLE_ITEMS + ((unitId & 0x7F) * sizeof(uintptr_t));
        return true;
    case 5:
        tableAddress = currentGame + D2_GAME_UNIT_TABLE_TILES;
        return true;
    default:
        tableAddress = 0;
        return false;
    }
}

static bool GetServerUnitTableBase(uintptr_t currentGame, uint32_t unitType, uintptr_t& tableAddress) {
    if (!IsLikelyRemotePointer(currentGame)) return false;

    switch (unitType) {
    case 0:
        tableAddress = currentGame + D2_GAME_UNIT_TABLE_PLAYERS;
        return true;
    case 1:
        tableAddress = currentGame + D2_GAME_UNIT_TABLE_MONSTERS;
        return true;
    case 2:
        tableAddress = currentGame + D2_GAME_UNIT_TABLE_OBJECTS;
        return true;
    case 3:
        tableAddress = currentGame + D2_GAME_UNIT_TABLE_MISSILES;
        return true;
    case 4:
        tableAddress = currentGame + D2_GAME_UNIT_TABLE_ITEMS;
        return true;
    case 5:
        tableAddress = currentGame + D2_GAME_UNIT_TABLE_TILES;
        return true;
    default:
        tableAddress = 0;
        return false;
    }
}

static bool FindFirstServerUnitByType(uintptr_t currentGame, uint32_t unitType, uintptr_t& unitAddress) {
    unitAddress = 0;

    uintptr_t tableBase = 0;
    if (!GetServerUnitTableBase(currentGame, unitType, tableBase)) {
        return false;
    }

    const int bucketCount = unitType == 5 ? 1 : D2_UNIT_HASH_BUCKETS;
    for (int bucket = 0; bucket < bucketCount; ++bucket) {
        uintptr_t current = 0;
        if (!ReadRemoteValue(tableBase + (bucket * sizeof(uintptr_t)), current)) {
            continue;
        }

        int guard = 0;
        while (IsLikelyRemotePointer(current) && guard++ < 512) {
            unitAddress = current;
            return true;
        }
    }

    return false;
}

static bool FindServerUnitByTypeAndId(
    uintptr_t currentGame,
    uint32_t unitType,
    uint32_t unitId,
    uintptr_t& unitAddress
) {
    unitAddress = 0;

    uintptr_t tableAddress = 0;
    if (!GetServerUnitTable(currentGame, unitType, unitId, tableAddress)) {
        return false;
    }

    uintptr_t current = 0;
    if (!ReadRemoteValue(tableAddress, current)) {
        return false;
    }

    int guard = 0;
    while (IsLikelyRemotePointer(current) && guard++ < 512) {
        uint32_t remoteUnitId = UINT32_MAX;
        uintptr_t next = 0;

        if (!ReadRemoteValue(current + D2_UNIT_OFFSET_ID, remoteUnitId)) {
            break;
        }

        if (remoteUnitId == unitId) {
            unitAddress = current;
            return true;
        }

        if (!ReadRemoteValue(current + D2_SERVER_UNIT_OFFSET_NEXT, next)) {
            break;
        }

        current = next;
    }

    return false;
}

static bool EnumerateServerUnitsByType(
    uintptr_t currentGame,
    uint32_t unitType,
    std::vector<ServerUnitEntry>& units,
    size_t maxUnits
) {
    units.clear();

    uintptr_t tableBase = 0;
    if (!GetServerUnitTableBase(currentGame, unitType, tableBase)) {
        return false;
    }

    const int bucketCount = unitType == 5 ? 1 : D2_UNIT_HASH_BUCKETS;
    for (int bucket = 0; bucket < bucketCount && units.size() < maxUnits; ++bucket) {
        uintptr_t current = 0;
        if (!ReadRemoteValue(tableBase + (bucket * sizeof(uintptr_t)), current)) {
            continue;
        }

        int guard = 0;
        while (IsLikelyRemotePointer(current) && guard++ < 512 && units.size() < maxUnits) {
            uint32_t remoteUnitId = UINT32_MAX;
            uintptr_t next = 0;

            if (!ReadRemoteValue(current + D2_UNIT_OFFSET_ID, remoteUnitId)) {
                break;
            }

            if (remoteUnitId != UINT32_MAX) {
                ServerUnitEntry entry{};
                entry.unitType = unitType;
                entry.unitId = remoteUnitId;
                entry.serverUnit = current;
                units.push_back(entry);
            }

            if (!ReadRemoteValue(current + D2_SERVER_UNIT_OFFSET_NEXT, next)) {
                break;
            }

            current = next;
        }
    }

    return true;
}

static bool TryReadStatFromArray(uintptr_t statArrayAddress, uint16_t statId, int32_t& value) {
    D2StatArrayMemory statArray{};
    if (!ReadRemoteValue(statArrayAddress, statArray)) return false;
    if (!IsLikelyRemotePointer(statArray.firstStat) || statArray.size == 0 || statArray.size > D2_MAX_STAT_COUNT) {
        return false;
    }

    for (uint64_t i = 0; i < statArray.size; ++i) {
        D2StatValueMemory stat{};
        if (!ReadRemoteValue(statArray.firstStat + (i * sizeof(D2StatValueMemory)), stat)) {
            return false;
        }

        if (stat.layer == 0 && stat.stat == statId) {
            value = stat.value;
            return true;
        }
    }

    return false;
}

static bool TryReadUnitStat(uintptr_t unitAddress, uint16_t statId, int32_t& value) {
    uintptr_t statsListEx = 0;
    if (!ReadRemoteValue(unitAddress + D2_UNIT_OFFSET_STATS, statsListEx) || !IsLikelyRemotePointer(statsListEx)) {
        return false;
    }

    return TryReadStatFromArray(statsListEx + D2_STATLIST_BASE_ARRAY, statId, value) ||
        TryReadStatFromArray(statsListEx + D2_STATLIST_EXTRA_ARRAY, statId, value);
}

static bool TryReadUnitHp(uintptr_t unitAddress, int& hp, int& maxHp) {
    int32_t rawHp = 0;
    int32_t rawMaxHp = 0;
    const bool foundHp = TryReadUnitStat(unitAddress, D2_STAT_HITPOINTS, rawHp);
    const bool foundMaxHp = TryReadUnitStat(unitAddress, D2_STAT_MAXHP, rawMaxHp);

    if (!foundHp && !foundMaxHp) {
        return false;
    }

    hp = foundHp ? max(0, rawHp >> 8) : 0;
    maxHp = foundMaxHp ? max(0, rawMaxHp >> 8) : hp;
    return true;
}

static const wchar_t* DamageKindName(DamageKind damageKind) {
    switch (damageKind) {
    case DamageKind::Critical:
        return L"critical";
    case DamageKind::Normal:
    default:
        return L"normal";
    }
}

static uint32_t ReadSnapshotDword(const std::vector<uint8_t>& snapshot, size_t offset) {
    if (offset + sizeof(uint32_t) > snapshot.size()) return 0;

    uint32_t value = 0;
    memcpy(&value, snapshot.data() + offset, sizeof(value));
    return value;
}

static uint64_t ReadSnapshotQword(const std::vector<uint8_t>& snapshot, size_t offset) {
    if (offset + sizeof(uint64_t) > snapshot.size()) return 0;

    uint64_t value = 0;
    memcpy(&value, snapshot.data() + offset, sizeof(value));
    return value;
}

static uint16_t ReadSnapshotWord(const std::vector<uint8_t>& snapshot, size_t offset) {
    if (offset + sizeof(uint16_t) > snapshot.size()) return 0;

    uint16_t value = 0;
    memcpy(&value, snapshot.data() + offset, sizeof(value));
    return value;
}

static float ReadSnapshotFloat(const std::vector<uint8_t>& snapshot, size_t offset) {
    if (offset + sizeof(float) > snapshot.size()) return 0.0f;

    float value = 0.0f;
    memcpy(&value, snapshot.data() + offset, sizeof(value));
    return value;
}

static std::wstring FormatDwordHex(uint32_t value) {
    wchar_t text[16]{};
    swprintf_s(text, L"%08X", value);
    return text;
}

static std::wstring FormatOffset(size_t offset) {
    wchar_t text[32]{};
    swprintf_s(text, L"+0x%03llX", static_cast<unsigned long long>(offset));
    return text;
}

static int AbsInt(int value) {
    return value < 0 ? -value : value;
}

static std::wstring FormatSnapshotDwordChange(
    const std::vector<uint8_t>& before,
    const std::vector<uint8_t>& after,
    size_t offset
) {
    if (offset + sizeof(uint32_t) > before.size() || offset + sizeof(uint32_t) > after.size()) {
        return L"unavailable";
    }

    const uint32_t oldValue = ReadSnapshotDword(before, offset);
    const uint32_t newValue = ReadSnapshotDword(after, offset);

    std::wstring text = FormatDwordHex(oldValue);
    text += oldValue == newValue ? L" unchanged" : L" -> ";
    if (oldValue != newValue) {
        text += FormatDwordHex(newValue);
    }

    return text;
}

static std::wstring FormatOptionalStat(int32_t value, bool found) {
    return found ? std::to_wstring(value) : L"?";
}

static const wchar_t* KnownStatName(uint16_t statId) {
    switch (statId) {
    case D2_STAT_HITPOINTS:
        return L"HITPOINTS";
    case D2_STAT_MAXHP:
        return L"MAXHP";
    case D2_STAT_ITEM_CRUSHINGBLOW:
        return L"ITEM_CRUSHINGBLOW";
    case D2_STAT_ITEM_DEADLYSTRIKE:
        return L"ITEM_DEADLYSTRIKE";
    case 328:
        return L"PIERCE_IDX";
    case D2_STAT_PASSIVE_CRITICAL_STRIKE:
        return L"PASSIVE_CRITICAL_STRIKE";
    case D2_STAT_PASSIVE_MASTERY_MELEE_CRIT:
        return L"PASSIVE_MASTERY_MELEE_CRIT";
    case D2_STAT_PASSIVE_MASTERY_THROW_CRIT:
        return L"PASSIVE_MASTERY_THROW_CRIT";
    default:
        return L"";
    }
}

static bool ReadStatArraySnapshot(
    uintptr_t statArrayAddress,
    std::vector<HitRecorderStatValue>& stats
) {
    stats.clear();

    D2StatArrayMemory statArray{};
    if (!ReadRemoteValue(statArrayAddress, statArray)) return false;
    if (!IsLikelyRemotePointer(statArray.firstStat) || statArray.size == 0 || statArray.size > D2_MAX_STAT_COUNT) {
        return false;
    }

    stats.reserve(static_cast<size_t>(statArray.size));
    for (uint64_t i = 0; i < statArray.size; ++i) {
        D2StatValueMemory remoteStat{};
        if (!ReadRemoteValue(statArray.firstStat + (i * sizeof(D2StatValueMemory)), remoteStat)) {
            return false;
        }

        HitRecorderStatValue stat{};
        stat.layer = remoteStat.layer;
        stat.stat = remoteStat.stat;
        stat.value = remoteStat.value;
        stats.push_back(stat);
    }

    return true;
}

static bool ReadUnitStatSnapshots(
    uintptr_t unitAddress,
    std::vector<HitRecorderStatValue>& baseStats,
    std::vector<HitRecorderStatValue>& extraStats
) {
    baseStats.clear();
    extraStats.clear();

    uintptr_t statsListEx = 0;
    if (!ReadRemoteValue(unitAddress + D2_UNIT_OFFSET_STATS, statsListEx) || !IsLikelyRemotePointer(statsListEx)) {
        return false;
    }

    const bool readBase = ReadStatArraySnapshot(statsListEx + D2_STATLIST_BASE_ARRAY, baseStats);
    const bool readExtra = ReadStatArraySnapshot(statsListEx + D2_STATLIST_EXTRA_ARRAY, extraStats);
    return readBase || readExtra;
}

static bool ReadStatEntryBytesSnapshot(
    uintptr_t statArrayAddress,
    std::vector<uint8_t>& bytes
) {
    bytes.clear();

    D2StatArrayMemory statArray{};
    if (!ReadRemoteValue(statArrayAddress, statArray)) return false;
    if (!IsLikelyRemotePointer(statArray.firstStat) || statArray.size == 0 || statArray.size > D2_MAX_STAT_COUNT) {
        return false;
    }

    const uint64_t entryCount = statArray.size < static_cast<uint64_t>(D2_MAX_STAT_COUNT)
        ? statArray.size
        : static_cast<uint64_t>(D2_MAX_STAT_COUNT);
    const size_t byteCount = static_cast<size_t>(entryCount * sizeof(D2StatValueMemory));
    const size_t cappedByteCount = byteCount < D2_STAT_ENTRIES_SNAPSHOT_BYTES
        ? byteCount
        : D2_STAT_ENTRIES_SNAPSHOT_BYTES;

    return ReadRemoteBytesBestEffort(statArray.firstStat, cappedByteCount, bytes);
}

static void ReadUnitRawContextSnapshots(
    uintptr_t unitAddress,
    std::vector<uint8_t>& extendedUnitSnapshot,
    uintptr_t& statsListEx,
    std::vector<uint8_t>& statsListSnapshot,
    std::vector<uint8_t>& baseStatEntrySnapshot,
    std::vector<uint8_t>& extraStatEntrySnapshot
) {
    extendedUnitSnapshot.clear();
    statsListEx = 0;
    statsListSnapshot.clear();
    baseStatEntrySnapshot.clear();
    extraStatEntrySnapshot.clear();

    if (!IsLikelyRemotePointer(unitAddress)) return;

    ReadRemoteBytesBestEffort(unitAddress, D2_EXTENDED_UNIT_SNAPSHOT_BYTES, extendedUnitSnapshot);

    uintptr_t remoteStatsListEx = 0;
    if (!ReadRemoteValue(unitAddress + D2_UNIT_OFFSET_STATS, remoteStatsListEx) || !IsLikelyRemotePointer(remoteStatsListEx)) {
        return;
    }

    statsListEx = remoteStatsListEx;
    ReadRemoteBytesBestEffort(statsListEx, D2_STATLIST_SNAPSHOT_BYTES, statsListSnapshot);
    ReadStatEntryBytesSnapshot(statsListEx + D2_STATLIST_BASE_ARRAY, baseStatEntrySnapshot);
    ReadStatEntryBytesSnapshot(statsListEx + D2_STATLIST_EXTRA_ARRAY, extraStatEntrySnapshot);
}

static std::wstring FormatHitRecorderStatKey(const HitRecorderStatValue& stat) {
    std::wstring text;
    if (stat.layer != 0) {
        text += L"L";
        text += std::to_wstring(stat.layer);
        text += L"/";
    }

    text += std::to_wstring(stat.stat);
    const wchar_t* statName = KnownStatName(stat.stat);
    if (statName && statName[0] != L'\0') {
        text += L"(";
        text += statName;
        text += L")";
    }

    return text;
}

static std::wstring FormatHitRecorderStatValue(uint16_t statId, int32_t value) {
    std::wstring text = std::to_wstring(value);
    if (statId == D2_STAT_HITPOINTS || statId == D2_STAT_MAXHP) {
        text += L"[";
        text += std::to_wstring(value >> 8);
        text += L"]";
    }
    return text;
}

static const HitRecorderStatValue* FindHitRecorderStat(
    const std::vector<HitRecorderStatValue>& stats,
    uint16_t layer,
    uint16_t statId
) {
    for (const auto& stat : stats) {
        if (stat.layer == layer && stat.stat == statId) return &stat;
    }

    return nullptr;
}

static bool HasHitRecorderStat(
    const std::vector<HitRecorderStatValue>& stats,
    uint16_t layer,
    uint16_t statId
) {
    return FindHitRecorderStat(stats, layer, statId) != nullptr;
}

static std::wstring FormatHitRecorderStatDiffs(
    const std::vector<HitRecorderStatValue>& before,
    const std::vector<HitRecorderStatValue>& after,
    const wchar_t* label,
    int maxChanges
) {
    std::wstring text = (label && label[0] != L'\0') ? label : L"stats";

    if (before.empty() && after.empty()) {
        text += L": none";
        return text;
    }

    text += L": ";
    int count = 0;

    auto appendSeparator = [&]() {
        if (count > 0) text += L", ";
    };

    for (const auto& oldStat : before) {
        const HitRecorderStatValue* newStat = FindHitRecorderStat(after, oldStat.layer, oldStat.stat);
        if (newStat && newStat->value == oldStat.value) continue;

        appendSeparator();
        text += FormatHitRecorderStatKey(oldStat);
        text += L"=";
        text += FormatHitRecorderStatValue(oldStat.stat, oldStat.value);
        text += newStat ? L">" : L">removed";
        if (newStat) {
            text += FormatHitRecorderStatValue(newStat->stat, newStat->value);
        }

        ++count;
        if (count >= max(1, maxChanges)) {
            text += L", ...";
            break;
        }
    }

    if (count < max(1, maxChanges)) {
        for (const auto& newStat : after) {
            if (HasHitRecorderStat(before, newStat.layer, newStat.stat)) continue;

            appendSeparator();
            text += FormatHitRecorderStatKey(newStat);
            text += L"=added>";
            text += FormatHitRecorderStatValue(newStat.stat, newStat.value);

            ++count;
            if (count >= max(1, maxChanges)) {
                text += L", ...";
                break;
            }
        }
    }

    if (count == 0) {
        text += L"unchanged";
    }

    return text;
}

static std::wstring BuildStatArraySummary(uintptr_t statArrayAddress, const wchar_t* label, int maxEntries) {
    std::wstring text = label ? label : L"stats";

    D2StatArrayMemory statArray{};
    if (!ReadRemoteValue(statArrayAddress, statArray)) {
        text += L"=read-failed";
        return text;
    }

    text += L"(size=";
    text += std::to_wstring(statArray.size);
    text += L")";

    if (!IsLikelyRemotePointer(statArray.firstStat) || statArray.size == 0 || statArray.size > D2_MAX_STAT_COUNT) {
        text += L": none";
        return text;
    }

    text += L": ";
    const uint64_t entryLimit = static_cast<uint64_t>(max(0, maxEntries));
    const uint64_t count = statArray.size < entryLimit ? statArray.size : entryLimit;
    for (uint64_t i = 0; i < count; ++i) {
        D2StatValueMemory stat{};
        if (!ReadRemoteValue(statArray.firstStat + (i * sizeof(D2StatValueMemory)), stat)) {
            if (i > 0) text += L", ";
            text += L"read-failed";
            break;
        }

        if (i > 0) text += L", ";
        if (stat.layer != 0) {
            text += L"L";
            text += std::to_wstring(stat.layer);
            text += L"/";
        }
        text += std::to_wstring(stat.stat);
        const wchar_t* statName = KnownStatName(stat.stat);
        if (statName && statName[0] != L'\0') {
            text += L"(";
            text += statName;
            text += L")";
        }
        text += L"=";
        text += std::to_wstring(stat.value);
    }

    if (statArray.size > count) {
        text += L", ...";
    }

    return text;
}

static std::wstring BuildPlayerStatDump(uintptr_t playerUnit, int maxEntries = 96) {
    uintptr_t statsListEx = 0;
    if (!ReadRemoteValue(playerUnit + D2_UNIT_OFFSET_STATS, statsListEx) || !IsLikelyRemotePointer(statsListEx)) {
        return L"statsListEx=unavailable";
    }

    std::wstring text = L"statsListEx=";
    text += FormatHex(statsListEx);
    text += L" ";
    text += BuildStatArraySummary(statsListEx + D2_STATLIST_BASE_ARRAY, L"base", maxEntries);
    text += L" ";
    text += BuildStatArraySummary(statsListEx + D2_STATLIST_EXTRA_ARRAY, L"extra", maxEntries);
    return text;
}

static std::wstring BuildPlayerCritStatText(uintptr_t currentGame) {
    uintptr_t playerUnit = 0;
    if (!FindFirstServerUnitByType(currentGame, 0, playerUnit)) {
        return L"unavailable";
    }

    int32_t crushingBlow = 0;
    int32_t deadlyStrike = 0;
    int32_t passiveCritical = 0;
    int32_t meleeMasteryCritical = 0;
    int32_t throwMasteryCritical = 0;

    const bool foundCrushingBlow = TryReadUnitStat(playerUnit, D2_STAT_ITEM_CRUSHINGBLOW, crushingBlow);
    const bool foundDeadlyStrike = TryReadUnitStat(playerUnit, D2_STAT_ITEM_DEADLYSTRIKE, deadlyStrike);
    const bool foundPassiveCritical = TryReadUnitStat(playerUnit, D2_STAT_PASSIVE_CRITICAL_STRIKE, passiveCritical);
    const bool foundMeleeMasteryCritical = TryReadUnitStat(playerUnit, D2_STAT_PASSIVE_MASTERY_MELEE_CRIT, meleeMasteryCritical);
    const bool foundThrowMasteryCritical = TryReadUnitStat(playerUnit, D2_STAT_PASSIVE_MASTERY_THROW_CRIT, throwMasteryCritical);

    std::wstring text = L"playerUnit=";
    text += FormatHex(playerUnit);
    text += L" crushingBlow=";
    text += FormatOptionalStat(crushingBlow, foundCrushingBlow);
    text += L" deadlyStrike=";
    text += FormatOptionalStat(deadlyStrike, foundDeadlyStrike);
    text += L" passiveCritical=";
    text += FormatOptionalStat(passiveCritical, foundPassiveCritical);
    text += L" masteryMeleeCrit=";
    text += FormatOptionalStat(meleeMasteryCritical, foundMeleeMasteryCritical);
    text += L" masteryThrowCrit=";
    text += FormatOptionalStat(throwMasteryCritical, foundThrowMasteryCritical);

    if (!foundCrushingBlow &&
        !foundDeadlyStrike &&
        !foundPassiveCritical &&
        !foundMeleeMasteryCritical &&
        !foundThrowMasteryCritical) {
        text += L" ";
        text += BuildPlayerStatDump(playerUnit);
    }

    return text;
}

static bool ReadPlayerCombatSnapshots(
    uintptr_t currentGame,
    uintptr_t& playerUnitAddress,
    uint32_t& playerUnitId,
    uintptr_t& playerClientUnitAddress,
    std::vector<uint8_t>& playerUnitSnapshot,
    std::vector<uint8_t>& playerClientUnitSnapshot
) {
    playerUnitAddress = 0;
    playerUnitId = UINT32_MAX;
    playerClientUnitAddress = 0;
    playerUnitSnapshot.clear();
    playerClientUnitSnapshot.clear();

    if (!FindFirstServerUnitByType(currentGame, 0, playerUnitAddress)) {
        return false;
    }

    ReadRemoteValue(playerUnitAddress + D2_UNIT_OFFSET_ID, playerUnitId);
    ReadRemoteBytes(playerUnitAddress, D2_SERVER_UNIT_SNAPSHOT_BYTES, playerUnitSnapshot);

    if (playerUnitId != UINT32_MAX && FindUnitByTypeAndId(0, playerUnitId, playerClientUnitAddress)) {
        ReadRemoteBytes(playerClientUnitAddress, D2_CLIENT_UNIT_SNAPSHOT_BYTES, playerClientUnitSnapshot);
    }

    return !playerUnitSnapshot.empty() || !playerClientUnitSnapshot.empty();
}

static void ClearTrackedPlayerCombatSnapshots() {
    g_memorySource.trackedPlayerGame = 0;
    g_memorySource.trackedPlayerUnitId = UINT32_MAX;
    g_memorySource.trackedPlayerUnitAddress = 0;
    g_memorySource.trackedPlayerClientUnitAddress = 0;
    g_memorySource.trackedPlayerUnitSnapshot.clear();
    g_memorySource.trackedPlayerClientUnitSnapshot.clear();
}

static void StoreTrackedPlayerCombatSnapshots(
    uintptr_t currentGame,
    uint32_t playerUnitId,
    uintptr_t playerUnitAddress,
    uintptr_t playerClientUnitAddress,
    const std::vector<uint8_t>& playerUnitSnapshot,
    const std::vector<uint8_t>& playerClientUnitSnapshot
) {
    g_memorySource.trackedPlayerGame = currentGame;
    g_memorySource.trackedPlayerUnitId = playerUnitId;
    g_memorySource.trackedPlayerUnitAddress = playerUnitAddress;
    g_memorySource.trackedPlayerClientUnitAddress = playerClientUnitAddress;
    g_memorySource.trackedPlayerUnitSnapshot = playerUnitSnapshot;
    g_memorySource.trackedPlayerClientUnitSnapshot = playerClientUnitSnapshot;
}

static std::wstring FormatSnapshotDwordDiffsFrom(
    const std::vector<uint8_t>& before,
    const std::vector<uint8_t>& after,
    size_t startOffset,
    int maxChanges,
    bool annotateHitClassFlag
) {
    const size_t size = min(before.size(), after.size());
    if (size < sizeof(uint32_t) || startOffset + sizeof(uint32_t) > size) return L"unavailable";

    if ((startOffset % sizeof(uint32_t)) != 0) {
        startOffset += sizeof(uint32_t) - (startOffset % sizeof(uint32_t));
    }

    std::wstring text;
    int count = 0;

    for (size_t offset = startOffset; offset + sizeof(uint32_t) <= size; offset += sizeof(uint32_t)) {
        const uint32_t oldValue = ReadSnapshotDword(before, offset);
        const uint32_t newValue = ReadSnapshotDword(after, offset);
        if (oldValue == newValue) continue;

        if (count > 0) text += L", ";

        wchar_t item[96]{};
        swprintf_s(
            item,
            L"+0x%03llX:%08X>%08X%s",
            static_cast<unsigned long long>(offset),
            oldValue,
            newValue,
            (annotateHitClassFlag && ((oldValue ^ newValue) & D2_UNITFLAG_UPGRLIFENHITCLASS)) ? L" hitclass-flag" : L""
        );
        text += item;

        ++count;
        if (count >= max(1, maxChanges)) {
            text += L", ...";
            break;
        }
    }

    return text.empty() ? L"none" : text;
}

static std::wstring FormatSnapshotDwordDiffs(
    const std::vector<uint8_t>& before,
    const std::vector<uint8_t>& after,
    int maxChanges,
    bool annotateHitClassFlag
) {
    return FormatSnapshotDwordDiffsFrom(before, after, 0, maxChanges, annotateHitClassFlag);
}

static std::wstring FormatClientUnitDiffs(
    const std::vector<uint8_t>& before,
    const std::vector<uint8_t>& after
) {
    return FormatSnapshotDwordDiffs(before, after, 12, true);
}

static uint64_t RecordHitRecorderSample(
    uintptr_t currentGame,
    uint32_t targetUnitType,
    uint32_t targetUnitId,
    uintptr_t targetServerUnit,
    uintptr_t targetClientUnit,
    uintptr_t playerServerUnit,
    uintptr_t playerClientUnit,
    int hp,
    int maxHp,
    const std::vector<uint8_t>& targetServerSnapshot,
    const std::vector<uint8_t>& targetClientSnapshot,
    const std::vector<uint8_t>& playerServerSnapshot,
    const std::vector<uint8_t>& playerClientSnapshot
) {
    if (!g_config.hitRecorderEnabled) return 0;

    HitRecorderSample sample{};
    sample.id = g_nextHitRecorderSampleId++;
    sample.capturedAt = std::chrono::steady_clock::now();
    sample.currentGame = currentGame;
    sample.targetUnitType = targetUnitType;
    sample.targetUnitId = targetUnitId;
    sample.targetServerUnit = targetServerUnit;
    sample.targetClientUnit = targetClientUnit;
    sample.playerServerUnit = playerServerUnit;
    sample.playerClientUnit = playerClientUnit;
    sample.hp = hp;
    sample.maxHp = maxHp;
    sample.targetServerSnapshot = targetServerSnapshot;
    sample.targetClientSnapshot = targetClientSnapshot;
    sample.playerServerSnapshot = playerServerSnapshot;
    sample.playerClientSnapshot = playerClientSnapshot;
    ReadUnitRawContextSnapshots(
        targetServerUnit,
        sample.targetServerExtendedSnapshot,
        sample.targetStatsListEx,
        sample.targetStatsListSnapshot,
        sample.targetBaseStatEntrySnapshot,
        sample.targetExtraStatEntrySnapshot
    );
    ReadRemoteBytesBestEffort(targetClientUnit, D2_EXTENDED_UNIT_SNAPSHOT_BYTES, sample.targetClientExtendedSnapshot);
    ReadUnitRawContextSnapshots(
        playerServerUnit,
        sample.playerServerExtendedSnapshot,
        sample.playerStatsListEx,
        sample.playerStatsListSnapshot,
        sample.playerBaseStatEntrySnapshot,
        sample.playerExtraStatEntrySnapshot
    );
    ReadRemoteBytesBestEffort(playerClientUnit, D2_EXTENDED_UNIT_SNAPSHOT_BYTES, sample.playerClientExtendedSnapshot);
    ReadUnitStatSnapshots(targetServerUnit, sample.targetBaseStats, sample.targetExtraStats);
    ReadUnitStatSnapshots(playerServerUnit, sample.playerBaseStats, sample.playerExtraStats);

    g_hitRecorderSamples.push_back(sample);
    if (g_hitRecorderSamples.size() > HIT_RECORDER_MAX_SAMPLES) {
        g_hitRecorderSamples.erase(g_hitRecorderSamples.begin());
    }

    return sample.id;
}

static const HitRecorderSample* FindHitRecorderSample(uint64_t sampleId) {
    for (const auto& sample : g_hitRecorderSamples) {
        if (sample.id == sampleId) return &sample;
    }

    return nullptr;
}

static bool IsSampleNear(uint64_t sampleId, uint64_t centerSampleId) {
    if (sampleId >= centerSampleId) {
        return sampleId - centerSampleId <= HIT_RECORDER_CONTEXT_SAMPLES;
    }

    return centerSampleId - sampleId <= HIT_RECORDER_CONTEXT_SAMPLES;
}

static std::wstring FormatSampleDelta(
    const std::chrono::steady_clock::time_point& sampleTime,
    const std::chrono::steady_clock::time_point& centerTime
) {
    const auto delta = std::chrono::duration_cast<std::chrono::milliseconds>(sampleTime - centerTime).count();
    std::wstring text;
    if (delta > 0) {
        text += L"+";
    }
    text += std::to_wstring(delta);
    text += L"ms";
    return text;
}

static std::wstring FormatSnapshotKeys(
    const wchar_t* label,
    const std::vector<uint8_t>& snapshot,
    const size_t* offsets,
    size_t offsetCount
) {
    std::wstring text = label ? label : L"keys";
    text += L"{";

    if (snapshot.empty()) {
        text += L"n/a}";
        return text;
    }

    for (size_t i = 0; i < offsetCount; ++i) {
        if (i > 0) text += L" ";

        wchar_t item[48]{};
        swprintf_s(item, L"+0x%03llX=", static_cast<unsigned long long>(offsets[i]));
        text += item;

        if (offsets[i] + sizeof(uint32_t) <= snapshot.size()) {
            text += FormatDwordHex(ReadSnapshotDword(snapshot, offsets[i]));
        }
        else {
            text += L"????";
        }
    }

    text += L"}";
    return text;
}

static std::wstring FormatSnapshotByteCount(const std::vector<uint8_t>& snapshot) {
    if (snapshot.empty()) return L"n/a";

    std::wstring text = std::to_wstring(snapshot.size());
    text += L"B";
    return text;
}

static std::wstring FormatHitRecorderSampleLine(
    const HitRecorderSample& sample,
    const HitRecorderSample& center
) {
    static const size_t targetServerOffsets[] = { 0x00C, 0x0F4, 0x110, 0x124, 0x128, 0x148, 0x1C0, 0x1C4, 0x1C8, 0x1CC, 0x21C };
    static const size_t targetClientOffsets[] = { 0x00C, 0x110, 0x124, 0x128, 0x160, 0x1AC };
    static const size_t playerServerOffsets[] = { 0x00C, 0x0F0, 0x0F4, 0x110, 0x124, 0x128, 0x148 };
    static const size_t playerClientOffsets[] = { 0x00C, 0x0F0, 0x0F4, 0x110, 0x124, 0x128, 0x160, 0x1AC };

    std::wstring text = L"#";
    text += std::to_wstring(sample.id);
    text += L" ";
    text += FormatSampleDelta(sample.capturedAt, center.capturedAt);
    text += L" hp=";
    text += std::to_wstring(sample.hp);
    text += L"/";
    text += std::to_wstring(sample.maxHp);
    text += L" unit=";
    text += std::to_wstring(sample.targetUnitType);
    text += L":";
    text += std::to_wstring(sample.targetUnitId);
    text += L" target=";
    text += FormatHex(sample.targetServerUnit);
    text += L"/";
    text += FormatHex(sample.targetClientUnit);
    text += L" player=";
    text += FormatHex(sample.playerServerUnit);
    text += L"/";
    text += FormatHex(sample.playerClientUnit);
    text += L"\n  ";
    text += FormatSnapshotKeys(L"TS", sample.targetServerSnapshot, targetServerOffsets, ARRAYSIZE(targetServerOffsets));
    text += L"\n  ";
    text += FormatSnapshotKeys(L"TC", sample.targetClientSnapshot, targetClientOffsets, ARRAYSIZE(targetClientOffsets));
    text += L"\n  ";
    text += FormatSnapshotKeys(L"PS", sample.playerServerSnapshot, playerServerOffsets, ARRAYSIZE(playerServerOffsets));
    text += L"\n  ";
    text += FormatSnapshotKeys(L"PC", sample.playerClientSnapshot, playerClientOffsets, ARRAYSIZE(playerClientOffsets));
    text += L"\n  Raw{TSE=";
    text += FormatSnapshotByteCount(sample.targetServerExtendedSnapshot);
    text += L" TCE=";
    text += FormatSnapshotByteCount(sample.targetClientExtendedSnapshot);
    text += L" PSE=";
    text += FormatSnapshotByteCount(sample.playerServerExtendedSnapshot);
    text += L" PCE=";
    text += FormatSnapshotByteCount(sample.playerClientExtendedSnapshot);
    text += L" TSL=";
    text += FormatHex(sample.targetStatsListEx);
    text += L"/";
    text += FormatSnapshotByteCount(sample.targetStatsListSnapshot);
    text += L" PSL=";
    text += FormatHex(sample.playerStatsListEx);
    text += L"/";
    text += FormatSnapshotByteCount(sample.playerStatsListSnapshot);
    text += L"}";

    return text;
}

static int64_t AbsRecorderMilliseconds(int64_t value) {
    return value < 0 ? -value : value;
}

static std::chrono::steady_clock::time_point GetHitRecorderDamageTime(const HitRecorderDamage& damage) {
    const HitRecorderSample* sample = FindHitRecorderSample(damage.sampleId);
    return sample ? sample->capturedAt : damage.capturedAt;
}

static const HitRecorderSample* FindPreviousSampleForDamage(const HitRecorderDamage& damage) {
    const HitRecorderSample* after = FindHitRecorderSample(damage.sampleId);
    if (!after) return nullptr;

    const HitRecorderSample* before = nullptr;
    for (const auto& sample : g_hitRecorderSamples) {
        if (sample.id >= after->id) break;
        if (sample.currentGame != after->currentGame) continue;
        if (sample.targetUnitType != after->targetUnitType) continue;
        if (sample.targetUnitId != after->targetUnitId) continue;
        if (after->targetServerUnit != 0 && sample.targetServerUnit != after->targetServerUnit) continue;

        before = &sample;
    }

    return before;
}

static std::wstring FormatCompactHitSnapshotKeys(const HitRecorderSample* sample) {
    if (!sample) return L"keys=n/a";

    static const size_t targetServerOffsets[] = { 0x00C, 0x110, 0x124, 0x128, 0x1CC, 0x21C };
    static const size_t targetClientOffsets[] = { 0x00C, 0x110, 0x124, 0x128, 0x1AC };
    static const size_t playerServerOffsets[] = { 0x00C, 0x110, 0x124, 0x128 };
    static const size_t playerClientOffsets[] = { 0x00C, 0x110, 0x124, 0x128, 0x1AC };

    std::wstring text = FormatSnapshotKeys(L"TS", sample->targetServerSnapshot, targetServerOffsets, ARRAYSIZE(targetServerOffsets));
    text += L" ";
    text += FormatSnapshotKeys(L"TC", sample->targetClientSnapshot, targetClientOffsets, ARRAYSIZE(targetClientOffsets));
    text += L" ";
    text += FormatSnapshotKeys(L"PS", sample->playerServerSnapshot, playerServerOffsets, ARRAYSIZE(playerServerOffsets));
    text += L" ";
    text += FormatSnapshotKeys(L"PC", sample->playerClientSnapshot, playerClientOffsets, ARRAYSIZE(playerClientOffsets));
    text += L" Stats{TB=";
    text += std::to_wstring(sample->targetBaseStats.size());
    text += L" TE=";
    text += std::to_wstring(sample->targetExtraStats.size());
    text += L" PB=";
    text += std::to_wstring(sample->playerBaseStats.size());
    text += L" PE=";
    text += std::to_wstring(sample->playerExtraStats.size());
    text += L"}";
    text += L" Raw{TSE=";
    text += FormatSnapshotByteCount(sample->targetServerExtendedSnapshot);
    text += L" TCE=";
    text += FormatSnapshotByteCount(sample->targetClientExtendedSnapshot);
    text += L" PSE=";
    text += FormatSnapshotByteCount(sample->playerServerExtendedSnapshot);
    text += L" PCE=";
    text += FormatSnapshotByteCount(sample->playerClientExtendedSnapshot);
    text += L" TSL=";
    text += FormatHex(sample->targetStatsListEx);
    text += L"/";
    text += FormatSnapshotByteCount(sample->targetStatsListSnapshot);
    text += L" PSL=";
    text += FormatHex(sample->playerStatsListEx);
    text += L"/";
    text += FormatSnapshotByteCount(sample->playerStatsListSnapshot);
    text += L"}";
    return text;
}

static std::wstring FormatHitRecorderDamageSummary(
    const HitRecorderDamage& damage,
    const HitRecorderSample& center
) {
    const HitRecorderSample* sample = FindHitRecorderSample(damage.sampleId);

    std::wstring text = L"#";
    text += std::to_wstring(damage.sampleId);
    text += L" ";
    text += FormatSampleDelta(GetHitRecorderDamageTime(damage), center.capturedAt);
    text += L" dmg=";
    text += std::to_wstring(damage.amount);
    text += L" hp=";
    text += std::to_wstring(damage.oldHp);
    text += L"->";
    text += std::to_wstring(damage.newHp);
    text += L"/";
    text += std::to_wstring(damage.maxHp);
    text += L" unit=";
    text += std::to_wstring(damage.targetUnitType);
    text += L":";
    text += std::to_wstring(damage.targetUnitId);

    if (sample) {
        text += L" target=";
        text += FormatHex(sample->targetServerUnit);
        text += L"/";
        text += FormatHex(sample->targetClientUnit);
        text += L"\n    ";
        text += FormatCompactHitSnapshotKeys(sample);
    }
    else {
        text += L" sample=expired";
    }

    return text;
}

static std::wstring FormatHitRecorderDamageDetails(
    const HitRecorderDamage& damage,
    const HitRecorderSample& center
) {
    const HitRecorderSample* after = FindHitRecorderSample(damage.sampleId);
    if (!after) {
        std::wstring text = L"\n  Hit #";
        text += std::to_wstring(damage.sampleId);
        text += L" details unavailable: sample expired";
        return text;
    }

    const HitRecorderSample* before = FindPreviousSampleForDamage(damage);

    std::wstring text = L"\n  Hit #";
    text += std::to_wstring(damage.sampleId);
    text += L" ";
    text += FormatSampleDelta(after->capturedAt, center.capturedAt);
    text += L" dmg=";
    text += std::to_wstring(damage.amount);
    text += L" hp=";
    text += std::to_wstring(damage.oldHp);
    text += L"->";
    text += std::to_wstring(damage.newHp);
    text += L"/";
    text += std::to_wstring(damage.maxHp);
    text += L" unit=";
    text += std::to_wstring(damage.targetUnitType);
    text += L":";
    text += std::to_wstring(damage.targetUnitId);

    if (!before) {
        text += L"\n    before: unavailable";
        text += L"\n    after: #";
        text += std::to_wstring(after->id);
        text += L" hp=";
        text += std::to_wstring(after->hp);
        text += L"/";
        text += std::to_wstring(after->maxHp);
        return text;
    }

    text += L"\n    before: #";
    text += std::to_wstring(before->id);
    text += L" ";
    text += FormatSampleDelta(before->capturedAt, center.capturedAt);
    text += L" hp=";
    text += std::to_wstring(before->hp);
    text += L"/";
    text += std::to_wstring(before->maxHp);
    text += L"\n    after:  #";
    text += std::to_wstring(after->id);
    text += L" ";
    text += FormatSampleDelta(after->capturedAt, center.capturedAt);
    text += L" hp=";
    text += std::to_wstring(after->hp);
    text += L"/";
    text += std::to_wstring(after->maxHp);
    text += L"\n    target server wide diff: ";
    text += FormatSnapshotDwordDiffs(before->targetServerSnapshot, after->targetServerSnapshot, HIT_RECORDER_TARGET_DIFF_LIMIT, true);
    text += L"\n    target client wide diff: ";
    text += FormatSnapshotDwordDiffs(before->targetClientSnapshot, after->targetClientSnapshot, HIT_RECORDER_TARGET_DIFF_LIMIT, true);
    text += L"\n    player server wide diff: ";
    text += FormatSnapshotDwordDiffs(before->playerServerSnapshot, after->playerServerSnapshot, HIT_RECORDER_PLAYER_DIFF_LIMIT, true);
    text += L"\n    player client wide diff: ";
    text += FormatSnapshotDwordDiffs(before->playerClientSnapshot, after->playerClientSnapshot, HIT_RECORDER_PLAYER_DIFF_LIMIT, true);
    text += L"\n    target server extended tail diff: ";
    text += FormatSnapshotDwordDiffsFrom(
        before->targetServerExtendedSnapshot,
        after->targetServerExtendedSnapshot,
        D2_SERVER_UNIT_SNAPSHOT_BYTES,
        HIT_RECORDER_EXTENDED_DIFF_LIMIT,
        true
    );
    text += L"\n    target client extended tail diff: ";
    text += FormatSnapshotDwordDiffsFrom(
        before->targetClientExtendedSnapshot,
        after->targetClientExtendedSnapshot,
        D2_CLIENT_UNIT_SNAPSHOT_BYTES,
        HIT_RECORDER_EXTENDED_DIFF_LIMIT,
        true
    );
    text += L"\n    player server extended tail diff: ";
    text += FormatSnapshotDwordDiffsFrom(
        before->playerServerExtendedSnapshot,
        after->playerServerExtendedSnapshot,
        D2_SERVER_UNIT_SNAPSHOT_BYTES,
        HIT_RECORDER_EXTENDED_DIFF_LIMIT,
        true
    );
    text += L"\n    player client extended tail diff: ";
    text += FormatSnapshotDwordDiffsFrom(
        before->playerClientExtendedSnapshot,
        after->playerClientExtendedSnapshot,
        D2_CLIENT_UNIT_SNAPSHOT_BYTES,
        HIT_RECORDER_EXTENDED_DIFF_LIMIT,
        true
    );
    text += L"\n    target stat-list raw diff: ";
    text += FormatHex(before->targetStatsListEx);
    text += L">";
    text += FormatHex(after->targetStatsListEx);
    text += L" ";
    text += FormatSnapshotDwordDiffs(before->targetStatsListSnapshot, after->targetStatsListSnapshot, HIT_RECORDER_STATLIST_DIFF_LIMIT, false);
    text += L"\n    target base-entry raw diff: ";
    text += FormatSnapshotDwordDiffs(before->targetBaseStatEntrySnapshot, after->targetBaseStatEntrySnapshot, HIT_RECORDER_STATLIST_DIFF_LIMIT, false);
    text += L"\n    target extra-entry raw diff: ";
    text += FormatSnapshotDwordDiffs(before->targetExtraStatEntrySnapshot, after->targetExtraStatEntrySnapshot, HIT_RECORDER_STATLIST_DIFF_LIMIT, false);
    text += L"\n    player stat-list raw diff: ";
    text += FormatHex(before->playerStatsListEx);
    text += L">";
    text += FormatHex(after->playerStatsListEx);
    text += L" ";
    text += FormatSnapshotDwordDiffs(before->playerStatsListSnapshot, after->playerStatsListSnapshot, HIT_RECORDER_STATLIST_DIFF_LIMIT, false);
    text += L"\n    player base-entry raw diff: ";
    text += FormatSnapshotDwordDiffs(before->playerBaseStatEntrySnapshot, after->playerBaseStatEntrySnapshot, HIT_RECORDER_STATLIST_DIFF_LIMIT, false);
    text += L"\n    player extra-entry raw diff: ";
    text += FormatSnapshotDwordDiffs(before->playerExtraStatEntrySnapshot, after->playerExtraStatEntrySnapshot, HIT_RECORDER_STATLIST_DIFF_LIMIT, false);
    text += L"\n    target base stat diff: ";
    text += FormatHitRecorderStatDiffs(before->targetBaseStats, after->targetBaseStats, L"", HIT_RECORDER_STAT_DIFF_LIMIT);
    text += L"\n    target extra stat diff: ";
    text += FormatHitRecorderStatDiffs(before->targetExtraStats, after->targetExtraStats, L"", HIT_RECORDER_STAT_DIFF_LIMIT);
    text += L"\n    player base stat diff: ";
    text += FormatHitRecorderStatDiffs(before->playerBaseStats, after->playerBaseStats, L"", HIT_RECORDER_STAT_DIFF_LIMIT);
    text += L"\n    player extra stat diff: ";
    text += FormatHitRecorderStatDiffs(before->playerExtraStats, after->playerExtraStats, L"", HIT_RECORDER_STAT_DIFF_LIMIT);

    return text;
}

static void AppendNearbyHitRecorderDamages(
    std::wstring& text,
    const HitRecorderSample& center,
    bool includeDetails
) {
    std::vector<const HitRecorderDamage*> nearbyHits;
    for (const auto& damage : g_hitRecorderDamages) {
        if (damage.amount <= 0) continue;

        const int64_t delta = std::chrono::duration_cast<std::chrono::milliseconds>(
            GetHitRecorderDamageTime(damage) - center.capturedAt
        ).count();
        if (AbsRecorderMilliseconds(delta) > HIT_RECORDER_NEARBY_HIT_MS) continue;

        nearbyHits.push_back(&damage);
    }

    text += L"\nNearby hits (+/-";
    text += std::to_wstring(HIT_RECORDER_NEARBY_HIT_MS);
    text += L"ms): ";
    text += std::to_wstring(nearbyHits.size());

    for (const HitRecorderDamage* damage : nearbyHits) {
        text += L"\n  ";
        text += FormatHitRecorderDamageSummary(*damage, center);
    }

    if (!includeDetails || nearbyHits.empty()) return;

    text += L"\nNearby hit wide diffs:";
    size_t detailCount = 0;
    for (const HitRecorderDamage* damage : nearbyHits) {
        if (detailCount >= HIT_RECORDER_MAX_NEARBY_HIT_DETAILS) {
            text += L"\n  ...";
            break;
        }

        text += FormatHitRecorderDamageDetails(*damage, center);
        ++detailCount;
    }
}

static std::wstring BuildHitRecorderBlock(
    const wchar_t* title,
    uint64_t centerSampleId,
    const HitRecorderDamage* damage,
    bool includeNearbyHitDetails
) {
    std::wstring text = title ? title : L"Hit recorder";

    if (damage && damage->amount > 0) {
        text += L"\nDamage: ";
        text += std::to_wstring(damage->amount);
        text += L"\nHP: ";
        text += std::to_wstring(damage->oldHp);
        text += L" -> ";
        text += std::to_wstring(damage->newHp);
        text += L"/";
        text += std::to_wstring(damage->maxHp);
        text += L"\nUnit: ";
        text += std::to_wstring(damage->targetUnitType);
        text += L":";
        text += std::to_wstring(damage->targetUnitId);
    }

    const HitRecorderSample* center = FindHitRecorderSample(centerSampleId);
    if (!center && !g_hitRecorderSamples.empty()) {
        center = &g_hitRecorderSamples.back();
        centerSampleId = center->id;
    }

    text += L"\nCenter sample: #";
    text += std::to_wstring(centerSampleId);
    text += L"\nRecorder samples: ";
    text += std::to_wstring(g_hitRecorderSamples.size());
    text += L" context=+/-";
    text += std::to_wstring(HIT_RECORDER_CONTEXT_SAMPLES);
    text += L" poll=";
    text += std::to_wstring(g_config.memoryPollSeconds);
    text += L"s";

    if (!center) {
        text += L"\nNo recorder samples available";
        return text;
    }

    AppendNearbyHitRecorderDamages(text, *center, includeNearbyHitDetails);

    for (const auto& sample : g_hitRecorderSamples) {
        if (!IsSampleNear(sample.id, centerSampleId)) continue;

        text += L"\n";
        text += FormatHitRecorderSampleLine(sample, *center);
    }

    return text;
}

static void LogHitRecorderDamage(
    uint64_t sampleId,
    int damage,
    int oldHp,
    int newHp,
    int maxHp,
    uint32_t targetUnitType,
    uint32_t targetUnitId
) {
    if (!g_config.hitRecorderEnabled || sampleId == 0) return;

    HitRecorderDamage recordedDamage{};
    recordedDamage.sampleId = sampleId;
    const HitRecorderSample* sample = FindHitRecorderSample(sampleId);
    recordedDamage.capturedAt = sample ? sample->capturedAt : std::chrono::steady_clock::now();
    recordedDamage.amount = damage;
    recordedDamage.oldHp = oldHp;
    recordedDamage.newHp = newHp;
    recordedDamage.maxHp = maxHp;
    recordedDamage.targetUnitType = targetUnitType;
    recordedDamage.targetUnitId = targetUnitId;

    g_lastHitRecorderDamage = recordedDamage;
    g_hitRecorderDamages.push_back(recordedDamage);
    if (g_hitRecorderDamages.size() > HIT_RECORDER_MAX_HITS) {
        g_hitRecorderDamages.erase(g_hitRecorderDamages.begin());
    }

    AppendLogBlock(BuildHitRecorderBlock(L"Hit recorder damage", sampleId, &g_lastHitRecorderDamage, false));
}

static void WriteHitRecorderMarker() {
    if (!g_config.logEnabled) {
        ShowStatusNotice(L"Log disabled; marker not written");
        return;
    }

    if (!g_config.hitRecorderEnabled) {
        ShowStatusNotice(L"Hit recorder disabled");
        return;
    }

    if (g_hitRecorderSamples.empty()) {
        ShowStatusNotice(L"No hit recorder samples yet");
        return;
    }

    const uint64_t centerSampleId = g_hitRecorderSamples.back().id;

    AppendLogBlock(BuildHitRecorderBlock(L"Hit recorder marker (F7)", centerSampleId, nullptr, true));
    ShowStatusNotice(L"Hit marker written");
}

static bool IsScreenishCoordinate(int x, int y, int width, int height) {
    const int marginX = max(320, width / 4);
    const int marginY = max(240, height / 4);
    return x >= -marginX && x <= width + marginX &&
        y >= -marginY && y <= height + marginY;
}

static bool IsWorldishCoordinate(int x, int y) {
    return x >= 0 && x <= 100000 && y >= 0 && y <= 100000 && (x != 0 || y != 0);
}

static void AddPositionProbeCandidate(
    std::vector<PositionProbeCandidate>& candidates,
    const wchar_t* type,
    size_t offset,
    int x,
    int y,
    int cursorX,
    int cursorY,
    size_t maxCandidatePool
) {
    PositionProbeCandidate candidate{};
    candidate.type = type ? type : L"?";
    candidate.offset = offset;
    candidate.x = x;
    candidate.y = y;
    candidate.distance = AbsInt(x - cursorX) + AbsInt(y - cursorY);
    candidates.push_back(candidate);

    if (candidates.size() > maxCandidatePool) {
        std::sort(
            candidates.begin(),
            candidates.end(),
            [](const PositionProbeCandidate& a, const PositionProbeCandidate& b) {
                return a.distance < b.distance;
            }
        );
        candidates.resize(maxCandidatePool);
    }
}

static std::wstring FormatPositionProbeCandidates(
    std::vector<PositionProbeCandidate>& candidates,
    size_t maxCandidates
) {
    if (candidates.empty()) return L"none";

    std::sort(
        candidates.begin(),
        candidates.end(),
        [](const PositionProbeCandidate& a, const PositionProbeCandidate& b) {
            if (a.distance != b.distance) return a.distance < b.distance;
            return a.offset < b.offset;
        }
    );

    std::wstring text;
    const size_t count = min(maxCandidates, candidates.size());
    for (size_t i = 0; i < count; ++i) {
        const auto& candidate = candidates[i];
        if (i > 0) text += L", ";

        text += candidate.type;
        text += FormatOffset(candidate.offset);
        text += L"=(";
        text += std::to_wstring(candidate.x);
        text += L",";
        text += std::to_wstring(candidate.y);
        text += L") d=";
        text += std::to_wstring(candidate.distance);
    }

    if (candidates.size() > count) {
        text += L", ...";
    }

    return text;
}

static std::wstring FormatProbeFloat(float value) {
    if (!std::isfinite(value) || fabsf(value) > 1000000.0f) return L"?";

    wchar_t text[32]{};
    swprintf_s(text, L"%.2f", value);
    return text;
}

static std::wstring BuildPositionProbeScalarValues(const std::vector<uint8_t>& snapshot) {
    if (snapshot.empty()) return L"unavailable";

    static constexpr size_t offsets[] = {
        0x014, 0x02C, 0x030, 0x032, 0x03C,
        0x058, 0x05A, 0x05C, 0x05E, 0x060, 0x064, 0x068,
        0x0C0, 0x0C4, 0x0C8,
        0x104, 0x110, 0x128, 0x144, 0x160, 0x164,
        0x19C, 0x19E, 0x1A0, 0x1A4, 0x1AC, 0x1B8, 0x1BC, 0x1C0, 0x1C4, 0x1C8, 0x1CC,
        0x21C, 0x220, 0x224,
        0x2E8,
        0x35C, 0x360, 0x364, 0x37C,
        0x3DC, 0x3E0, 0x3E4,
        0x51C, 0x520, 0x524, 0x53C, 0x544, 0x568, 0x56C, 0x5A4,
        0x66C, 0x6DC, 0x6E4, 0x6FC, 0x704, 0x75C, 0x764
    };

    std::wstring text;
    for (size_t offset : offsets) {
        if (offset >= snapshot.size()) continue;

        text += L"\n    ";
        text += FormatOffset(offset);

        if (offset + sizeof(uint16_t) <= snapshot.size()) {
            text += L" u16=";
            text += std::to_wstring(ReadSnapshotWord(snapshot, offset));
        }

        if (offset + sizeof(uint32_t) <= snapshot.size()) {
            const uint32_t dword = ReadSnapshotDword(snapshot, offset);
            text += L" i32=";
            text += std::to_wstring(static_cast<int32_t>(dword));
            text += L" u32=";
            text += std::to_wstring(dword);
            text += L" f32=";
            text += FormatProbeFloat(ReadSnapshotFloat(snapshot, offset));
        }
    }

    return text.empty() ? L"none" : text;
}

static std::wstring BuildPositionProbePairCandidates(
    const std::vector<uint8_t>& snapshot,
    int cursorX,
    int cursorY,
    int width,
    int height,
    bool screenOnly
) {
    if (snapshot.empty()) return L"unavailable";

    std::vector<PositionProbeCandidate> candidates;
    const size_t maxCandidatePool = 96;

    for (size_t offset = 0; offset + sizeof(uint32_t) * 2 <= snapshot.size(); offset += sizeof(uint32_t)) {
        const int32_t x = static_cast<int32_t>(ReadSnapshotDword(snapshot, offset));
        const int32_t y = static_cast<int32_t>(ReadSnapshotDword(snapshot, offset + sizeof(uint32_t)));

        if (screenOnly ? IsScreenishCoordinate(x, y, width, height) : IsWorldishCoordinate(x, y)) {
            AddPositionProbeCandidate(candidates, L"i32", offset, x, y, cursorX, cursorY, maxCandidatePool);
        }

        const uint32_t ux = ReadSnapshotDword(snapshot, offset);
        const uint32_t uy = ReadSnapshotDword(snapshot, offset + sizeof(uint32_t));
        if (ux <= static_cast<uint32_t>(INT_MAX) && uy <= static_cast<uint32_t>(INT_MAX)) {
            const int unsignedX = static_cast<int>(ux);
            const int unsignedY = static_cast<int>(uy);
            if ((unsignedX != x || unsignedY != y) &&
                (screenOnly ? IsScreenishCoordinate(unsignedX, unsignedY, width, height) : IsWorldishCoordinate(unsignedX, unsignedY))) {
                AddPositionProbeCandidate(candidates, L"u32", offset, unsignedX, unsignedY, cursorX, cursorY, maxCandidatePool);
            }
        }
    }

    for (size_t offset = 0; offset + sizeof(uint16_t) * 2 <= snapshot.size(); offset += sizeof(uint16_t)) {
        const int x = static_cast<int>(ReadSnapshotWord(snapshot, offset));
        const int y = static_cast<int>(ReadSnapshotWord(snapshot, offset + sizeof(uint16_t)));
        if (screenOnly ? IsScreenishCoordinate(x, y, width, height) : IsWorldishCoordinate(x, y)) {
            AddPositionProbeCandidate(candidates, L"u16", offset, x, y, cursorX, cursorY, maxCandidatePool);
        }
    }

    for (size_t offset = 0; offset + sizeof(float) * 2 <= snapshot.size(); offset += sizeof(float)) {
        const float fx = ReadSnapshotFloat(snapshot, offset);
        const float fy = ReadSnapshotFloat(snapshot, offset + sizeof(float));
        if (!std::isfinite(fx) || !std::isfinite(fy)) continue;
        if (fx < static_cast<float>(INT_MIN) || fx > static_cast<float>(INT_MAX) ||
            fy < static_cast<float>(INT_MIN) || fy > static_cast<float>(INT_MAX)) {
            continue;
        }

        const int x = static_cast<int>(fx);
        const int y = static_cast<int>(fy);
        if (screenOnly ? IsScreenishCoordinate(x, y, width, height) : IsWorldishCoordinate(x, y)) {
            AddPositionProbeCandidate(candidates, L"f32", offset, x, y, cursorX, cursorY, maxCandidatePool);
        }
    }

    return FormatPositionProbeCandidates(candidates, 24);
}

static std::wstring BuildPositionProbePointerList(const std::vector<uint8_t>& snapshot) {
    if (snapshot.empty()) return L"unavailable";

    std::wstring text;
    size_t count = 0;
    for (size_t offset = 0; offset + sizeof(uint64_t) <= snapshot.size(); offset += sizeof(uint64_t)) {
        const uintptr_t value = static_cast<uintptr_t>(ReadSnapshotQword(snapshot, offset));
        if (!IsLikelyRemotePointer(value)) continue;

        if (count > 0) text += L", ";
        text += FormatOffset(offset);
        text += L"=";
        text += FormatHex(value);

        ++count;
        if (count >= 24) {
            text += L", ...";
            break;
        }
    }

    return count == 0 ? L"none" : text;
}

static std::wstring BuildPositionProbeChildCandidates(
    const std::vector<uint8_t>& snapshot,
    int cursorX,
    int cursorY,
    int width,
    int height
) {
    if (snapshot.empty()) return L"unavailable";

    std::wstring text;
    size_t childCount = 0;
    for (size_t offset = 0; offset + sizeof(uint64_t) <= snapshot.size(); offset += sizeof(uint64_t)) {
        const uintptr_t pointer = static_cast<uintptr_t>(ReadSnapshotQword(snapshot, offset));
        if (!IsLikelyRemotePointer(pointer)) continue;

        std::vector<uint8_t> childSnapshot;
        if (!ReadRemoteBytesBestEffort(pointer, 0x300, childSnapshot)) continue;

        const std::wstring candidates = BuildPositionProbePairCandidates(
            childSnapshot,
            cursorX,
            cursorY,
            width,
            height,
            true
        );
        if (candidates == L"none" || candidates == L"unavailable") continue;

        if (childCount > 0) text += L"\n    ";
        text += FormatOffset(offset);
        text += L" -> ";
        text += FormatHex(pointer);
        text += L": ";
        text += candidates;

        ++childCount;
        if (childCount >= 12) {
            text += L"\n    ...";
            break;
        }
    }

    return childCount == 0 ? L"none" : text;
}

static bool TryReadUnitWorldPosition(uintptr_t unitAddress, WorldPosition& position) {
    if (!IsLikelyRemotePointer(unitAddress)) return false;

    uintptr_t pathAddress = 0;
    if (!ReadRemoteValue(unitAddress + D2_UNIT_OFFSET_PATH, pathAddress) ||
        !IsLikelyRemotePointer(pathAddress)) {
        return false;
    }

    int32_t rawX = 0;
    int32_t rawY = 0;
    if (!ReadRemoteValue(pathAddress + D2_PATH_OFFSET_X, rawX) ||
        !ReadRemoteValue(pathAddress + D2_PATH_OFFSET_Y, rawY)) {
        return false;
    }

    const float x = static_cast<float>(rawX) / 65536.0f;
    const float y = static_cast<float>(rawY) / 65536.0f;
    if (x < -1000.0f || y < -1000.0f || x > 100000.0f || y > 100000.0f) {
        return false;
    }

    position.x = x;
    position.y = y;
    return true;
}

static bool TryReadPlayerWorldPosition(uintptr_t currentGame, WorldPosition& position) {
    if (!IsLikelyRemotePointer(currentGame)) return false;

    uintptr_t playerUnit = 0;
    if (!FindFirstServerUnitByType(currentGame, 0, playerUnit)) {
        return false;
    }

    uint32_t playerUnitId = UINT32_MAX;
    uintptr_t playerClientUnit = 0;
    if (ReadRemoteValue(playerUnit + D2_UNIT_OFFSET_ID, playerUnitId) &&
        playerUnitId != UINT32_MAX &&
        FindUnitByTypeAndId(0, playerUnitId, playerClientUnit) &&
        TryReadUnitWorldPosition(playerClientUnit, position)) {
        return true;
    }

    return TryReadUnitWorldPosition(playerUnit, position);
}

static bool TryResolveTargetWorldPosition(
    uintptr_t currentGame,
    uint32_t targetUnitType,
    uint32_t targetUnitId,
    uintptr_t targetServerUnit,
    uintptr_t targetClientUnit,
    WorldPosition& position
) {
    if (TryReadUnitWorldPosition(targetClientUnit, position)) return true;

    uintptr_t clientUnit = 0;
    if (targetUnitType != UINT32_MAX &&
        targetUnitId != UINT32_MAX &&
        FindUnitByTypeAndId(targetUnitType, targetUnitId, clientUnit) &&
        TryReadUnitWorldPosition(clientUnit, position)) {
        return true;
    }

    if (TryReadUnitWorldPosition(targetServerUnit, position)) return true;

    uintptr_t serverUnit = 0;
    if (IsLikelyRemotePointer(currentGame) &&
        targetUnitType != UINT32_MAX &&
        targetUnitId != UINT32_MAX &&
        FindServerUnitByTypeAndId(currentGame, targetUnitType, targetUnitId, serverUnit) &&
        TryReadUnitWorldPosition(serverUnit, position)) {
        return true;
    }

    return false;
}

static bool SolveProjectionAxis(
    const std::vector<WorldProjectionSample>& samples,
    bool solveScreenX,
    float& a,
    float& b,
    float& c
) {
    double matrix[3][4]{};

    for (const auto& sample : samples) {
        const double x0 = sample.dx;
        const double x1 = sample.dy;
        const double x2 = 1.0;
        const double y = solveScreenX ? sample.screenX : sample.screenY;

        matrix[0][0] += x0 * x0;
        matrix[0][1] += x0 * x1;
        matrix[0][2] += x0 * x2;
        matrix[0][3] += x0 * y;
        matrix[1][0] += x1 * x0;
        matrix[1][1] += x1 * x1;
        matrix[1][2] += x1 * x2;
        matrix[1][3] += x1 * y;
        matrix[2][0] += x2 * x0;
        matrix[2][1] += x2 * x1;
        matrix[2][2] += x2 * x2;
        matrix[2][3] += x2 * y;
    }

    for (int pivot = 0; pivot < 3; ++pivot) {
        int bestRow = pivot;
        double bestValue = std::abs(matrix[pivot][pivot]);
        for (int row = pivot + 1; row < 3; ++row) {
            const double value = std::abs(matrix[row][pivot]);
            if (value > bestValue) {
                bestValue = value;
                bestRow = row;
            }
        }

        if (bestValue < 0.000001) return false;

        if (bestRow != pivot) {
            for (int col = pivot; col < 4; ++col) {
                std::swap(matrix[pivot][col], matrix[bestRow][col]);
            }
        }

        const double divisor = matrix[pivot][pivot];
        for (int col = pivot; col < 4; ++col) {
            matrix[pivot][col] /= divisor;
        }

        for (int row = 0; row < 3; ++row) {
            if (row == pivot) continue;

            const double factor = matrix[row][pivot];
            for (int col = pivot; col < 4; ++col) {
                matrix[row][col] -= factor * matrix[pivot][col];
            }
        }
    }

    a = static_cast<float>(matrix[0][3]);
    b = static_cast<float>(matrix[1][3]);
    c = static_cast<float>(matrix[2][3]);
    return true;
}

static bool FitWorldProjectionModel() {
    if (g_worldProjectionSamples.size() < static_cast<size_t>(max(3, g_config.worldProjectionMinSamples))) {
        g_worldProjectionModel.ready = false;
        return false;
    }

    float minDx = g_worldProjectionSamples.front().dx;
    float maxDx = g_worldProjectionSamples.front().dx;
    float minDy = g_worldProjectionSamples.front().dy;
    float maxDy = g_worldProjectionSamples.front().dy;
    for (const auto& sample : g_worldProjectionSamples) {
        minDx = min(minDx, sample.dx);
        maxDx = max(maxDx, sample.dx);
        minDy = min(minDy, sample.dy);
        maxDy = max(maxDy, sample.dy);
    }

    if ((maxDx - minDx) + (maxDy - minDy) < 2.0f) {
        g_worldProjectionModel.ready = false;
        return false;
    }

    WorldProjectionModel model{};
    if (!SolveProjectionAxis(g_worldProjectionSamples, true, model.ax, model.bx, model.cx) ||
        !SolveProjectionAxis(g_worldProjectionSamples, false, model.ay, model.by, model.cy)) {
        g_worldProjectionModel.ready = false;
        return false;
    }

    double squaredError = 0.0;
    for (const auto& sample : g_worldProjectionSamples) {
        const float projectedX = (model.ax * sample.dx) + (model.bx * sample.dy) + model.cx;
        const float projectedY = (model.ay * sample.dx) + (model.by * sample.dy) + model.cy;
        const float errorX = projectedX - sample.screenX;
        const float errorY = projectedY - sample.screenY;
        squaredError += (errorX * errorX) + (errorY * errorY);
    }

    model.ready = true;
    model.sampleCount = g_worldProjectionSamples.size();
    const size_t denominator = g_worldProjectionSamples.empty() ? 1 : g_worldProjectionSamples.size();
    model.rmsError = static_cast<float>(std::sqrt(squaredError / static_cast<double>(denominator)));
    g_worldProjectionModel = model;
    return true;
}

static void AddWorldProjectionSample(float dx, float dy, float screenX, float screenY) {
    WorldProjectionSample sample{};
    sample.dx = dx;
    sample.dy = dy;
    sample.screenX = screenX;
    sample.screenY = screenY;
    sample.capturedAt = std::chrono::steady_clock::now();

    if (!g_worldProjectionSamples.empty()) {
        const auto& last = g_worldProjectionSamples.back();
        const float screenDistance = std::abs(last.screenX - sample.screenX) + std::abs(last.screenY - sample.screenY);
        const float worldDistance = std::abs(last.dx - sample.dx) + std::abs(last.dy - sample.dy);
        const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(sample.capturedAt - last.capturedAt).count();
        if (elapsedMs < 120 && screenDistance < 24.0f && worldDistance < 0.30f) {
            return;
        }
    }

    for (auto& existing : g_worldProjectionSamples) {
        const float screenDistance = std::abs(existing.screenX - sample.screenX) + std::abs(existing.screenY - sample.screenY);
        const float worldDistance = std::abs(existing.dx - sample.dx) + std::abs(existing.dy - sample.dy);
        if (screenDistance < 10.0f && worldDistance < 0.15f) {
            existing = sample;
            FitWorldProjectionModel();
            return;
        }
    }

    g_worldProjectionSamples.push_back(sample);
    constexpr size_t maxSamples = 80;
    if (g_worldProjectionSamples.size() > maxSamples) {
        g_worldProjectionSamples.erase(g_worldProjectionSamples.begin(), g_worldProjectionSamples.begin() + (g_worldProjectionSamples.size() - maxSamples));
    }

    FitWorldProjectionModel();
}

static bool ProjectWorldPositionRelativeToPlayer(
    const WorldPosition& targetPosition,
    const WorldPosition& playerPosition,
    bool useCalibration,
    float& screenX,
    float& screenY
) {
    if (!g_hasGameRect) return false;

    const float width = static_cast<float>(max(0, g_gameRect.right - g_gameRect.left));
    const float height = static_cast<float>(max(0, g_gameRect.bottom - g_gameRect.top));
    if (width <= 0.0f || height <= 0.0f) return false;

    const float dx = targetPosition.x - playerPosition.x;
    const float dy = targetPosition.y - playerPosition.y;

    if (useCalibration && g_config.learnWorldProjection && g_worldProjectionModel.ready) {
        screenX = (g_worldProjectionModel.ax * dx) + (g_worldProjectionModel.bx * dy) + g_worldProjectionModel.cx;
        screenY = (g_worldProjectionModel.ay * dx) + (g_worldProjectionModel.by * dy) + g_worldProjectionModel.cy;
    }
    else {
        screenX = (width * g_config.worldAnchorX) + ((dx - dy) * g_config.worldTileWidth);
        screenY = (height * g_config.worldAnchorY) + ((dx + dy) * g_config.worldTileHeight);

        if (useCalibration && g_worldProjectionCalibrated) {
            screenX += g_worldProjectionBiasX;
            screenY += g_worldProjectionBiasY;
        }
    }

    const float margin = max(width, height) * 0.35f;
    return screenX >= -margin &&
        screenY >= -margin &&
        screenX <= width + margin &&
        screenY <= height + margin;
}

static float GetScreenDistance(float ax, float ay, float bx, float by) {
    const float dx = ax - bx;
    const float dy = ay - by;
    return std::sqrt((dx * dx) + (dy * dy));
}

static bool IsProjectionCloseToReference(float screenX, float screenY, float referenceX, float referenceY) {
    const float width = static_cast<float>(max(0, g_gameRect.right - g_gameRect.left));
    const float height = static_cast<float>(max(0, g_gameRect.bottom - g_gameRect.top));
    const float maxDimension = max(width, height);
    const float threshold = max(180.0f, max(maxDimension * 0.16f, g_config.worldCoordMaxRmsError * 3.0f));
    return GetScreenDistance(screenX, screenY, referenceX, referenceY) <= threshold;
}

static bool IsLargeProjectionOutlier(const DamageNumber& number, float projectedX, float projectedY) {
    if (number.age < 0.06f || !g_hasGameRect) return false;

    const float width = static_cast<float>(max(0, g_gameRect.right - g_gameRect.left));
    const float height = static_cast<float>(max(0, g_gameRect.bottom - g_gameRect.top));
    const float maxDimension = max(width, height);
    const float outlierDistance = max(g_config.followSnapDistance * 2.0f, maxDimension * 0.24f);
    return GetScreenDistance(projectedX, projectedY, number.anchorX, number.anchorY) > outlierDistance;
}

static bool TryProjectUnitWithPathWorldPosition(
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
) {
    if (hasTargetWorldPosition) {
        *hasTargetWorldPosition = false;
    }
    if (!g_config.useWorldPositions) return false;

    WorldPosition targetPosition;
    if (!TryResolveTargetWorldPosition(
            currentGame,
            targetUnitType,
            targetUnitId,
            targetServerUnit,
            targetClientUnit,
            targetPosition
        )) {
        return false;
    }

    if (targetWorldPosition) {
        *targetWorldPosition = targetPosition;
    }
    if (hasTargetWorldPosition) {
        *hasTargetWorldPosition = true;
    }

    return ProjectWorldPositionRelativeToPlayer(targetPosition, playerPosition, true, screenX, screenY);
}

static bool ScreenAnchorDescriptorEquals(
    const ScreenAnchorDescriptor& a,
    const ScreenAnchorDescriptor& b
) {
    return a.sourceKind == b.sourceKind &&
        a.valueKind == b.valueKind &&
        a.pointerOffset == b.pointerOffset &&
        a.valueOffset == b.valueOffset;
}

static uint64_t MakeUnitKey(uint32_t unitType, uint32_t unitId) {
    return (static_cast<uint64_t>(unitType) << 32) | static_cast<uint64_t>(unitId);
}

static bool AddUniqueUnitKey(std::vector<uint64_t>& unitKeys, uint64_t unitKey) {
    if (unitKey == 0 || unitKey == UINT64_MAX) return false;
    if (std::find(unitKeys.begin(), unitKeys.end(), unitKey) != unitKeys.end()) return false;
    unitKeys.push_back(unitKey);
    return true;
}

static bool HasUnitKey(const std::vector<uint64_t>& unitKeys, uint64_t unitKey) {
    return std::find(unitKeys.begin(), unitKeys.end(), unitKey) != unitKeys.end();
}

static bool WorldCoordDescriptorEquals(
    const WorldCoordDescriptor& a,
    const WorldCoordDescriptor& b
) {
    return a.sourceKind == b.sourceKind &&
        a.valueKind == b.valueKind &&
        a.pointerOffset == b.pointerOffset &&
        a.valueOffset == b.valueOffset;
}

static std::wstring FormatWorldCoordDescriptor(const WorldCoordDescriptor& descriptor) {
    std::wstring text;
    switch (descriptor.sourceKind) {
    case WorldCoordSourceKind::ClientUnit:
        text += L"CU";
        break;
    case WorldCoordSourceKind::ServerUnit:
        text += L"SU";
        break;
    case WorldCoordSourceKind::ClientChild:
        text += L"CU*";
        text += FormatOffset(descriptor.pointerOffset);
        break;
    case WorldCoordSourceKind::ServerChild:
        text += L"SU*";
        text += FormatOffset(descriptor.pointerOffset);
        break;
    default:
        text += L"?";
        break;
    }

    text += L" ";
    switch (descriptor.valueKind) {
    case WorldCoordValueKind::Int32Pair:
        text += L"i32";
        break;
    case WorldCoordValueKind::UInt32Pair:
        text += L"u32";
        break;
    case WorldCoordValueKind::UInt16Pair:
        text += L"u16";
        break;
    case WorldCoordValueKind::FloatPair:
        text += L"f32";
        break;
    default:
        text += L"?";
        break;
    }
    text += FormatOffset(descriptor.valueOffset);
    return text;
}

static float NormalizeWorldCoordInteger(int64_t value) {
    const int64_t absValue = value < 0 ? -value : value;
    if (absValue > 1000000) {
        return static_cast<float>(static_cast<double>(value) / 65536.0);
    }
    return static_cast<float>(value);
}

static bool IsPlausibleWorldCoordComponent(float value) {
    return std::isfinite(value) && value >= -200000.0f && value <= 200000.0f;
}

static bool IsPlausibleWorldCoordPosition(const WorldPosition& position) {
    return IsPlausibleWorldCoordComponent(position.x) &&
        IsPlausibleWorldCoordComponent(position.y) &&
        (std::abs(position.x) + std::abs(position.y)) > 0.001f;
}

static bool TryReadWorldCoordDescriptor(
    const WorldCoordDescriptor& descriptor,
    uintptr_t serverUnit,
    uintptr_t clientUnit,
    WorldPosition& position
) {
    uintptr_t sourceAddress = 0;
    switch (descriptor.sourceKind) {
    case WorldCoordSourceKind::ClientUnit:
        sourceAddress = clientUnit;
        break;
    case WorldCoordSourceKind::ServerUnit:
        sourceAddress = serverUnit;
        break;
    case WorldCoordSourceKind::ClientChild:
        if (!IsLikelyRemotePointer(clientUnit) ||
            !ReadRemoteValue(clientUnit + descriptor.pointerOffset, sourceAddress)) {
            return false;
        }
        break;
    case WorldCoordSourceKind::ServerChild:
        if (!IsLikelyRemotePointer(serverUnit) ||
            !ReadRemoteValue(serverUnit + descriptor.pointerOffset, sourceAddress)) {
            return false;
        }
        break;
    default:
        return false;
    }

    if (!IsLikelyRemotePointer(sourceAddress)) return false;

    switch (descriptor.valueKind) {
    case WorldCoordValueKind::Int32Pair: {
        int32_t x = 0;
        int32_t y = 0;
        if (!ReadRemoteValue(sourceAddress + descriptor.valueOffset, x) ||
            !ReadRemoteValue(sourceAddress + descriptor.valueOffset + sizeof(int32_t), y)) {
            return false;
        }
        position.x = NormalizeWorldCoordInteger(x);
        position.y = NormalizeWorldCoordInteger(y);
        break;
    }
    case WorldCoordValueKind::UInt32Pair: {
        uint32_t x = 0;
        uint32_t y = 0;
        if (!ReadRemoteValue(sourceAddress + descriptor.valueOffset, x) ||
            !ReadRemoteValue(sourceAddress + descriptor.valueOffset + sizeof(uint32_t), y) ||
            x > 0x7FFFFFFFu ||
            y > 0x7FFFFFFFu) {
            return false;
        }
        position.x = NormalizeWorldCoordInteger(static_cast<int64_t>(x));
        position.y = NormalizeWorldCoordInteger(static_cast<int64_t>(y));
        break;
    }
    case WorldCoordValueKind::UInt16Pair: {
        uint16_t x = 0;
        uint16_t y = 0;
        if (!ReadRemoteValue(sourceAddress + descriptor.valueOffset, x) ||
            !ReadRemoteValue(sourceAddress + descriptor.valueOffset + sizeof(uint16_t), y)) {
            return false;
        }
        position.x = static_cast<float>(x);
        position.y = static_cast<float>(y);
        break;
    }
    case WorldCoordValueKind::FloatPair: {
        float x = 0.0f;
        float y = 0.0f;
        if (!ReadRemoteValue(sourceAddress + descriptor.valueOffset, x) ||
            !ReadRemoteValue(sourceAddress + descriptor.valueOffset + sizeof(float), y)) {
            return false;
        }
        position.x = x;
        position.y = y;
        break;
    }
    default:
        return false;
    }

    return IsPlausibleWorldCoordPosition(position);
}

static bool ReadSnapshotWorldCoordPair(
    const std::vector<uint8_t>& snapshot,
    size_t offset,
    WorldCoordValueKind valueKind,
    WorldPosition& position
) {
    switch (valueKind) {
    case WorldCoordValueKind::Int32Pair:
        if (offset + sizeof(uint32_t) * 2 > snapshot.size()) return false;
        position.x = NormalizeWorldCoordInteger(static_cast<int32_t>(ReadSnapshotDword(snapshot, offset)));
        position.y = NormalizeWorldCoordInteger(static_cast<int32_t>(ReadSnapshotDword(snapshot, offset + sizeof(uint32_t))));
        break;
    case WorldCoordValueKind::UInt32Pair: {
        if (offset + sizeof(uint32_t) * 2 > snapshot.size()) return false;
        const uint32_t x = ReadSnapshotDword(snapshot, offset);
        const uint32_t y = ReadSnapshotDword(snapshot, offset + sizeof(uint32_t));
        if (x > 0x7FFFFFFFu || y > 0x7FFFFFFFu) return false;
        position.x = NormalizeWorldCoordInteger(static_cast<int64_t>(x));
        position.y = NormalizeWorldCoordInteger(static_cast<int64_t>(y));
        break;
    }
    case WorldCoordValueKind::UInt16Pair:
        if (offset + sizeof(uint16_t) * 2 > snapshot.size()) return false;
        position.x = static_cast<float>(ReadSnapshotWord(snapshot, offset));
        position.y = static_cast<float>(ReadSnapshotWord(snapshot, offset + sizeof(uint16_t)));
        break;
    case WorldCoordValueKind::FloatPair:
        if (offset + sizeof(float) * 2 > snapshot.size()) return false;
        position.x = ReadSnapshotFloat(snapshot, offset);
        position.y = ReadSnapshotFloat(snapshot, offset + sizeof(float));
        break;
    default:
        return false;
    }

    return IsPlausibleWorldCoordPosition(position);
}

static void AddWorldCoordSample(
    std::vector<WorldCoordSample>& samples,
    const WorldCoordDescriptor& descriptor,
    const WorldPosition& targetPosition,
    const WorldPosition& playerPosition,
    float cursorX,
    float cursorY
) {
    if (!IsPlausibleWorldCoordPosition(targetPosition) ||
        !IsPlausibleWorldCoordPosition(playerPosition)) {
        return;
    }

    WorldCoordSample sample{};
    sample.descriptor = descriptor;
    sample.dx = targetPosition.x - playerPosition.x;
    sample.dy = targetPosition.y - playerPosition.y;
    sample.screenX = cursorX;
    sample.screenY = cursorY;

    if (!std::isfinite(sample.dx) || !std::isfinite(sample.dy) ||
        (std::abs(sample.dx) + std::abs(sample.dy)) < 0.001f ||
        std::abs(sample.dx) > 200000.0f ||
        std::abs(sample.dy) > 200000.0f) {
        return;
    }

    for (auto& existing : samples) {
        if (WorldCoordDescriptorEquals(existing.descriptor, descriptor)) {
            existing = sample;
            return;
        }
    }

    samples.push_back(sample);
}

static void CollectWorldCoordSamplesFromSnapshots(
    std::vector<WorldCoordSample>& samples,
    const std::vector<uint8_t>& targetSnapshot,
    const std::vector<uint8_t>& playerSnapshot,
    WorldCoordSourceKind sourceKind,
    size_t pointerOffset,
    float cursorX,
    float cursorY
) {
    if (targetSnapshot.empty() || playerSnapshot.empty()) return;

    const size_t size = min(targetSnapshot.size(), playerSnapshot.size());
    WorldPosition targetPosition;
    WorldPosition playerPosition;

    for (size_t offset = 0; offset + sizeof(uint32_t) * 2 <= size; offset += sizeof(uint32_t)) {
        WorldCoordDescriptor descriptor{};
        descriptor.sourceKind = sourceKind;
        descriptor.pointerOffset = pointerOffset;
        descriptor.valueOffset = offset;

        descriptor.valueKind = WorldCoordValueKind::Int32Pair;
        if (ReadSnapshotWorldCoordPair(targetSnapshot, offset, descriptor.valueKind, targetPosition) &&
            ReadSnapshotWorldCoordPair(playerSnapshot, offset, descriptor.valueKind, playerPosition)) {
            AddWorldCoordSample(samples, descriptor, targetPosition, playerPosition, cursorX, cursorY);
        }

        descriptor.valueKind = WorldCoordValueKind::UInt32Pair;
        if (ReadSnapshotWorldCoordPair(targetSnapshot, offset, descriptor.valueKind, targetPosition) &&
            ReadSnapshotWorldCoordPair(playerSnapshot, offset, descriptor.valueKind, playerPosition)) {
            AddWorldCoordSample(samples, descriptor, targetPosition, playerPosition, cursorX, cursorY);
        }

        descriptor.valueKind = WorldCoordValueKind::FloatPair;
        if (ReadSnapshotWorldCoordPair(targetSnapshot, offset, descriptor.valueKind, targetPosition) &&
            ReadSnapshotWorldCoordPair(playerSnapshot, offset, descriptor.valueKind, playerPosition)) {
            AddWorldCoordSample(samples, descriptor, targetPosition, playerPosition, cursorX, cursorY);
        }
    }

    for (size_t offset = 0; offset + sizeof(uint16_t) * 2 <= size; offset += sizeof(uint16_t)) {
        WorldCoordDescriptor descriptor{};
        descriptor.sourceKind = sourceKind;
        descriptor.valueKind = WorldCoordValueKind::UInt16Pair;
        descriptor.pointerOffset = pointerOffset;
        descriptor.valueOffset = offset;

        if (ReadSnapshotWorldCoordPair(targetSnapshot, offset, descriptor.valueKind, targetPosition) &&
            ReadSnapshotWorldCoordPair(playerSnapshot, offset, descriptor.valueKind, playerPosition)) {
            AddWorldCoordSample(samples, descriptor, targetPosition, playerPosition, cursorX, cursorY);
        }
    }
}

static void CollectWorldCoordSamplesFromChildPointers(
    std::vector<WorldCoordSample>& samples,
    const std::vector<uint8_t>& targetSnapshot,
    const std::vector<uint8_t>& playerSnapshot,
    WorldCoordSourceKind childSourceKind,
    float cursorX,
    float cursorY
) {
    if (targetSnapshot.empty() || playerSnapshot.empty()) return;

    size_t childReads = 0;
    constexpr size_t maxChildReads = 18;
    const size_t size = min(targetSnapshot.size(), playerSnapshot.size());
    for (size_t pointerOffset = 0; pointerOffset + sizeof(uint64_t) <= size; pointerOffset += sizeof(uint64_t)) {
        const uintptr_t targetPointer = static_cast<uintptr_t>(ReadSnapshotQword(targetSnapshot, pointerOffset));
        const uintptr_t playerPointer = static_cast<uintptr_t>(ReadSnapshotQword(playerSnapshot, pointerOffset));
        if (!IsLikelyRemotePointer(targetPointer) || !IsLikelyRemotePointer(playerPointer)) continue;

        std::vector<uint8_t> targetChildSnapshot;
        std::vector<uint8_t> playerChildSnapshot;
        if (!ReadRemoteBytesBestEffort(targetPointer, 0x500, targetChildSnapshot) ||
            !ReadRemoteBytesBestEffort(playerPointer, 0x500, playerChildSnapshot)) {
            continue;
        }

        CollectWorldCoordSamplesFromSnapshots(
            samples,
            targetChildSnapshot,
            playerChildSnapshot,
            childSourceKind,
            pointerOffset,
            cursorX,
            cursorY
        );

        if (++childReads >= maxChildReads) break;
    }
}

static float GetWorldCoordSampleSpan(const std::vector<WorldProjectionSample>& samples) {
    if (samples.empty()) return 0.0f;

    float minDx = samples.front().dx;
    float maxDx = samples.front().dx;
    float minDy = samples.front().dy;
    float maxDy = samples.front().dy;
    for (const auto& sample : samples) {
        minDx = min(minDx, sample.dx);
        maxDx = max(maxDx, sample.dx);
        minDy = min(minDy, sample.dy);
        maxDy = max(maxDy, sample.dy);
    }

    return (maxDx - minDx) + (maxDy - minDy);
}

static bool FitWorldCoordCandidate(WorldCoordCandidate& candidate) {
    if (candidate.samples.size() < static_cast<size_t>(max(3, g_config.worldProjectionMinSamples))) {
        candidate.model = WorldProjectionModel{};
        return false;
    }

    if (GetWorldCoordSampleSpan(candidate.samples) < g_config.worldCoordMinMovementSpan) {
        candidate.model = WorldProjectionModel{};
        return false;
    }

    WorldProjectionModel model{};
    if (!SolveProjectionAxis(candidate.samples, true, model.ax, model.bx, model.cx) ||
        !SolveProjectionAxis(candidate.samples, false, model.ay, model.by, model.cy)) {
        candidate.model = WorldProjectionModel{};
        return false;
    }

    double squaredError = 0.0;
    for (const auto& sample : candidate.samples) {
        const float projectedX = (model.ax * sample.dx) + (model.bx * sample.dy) + model.cx;
        const float projectedY = (model.ay * sample.dx) + (model.by * sample.dy) + model.cy;
        const float errorX = projectedX - sample.screenX;
        const float errorY = projectedY - sample.screenY;
        squaredError += (errorX * errorX) + (errorY * errorY);
    }

    model.ready = true;
    model.sampleCount = candidate.samples.size();
    model.rmsError = static_cast<float>(std::sqrt(squaredError / static_cast<double>(candidate.samples.size())));
    candidate.model = model;
    candidate.minDx = candidate.samples.front().dx;
    candidate.maxDx = candidate.samples.front().dx;
    candidate.minDy = candidate.samples.front().dy;
    candidate.maxDy = candidate.samples.front().dy;
    for (const auto& sample : candidate.samples) {
        candidate.minDx = min(candidate.minDx, sample.dx);
        candidate.maxDx = max(candidate.maxDx, sample.dx);
        candidate.minDy = min(candidate.minDy, sample.dy);
        candidate.maxDy = max(candidate.maxDy, sample.dy);
    }
    return true;
}

static float ScoreWorldCoordCandidate(const WorldCoordCandidate& candidate) {
    const float rms = candidate.model.ready ? candidate.model.rmsError : 10000.0f;
    const float span = (candidate.maxDx - candidate.minDx) + (candidate.maxDy - candidate.minDy);
    float descriptorPenalty = 0.0f;

    if ((candidate.descriptor.sourceKind == WorldCoordSourceKind::ClientChild ||
            candidate.descriptor.sourceKind == WorldCoordSourceKind::ServerChild) &&
        candidate.descriptor.pointerOffset == D2_UNIT_OFFSET_PATH) {
        if (candidate.descriptor.valueOffset == 0x004 ||
            candidate.descriptor.valueOffset == 0x008) {
            descriptorPenalty -= 6.0f;
        }
        else if (candidate.descriptor.valueOffset >= 0x080) {
            descriptorPenalty += 8.0f;
        }
    }
    else {
        descriptorPenalty += 4.0f;
    }

    if (candidate.descriptor.valueKind == WorldCoordValueKind::UInt16Pair ||
        candidate.descriptor.valueKind == WorldCoordValueKind::FloatPair) {
        descriptorPenalty += 2.0f;
    }

    return rms -
        min(8.0f, static_cast<float>(candidate.unitKeys.size())) * 2.0f -
        min(80.0f, span) * 0.02f +
        descriptorPenalty;
}

static void ObserveWorldCoordSample(const WorldCoordSample& input, uint64_t unitKey) {
    const auto now = std::chrono::steady_clock::now();
    WorldProjectionSample sample{};
    sample.dx = input.dx;
    sample.dy = input.dy;
    sample.screenX = input.screenX;
    sample.screenY = input.screenY;
    sample.capturedAt = now;

    for (auto& candidate : g_worldCoordCandidates) {
        if (!WorldCoordDescriptorEquals(candidate.descriptor, input.descriptor)) continue;

        if (!candidate.samples.empty()) {
            const auto& last = candidate.samples.back();
            const float screenDistance = std::abs(last.screenX - sample.screenX) + std::abs(last.screenY - sample.screenY);
            const float worldDistance = std::abs(last.dx - sample.dx) + std::abs(last.dy - sample.dy);
            const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(sample.capturedAt - last.capturedAt).count();
            if (elapsedMs < 120 && screenDistance < 24.0f && worldDistance < 0.30f) {
                return;
            }
        }

        for (auto& existing : candidate.samples) {
            const float screenDistance = std::abs(existing.screenX - sample.screenX) + std::abs(existing.screenY - sample.screenY);
            const float worldDistance = std::abs(existing.dx - sample.dx) + std::abs(existing.dy - sample.dy);
            if (worldDistance < 0.50f) {
                if (screenDistance < 36.0f) {
                    existing.screenX = (existing.screenX * 0.85f) + (sample.screenX * 0.15f);
                    existing.screenY = (existing.screenY * 0.85f) + (sample.screenY * 0.15f);
                    existing.capturedAt = sample.capturedAt;
                    AddUniqueUnitKey(candidate.unitKeys, unitKey);
                    candidate.lastSeen = now;
                    FitWorldCoordCandidate(candidate);
                }
                return;
            }
            if (screenDistance < 10.0f && worldDistance < 0.15f) {
                existing = sample;
                AddUniqueUnitKey(candidate.unitKeys, unitKey);
                candidate.lastSeen = now;
                FitWorldCoordCandidate(candidate);
                return;
            }
        }

        candidate.samples.push_back(sample);
        constexpr size_t maxSamples = 96;
        if (candidate.samples.size() > maxSamples) {
            candidate.samples.erase(candidate.samples.begin(), candidate.samples.begin() + (candidate.samples.size() - maxSamples));
        }
        AddUniqueUnitKey(candidate.unitKeys, unitKey);
        candidate.lastSeen = now;
        FitWorldCoordCandidate(candidate);
        return;
    }

    WorldCoordCandidate candidate{};
    candidate.descriptor = input.descriptor;
    candidate.samples.push_back(sample);
    AddUniqueUnitKey(candidate.unitKeys, unitKey);
    candidate.minDx = sample.dx;
    candidate.maxDx = sample.dx;
    candidate.minDy = sample.dy;
    candidate.maxDy = sample.dy;
    candidate.lastSeen = now;
    FitWorldCoordCandidate(candidate);
    g_worldCoordCandidates.push_back(candidate);
}

static void PruneWorldCoordCandidates() {
    constexpr size_t maxCandidates = 2600;
    if (g_worldCoordCandidates.size() <= maxCandidates) return;

    std::sort(
        g_worldCoordCandidates.begin(),
        g_worldCoordCandidates.end(),
        [](const WorldCoordCandidate& a, const WorldCoordCandidate& b) {
            const float aScore = ScoreWorldCoordCandidate(a);
            const float bScore = ScoreWorldCoordCandidate(b);
            if (fabsf(aScore - bScore) > 0.01f) return aScore < bScore;
            if (a.unitKeys.size() != b.unitKeys.size()) return a.unitKeys.size() > b.unitKeys.size();
            if (a.samples.size() != b.samples.size()) return a.samples.size() > b.samples.size();
            return FormatWorldCoordDescriptor(a.descriptor) < FormatWorldCoordDescriptor(b.descriptor);
        }
    );
    g_worldCoordCandidates.resize(maxCandidates);
}

static void RefreshWorldCoordModel() {
    WorldCoordModel best{};
    WorldCoordModel sticky{};
    bool found = false;
    bool stickyFound = false;
    float bestScore = 0.0f;
    float stickyScore = 0.0f;

    for (const auto& candidate : g_worldCoordCandidates) {
        if (!candidate.model.ready) continue;
        if (candidate.model.sampleCount < static_cast<size_t>(max(3, g_config.worldProjectionMinSamples))) continue;
        if (candidate.unitKeys.size() < static_cast<size_t>(max(1, g_config.worldCoordMinDistinctUnits))) continue;
        if (candidate.model.rmsError > g_config.worldCoordMaxRmsError) continue;

        const float span = (candidate.maxDx - candidate.minDx) + (candidate.maxDy - candidate.minDy);
        if (span < g_config.worldCoordMinMovementSpan) continue;

        const float score = ScoreWorldCoordCandidate(candidate);
        WorldCoordModel model{};
        model.ready = true;
        model.descriptor = candidate.descriptor;
        model.projection = candidate.model;
        model.distinctUnitCount = candidate.unitKeys.size();
        model.movementSpan = span;

        if (g_worldCoordModel.ready &&
            WorldCoordDescriptorEquals(candidate.descriptor, g_worldCoordModel.descriptor)) {
            stickyFound = true;
            stickyScore = score;
            sticky = model;
        }

        if (!found || score < bestScore) {
            found = true;
            bestScore = score;
            best = model;
        }
    }

    if (found) {
        constexpr float switchMargin = 8.0f;
        if (stickyFound && stickyScore <= bestScore + switchMargin) {
            best = sticky;
        }

        const std::wstring label = FormatWorldCoordDescriptor(best.descriptor);
        if (label != g_lastLoggedWorldCoordLabel) {
            std::wstring text = L"World coord projection learned: ";
            text += label;
            text += L" samples=";
            text += std::to_wstring(best.projection.sampleCount);
            text += L" units=";
            text += std::to_wstring(best.distinctUnitCount);
            text += L" rms=";
            text += std::to_wstring(static_cast<int>(best.projection.rmsError));
            text += L" span=";
            text += std::to_wstring(static_cast<int>(best.movementSpan));
            AppendLogBlock(text);
            g_lastLoggedWorldCoordLabel = label;
        }
        g_worldCoordModel = best;
    }
    else {
        g_worldCoordModel = WorldCoordModel{};
        g_lastLoggedWorldCoordLabel.clear();
    }
}

static std::wstring FormatTopWorldCoordCandidates(size_t maxCandidates) {
    if (g_worldCoordCandidates.empty()) return L"none";

    std::vector<WorldCoordCandidate> candidates = g_worldCoordCandidates;
    std::sort(
        candidates.begin(),
        candidates.end(),
        [](const WorldCoordCandidate& a, const WorldCoordCandidate& b) {
            const float aScore = ScoreWorldCoordCandidate(a);
            const float bScore = ScoreWorldCoordCandidate(b);
            if (fabsf(aScore - bScore) > 0.01f) return aScore < bScore;
            if (a.unitKeys.size() != b.unitKeys.size()) return a.unitKeys.size() > b.unitKeys.size();
            if (a.samples.size() != b.samples.size()) return a.samples.size() > b.samples.size();
            return FormatWorldCoordDescriptor(a.descriptor) < FormatWorldCoordDescriptor(b.descriptor);
        }
    );

    std::wstring text;
    const size_t count = min(maxCandidates, candidates.size());
    for (size_t i = 0; i < count; ++i) {
        const auto& candidate = candidates[i];
        if (i > 0) text += L"\n    ";

        text += FormatWorldCoordDescriptor(candidate.descriptor);
        text += L" samples=";
        text += std::to_wstring(candidate.samples.size());
        text += L" units=";
        text += std::to_wstring(candidate.unitKeys.size());
        text += L" rms=";
        text += candidate.model.ready
            ? std::to_wstring(static_cast<int>(candidate.model.rmsError))
            : L"?";
        text += L" span=";
        text += std::to_wstring(static_cast<int>((candidate.maxDx - candidate.minDx) + (candidate.maxDy - candidate.minDy)));
    }

    if (candidates.size() > count) {
        text += L"\n    ...";
    }

    return text;
}

static std::wstring FormatScreenAnchorDescriptor(const ScreenAnchorDescriptor& descriptor) {
    std::wstring text;
    switch (descriptor.sourceKind) {
    case ScreenAnchorSourceKind::TargetClient:
        text += L"TC";
        break;
    case ScreenAnchorSourceKind::TargetServer:
        text += L"TS";
        break;
    case ScreenAnchorSourceKind::TargetClientChild:
        text += L"TC*";
        text += FormatOffset(descriptor.pointerOffset);
        break;
    case ScreenAnchorSourceKind::TargetServerChild:
        text += L"TS*";
        text += FormatOffset(descriptor.pointerOffset);
        break;
    default:
        text += L"?";
        break;
    }

    text += L" ";
    switch (descriptor.valueKind) {
    case ScreenAnchorValueKind::Int32Pair:
        text += L"i32";
        break;
    case ScreenAnchorValueKind::UInt32Pair:
        text += L"u32";
        break;
    case ScreenAnchorValueKind::UInt16Pair:
        text += L"u16";
        break;
    case ScreenAnchorValueKind::FloatPair:
        text += L"f32";
        break;
    default:
        text += L"?";
        break;
    }
    text += FormatOffset(descriptor.valueOffset);
    return text;
}

static bool TryReadScreenAnchorDescriptor(
    const ScreenAnchorDescriptor& descriptor,
    uintptr_t targetServerUnit,
    uintptr_t targetClientUnit,
    int width,
    int height,
    float& screenX,
    float& screenY
) {
    uintptr_t sourceAddress = 0;
    switch (descriptor.sourceKind) {
    case ScreenAnchorSourceKind::TargetClient:
        sourceAddress = targetClientUnit;
        break;
    case ScreenAnchorSourceKind::TargetServer:
        sourceAddress = targetServerUnit;
        break;
    case ScreenAnchorSourceKind::TargetClientChild:
        if (!IsLikelyRemotePointer(targetClientUnit) ||
            !ReadRemoteValue(targetClientUnit + descriptor.pointerOffset, sourceAddress)) {
            return false;
        }
        break;
    case ScreenAnchorSourceKind::TargetServerChild:
        if (!IsLikelyRemotePointer(targetServerUnit) ||
            !ReadRemoteValue(targetServerUnit + descriptor.pointerOffset, sourceAddress)) {
            return false;
        }
        break;
    default:
        return false;
    }

    if (!IsLikelyRemotePointer(sourceAddress)) return false;

    switch (descriptor.valueKind) {
    case ScreenAnchorValueKind::Int32Pair: {
        int32_t x = 0;
        int32_t y = 0;
        if (!ReadRemoteValue(sourceAddress + descriptor.valueOffset, x) ||
            !ReadRemoteValue(sourceAddress + descriptor.valueOffset + sizeof(int32_t), y)) {
            return false;
        }
        screenX = static_cast<float>(x);
        screenY = static_cast<float>(y);
        break;
    }
    case ScreenAnchorValueKind::UInt32Pair: {
        uint32_t x = 0;
        uint32_t y = 0;
        if (!ReadRemoteValue(sourceAddress + descriptor.valueOffset, x) ||
            !ReadRemoteValue(sourceAddress + descriptor.valueOffset + sizeof(uint32_t), y) ||
            x > static_cast<uint32_t>(INT_MAX) ||
            y > static_cast<uint32_t>(INT_MAX)) {
            return false;
        }
        screenX = static_cast<float>(x);
        screenY = static_cast<float>(y);
        break;
    }
    case ScreenAnchorValueKind::UInt16Pair: {
        uint16_t x = 0;
        uint16_t y = 0;
        if (!ReadRemoteValue(sourceAddress + descriptor.valueOffset, x) ||
            !ReadRemoteValue(sourceAddress + descriptor.valueOffset + sizeof(uint16_t), y)) {
            return false;
        }
        screenX = static_cast<float>(x);
        screenY = static_cast<float>(y);
        break;
    }
    case ScreenAnchorValueKind::FloatPair: {
        float x = 0.0f;
        float y = 0.0f;
        if (!ReadRemoteValue(sourceAddress + descriptor.valueOffset, x) ||
            !ReadRemoteValue(sourceAddress + descriptor.valueOffset + sizeof(float), y) ||
            !std::isfinite(x) ||
            !std::isfinite(y)) {
            return false;
        }
        screenX = x;
        screenY = y;
        break;
    }
    default:
        return false;
    }

    if (!std::isfinite(screenX) || !std::isfinite(screenY) ||
        screenX < static_cast<float>(INT_MIN) || screenX > static_cast<float>(INT_MAX) ||
        screenY < static_cast<float>(INT_MIN) || screenY > static_cast<float>(INT_MAX)) {
        return false;
    }

    const int x = static_cast<int>(std::lround(screenX));
    const int y = static_cast<int>(std::lround(screenY));
    return IsScreenishCoordinate(x, y, width, height);
}

static void AddScreenAnchorSample(
    std::vector<ScreenAnchorSample>& samples,
    const ScreenAnchorDescriptor& descriptor,
    float x,
    float y,
    float cursorX,
    float cursorY,
    int width,
    int height
) {
    if (!std::isfinite(x) || !std::isfinite(y) ||
        x < static_cast<float>(INT_MIN) || x > static_cast<float>(INT_MAX) ||
        y < static_cast<float>(INT_MIN) || y > static_cast<float>(INT_MAX)) {
        return;
    }

    const int screenX = static_cast<int>(std::lround(x));
    const int screenY = static_cast<int>(std::lround(y));
    if (!IsScreenishCoordinate(screenX, screenY, width, height)) return;

    const float errorX = x - cursorX;
    const float errorY = y - cursorY;
    const float error = std::sqrt((errorX * errorX) + (errorY * errorY));
    if (error > g_config.screenAnchorMaxCursorDistance) return;

    for (auto& existing : samples) {
        if (ScreenAnchorDescriptorEquals(existing.descriptor, descriptor)) {
            if (error < existing.error) {
                existing.x = x;
                existing.y = y;
                existing.error = error;
            }
            return;
        }
    }

    ScreenAnchorSample sample{};
    sample.descriptor = descriptor;
    sample.x = x;
    sample.y = y;
    sample.error = error;
    samples.push_back(sample);
}

static void CollectScreenAnchorSamplesFromSnapshot(
    std::vector<ScreenAnchorSample>& samples,
    const std::vector<uint8_t>& snapshot,
    ScreenAnchorSourceKind sourceKind,
    size_t pointerOffset,
    float cursorX,
    float cursorY,
    int width,
    int height
) {
    if (snapshot.empty()) return;

    for (size_t offset = 0; offset + sizeof(uint32_t) * 2 <= snapshot.size(); offset += sizeof(uint32_t)) {
        ScreenAnchorDescriptor descriptor{};
        descriptor.sourceKind = sourceKind;
        descriptor.pointerOffset = pointerOffset;
        descriptor.valueOffset = offset;

        descriptor.valueKind = ScreenAnchorValueKind::Int32Pair;
        AddScreenAnchorSample(
            samples,
            descriptor,
            static_cast<float>(static_cast<int32_t>(ReadSnapshotDword(snapshot, offset))),
            static_cast<float>(static_cast<int32_t>(ReadSnapshotDword(snapshot, offset + sizeof(uint32_t)))),
            cursorX,
            cursorY,
            width,
            height
        );

        const uint32_t ux = ReadSnapshotDword(snapshot, offset);
        const uint32_t uy = ReadSnapshotDword(snapshot, offset + sizeof(uint32_t));
        if (ux <= static_cast<uint32_t>(INT_MAX) && uy <= static_cast<uint32_t>(INT_MAX)) {
            descriptor.valueKind = ScreenAnchorValueKind::UInt32Pair;
            AddScreenAnchorSample(
                samples,
                descriptor,
                static_cast<float>(ux),
                static_cast<float>(uy),
                cursorX,
                cursorY,
                width,
                height
            );
        }

        const float fx = ReadSnapshotFloat(snapshot, offset);
        const float fy = ReadSnapshotFloat(snapshot, offset + sizeof(float));
        if (std::isfinite(fx) && std::isfinite(fy)) {
            descriptor.valueKind = ScreenAnchorValueKind::FloatPair;
            AddScreenAnchorSample(samples, descriptor, fx, fy, cursorX, cursorY, width, height);
        }
    }

    for (size_t offset = 0; offset + sizeof(uint16_t) * 2 <= snapshot.size(); offset += sizeof(uint16_t)) {
        ScreenAnchorDescriptor descriptor{};
        descriptor.sourceKind = sourceKind;
        descriptor.valueKind = ScreenAnchorValueKind::UInt16Pair;
        descriptor.pointerOffset = pointerOffset;
        descriptor.valueOffset = offset;
        AddScreenAnchorSample(
            samples,
            descriptor,
            static_cast<float>(ReadSnapshotWord(snapshot, offset)),
            static_cast<float>(ReadSnapshotWord(snapshot, offset + sizeof(uint16_t))),
            cursorX,
            cursorY,
            width,
            height
        );
    }
}

static void CollectScreenAnchorSamplesFromChildPointers(
    std::vector<ScreenAnchorSample>& samples,
    const std::vector<uint8_t>& snapshot,
    ScreenAnchorSourceKind childSourceKind,
    float cursorX,
    float cursorY,
    int width,
    int height
) {
    if (snapshot.empty()) return;

    size_t childReads = 0;
    constexpr size_t maxChildReads = 32;
    for (size_t pointerOffset = 0; pointerOffset + sizeof(uint64_t) <= snapshot.size(); pointerOffset += sizeof(uint64_t)) {
        const uintptr_t pointer = static_cast<uintptr_t>(ReadSnapshotQword(snapshot, pointerOffset));
        if (!IsLikelyRemotePointer(pointer)) continue;

        std::vector<uint8_t> childSnapshot;
        if (!ReadRemoteBytesBestEffort(pointer, 0x500, childSnapshot)) continue;

        CollectScreenAnchorSamplesFromSnapshot(
            samples,
            childSnapshot,
            childSourceKind,
            pointerOffset,
            cursorX,
            cursorY,
            width,
            height
        );

        if (++childReads >= maxChildReads) break;
    }
}

static void ObserveScreenAnchorSample(const ScreenAnchorSample& sample, uint64_t unitKey) {
    const auto now = std::chrono::steady_clock::now();

    for (auto& candidate : g_screenAnchorCandidates) {
        if (!ScreenAnchorDescriptorEquals(candidate.descriptor, sample.descriptor)) continue;

        candidate.sampleCount += 1;
        candidate.totalError += sample.error;
        candidate.averageError = static_cast<float>(
            candidate.totalError / static_cast<double>(candidate.sampleCount)
        );
        candidate.lastX = sample.x;
        candidate.lastY = sample.y;
        candidate.lastError = sample.error;
        candidate.minX = min(candidate.minX, sample.x);
        candidate.maxX = max(candidate.maxX, sample.x);
        candidate.minY = min(candidate.minY, sample.y);
        candidate.maxY = max(candidate.maxY, sample.y);
        AddUniqueUnitKey(candidate.unitKeys, unitKey);
        candidate.lastSeen = now;
        return;
    }

    ScreenAnchorCandidate candidate{};
    candidate.descriptor = sample.descriptor;
    candidate.sampleCount = 1;
    candidate.totalError = sample.error;
    candidate.averageError = sample.error;
    candidate.lastX = sample.x;
    candidate.lastY = sample.y;
    candidate.lastError = sample.error;
    candidate.minX = sample.x;
    candidate.maxX = sample.x;
    candidate.minY = sample.y;
    candidate.maxY = sample.y;
    AddUniqueUnitKey(candidate.unitKeys, unitKey);
    candidate.lastSeen = now;
    g_screenAnchorCandidates.push_back(candidate);
}

static int ScreenAnchorSourcePreference(ScreenAnchorSourceKind sourceKind) {
    switch (sourceKind) {
    case ScreenAnchorSourceKind::TargetClient:
        return 0;
    case ScreenAnchorSourceKind::TargetClientChild:
        return 1;
    case ScreenAnchorSourceKind::TargetServer:
        return 2;
    case ScreenAnchorSourceKind::TargetServerChild:
        return 3;
    default:
        return 4;
    }
}

static float GetScreenAnchorMovementSpan(const ScreenAnchorCandidate& candidate) {
    return (candidate.maxX - candidate.minX) + (candidate.maxY - candidate.minY);
}

static float ScoreScreenAnchorCandidate(const ScreenAnchorCandidate& candidate) {
    const float movementSpan = GetScreenAnchorMovementSpan(candidate);
    return candidate.averageError -
        min(80.0f, static_cast<float>(candidate.sampleCount)) * 0.05f -
        min(80.0f, movementSpan) * 0.02f -
        min(8.0f, static_cast<float>(candidate.unitKeys.size())) * 2.0f +
        static_cast<float>(candidate.failedUnitKeys.size()) * 80.0f +
        static_cast<float>(candidate.farUnitKeys.size()) * 35.0f;
}

static void ProbeExistingScreenAnchorCandidates(
    uintptr_t targetServerUnit,
    uintptr_t targetClientUnit,
    float cursorX,
    float cursorY,
    int width,
    int height,
    uint64_t unitKey
) {
    if (unitKey == 0 || unitKey == UINT64_MAX) return;

    for (auto& candidate : g_screenAnchorCandidates) {
        if (HasUnitKey(candidate.unitKeys, unitKey) ||
            HasUnitKey(candidate.failedUnitKeys, unitKey) ||
            HasUnitKey(candidate.farUnitKeys, unitKey)) {
            continue;
        }

        float screenX = 0.0f;
        float screenY = 0.0f;
        if (!TryReadScreenAnchorDescriptor(
            candidate.descriptor,
            targetServerUnit,
            targetClientUnit,
            width,
            height,
            screenX,
            screenY
        )) {
            AddUniqueUnitKey(candidate.failedUnitKeys, unitKey);
            continue;
        }

        const float dx = screenX - cursorX;
        const float dy = screenY - cursorY;
        const float error = std::sqrt((dx * dx) + (dy * dy));
        if (error > g_config.screenAnchorMaxCursorDistance) {
            AddUniqueUnitKey(candidate.farUnitKeys, unitKey);
        }
    }
}

static void RefreshScreenAnchorModel() {
    ScreenAnchorModel best{};
    bool found = false;
    float bestScore = 0.0f;
    int bestPreference = 100;

    for (const auto& candidate : g_screenAnchorCandidates) {
        if (candidate.sampleCount < static_cast<size_t>(max(1, g_config.screenAnchorMinSamples))) continue;
        if (candidate.averageError > g_config.screenAnchorMaxAverageError) continue;
        if (candidate.unitKeys.size() < static_cast<size_t>(max(1, g_config.screenAnchorMinDistinctUnits))) continue;

        const float movementSpan = GetScreenAnchorMovementSpan(candidate);
        if (movementSpan < g_config.screenAnchorMinMovementSpan) continue;
        if (!candidate.failedUnitKeys.empty()) continue;
        if (!candidate.farUnitKeys.empty()) continue;

        const float score = ScoreScreenAnchorCandidate(candidate);
        const int preference = ScreenAnchorSourcePreference(candidate.descriptor.sourceKind);

        if (!found ||
            score < bestScore ||
            (fabsf(score - bestScore) < 0.50f && preference < bestPreference) ||
            (fabsf(score - bestScore) < 0.50f && preference == bestPreference && candidate.sampleCount > best.sampleCount)) {
            found = true;
            bestScore = score;
            bestPreference = preference;
            best.ready = true;
            best.descriptor = candidate.descriptor;
            best.sampleCount = candidate.sampleCount;
            best.averageError = candidate.averageError;
            best.lastX = candidate.lastX;
            best.lastY = candidate.lastY;
            best.lastError = candidate.lastError;
            best.distinctUnitCount = candidate.unitKeys.size();
            best.movementSpan = movementSpan;
            best.failedUnitCount = candidate.failedUnitKeys.size();
            best.farUnitCount = candidate.farUnitKeys.size();
        }
    }

    if (found) {
        const std::wstring label = FormatScreenAnchorDescriptor(best.descriptor);
        if (label != g_lastLoggedScreenAnchorLabel) {
            std::wstring text = L"Screen anchor learned: ";
            text += label;
            text += L" samples=";
            text += std::to_wstring(best.sampleCount);
            text += L" avgErr=";
            text += std::to_wstring(static_cast<int>(best.averageError));
            text += L" lastErr=";
            text += std::to_wstring(static_cast<int>(best.lastError));
            text += L" units=";
            text += std::to_wstring(best.distinctUnitCount);
            text += L" span=";
            text += std::to_wstring(static_cast<int>(best.movementSpan));
            AppendLogBlock(text);
            g_lastLoggedScreenAnchorLabel = label;
        }
        g_screenAnchorModel = best;
    }
    else {
        g_screenAnchorModel = ScreenAnchorModel{};
        g_lastLoggedScreenAnchorLabel.clear();
    }
}

static void PruneScreenAnchorCandidates() {
    constexpr size_t maxCandidates = 1600;
    if (g_screenAnchorCandidates.size() <= maxCandidates) return;

    std::sort(
        g_screenAnchorCandidates.begin(),
        g_screenAnchorCandidates.end(),
        [](const ScreenAnchorCandidate& a, const ScreenAnchorCandidate& b) {
            const float aScore = ScoreScreenAnchorCandidate(a);
            const float bScore = ScoreScreenAnchorCandidate(b);
            if (fabsf(aScore - bScore) > 0.01f) return aScore < bScore;
            if (fabsf(a.averageError - b.averageError) > 0.01f) return a.averageError < b.averageError;
            if (a.sampleCount != b.sampleCount) return a.sampleCount > b.sampleCount;
            return FormatScreenAnchorDescriptor(a.descriptor) < FormatScreenAnchorDescriptor(b.descriptor);
        }
    );
    g_screenAnchorCandidates.resize(maxCandidates);
}

static std::wstring FormatTopScreenAnchorCandidates(size_t maxCandidates) {
    if (g_screenAnchorCandidates.empty()) return L"none";

    std::vector<ScreenAnchorCandidate> candidates = g_screenAnchorCandidates;
    std::sort(
        candidates.begin(),
        candidates.end(),
        [](const ScreenAnchorCandidate& a, const ScreenAnchorCandidate& b) {
            const float aScore = ScoreScreenAnchorCandidate(a);
            const float bScore = ScoreScreenAnchorCandidate(b);
            if (fabsf(aScore - bScore) > 0.01f) return aScore < bScore;
            if (fabsf(a.averageError - b.averageError) > 0.01f) return a.averageError < b.averageError;
            if (a.sampleCount != b.sampleCount) return a.sampleCount > b.sampleCount;
            return FormatScreenAnchorDescriptor(a.descriptor) < FormatScreenAnchorDescriptor(b.descriptor);
        }
    );

    std::wstring text;
    const size_t count = min(maxCandidates, candidates.size());
    for (size_t i = 0; i < count; ++i) {
        const auto& candidate = candidates[i];
        if (i > 0) text += L"\n    ";

        text += FormatScreenAnchorDescriptor(candidate.descriptor);
        text += L" samples=";
        text += std::to_wstring(candidate.sampleCount);
        text += L" avg=";
        text += std::to_wstring(static_cast<int>(candidate.averageError));
        text += L" last=";
        text += std::to_wstring(static_cast<int>(candidate.lastError));
        text += L" xy=";
        text += std::to_wstring(static_cast<int>(candidate.lastX));
        text += L",";
        text += std::to_wstring(static_cast<int>(candidate.lastY));
        text += L" span=";
        text += std::to_wstring(static_cast<int>(GetScreenAnchorMovementSpan(candidate)));
        text += L" units=";
        text += std::to_wstring(candidate.unitKeys.size());
        if (!candidate.failedUnitKeys.empty() || !candidate.farUnitKeys.empty()) {
            text += L" fail=";
            text += std::to_wstring(candidate.failedUnitKeys.size());
            text += L" far=";
            text += std::to_wstring(candidate.farUnitKeys.size());
        }
    }

    if (candidates.size() > count) {
        text += L"\n    ...";
    }

    return text;
}

static void UpdateScreenAnchorCandidateTracking() {
    if (!g_config.useScreenAnchorCandidates ||
        g_worldCoordModel.ready ||
        !g_memorySource.hoverReady ||
        !g_memorySource.hover.isHovered ||
        g_memorySource.hover.hoveredUnitType != D2_UNIT_MONSTER) {
        return;
    }

    float cursorX = 0.0f;
    float cursorY = 0.0f;
    if (!TryGetCursorInGameRect(cursorX, cursorY)) return;

    const int width = max(0, g_gameRect.right - g_gameRect.left);
    const int height = max(0, g_gameRect.bottom - g_gameRect.top);
    if (width <= 0 || height <= 0) return;

    uintptr_t targetServerUnit = g_memorySource.hoveredUnitAddress;
    uintptr_t targetClientUnit = g_memorySource.hoveredClientUnitAddress;

    if (!IsLikelyRemotePointer(targetServerUnit) && IsLikelyRemotePointer(g_memorySource.currentSinglePlayerGame)) {
        FindServerUnitByTypeAndId(
            g_memorySource.currentSinglePlayerGame,
            g_memorySource.hover.hoveredUnitType,
            g_memorySource.hover.hoveredUnitId,
            targetServerUnit
        );
    }

    if (!IsLikelyRemotePointer(targetClientUnit)) {
        FindUnitByTypeAndId(
            g_memorySource.hover.hoveredUnitType,
            g_memorySource.hover.hoveredUnitId,
            targetClientUnit
        );
    }
    const uint64_t unitKey = MakeUnitKey(
        g_memorySource.hover.hoveredUnitType,
        g_memorySource.hover.hoveredUnitId
    );

    std::vector<uint8_t> targetClientSnapshot;
    std::vector<uint8_t> targetServerSnapshot;
    ReadRemoteBytesBestEffort(targetClientUnit, D2_EXTENDED_UNIT_SNAPSHOT_BYTES, targetClientSnapshot);
    ReadRemoteBytesBestEffort(targetServerUnit, D2_EXTENDED_UNIT_SNAPSHOT_BYTES, targetServerSnapshot);

    std::vector<ScreenAnchorSample> samples;
    samples.reserve(128);
    CollectScreenAnchorSamplesFromSnapshot(
        samples,
        targetClientSnapshot,
        ScreenAnchorSourceKind::TargetClient,
        0,
        cursorX,
        cursorY,
        width,
        height
    );
    CollectScreenAnchorSamplesFromSnapshot(
        samples,
        targetServerSnapshot,
        ScreenAnchorSourceKind::TargetServer,
        0,
        cursorX,
        cursorY,
        width,
        height
    );
    CollectScreenAnchorSamplesFromChildPointers(
        samples,
        targetClientSnapshot,
        ScreenAnchorSourceKind::TargetClientChild,
        cursorX,
        cursorY,
        width,
        height
    );
    CollectScreenAnchorSamplesFromChildPointers(
        samples,
        targetServerSnapshot,
        ScreenAnchorSourceKind::TargetServerChild,
        cursorX,
        cursorY,
        width,
        height
    );

    if (samples.empty() && g_screenAnchorCandidates.empty()) return;

    ++g_screenAnchorHoverSamples;
    for (const auto& sample : samples) {
        ObserveScreenAnchorSample(sample, unitKey);
    }
    ProbeExistingScreenAnchorCandidates(targetServerUnit, targetClientUnit, cursorX, cursorY, width, height, unitKey);

    PruneScreenAnchorCandidates();
    RefreshScreenAnchorModel();
}

static bool TryResolvePlayerUnitPair(
    uintptr_t currentGame,
    uintptr_t& playerServerUnit,
    uintptr_t& playerClientUnit
) {
    playerServerUnit = 0;
    playerClientUnit = 0;
    if (!IsLikelyRemotePointer(currentGame)) return false;

    uint32_t playerUnitId = UINT32_MAX;
    if (!FindFirstServerUnitByType(currentGame, 0, playerServerUnit) ||
        !ReadRemoteValue(playerServerUnit + D2_UNIT_OFFSET_ID, playerUnitId) ||
        playerUnitId == UINT32_MAX) {
        return false;
    }

    FindUnitByTypeAndId(0, playerUnitId, playerClientUnit);
    return IsLikelyRemotePointer(playerServerUnit) || IsLikelyRemotePointer(playerClientUnit);
}

static void UpdateWorldCoordProjectionTracking() {
    if (!g_config.useWorldCoordCandidates ||
        !g_config.learnWorldProjection ||
        !g_memorySource.hoverReady ||
        !g_memorySource.hover.isHovered ||
        g_memorySource.hover.hoveredUnitType != D2_UNIT_MONSTER) {
        if (!g_config.useWorldCoordCandidates && g_worldCoordModel.ready) {
            g_worldCoordModel = WorldCoordModel{};
            g_lastLoggedWorldCoordLabel.clear();
        }
        return;
    }

    float cursorX = 0.0f;
    float cursorY = 0.0f;
    if (!TryGetCursorInGameRect(cursorX, cursorY)) return;

    uintptr_t currentGame = g_memorySource.currentSinglePlayerGame;
    if (!IsLikelyRemotePointer(currentGame)) return;

    uintptr_t targetServerUnit = g_memorySource.hoveredUnitAddress;
    uintptr_t targetClientUnit = g_memorySource.hoveredClientUnitAddress;

    if (!IsLikelyRemotePointer(targetServerUnit)) {
        FindServerUnitByTypeAndId(
            currentGame,
            g_memorySource.hover.hoveredUnitType,
            g_memorySource.hover.hoveredUnitId,
            targetServerUnit
        );
    }

    if (!IsLikelyRemotePointer(targetClientUnit)) {
        FindUnitByTypeAndId(
            g_memorySource.hover.hoveredUnitType,
            g_memorySource.hover.hoveredUnitId,
            targetClientUnit
        );
    }

    uintptr_t playerServerUnit = 0;
    uintptr_t playerClientUnit = 0;
    if (!TryResolvePlayerUnitPair(currentGame, playerServerUnit, playerClientUnit)) return;

    const uint64_t unitKey = MakeUnitKey(
        g_memorySource.hover.hoveredUnitType,
        g_memorySource.hover.hoveredUnitId
    );

    if (g_worldCoordModel.ready) {
        return;
    }

    std::vector<uint8_t> targetClientSnapshot;
    std::vector<uint8_t> targetServerSnapshot;
    std::vector<uint8_t> playerClientSnapshot;
    std::vector<uint8_t> playerServerSnapshot;
    ReadRemoteBytesBestEffort(targetClientUnit, D2_EXTENDED_UNIT_SNAPSHOT_BYTES, targetClientSnapshot);
    ReadRemoteBytesBestEffort(targetServerUnit, D2_EXTENDED_UNIT_SNAPSHOT_BYTES, targetServerSnapshot);
    ReadRemoteBytesBestEffort(playerClientUnit, D2_EXTENDED_UNIT_SNAPSHOT_BYTES, playerClientSnapshot);
    ReadRemoteBytesBestEffort(playerServerUnit, D2_EXTENDED_UNIT_SNAPSHOT_BYTES, playerServerSnapshot);

    std::vector<WorldCoordSample> samples;
    samples.reserve(512);
    CollectWorldCoordSamplesFromSnapshots(
        samples,
        targetClientSnapshot,
        playerClientSnapshot,
        WorldCoordSourceKind::ClientUnit,
        0,
        cursorX,
        cursorY
    );
    CollectWorldCoordSamplesFromSnapshots(
        samples,
        targetServerSnapshot,
        playerServerSnapshot,
        WorldCoordSourceKind::ServerUnit,
        0,
        cursorX,
        cursorY
    );
    CollectWorldCoordSamplesFromChildPointers(
        samples,
        targetClientSnapshot,
        playerClientSnapshot,
        WorldCoordSourceKind::ClientChild,
        cursorX,
        cursorY
    );
    CollectWorldCoordSamplesFromChildPointers(
        samples,
        targetServerSnapshot,
        playerServerSnapshot,
        WorldCoordSourceKind::ServerChild,
        cursorX,
        cursorY
    );

    if (samples.empty()) return;

    ++g_worldCoordHoverSamples;
    for (const auto& sample : samples) {
        ObserveWorldCoordSample(sample, unitKey);
    }

    PruneWorldCoordCandidates();
    RefreshWorldCoordModel();
}

static bool TryProjectUnitWithWorldCoordModel(
    uintptr_t currentGame,
    uint32_t targetUnitType,
    uint32_t targetUnitId,
    uintptr_t targetServerUnit,
    uintptr_t targetClientUnit,
    float& screenX,
    float& screenY,
    WorldPosition* targetWorldPosition = nullptr,
    bool* hasTargetWorldPosition = nullptr
) {
    if (!g_config.useWorldCoordCandidates || !g_worldCoordModel.ready || !g_hasGameRect) return false;

    if (hasTargetWorldPosition) {
        *hasTargetWorldPosition = false;
    }

    if (!IsLikelyRemotePointer(targetServerUnit) && IsLikelyRemotePointer(currentGame)) {
        FindServerUnitByTypeAndId(currentGame, targetUnitType, targetUnitId, targetServerUnit);
    }

    if (!IsLikelyRemotePointer(targetClientUnit)) {
        FindUnitByTypeAndId(targetUnitType, targetUnitId, targetClientUnit);
    }

    uintptr_t playerServerUnit = 0;
    uintptr_t playerClientUnit = 0;
    if (!TryResolvePlayerUnitPair(currentGame, playerServerUnit, playerClientUnit)) {
        return false;
    }

    WorldPosition targetPosition;
    WorldPosition playerPosition;
    if (!TryReadWorldCoordDescriptor(g_worldCoordModel.descriptor, targetServerUnit, targetClientUnit, targetPosition) ||
        !TryReadWorldCoordDescriptor(g_worldCoordModel.descriptor, playerServerUnit, playerClientUnit, playerPosition)) {
        return false;
    }

    const float dx = targetPosition.x - playerPosition.x;
    const float dy = targetPosition.y - playerPosition.y;
    screenX = (g_worldCoordModel.projection.ax * dx) +
        (g_worldCoordModel.projection.bx * dy) +
        g_worldCoordModel.projection.cx;
    screenY = (g_worldCoordModel.projection.ay * dx) +
        (g_worldCoordModel.projection.by * dy) +
        g_worldCoordModel.projection.cy;
    screenX += g_config.worldCoordScreenOffsetX;
    screenY += g_config.worldCoordScreenOffsetY;

    const float width = static_cast<float>(max(0, g_gameRect.right - g_gameRect.left));
    const float height = static_cast<float>(max(0, g_gameRect.bottom - g_gameRect.top));
    const float margin = max(width, height) * 0.35f;
    if (!std::isfinite(screenX) || !std::isfinite(screenY) ||
        screenX < -margin || screenY < -margin ||
        screenX > width + margin || screenY > height + margin) {
        return false;
    }

    if (hasTargetWorldPosition) {
        *hasTargetWorldPosition = false;
    }
    return true;
}

static bool TryProjectUnitToScreenFromPlayer(
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
) {
    if (!g_hasGameRect) return false;

    if (hasTargetWorldPosition) {
        *hasTargetWorldPosition = false;
    }

    const int width = max(0, g_gameRect.right - g_gameRect.left);
    const int height = max(0, g_gameRect.bottom - g_gameRect.top);

    if (g_config.useScreenAnchorCandidates && g_screenAnchorModel.ready) {
        if (TryReadScreenAnchorDescriptor(
            g_screenAnchorModel.descriptor,
            targetServerUnit,
            targetClientUnit,
            width,
            height,
            screenX,
            screenY
        )) {
            if (targetWorldPosition || hasTargetWorldPosition) {
                WorldPosition targetPosition;
                if (TryResolveTargetWorldPosition(
                    currentGame,
                    targetUnitType,
                    targetUnitId,
                    targetServerUnit,
                    targetClientUnit,
                    targetPosition
                )) {
                    if (targetWorldPosition) {
                        *targetWorldPosition = targetPosition;
                    }
                    if (hasTargetWorldPosition) {
                        *hasTargetWorldPosition = true;
                    }
                }
            }
            return true;
        }
    }

    float pathScreenX = 0.0f;
    float pathScreenY = 0.0f;
    WorldPosition pathTargetWorldPosition;
    bool hasPathTargetWorldPosition = false;
    const bool pathProjected = TryProjectUnitWithPathWorldPosition(
        currentGame,
        playerPosition,
        targetUnitType,
        targetUnitId,
        targetServerUnit,
        targetClientUnit,
        pathScreenX,
        pathScreenY,
        &pathTargetWorldPosition,
        &hasPathTargetWorldPosition
    );

    if (g_config.useWorldPositions && g_config.learnWorldProjection) {
        float coordScreenX = 0.0f;
        float coordScreenY = 0.0f;
        if (TryProjectUnitWithWorldCoordModel(
                currentGame,
                targetUnitType,
                targetUnitId,
                targetServerUnit,
                targetClientUnit,
                coordScreenX,
                coordScreenY
            )) {
            if (!pathProjected || IsProjectionCloseToReference(coordScreenX, coordScreenY, pathScreenX, pathScreenY)) {
                screenX = coordScreenX;
                screenY = coordScreenY;
                if (pathProjected && targetWorldPosition) {
                    *targetWorldPosition = pathTargetWorldPosition;
                }
                if (hasTargetWorldPosition) {
                    *hasTargetWorldPosition = pathProjected && hasPathTargetWorldPosition;
                }
                return true;
            }
        }
    }

    if (!pathProjected) return false;

    screenX = pathScreenX;
    screenY = pathScreenY;
    if (targetWorldPosition) {
        *targetWorldPosition = pathTargetWorldPosition;
    }
    if (hasTargetWorldPosition) {
        *hasTargetWorldPosition = hasPathTargetWorldPosition;
    }
    return true;
}

static bool TryProjectUnitToScreen(
    uint32_t targetUnitType,
    uint32_t targetUnitId,
    uintptr_t targetServerUnit,
    uintptr_t targetClientUnit,
    float& screenX,
    float& screenY,
    WorldPosition* targetWorldPosition = nullptr,
    bool* hasTargetWorldPosition = nullptr
) {
    if (!g_hasGameRect) return false;

    if (hasTargetWorldPosition) {
        *hasTargetWorldPosition = false;
    }

    const int width = max(0, g_gameRect.right - g_gameRect.left);
    const int height = max(0, g_gameRect.bottom - g_gameRect.top);
    const uintptr_t currentGame = g_memorySource.currentSinglePlayerGame;

    if (g_config.useScreenAnchorCandidates && g_screenAnchorModel.ready) {
        if (TryReadScreenAnchorDescriptor(
            g_screenAnchorModel.descriptor,
            targetServerUnit,
            targetClientUnit,
            width,
            height,
            screenX,
            screenY
        )) {
            if (targetWorldPosition || hasTargetWorldPosition) {
                WorldPosition targetPosition;
                if (TryResolveTargetWorldPosition(
                    currentGame,
                    targetUnitType,
                    targetUnitId,
                    targetServerUnit,
                    targetClientUnit,
                    targetPosition
                )) {
                    if (targetWorldPosition) {
                        *targetWorldPosition = targetPosition;
                    }
                    if (hasTargetWorldPosition) {
                        *hasTargetWorldPosition = true;
                    }
                }
            }
            return true;
        }
    }

    if (!g_config.useWorldPositions) return false;

    WorldPosition playerPosition;
    if (!TryReadPlayerWorldPosition(currentGame, playerPosition)) {
        return false;
    }

    return TryProjectUnitToScreenFromPlayer(
        currentGame,
        playerPosition,
        targetUnitType,
        targetUnitId,
        targetServerUnit,
        targetClientUnit,
        screenX,
        screenY,
        targetWorldPosition,
        hasTargetWorldPosition
    );
}

static void UpdateWorldProjectionCalibration() {
    if (!g_config.useWorldPositions ||
        !g_memorySource.hoverReady ||
        !g_memorySource.hover.isHovered ||
        g_memorySource.hover.hoveredUnitType != D2_UNIT_MONSTER) {
        return;
    }

    float cursorX = 0.0f;
    float cursorY = 0.0f;
    if (!TryGetCursorInGameRect(cursorX, cursorY)) return;

    WorldPosition playerPosition;
    WorldPosition targetPosition;
    if (!TryReadPlayerWorldPosition(g_memorySource.currentSinglePlayerGame, playerPosition) ||
        !TryResolveTargetWorldPosition(
            g_memorySource.currentSinglePlayerGame,
            g_memorySource.hover.hoveredUnitType,
            g_memorySource.hover.hoveredUnitId,
            g_memorySource.hoveredUnitAddress,
            g_memorySource.hoveredClientUnitAddress,
            targetPosition
        )) {
        return;
    }

    AddWorldProjectionSample(
        targetPosition.x - playerPosition.x,
        targetPosition.y - playerPosition.y,
        cursorX,
        cursorY
    );

    float projectedX = 0.0f;
    float projectedY = 0.0f;
    if (!ProjectWorldPositionRelativeToPlayer(targetPosition, playerPosition, false, projectedX, projectedY)) {
        return;
    }

    const float wantedBiasX = cursorX - projectedX;
    const float wantedBiasY = cursorY - projectedY;
    if (!g_worldProjectionCalibrated) {
        g_worldProjectionBiasX = wantedBiasX;
        g_worldProjectionBiasY = wantedBiasY;
        g_worldProjectionCalibrated = true;
        return;
    }

    constexpr float smoothing = 0.18f;
    g_worldProjectionBiasX += (wantedBiasX - g_worldProjectionBiasX) * smoothing;
    g_worldProjectionBiasY += (wantedBiasY - g_worldProjectionBiasY) * smoothing;
}

static void AddPositionDebugCandidate(
    std::vector<PositionDebugCandidate>& candidates,
    const std::wstring& label,
    int x,
    int y,
    int cursorX,
    int cursorY,
    int width,
    int height,
    COLORREF color,
    size_t maxCandidates
) {
    if (!IsScreenishCoordinate(x, y, width, height)) return;

    for (const auto& existing : candidates) {
        if (existing.marker.x == x && existing.marker.y == y && existing.marker.label == label) {
            return;
        }
    }

    PositionDebugCandidate candidate{};
    candidate.marker.label = label;
    candidate.marker.x = x;
    candidate.marker.y = y;
    candidate.marker.color = color;
    candidate.distance = AbsInt(x - cursorX) + AbsInt(y - cursorY);
    candidates.push_back(candidate);

    std::sort(
        candidates.begin(),
        candidates.end(),
        [](const PositionDebugCandidate& a, const PositionDebugCandidate& b) {
            if (a.distance != b.distance) return a.distance < b.distance;
            return a.marker.label < b.marker.label;
        }
    );

    if (candidates.size() > maxCandidates) {
        candidates.resize(maxCandidates);
    }
}

static void CollectPositionDebugCandidatesFromSnapshot(
    std::vector<PositionDebugCandidate>& candidates,
    const std::vector<uint8_t>& snapshot,
    const wchar_t* prefix,
    int cursorX,
    int cursorY,
    int width,
    int height,
    COLORREF color,
    size_t maxCandidates
) {
    if (snapshot.empty()) return;

    for (size_t offset = 0; offset + sizeof(uint32_t) * 2 <= snapshot.size(); offset += sizeof(uint32_t)) {
        const int x = static_cast<int32_t>(ReadSnapshotDword(snapshot, offset));
        const int y = static_cast<int32_t>(ReadSnapshotDword(snapshot, offset + sizeof(uint32_t)));

        std::wstring label = prefix ? prefix : L"?";
        label += L" ";
        label += FormatOffset(offset);
        AddPositionDebugCandidate(candidates, label, x, y, cursorX, cursorY, width, height, color, maxCandidates);
    }

    for (size_t offset = 0; offset + sizeof(uint16_t) * 2 <= snapshot.size(); offset += sizeof(uint16_t)) {
        const int x = static_cast<int>(ReadSnapshotWord(snapshot, offset));
        const int y = static_cast<int>(ReadSnapshotWord(snapshot, offset + sizeof(uint16_t)));

        std::wstring label = prefix ? prefix : L"?";
        label += L"w ";
        label += FormatOffset(offset);
        AddPositionDebugCandidate(candidates, label, x, y, cursorX, cursorY, width, height, color, maxCandidates);
    }
}

static void CollectPositionDebugCandidatesFromChildPointers(
    std::vector<PositionDebugCandidate>& candidates,
    const std::vector<uint8_t>& snapshot,
    const wchar_t* prefix,
    int cursorX,
    int cursorY,
    int width,
    int height,
    COLORREF color,
    size_t maxCandidates
) {
    if (snapshot.empty()) return;

    size_t childReads = 0;
    constexpr size_t maxChildReads = 24;

    for (size_t pointerOffset = 0; pointerOffset + sizeof(uint64_t) <= snapshot.size(); pointerOffset += sizeof(uint64_t)) {
        const uintptr_t pointer = static_cast<uintptr_t>(ReadSnapshotQword(snapshot, pointerOffset));
        if (!IsLikelyRemotePointer(pointer)) continue;

        std::vector<uint8_t> childSnapshot;
        if (!ReadRemoteBytesBestEffort(pointer, 0x300, childSnapshot)) continue;
        ++childReads;

        for (size_t offset = 0; offset + sizeof(uint32_t) * 2 <= childSnapshot.size(); offset += sizeof(uint32_t)) {
            const int x = static_cast<int32_t>(ReadSnapshotDword(childSnapshot, offset));
            const int y = static_cast<int32_t>(ReadSnapshotDword(childSnapshot, offset + sizeof(uint32_t)));

            std::wstring label = prefix ? prefix : L"?";
            label += L"*";
            label += FormatOffset(pointerOffset);
            label += L" ";
            label += FormatOffset(offset);
            AddPositionDebugCandidate(candidates, label, x, y, cursorX, cursorY, width, height, color, maxCandidates);
        }

        for (size_t offset = 0; offset + sizeof(uint16_t) * 2 <= childSnapshot.size(); offset += sizeof(uint16_t)) {
            const int x = static_cast<int>(ReadSnapshotWord(childSnapshot, offset));
            const int y = static_cast<int>(ReadSnapshotWord(childSnapshot, offset + sizeof(uint16_t)));

            std::wstring label = prefix ? prefix : L"?";
            label += L"*w";
            label += FormatOffset(pointerOffset);
            label += L" ";
            label += FormatOffset(offset);
            AddPositionDebugCandidate(candidates, label, x, y, cursorX, cursorY, width, height, color, maxCandidates);
        }

        if (childReads >= maxChildReads) break;
    }
}

static void UpdatePositionDebugMarkers() {
    g_positionDebugMarkers.clear();

    if (!g_config.showPositionCandidates ||
        !g_memorySource.hoverReady ||
        !g_memorySource.hover.isHovered ||
        g_memorySource.hover.hoveredUnitType != D2_UNIT_MONSTER) {
        return;
    }

    float cursorXF = 0.0f;
    float cursorYF = 0.0f;
    if (!TryGetCursorInGameRect(cursorXF, cursorYF)) return;

    const int width = max(0, g_gameRect.right - g_gameRect.left);
    const int height = max(0, g_gameRect.bottom - g_gameRect.top);
    const int cursorX = static_cast<int>(cursorXF);
    const int cursorY = static_cast<int>(cursorYF);

    uintptr_t targetServerUnit = g_memorySource.hoveredUnitAddress;
    uintptr_t targetClientUnit = g_memorySource.hoveredClientUnitAddress;

    if (!IsLikelyRemotePointer(targetServerUnit) && IsLikelyRemotePointer(g_memorySource.currentSinglePlayerGame)) {
        FindServerUnitByTypeAndId(
            g_memorySource.currentSinglePlayerGame,
            g_memorySource.hover.hoveredUnitType,
            g_memorySource.hover.hoveredUnitId,
            targetServerUnit
        );
    }

    if (!IsLikelyRemotePointer(targetClientUnit)) {
        FindUnitByTypeAndId(
            g_memorySource.hover.hoveredUnitType,
            g_memorySource.hover.hoveredUnitId,
            targetClientUnit
        );
    }

    std::vector<PositionDebugCandidate> candidates;
    const size_t maxCandidates = 6;

    AddPositionDebugCandidate(
        candidates,
        L"cursor",
        cursorX,
        cursorY,
        cursorX,
        cursorY,
        width,
        height,
        RGB(255, 255, 255),
        maxCandidates
    );

    PositionDebugMarker screenLockMarker{};
    bool hasScreenLockMarker = false;
    float screenLockX = 0.0f;
    float screenLockY = 0.0f;
    if (g_config.useScreenAnchorCandidates &&
        g_screenAnchorModel.ready &&
        TryReadScreenAnchorDescriptor(
            g_screenAnchorModel.descriptor,
            targetServerUnit,
            targetClientUnit,
            width,
            height,
            screenLockX,
            screenLockY
        )) {
        std::wstring label = L"screen lock ";
        label += FormatScreenAnchorDescriptor(g_screenAnchorModel.descriptor);
        screenLockMarker.label = label;
        screenLockMarker.x = static_cast<int>(std::lround(screenLockX));
        screenLockMarker.y = static_cast<int>(std::lround(screenLockY));
        screenLockMarker.color = RGB(255, 110, 200);
        hasScreenLockMarker = true;
    }

    float projectedX = 0.0f;
    float projectedY = 0.0f;
    if (TryProjectUnitToScreen(
        g_memorySource.hover.hoveredUnitType,
        g_memorySource.hover.hoveredUnitId,
        targetServerUnit,
        targetClientUnit,
        projectedX,
        projectedY
    )) {
        AddPositionDebugCandidate(
            candidates,
            L"target pos",
            static_cast<int>(projectedX),
            static_cast<int>(projectedY),
            cursorX,
            cursorY,
            width,
            height,
            RGB(110, 255, 140),
            maxCandidates
        );
    }

    for (const auto& candidate : candidates) {
        g_positionDebugMarkers.push_back(candidate.marker);
    }
    if (hasScreenLockMarker) {
        g_positionDebugMarkers.push_back(screenLockMarker);
    }
}

static void AppendPositionProbeSnapshot(
    std::wstring& text,
    const wchar_t* label,
    const std::vector<uint8_t>& snapshot,
    int cursorX,
    int cursorY,
    int width,
    int height
) {
    text += L"\n";
    text += label ? label : L"snapshot";
    text += L" pointers: ";
    text += BuildPositionProbePointerList(snapshot);
    text += L"\n";
    text += label ? label : L"snapshot";
    text += L" screen-ish pairs: ";
    text += BuildPositionProbePairCandidates(snapshot, cursorX, cursorY, width, height, true);
    text += L"\n";
    text += label ? label : L"snapshot";
    text += L" world-ish pairs: ";
    text += BuildPositionProbePairCandidates(snapshot, cursorX, cursorY, width, height, false);
    text += L"\n";
    text += label ? label : L"snapshot";
    text += L" scalar offsets:";
    text += BuildPositionProbeScalarValues(snapshot);
    text += L"\n";
    text += label ? label : L"snapshot";
    text += L" pointer child screen-ish pairs:\n    ";
    text += BuildPositionProbeChildCandidates(snapshot, cursorX, cursorY, width, height);
}

static void WritePositionProbeMarker() {
    if (!g_config.logEnabled) {
        ShowStatusNotice(L"Log disabled; position probe not written");
        return;
    }

    uintptr_t currentGame = g_memorySource.currentSinglePlayerGame;
    if (!IsLikelyRemotePointer(currentGame)) {
        if (g_memorySource.currentSinglePlayerGameAddress == 0) {
            TryRefreshCurrentGamePatternAddress();
        }

        if (g_memorySource.currentSinglePlayerGameAddress != 0) {
            ReadRemoteValue(g_memorySource.currentSinglePlayerGameAddress, currentGame);
            g_memorySource.currentSinglePlayerGame = currentGame;
        }
    }

    if (!IsLikelyRemotePointer(currentGame)) {
        AppendLogBlock(BuildMemoryLogText());
        ShowStatusNotice(L"No active game for position probe");
        return;
    }

    if (!g_memorySource.hoverReady ||
        !g_memorySource.hover.isHovered ||
        g_memorySource.hover.hoveredUnitType != D2_UNIT_MONSTER) {
        ShowStatusNotice(L"Hover a monster for F6 probe");
        return;
    }

    float cursorXF = 0.0f;
    float cursorYF = 0.0f;
    if (!TryGetCursorInGameRect(cursorXF, cursorYF)) {
        ShowStatusNotice(L"Cursor outside game rect");
        return;
    }

    const int width = max(0, g_gameRect.right - g_gameRect.left);
    const int height = max(0, g_gameRect.bottom - g_gameRect.top);
    const int cursorX = static_cast<int>(cursorXF);
    const int cursorY = static_cast<int>(cursorYF);

    uintptr_t targetServerUnit = 0;
    if (!FindServerUnitByTypeAndId(
        currentGame,
        g_memorySource.hover.hoveredUnitType,
        g_memorySource.hover.hoveredUnitId,
        targetServerUnit
    )) {
        ShowStatusNotice(L"Position probe target server unit missing");
        return;
    }

    uintptr_t targetClientUnit = 0;
    FindUnitByTypeAndId(g_memorySource.hover.hoveredUnitType, g_memorySource.hover.hoveredUnitId, targetClientUnit);

    uintptr_t playerServerUnit = 0;
    uintptr_t playerClientUnit = 0;
    uint32_t playerUnitId = UINT32_MAX;
    if (FindFirstServerUnitByType(currentGame, 0, playerServerUnit)) {
        ReadRemoteValue(playerServerUnit + D2_UNIT_OFFSET_ID, playerUnitId);
        if (playerUnitId != UINT32_MAX) {
            FindUnitByTypeAndId(0, playerUnitId, playerClientUnit);
        }
    }

    int hp = -1;
    int maxHp = -1;
    TryReadUnitHp(targetServerUnit, hp, maxHp);

    std::vector<uint8_t> targetServerSnapshot;
    std::vector<uint8_t> targetClientSnapshot;
    std::vector<uint8_t> playerServerSnapshot;
    std::vector<uint8_t> playerClientSnapshot;
    ReadRemoteBytesBestEffort(targetServerUnit, D2_EXTENDED_UNIT_SNAPSHOT_BYTES, targetServerSnapshot);
    ReadRemoteBytesBestEffort(targetClientUnit, D2_EXTENDED_UNIT_SNAPSHOT_BYTES, targetClientSnapshot);
    ReadRemoteBytesBestEffort(playerServerUnit, D2_EXTENDED_UNIT_SNAPSHOT_BYTES, playerServerSnapshot);
    ReadRemoteBytesBestEffort(playerClientUnit, D2_EXTENDED_UNIT_SNAPSHOT_BYTES, playerClientSnapshot);

    std::wstring text = L"Position probe marker (F6)";
    text += L"\nCursor: ";
    text += std::to_wstring(cursorX);
    text += L",";
    text += std::to_wstring(cursorY);
    text += L" overlay=";
    text += std::to_wstring(width);
    text += L"x";
    text += std::to_wstring(height);
    text += L"\nTarget: ";
    text += std::to_wstring(g_memorySource.hover.hoveredUnitType);
    text += L":";
    text += std::to_wstring(g_memorySource.hover.hoveredUnitId);
    text += L" hp=";
    text += std::to_wstring(hp);
    text += L"/";
    text += std::to_wstring(maxHp);
    text += L" server=";
    text += FormatHex(targetServerUnit);
    text += L" client=";
    text += FormatHex(targetClientUnit);
    text += L"\nPlayer: ";
    text += std::to_wstring(playerUnitId);
    text += L" server=";
    text += FormatHex(playerServerUnit);
    text += L" client=";
    text += FormatHex(playerClientUnit);
    bool worldCoordOffsetCalibrated = false;
    text += L"\nScreen lock: ";
    if (!g_config.useScreenAnchorCandidates) {
        text += L"off";
    }
    else if (g_screenAnchorModel.ready) {
        text += FormatScreenAnchorDescriptor(g_screenAnchorModel.descriptor);
        text += L" samples=";
        text += std::to_wstring(g_screenAnchorModel.sampleCount);
        text += L" avgErr=";
        text += std::to_wstring(static_cast<int>(g_screenAnchorModel.averageError));
        text += L" lastErr=";
        text += std::to_wstring(static_cast<int>(g_screenAnchorModel.lastError));
        text += L" units=";
        text += std::to_wstring(g_screenAnchorModel.distinctUnitCount);
        text += L" span=";
        text += std::to_wstring(static_cast<int>(g_screenAnchorModel.movementSpan));
        if (g_screenAnchorModel.failedUnitCount > 0 || g_screenAnchorModel.farUnitCount > 0) {
            text += L" fail=";
            text += std::to_wstring(g_screenAnchorModel.failedUnitCount);
            text += L" far=";
            text += std::to_wstring(g_screenAnchorModel.farUnitCount);
        }
        float currentLockX = 0.0f;
        float currentLockY = 0.0f;
        if (TryReadScreenAnchorDescriptor(
            g_screenAnchorModel.descriptor,
            targetServerUnit,
            targetClientUnit,
            width,
            height,
            currentLockX,
            currentLockY
        )) {
            const float dx = currentLockX - cursorXF;
            const float dy = currentLockY - cursorYF;
            const int currentError = static_cast<int>(std::lround(std::sqrt((dx * dx) + (dy * dy))));
            text += L" current=";
            text += std::to_wstring(static_cast<int>(std::lround(currentLockX)));
            text += L",";
            text += std::to_wstring(static_cast<int>(std::lround(currentLockY)));
            text += L" currentErr=";
            text += std::to_wstring(currentError);
        }
        else {
            text += L" current=read-failed";
        }
    }
    else {
        text += L"learning hovers=";
        text += std::to_wstring(g_screenAnchorHoverSamples);
        text += L" candidates=";
        text += std::to_wstring(g_screenAnchorCandidates.size());
    }
    text += L"\nTop screen-lock candidates:\n    ";
    text += FormatTopScreenAnchorCandidates(8);
    text += L"\nWorld coord lock: ";
    if (!g_config.useWorldCoordCandidates) {
        text += L"off";
    }
    else if (!g_config.learnWorldProjection) {
        text += L"off";
    }
    else if (g_worldCoordModel.ready) {
        text += FormatWorldCoordDescriptor(g_worldCoordModel.descriptor);
        text += L" samples=";
        text += std::to_wstring(g_worldCoordModel.projection.sampleCount);
        text += L" units=";
        text += std::to_wstring(g_worldCoordModel.distinctUnitCount);
        text += L" rms=";
        text += std::to_wstring(static_cast<int>(g_worldCoordModel.projection.rmsError));
        text += L" span=";
        text += std::to_wstring(static_cast<int>(g_worldCoordModel.movementSpan));

        float worldLockX = 0.0f;
        float worldLockY = 0.0f;
        if (TryProjectUnitWithWorldCoordModel(
            currentGame,
            g_memorySource.hover.hoveredUnitType,
            g_memorySource.hover.hoveredUnitId,
            targetServerUnit,
            targetClientUnit,
            worldLockX,
            worldLockY
        )) {
            const float dx = worldLockX - cursorXF;
            const float dy = worldLockY - cursorYF;
            const int currentError = static_cast<int>(std::lround(std::sqrt((dx * dx) + (dy * dy))));
            text += L" current=";
            text += std::to_wstring(static_cast<int>(std::lround(worldLockX)));
            text += L",";
            text += std::to_wstring(static_cast<int>(std::lround(worldLockY)));
            text += L" currentErr=";
            text += std::to_wstring(currentError);

            constexpr float kWorldCoordScreenOffsetLimit = 400.0f;
            const float rawLockX = worldLockX - g_config.worldCoordScreenOffsetX;
            const float rawLockY = worldLockY - g_config.worldCoordScreenOffsetY;
            const float desiredOffsetX = cursorXF - rawLockX;
            const float desiredOffsetY = cursorYF - rawLockY;
            if (std::isfinite(desiredOffsetX) &&
                std::isfinite(desiredOffsetY) &&
                fabsf(desiredOffsetX) <= kWorldCoordScreenOffsetLimit &&
                fabsf(desiredOffsetY) <= kWorldCoordScreenOffsetLimit) {
                ++g_worldCoordScreenOffsetCalibrationSamples;
                size_t blendDivisor = g_worldCoordScreenOffsetCalibrationSamples;
                if (blendDivisor > 8) {
                    blendDivisor = 8;
                }
                const float blend = 1.0f / static_cast<float>(blendDivisor);
                g_config.worldCoordScreenOffsetX = ClampFloat(
                    (g_config.worldCoordScreenOffsetX * (1.0f - blend)) + (desiredOffsetX * blend),
                    -kWorldCoordScreenOffsetLimit,
                    kWorldCoordScreenOffsetLimit
                );
                g_config.worldCoordScreenOffsetY = ClampFloat(
                    (g_config.worldCoordScreenOffsetY * (1.0f - blend)) + (desiredOffsetY * blend),
                    -kWorldCoordScreenOffsetLimit,
                    kWorldCoordScreenOffsetLimit
                );
                const bool savedX = WriteConfigFloat(L"Memory", L"WorldCoordScreenOffsetX", g_config.worldCoordScreenOffsetX);
                const bool savedY = WriteConfigFloat(L"Memory", L"WorldCoordScreenOffsetY", g_config.worldCoordScreenOffsetY);
                worldCoordOffsetCalibrated = true;
                text += L" desiredOffset=";
                text += std::to_wstring(static_cast<int>(std::lround(desiredOffsetX)));
                text += L",";
                text += std::to_wstring(static_cast<int>(std::lround(desiredOffsetY)));
                text += L" calibratedOffset=";
                text += std::to_wstring(static_cast<int>(std::lround(g_config.worldCoordScreenOffsetX)));
                text += L",";
                text += std::to_wstring(static_cast<int>(std::lround(g_config.worldCoordScreenOffsetY)));
                text += L" samples=";
                text += std::to_wstring(g_worldCoordScreenOffsetCalibrationSamples);
                text += (savedX && savedY) ? L" saved=1" : L" saved=0";
            }
            else {
                text += L" calibration=skipped";
            }
        }
        else {
            text += L" current=read-failed";
        }
    }
    else {
        text += L"learning hovers=";
        text += std::to_wstring(g_worldCoordHoverSamples);
        text += L" candidates=";
        text += std::to_wstring(g_worldCoordCandidates.size());
    }
    text += L"\nTop world-coord candidates:\n    ";
    text += FormatTopWorldCoordCandidates(8);

    AppendPositionProbeSnapshot(text, L"Target client", targetClientSnapshot, cursorX, cursorY, width, height);
    AppendPositionProbeSnapshot(text, L"Target server", targetServerSnapshot, cursorX, cursorY, width, height);
    AppendPositionProbeSnapshot(text, L"Player client", playerClientSnapshot, cursorX, cursorY, width, height);
    AppendPositionProbeSnapshot(text, L"Player server", playerServerSnapshot, cursorX, cursorY, width, height);

    AppendLogBlock(text);
    ShowStatusNotice(worldCoordOffsetCalibrated ? L"Position probe calibrated" : L"Position probe written");
}

static void LogDamageEvent(
    int damage,
    int oldHp,
    int newHp,
    int maxHp,
    uintptr_t currentGame,
    uintptr_t serverUnitAddress,
    uintptr_t clientUnitAddress,
    uintptr_t playerUnitAddress,
    uintptr_t playerClientUnitAddress,
    const std::wstring& targetServerUnitDiffs,
    const std::vector<uint8_t>& beforeClientUnit,
    const std::vector<uint8_t>& afterClientUnit,
    const std::wstring& clientUnitDiffs,
    const std::wstring& playerUnitDiffs,
    const std::wstring& playerClientUnitDiffs
) {
    if (!g_config.logEnabled) return;

    std::wstring text = L"Damage event";
    text += L"\nAmount: ";
    text += std::to_wstring(damage);
    text += L"\nHP: ";
    text += std::to_wstring(oldHp);
    text += L" -> ";
    text += std::to_wstring(newHp);
    text += L"/";
    text += std::to_wstring(maxHp);
    text += L"\nKind: normal critSource=none";
    text += L"\nServerUnit: ";
    text += FormatHex(serverUnitAddress);
    text += L"\nClientUnit: ";
    text += FormatHex(clientUnitAddress);
    text += L"\nTarget server unit diff: ";
    text += targetServerUnitDiffs.empty() ? L"unavailable" : targetServerUnitDiffs;
    text += L"\nPlayerUnit: ";
    text += FormatHex(playerUnitAddress);
    text += L"\nPlayerClientUnit: ";
    text += FormatHex(playerClientUnitAddress);
    text += L"\nPlayer crit stats: ";
    text += BuildPlayerCritStatText(currentGame);
    text += L"\nCandidate +0x110: ";
    text += FormatSnapshotDwordChange(beforeClientUnit, afterClientUnit, D2_CLIENT_HITCLASS_CANDIDATE_OFFSET);
    text += L"\nFlags +0x124: ";
    text += FormatSnapshotDwordChange(beforeClientUnit, afterClientUnit, D2_CLIENT_UNIT_FLAGS_CANDIDATE_OFFSET);
    text += L"\nFlagsEx +0x128: ";
    text += FormatSnapshotDwordChange(beforeClientUnit, afterClientUnit, D2_CLIENT_UNIT_FLAGS_EX_CANDIDATE_OFFSET);
    text += L"\nClient unit diff: ";
    text += clientUnitDiffs.empty() ? L"unavailable" : clientUnitDiffs;
    text += L"\nPlayer server unit diff: ";
    text += playerUnitDiffs.empty() ? L"unavailable" : playerUnitDiffs;
    text += L"\nPlayer client unit diff: ";
    text += playerClientUnitDiffs.empty() ? L"unavailable" : playerClientUnitDiffs;

    AppendLogBlock(text);
}

static void QueueRealDamageAtTarget(
    int amount,
    DamageKind damageKind,
    uint32_t targetUnitType,
    uint32_t targetUnitId,
    uintptr_t targetServerUnit,
    uintptr_t targetClientUnit
) {
    if (amount <= 0) return;

    float x = 0.0f;
    float y = 0.0f;
    WorldPosition targetWorldPosition;
    bool hasTargetWorldPosition = false;
    const bool hasProjectedPosition =
        TryProjectUnitToScreen(
            targetUnitType,
            targetUnitId,
            targetServerUnit,
            targetClientUnit,
            x,
            y,
            &targetWorldPosition,
            &hasTargetWorldPosition
        );

    if (!hasProjectedPosition &&
        !TryGetCursorInGameRect(x, y)) {
        const float width = static_cast<float>(g_gameRect.right - g_gameRect.left);
        const float height = static_cast<float>(g_gameRect.bottom - g_gameRect.top);
        x = width * 0.5f;
        y = height * 0.45f;
    }

    DamageEvent event{};
    event.amount = amount;
    event.screenX = x;
    event.screenY = y;
    event.damageKind = damageKind;
    event.durationSeconds = damageKind == DamageKind::Normal ? g_config.lifetimeSeconds : g_config.critLifetimeSeconds;
    event.createdAt = std::chrono::steady_clock::now();
    event.targetUnitType = targetUnitType;
    event.targetUnitId = targetUnitId;
    event.targetServerUnit = targetServerUnit;
    event.targetClientUnit = targetClientUnit;
    event.hasTargetWorldPosition = hasTargetWorldPosition;
    event.targetWorldPosition = targetWorldPosition;

    QueueDamageEvent(event);
}

static void QueueRealDamageAtCursor(int amount, DamageKind damageKind) {
    QueueRealDamageAtTarget(amount, damageKind, UINT32_MAX, UINT32_MAX, 0, 0);
}

static std::wstring BuildMonsterTrackingStatus() {
    std::wstring status = L"all monsters scanned=";
    status += std::to_wstring(g_memorySource.monsterScanCount);
    status += L" tracked=";
    status += std::to_wstring(g_memorySource.monsterTrackedCount);
    return status;
}

static TrackedMonsterHp* FindTrackedMonsterHp(uint32_t unitId, uintptr_t serverUnit) {
    for (auto& tracked : g_memorySource.trackedMonsters) {
        if (tracked.unitId == unitId && tracked.serverUnit == serverUnit) {
            return &tracked;
        }
    }

    return nullptr;
}

static void RemoveStaleMonsterHpTracks(uint64_t currentGeneration) {
    g_memorySource.trackedMonsters.erase(
        std::remove_if(
            g_memorySource.trackedMonsters.begin(),
            g_memorySource.trackedMonsters.end(),
            [currentGeneration](const TrackedMonsterHp& tracked) {
                return tracked.seenGeneration != currentGeneration;
            }
        ),
        g_memorySource.trackedMonsters.end()
    );
}

static void UpdateHoveredMonsterStatus(uintptr_t currentGame) {
    g_memorySource.hoveredUnitAddress = 0;
    g_memorySource.hoveredClientUnitAddress = 0;
    g_memorySource.hoveredHp = -1;
    g_memorySource.hoveredMaxHp = -1;

    if (!g_memorySource.hoverReady ||
        !g_memorySource.hover.isHovered ||
        g_memorySource.hover.hoveredUnitType != D2_UNIT_MONSTER ||
        !IsLikelyRemotePointer(currentGame)) {
        g_memorySource.hpReady = false;
        g_memorySource.hpStatus = BuildMonsterTrackingStatus();
        return;
    }

    uintptr_t unitAddress = 0;
    if (!FindServerUnitByTypeAndId(
        currentGame,
        g_memorySource.hover.hoveredUnitType,
        g_memorySource.hover.hoveredUnitId,
        unitAddress
    )) {
        g_memorySource.hpReady = false;
        g_memorySource.hpStatus = BuildMonsterTrackingStatus() + L"; hover server unit not found";
        return;
    }

    int hp = -1;
    int maxHp = -1;
    if (!TryReadUnitHp(unitAddress, hp, maxHp)) {
        g_memorySource.hpReady = false;
        g_memorySource.hoveredUnitAddress = unitAddress;
        g_memorySource.hpStatus = BuildMonsterTrackingStatus() + L"; hover hp not found";
        return;
    }

    uintptr_t clientUnitAddress = 0;
    FindUnitByTypeAndId(g_memorySource.hover.hoveredUnitType, g_memorySource.hover.hoveredUnitId, clientUnitAddress);

    g_memorySource.hoveredUnitAddress = unitAddress;
    g_memorySource.hoveredClientUnitAddress = clientUnitAddress;
    g_memorySource.hoveredHp = hp;
    g_memorySource.hoveredMaxHp = maxHp;
    g_memorySource.hpReady = true;
    g_memorySource.hpStatus = BuildMonsterTrackingStatus();
}

static void PollAllMonsterHp(uintptr_t currentGame) {
    if (!IsLikelyRemotePointer(currentGame)) {
        ClearTrackedPlayerCombatSnapshots();
        ClearMonsterHpTracking();
        ResetHpTracking(L"no active game");
        return;
    }

    if (g_memorySource.trackedMonsterGame != currentGame) {
        ClearMonsterHpTracking();
        g_memorySource.trackedMonsterGame = currentGame;
    }

    uintptr_t playerUnitAddress = 0;
    uint32_t playerUnitId = UINT32_MAX;
    uintptr_t playerClientUnitAddress = 0;
    std::vector<uint8_t> playerUnitSnapshot;
    std::vector<uint8_t> playerClientUnitSnapshot;
    const bool playerSnapshotReady = ReadPlayerCombatSnapshots(
        currentGame,
        playerUnitAddress,
        playerUnitId,
        playerClientUnitAddress,
        playerUnitSnapshot,
        playerClientUnitSnapshot
    );
    const bool sameTrackedPlayer =
        playerSnapshotReady &&
        g_memorySource.trackedPlayerGame == currentGame &&
        g_memorySource.trackedPlayerUnitId == playerUnitId &&
        g_memorySource.trackedPlayerUnitAddress == playerUnitAddress;

    std::vector<ServerUnitEntry> monsters;
    if (!EnumerateServerUnitsByType(currentGame, D2_UNIT_MONSTER, monsters, MONSTER_TRACK_MAX_UNITS)) {
        ResetHpTracking(L"monster table unavailable");
        ClearMonsterHpTracking();
        if (playerSnapshotReady) {
            StoreTrackedPlayerCombatSnapshots(
                currentGame,
                playerUnitId,
                playerUnitAddress,
                playerClientUnitAddress,
                playerUnitSnapshot,
                playerClientUnitSnapshot
            );
        }
        return;
    }

    const uint64_t generation = ++g_memorySource.monsterScanGeneration;
    size_t readableMonsterCount = 0;

    for (const auto& monster : monsters) {
        int hp = -1;
        int maxHp = -1;
        if (!TryReadUnitHp(monster.serverUnit, hp, maxHp)) {
            continue;
        }

        ++readableMonsterCount;

        TrackedMonsterHp* tracked = FindTrackedMonsterHp(monster.unitId, monster.serverUnit);
        if (!tracked) {
            TrackedMonsterHp newTracked{};
            newTracked.unitId = monster.unitId;
            newTracked.serverUnit = monster.serverUnit;
            newTracked.hp = hp;
            newTracked.maxHp = maxHp;
            newTracked.seenGeneration = generation;
            g_memorySource.trackedMonsters.push_back(newTracked);
            continue;
        }

        tracked->seenGeneration = generation;

        if (tracked->hp >= 0 && hp < tracked->hp) {
            const int damage = tracked->hp - hp;
            if (damage > 0 && damage <= max(1, tracked->hp)) {
                uintptr_t clientUnitAddress = 0;
                FindUnitByTypeAndId(D2_UNIT_MONSTER, monster.unitId, clientUnitAddress);
                tracked->clientUnit = clientUnitAddress;

                std::vector<uint8_t> serverUnitSnapshot;
                std::vector<uint8_t> clientUnitSnapshot;
                if (g_config.logEnabled || g_config.hitRecorderEnabled) {
                    ReadRemoteBytes(monster.serverUnit, D2_SERVER_UNIT_SNAPSHOT_BYTES, serverUnitSnapshot);
                    if (clientUnitAddress != 0) {
                        ReadRemoteBytes(clientUnitAddress, D2_CLIENT_UNIT_SNAPSHOT_BYTES, clientUnitSnapshot);
                    }
                }

                const uint64_t recorderSampleId = RecordHitRecorderSample(
                    currentGame,
                    D2_UNIT_MONSTER,
                    monster.unitId,
                    monster.serverUnit,
                    clientUnitAddress,
                    playerUnitAddress,
                    playerClientUnitAddress,
                    hp,
                    maxHp,
                    serverUnitSnapshot,
                    clientUnitSnapshot,
                    playerUnitSnapshot,
                    playerClientUnitSnapshot
                );

                const DamageKind damageKind = DamageKind::Normal;
                g_memorySource.lastDamageAmount = damage;
                g_memorySource.lastDamageKind = damageKind;
                g_memorySource.lastClientUnitDiffs = L"unavailable (all-monster HP tracker stores previous HP only)";

                const std::wstring targetServerUnitDiffs = L"unavailable (all-monster HP tracker stores previous HP only)";
                const std::wstring playerUnitDiffs =
                    sameTrackedPlayer && !g_memorySource.trackedPlayerUnitSnapshot.empty() && !playerUnitSnapshot.empty()
                        ? FormatSnapshotDwordDiffs(g_memorySource.trackedPlayerUnitSnapshot, playerUnitSnapshot, 24, false)
                        : L"unavailable";
                const std::wstring playerClientUnitDiffs =
                    sameTrackedPlayer && !g_memorySource.trackedPlayerClientUnitSnapshot.empty() && !playerClientUnitSnapshot.empty()
                        ? FormatSnapshotDwordDiffs(g_memorySource.trackedPlayerClientUnitSnapshot, playerClientUnitSnapshot, 24, true)
                        : L"unavailable";

                LogDamageEvent(
                    damage,
                    tracked->hp,
                    hp,
                    maxHp,
                    currentGame,
                    monster.serverUnit,
                    clientUnitAddress,
                    playerUnitAddress,
                    playerClientUnitAddress,
                    targetServerUnitDiffs,
                    std::vector<uint8_t>{},
                    clientUnitSnapshot,
                    g_memorySource.lastClientUnitDiffs,
                    playerUnitDiffs,
                    playerClientUnitDiffs
                );
                LogHitRecorderDamage(
                    recorderSampleId,
                    damage,
                    tracked->hp,
                    hp,
                    maxHp,
                    D2_UNIT_MONSTER,
                    monster.unitId
                );
                QueueRealDamageAtTarget(
                    damage,
                    damageKind,
                    D2_UNIT_MONSTER,
                    monster.unitId,
                    monster.serverUnit,
                    clientUnitAddress
                );
            }
        }

        tracked->hp = hp;
        tracked->maxHp = maxHp;
    }

    RemoveStaleMonsterHpTracks(generation);

    g_memorySource.monsterScanCount = readableMonsterCount;
    g_memorySource.monsterTrackedCount = g_memorySource.trackedMonsters.size();
    g_memorySource.status = L"reading all monsters";
    UpdateHoveredMonsterStatus(currentGame);

    if (playerSnapshotReady) {
        StoreTrackedPlayerCombatSnapshots(
            currentGame,
            playerUnitId,
            playerUnitAddress,
            playerClientUnitAddress,
            playerUnitSnapshot,
            playerClientUnitSnapshot
        );
    }
}

static void PollHoveredUnitHp(uintptr_t currentGame) {
    if (!IsLikelyRemotePointer(currentGame)) {
        ClearTrackedPlayerCombatSnapshots();
        ResetHpTracking(L"no active game");
        return;
    }

    uintptr_t playerUnitAddress = 0;
    uint32_t playerUnitId = UINT32_MAX;
    uintptr_t playerClientUnitAddress = 0;
    std::vector<uint8_t> playerUnitSnapshot;
    std::vector<uint8_t> playerClientUnitSnapshot;
    const bool playerSnapshotReady = ReadPlayerCombatSnapshots(
        currentGame,
        playerUnitAddress,
        playerUnitId,
        playerClientUnitAddress,
        playerUnitSnapshot,
        playerClientUnitSnapshot
    );
    const bool sameTrackedPlayer =
        playerSnapshotReady &&
        g_memorySource.trackedPlayerGame == currentGame &&
        g_memorySource.trackedPlayerUnitId == playerUnitId &&
        g_memorySource.trackedPlayerUnitAddress == playerUnitAddress;

    if (!g_memorySource.hoverReady || g_memorySource.hover.hoveredUnitType != D2_UNIT_MONSTER) {
        if (playerSnapshotReady) {
            StoreTrackedPlayerCombatSnapshots(
                currentGame,
                playerUnitId,
                playerUnitAddress,
                playerClientUnitAddress,
                playerUnitSnapshot,
                playerClientUnitSnapshot
            );
        }
        ResetHpTracking(L"no monster hover");
        return;
    }

    uintptr_t unitAddress = 0;
    if (!FindServerUnitByTypeAndId(
        currentGame,
        g_memorySource.hover.hoveredUnitType,
        g_memorySource.hover.hoveredUnitId,
        unitAddress
    )) {
        if (playerSnapshotReady) {
            StoreTrackedPlayerCombatSnapshots(
                currentGame,
                playerUnitId,
                playerUnitAddress,
                playerClientUnitAddress,
                playerUnitSnapshot,
                playerClientUnitSnapshot
            );
        }
        ResetHpTracking(L"server unit not found");
        return;
    }

    int hp = -1;
    int maxHp = -1;
    if (!TryReadUnitHp(unitAddress, hp, maxHp)) {
        if (playerSnapshotReady) {
            StoreTrackedPlayerCombatSnapshots(
                currentGame,
                playerUnitId,
                playerUnitAddress,
                playerClientUnitAddress,
                playerUnitSnapshot,
                playerClientUnitSnapshot
            );
        }
        ResetHpTracking(L"server hp stat not found");
        g_memorySource.hoveredUnitAddress = unitAddress;
        return;
    }

    std::vector<uint8_t> serverUnitSnapshot;
    ReadRemoteBytes(unitAddress, D2_SERVER_UNIT_SNAPSHOT_BYTES, serverUnitSnapshot);

    uintptr_t clientUnitAddress = 0;
    std::vector<uint8_t> clientUnitSnapshot;
    if (FindUnitByTypeAndId(
        g_memorySource.hover.hoveredUnitType,
        g_memorySource.hover.hoveredUnitId,
        clientUnitAddress
    )) {
        ReadRemoteBytes(clientUnitAddress, D2_CLIENT_UNIT_SNAPSHOT_BYTES, clientUnitSnapshot);
    }

    const uint64_t recorderSampleId = RecordHitRecorderSample(
        currentGame,
        g_memorySource.hover.hoveredUnitType,
        g_memorySource.hover.hoveredUnitId,
        unitAddress,
        clientUnitAddress,
        playerUnitAddress,
        playerClientUnitAddress,
        hp,
        maxHp,
        serverUnitSnapshot,
        clientUnitSnapshot,
        playerUnitSnapshot,
        playerClientUnitSnapshot
    );

    const bool sameTrackedUnit =
        g_memorySource.trackedGame == currentGame &&
        g_memorySource.trackedUnitType == g_memorySource.hover.hoveredUnitType &&
        g_memorySource.trackedUnitId == g_memorySource.hover.hoveredUnitId;

    if (!sameTrackedUnit) {
        g_memorySource.trackedGame = currentGame;
        g_memorySource.trackedUnitType = g_memorySource.hover.hoveredUnitType;
        g_memorySource.trackedUnitId = g_memorySource.hover.hoveredUnitId;
        g_memorySource.trackedHp = hp;
        g_memorySource.trackedServerUnitAddress = unitAddress;
        g_memorySource.trackedServerUnitSnapshot = serverUnitSnapshot;
        g_memorySource.trackedClientUnitAddress = clientUnitAddress;
        g_memorySource.trackedClientUnitSnapshot = clientUnitSnapshot;
    }
    else if (g_memorySource.trackedHp >= 0 && hp < g_memorySource.trackedHp) {
        const int damage = g_memorySource.trackedHp - hp;
        if (damage > 0 && damage <= max(1, g_memorySource.trackedHp)) {
            const DamageKind damageKind = DamageKind::Normal;
            g_memorySource.lastDamageAmount = damage;
            g_memorySource.lastDamageKind = damageKind;
            g_memorySource.lastClientUnitDiffs =
                !g_memorySource.trackedClientUnitSnapshot.empty() && !clientUnitSnapshot.empty()
                    ? FormatClientUnitDiffs(g_memorySource.trackedClientUnitSnapshot, clientUnitSnapshot)
                    : L"unavailable";
            const std::wstring targetServerUnitDiffs =
                g_memorySource.trackedServerUnitAddress == unitAddress &&
                !g_memorySource.trackedServerUnitSnapshot.empty() && !serverUnitSnapshot.empty()
                    ? FormatSnapshotDwordDiffs(g_memorySource.trackedServerUnitSnapshot, serverUnitSnapshot, 32, true)
                    : L"unavailable";
            const std::wstring playerUnitDiffs =
                sameTrackedPlayer && !g_memorySource.trackedPlayerUnitSnapshot.empty() && !playerUnitSnapshot.empty()
                    ? FormatSnapshotDwordDiffs(g_memorySource.trackedPlayerUnitSnapshot, playerUnitSnapshot, 24, false)
                    : L"unavailable";
            const std::wstring playerClientUnitDiffs =
                sameTrackedPlayer && !g_memorySource.trackedPlayerClientUnitSnapshot.empty() && !playerClientUnitSnapshot.empty()
                    ? FormatSnapshotDwordDiffs(g_memorySource.trackedPlayerClientUnitSnapshot, playerClientUnitSnapshot, 24, true)
                    : L"unavailable";
            LogDamageEvent(
                damage,
                g_memorySource.trackedHp,
                hp,
                maxHp,
                currentGame,
                unitAddress,
                clientUnitAddress,
                playerUnitAddress,
                playerClientUnitAddress,
                targetServerUnitDiffs,
                g_memorySource.trackedClientUnitSnapshot,
                clientUnitSnapshot,
                g_memorySource.lastClientUnitDiffs,
                playerUnitDiffs,
                playerClientUnitDiffs
            );
            LogHitRecorderDamage(
                recorderSampleId,
                damage,
                g_memorySource.trackedHp,
                hp,
                maxHp,
                g_memorySource.hover.hoveredUnitType,
                g_memorySource.hover.hoveredUnitId
            );
            QueueRealDamageAtCursor(damage, damageKind);
        }
        g_memorySource.trackedHp = hp;
    }
    else if (hp > g_memorySource.trackedHp) {
        g_memorySource.trackedHp = hp;
    }

    if (clientUnitAddress != 0 && !clientUnitSnapshot.empty()) {
        g_memorySource.trackedClientUnitAddress = clientUnitAddress;
        g_memorySource.trackedClientUnitSnapshot = clientUnitSnapshot;
    }

    if (!serverUnitSnapshot.empty()) {
        g_memorySource.trackedServerUnitAddress = unitAddress;
        g_memorySource.trackedServerUnitSnapshot = serverUnitSnapshot;
    }

    if (playerSnapshotReady) {
        StoreTrackedPlayerCombatSnapshots(
            currentGame,
            playerUnitId,
            playerUnitAddress,
            playerClientUnitAddress,
            playerUnitSnapshot,
            playerClientUnitSnapshot
        );
    }

    g_memorySource.hoveredUnitAddress = unitAddress;
    g_memorySource.hoveredClientUnitAddress = clientUnitAddress;
    g_memorySource.hoveredHp = hp;
    g_memorySource.hoveredMaxHp = maxHp;
    g_memorySource.hpReady = true;
    g_memorySource.hpStatus = L"reading server hp";
}

static void PollMemorySource(float dt) {
    if (!g_hasGameRect || !g_gameWnd) {
        if (g_memorySource.connected) {
            CloseMemorySource();
        }
        g_memorySource.status = L"waiting for D2R window";
        g_positionDebugMarkers.clear();
        return;
    }

    if (!g_memorySource.connected) {
        g_memorySource.reconnectSeconds += dt;
        if (g_memorySource.reconnectSeconds < 1.0f) return;

        g_memorySource.reconnectSeconds = 0.0f;
        AttachMemorySource(g_gameWnd);
        return;
    }

    DWORD processId = 0;
    GetWindowThreadProcessId(g_gameWnd, &processId);
    if (processId == 0 || processId != g_memorySource.processId) {
        CloseMemorySource();
        g_memorySource.status = L"process changed";
        return;
    }

    if (!g_memorySource.patternsReady && g_memorySource.status.find(L"patterns") == 0) {
        g_memorySource.patternRetrySeconds += dt;
        if (g_memorySource.patternRetrySeconds < 2.0f) return;
    }
    else {
        g_memorySource.patternRetrySeconds = 0.0f;
    }

    if (!g_memorySource.patternsReady) {
        g_memorySource.patternRetrySeconds = 0.0f;
    }

    if (!InitializeMemoryPatterns()) {
        return;
    }

    if (g_memorySource.currentSinglePlayerGameAddress == 0) {
        g_memorySource.gamePatternRetrySeconds += dt;
        if (g_memorySource.gamePatternRetrySeconds >= 2.0f) {
            g_memorySource.gamePatternRetrySeconds = 0.0f;
            if (TryRefreshCurrentGamePatternAddress()) {
                g_memorySource.status = L"game pattern resolved";
            }
        }
    }
    else {
        g_memorySource.gamePatternRetrySeconds = 0.0f;
    }

    g_memorySource.pollSeconds += dt;
    if (g_memorySource.pollSeconds < g_config.memoryPollSeconds) return;
    g_memorySource.pollSeconds = 0.0f;

    D2MouseHoverMemory hover{};
    if (!ReadRemoteValue(g_memorySource.mouseHoverAddress, hover)) {
        g_memorySource.hoverReady = false;
        g_memorySource.status = L"hover read failed ";
        g_memorySource.status += FormatHex(g_memorySource.mouseHoverAddress);
        g_memorySource.status += L" err=";
        g_memorySource.status += std::to_wstring(GetLastError());
        return;
    }

    uintptr_t currentGame = 0;
    if (g_memorySource.currentSinglePlayerGameAddress != 0) {
        if (!ReadRemoteValue(g_memorySource.currentSinglePlayerGameAddress, currentGame)) {
            g_memorySource.status = L"game ptr read failed ";
            g_memorySource.status += FormatHex(g_memorySource.currentSinglePlayerGameAddress);
            g_memorySource.status += L" err=";
            g_memorySource.status += std::to_wstring(GetLastError());
            return;
        }
    }

    g_memorySource.hover = hover;
    g_memorySource.currentSinglePlayerGame = currentGame;
    g_memorySource.hoverReady = true;
    g_memorySource.status = g_memorySource.currentSinglePlayerGameAddress != 0 ? L"reading" : L"reading hover only";

    if (g_config.trackAllMonsters) {
        PollAllMonsterHp(currentGame);
    }
    else {
        PollHoveredUnitHp(currentGame);
    }

    UpdateWorldProjectionCalibration();
    UpdateWorldCoordProjectionTracking();
    UpdateScreenAnchorCandidateTracking();
    UpdatePositionDebugMarkers();
}

#undef g_config
#undef g_gameWnd
#undef g_gameRect
#undef g_hasGameRect
#undef AppendLogBlock
#undef QueueDamageEvent
#undef TryGetCursorInGameRect
#undef ShowStatusNotice
#undef BuildMemoryLogText
#undef CloseMemorySource
#undef FormatHex
#undef IsLikelyRemotePointer
#undef DamageKindName
#undef GetScreenDistance
#undef TryReadPlayerWorldPosition
#undef ProjectWorldPositionRelativeToPlayer
#undef IsLargeProjectionOutlier
#undef TryProjectUnitToScreenFromPlayer
#undef TryProjectUnitToScreen
#undef FormatWorldCoordDescriptor
#undef FormatTopWorldCoordCandidates
#undef FormatScreenAnchorDescriptor
#undef FormatTopScreenAnchorCandidates
#undef PollMemorySource
#undef WritePositionProbeMarker
#undef WriteHitRecorderMarker

void ConfigureMemoryScannerCallbacks(const MemoryScannerCallbacks& callbacks) {
    g_callbacks = callbacks;
}

void SetMemoryScannerFrameContext(const MemoryScannerFrameContext& context) {
    g_frameContext = context;
}

void PollMemorySource(const MemoryScannerFrameContext& context, float dt) {
    SetMemoryScannerFrameContext(context);
    PollMemorySourceCore(dt);
}

void CloseMemorySource() {
    CloseMemorySourceCore();
}

int RunMemoryScanOnce(HWND gameWnd, OverlayConfig& config) {
    MemoryScannerFrameContext context{};
    context.gameWnd = gameWnd;
    context.hasGameRect = gameWnd != nullptr && IsWindow(gameWnd);
    context.config = &config;
    if (context.hasGameRect) {
        GetWindowRect(gameWnd, &context.gameRect);
    }
    SetMemoryScannerFrameContext(context);

    std::wstring text = L"One-shot memory scan";

    if (!gameWnd || !IsWindow(gameWnd)) {
        text += L"\nD2R window: not found";

        DWORD processId = 0;
        if (!FindD2RProcessId(processId)) {
            text += L"\nD2R process: not found";
            ScannerAppendLogBlock(text);
            return 2;
        }

        text += L"\nD2R process: found pid=";
        text += std::to_wstring(processId);

        if (!AttachMemorySourceProcess(processId)) {
            text += L"\nMemory: ";
            text += g_memorySource.status;
            ScannerAppendLogBlock(text);
            CloseMemorySourceCore();
            return 3;
        }
    }
    else {
        RECT rc{};
        GetWindowRect(gameWnd, &rc);
        text += L"\nD2R window: found ";
        text += std::to_wstring(rc.right - rc.left);
        text += L"x";
        text += std::to_wstring(rc.bottom - rc.top);

        if (!AttachMemorySource(gameWnd)) {
            text += L"\nMemory: ";
            text += g_memorySource.status;
            ScannerAppendLogBlock(text);
            CloseMemorySourceCore();
            return 3;
        }
    }

    text += L"\nPID: ";
    text += std::to_wstring(g_memorySource.processId);
    text += L"\nModule: ";
    text += g_memorySource.moduleName;
    text += L" ";
    text += FormatHexCore(g_memorySource.moduleBase);
    text += L" size=";
    text += std::to_wstring(g_memorySource.moduleSize / (1024 * 1024));
    text += L"MB";

    if (!InitializeMemoryPatterns()) {
        text += L"\nMemory: ";
        text += g_memorySource.status;
        if (!g_memorySource.patternDiagnostics.empty()) {
            text += L"\n";
            text += g_memorySource.patternDiagnostics;
        }
        ScannerAppendLogBlock(text);
        CloseMemorySourceCore();
        return 4;
    }

    text += L"\nPatterns: hover=";
    text += FormatHexCore(g_memorySource.mouseHoverAddress);
    text += L" ";
    text += g_memorySource.mouseHoverPatternName;
    text += L" game=";
    text += g_memorySource.currentSinglePlayerGameAddress != 0
        ? FormatHexCore(g_memorySource.currentSinglePlayerGameAddress)
        : L"missing";
    if (!g_memorySource.currentGamePatternName.empty()) {
        text += L" ";
        text += g_memorySource.currentGamePatternName;
    }
    text += L" unit=";
    text += FormatHexCore(g_memorySource.unitHashTableAddress);
    text += L" ";
    text += g_memorySource.unitHashPatternName;

    uintptr_t currentGame = 0;
    if (g_memorySource.currentSinglePlayerGameAddress != 0) {
        if (!ReadRemoteValue(g_memorySource.currentSinglePlayerGameAddress, currentGame)) {
            text += L"\nCurrent game: read failed ";
            text += FormatHexCore(g_memorySource.currentSinglePlayerGameAddress);
            text += L" err=";
            text += std::to_wstring(GetLastError());
        }
        else {
            text += L"\nCurrent game: ";
            text += FormatHexCore(currentGame);
        }
    }
    else {
        text += L"\nCurrent game: pattern missing";
    }

    if (IsLikelyRemotePointerCore(currentGame)) {
        uintptr_t playerUnit = 0;
        if (FindFirstServerUnitByType(currentGame, 0, playerUnit)) {
            text += L"\nPlayer unit: ";
            text += FormatHexCore(playerUnit);
            text += L"\nPlayer crit stats: ";
            text += BuildPlayerCritStatText(currentGame);
            text += L"\nPlayer stat dump: ";
            text += BuildPlayerStatDump(playerUnit, D2_MAX_STAT_COUNT);
        }
        else {
            text += L"\nPlayer unit: not found";
        }
    }

    D2MouseHoverMemory hover{};
    if (ReadRemoteValue(g_memorySource.mouseHoverAddress, hover)) {
        text += L"\nHover: isHovered=";
        text += std::to_wstring(hover.isHovered);
        text += L" type=";
        text += std::to_wstring(hover.hoveredUnitType);
        text += L" id=";
        text += std::to_wstring(hover.hoveredUnitId);
    }
    else {
        text += L"\nHover: read failed err=";
        text += std::to_wstring(GetLastError());
    }

    ScannerAppendLogBlock(text);
    CloseMemorySourceCore();
    return 0;
}

void WritePositionProbeMarker() {
    WritePositionProbeMarkerCore();
}

void WriteHitRecorderMarker() {
    WriteHitRecorderMarkerCore();
}

std::wstring FormatHex(uintptr_t value) {
    return FormatHexCore(value);
}

bool IsLikelyRemotePointer(uintptr_t address) {
    return IsLikelyRemotePointerCore(address);
}

const wchar_t* DamageKindName(DamageKind damageKind) {
    return DamageKindNameCore(damageKind);
}

float GetScreenDistance(float ax, float ay, float bx, float by) {
    return GetScreenDistanceCore(ax, ay, bx, by);
}

bool TryReadPlayerWorldPosition(uintptr_t currentGame, WorldPosition& position) {
    return TryReadPlayerWorldPositionCore(currentGame, position);
}

bool ProjectWorldPositionRelativeToPlayer(
    const WorldPosition& targetPosition,
    const WorldPosition& playerPosition,
    bool useCalibration,
    float& screenX,
    float& screenY
) {
    return ProjectWorldPositionRelativeToPlayerCore(targetPosition, playerPosition, useCalibration, screenX, screenY);
}

bool IsLargeProjectionOutlier(const DamageNumber& number, float projectedX, float projectedY) {
    return IsLargeProjectionOutlierCore(number, projectedX, projectedY);
}

bool TryProjectUnitToScreenFromPlayer(
    uintptr_t currentGame,
    const WorldPosition& playerPosition,
    uint32_t targetUnitType,
    uint32_t targetUnitId,
    uintptr_t targetServerUnit,
    uintptr_t targetClientUnit,
    float& screenX,
    float& screenY,
    WorldPosition* targetWorldPosition,
    bool* hasTargetWorldPosition
) {
    return TryProjectUnitToScreenFromPlayerCore(
        currentGame,
        playerPosition,
        targetUnitType,
        targetUnitId,
        targetServerUnit,
        targetClientUnit,
        screenX,
        screenY,
        targetWorldPosition,
        hasTargetWorldPosition
    );
}

bool TryProjectUnitToScreen(
    uint32_t targetUnitType,
    uint32_t targetUnitId,
    uintptr_t targetServerUnit,
    uintptr_t targetClientUnit,
    float& screenX,
    float& screenY,
    WorldPosition* targetWorldPosition,
    bool* hasTargetWorldPosition
) {
    return TryProjectUnitToScreenCore(
        targetUnitType,
        targetUnitId,
        targetServerUnit,
        targetClientUnit,
        screenX,
        screenY,
        targetWorldPosition,
        hasTargetWorldPosition
    );
}

std::wstring FormatWorldCoordDescriptor(const WorldCoordDescriptor& descriptor) {
    return FormatWorldCoordDescriptorCore(descriptor);
}

std::wstring FormatTopWorldCoordCandidates(size_t maxCandidates) {
    return FormatTopWorldCoordCandidatesCore(maxCandidates);
}

std::wstring FormatScreenAnchorDescriptor(const ScreenAnchorDescriptor& descriptor) {
    return FormatScreenAnchorDescriptorCore(descriptor);
}

std::wstring FormatTopScreenAnchorCandidates(size_t maxCandidates) {
    return FormatTopScreenAnchorCandidatesCore(maxCandidates);
}

