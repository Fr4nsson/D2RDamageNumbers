#pragma once

#include <windows.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

enum class DamageKind {
    Normal,
    Critical
};

struct WorldPosition {
    float x = 0.0f;
    float y = 0.0f;
};

struct DamageEvent {
    int amount;
    float screenX;
    float screenY;
    DamageKind damageKind;
    float durationSeconds;
    std::chrono::steady_clock::time_point createdAt;
    uint32_t targetUnitType = UINT32_MAX;
    uint32_t targetUnitId = UINT32_MAX;
    uintptr_t targetServerUnit = 0;
    uintptr_t targetClientUnit = 0;
    bool hasTargetWorldPosition = false;
    WorldPosition targetWorldPosition;
};

struct CoalesceTickPop {
    std::wstring text;
    float age;
    float duration;
    float startOffsetX;
    float startOffsetY;
    float endOffsetX;
    float endOffsetY;
};

struct DamageNumber {
    std::wstring text;
    float x;
    float y;
    float anchorX;
    float anchorY;
    float localOffsetX;
    float localOffsetY;
    float animatedOffsetX;
    float animatedOffsetY;
    float drawX;
    float drawY;
    float coalesceIdleSeconds;
    float coalescePulseAge;
    float vx;
    float vy;
    float age;
    float lifetime;
    COLORREF color;
    bool isProminent;
    bool canCoalesce;
    int coalescedAmount;
    int stackLane;
    float prominenceScale;
    std::vector<CoalesceTickPop> coalesceTickPops;
    uint32_t targetUnitType = UINT32_MAX;
    uint32_t targetUnitId = UINT32_MAX;
    uintptr_t targetServerUnit = 0;
    uintptr_t targetClientUnit = 0;
    bool hasLastTargetWorldPosition = false;
    WorldPosition lastTargetWorldPosition;
};
