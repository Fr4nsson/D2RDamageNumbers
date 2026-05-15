#include "DamageNumberSystem.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <random>
#include <string>

#include "DpsTracker.h"
#include "MemoryScanner.h"

namespace DamageNumberSystem {
namespace {

std::vector<DamageNumber> g_numbers;
std::vector<DamageEvent> g_pendingEvents;
std::mt19937 g_rng{ std::random_device{}() };

struct ProjectionFrameCacheEntry {
    uint32_t targetUnitType = UINT32_MAX;
    uint32_t targetUnitId = UINT32_MAX;
    uintptr_t targetServerUnit = 0;
    uintptr_t targetClientUnit = 0;
    bool valid = false;
    float screenX = 0.0f;
    float screenY = 0.0f;
    bool hasTargetWorldPosition = false;
    WorldPosition targetWorldPosition;
};

struct StackPlacement {
    int lane = 0;
    float sideOffset = 0.0f;
    float verticalOffset = 0.0f;
};

int ClampInt(int value, int minValue, int maxValue) {
    if (value < minValue) return minValue;
    if (value > maxValue) return maxValue;
    return value;
}

float ClampFloat(float value, float minValue, float maxValue) {
    if (value < minValue) return minValue;
    if (value > maxValue) return maxValue;
    return value;
}

float RandomFloat(float minValue, float maxValue) {
    std::uniform_real_distribution<float> dist(minValue, maxValue);
    return dist(g_rng);
}

int RandomInt(int minValue, int maxValue) {
    std::uniform_int_distribution<int> dist(minValue, maxValue);
    return dist(g_rng);
}

COLORREF GetDamageColor(const OverlayConfig& config, DamageKind damageKind) {
    switch (damageKind) {
    case DamageKind::Critical:
        return config.criticalColor;
    case DamageKind::Normal:
    default:
        return config.normalColor;
    }
}

bool IsProminentDamage(DamageKind damageKind) {
    return damageKind != DamageKind::Normal;
}

float GetProminenceScale(DamageKind damageKind) {
    switch (damageKind) {
    case DamageKind::Critical:
        return 1.06f;
    case DamageKind::Normal:
    default:
        return 1.0f;
    }
}

int GenerateTestAmount(DamageKind damageKind) {
    int amount = RandomInt(4, 1500);

    switch (damageKind) {
    case DamageKind::Critical:
        return amount * 2;
    case DamageKind::Normal:
    default:
        return amount;
    }
}

std::wstring FormatWholeWithThousands(int amount) {
    std::wstring raw = std::to_wstring(max(0, amount));
    std::wstring result;

    int digitCount = 0;
    for (auto it = raw.rbegin(); it != raw.rend(); ++it) {
        if (digitCount > 0 && digitCount % 3 == 0) {
            result.insert(result.begin(), L',');
        }

        result.insert(result.begin(), *it);
        ++digitCount;
    }

    return result;
}

std::wstring FormatDamageAmount(int amount) {
    static const wchar_t* suffixes[] = { L"", L"k", L"m", L"b", L"t" };

    const int clampedAmount = max(0, amount);
    double value = static_cast<double>(clampedAmount);
    int suffixIndex = 0;

    while (value >= 10000.0 && suffixIndex < 4) {
        value /= 1000.0;
        ++suffixIndex;
    }

    if (suffixIndex == 0) {
        return FormatWholeWithThousands(clampedAmount);
    }

    int roundedTenths = static_cast<int>((value * 10.0) + 0.5);
    if (roundedTenths >= 10000 && suffixIndex < 4) {
        value /= 1000.0;
        ++suffixIndex;
        roundedTenths = static_cast<int>((value * 10.0) + 0.5);
    }

    wchar_t text[32]{};

    if (roundedTenths < 1000 && roundedTenths % 10 != 0) {
        swprintf_s(text, L"%d,%d%s", roundedTenths / 10, roundedTenths % 10, suffixes[suffixIndex]);
    }
    else {
        swprintf_s(text, L"%d%s", (roundedTenths + 5) / 10, suffixes[suffixIndex]);
    }

    return text;
}

DamageKind RandomBurstDamageKind() {
    const int roll = RandomInt(1, 100);

    if (roll <= 70) {
        return DamageKind::Normal;
    }
    return DamageKind::Critical;
}

bool HasDamageTarget(uint32_t unitType, uint32_t unitId, uintptr_t serverUnit, uintptr_t clientUnit) {
    return (unitType != UINT32_MAX && unitId != UINT32_MAX) ||
        IsLikelyRemotePointer(serverUnit) ||
        IsLikelyRemotePointer(clientUnit);
}

bool IsCoalescibleDamageEvent(const OverlayConfig& config, const DamageEvent& event) {
    return config.coalesceSmallDamage &&
        event.damageKind == DamageKind::Normal &&
        event.amount > 0 &&
        event.amount <= config.coalesceMaxDamage &&
        HasDamageTarget(event.targetUnitType, event.targetUnitId, event.targetServerUnit, event.targetClientUnit);
}

bool IsSameDamageTarget(const DamageNumber& number, const DamageEvent& event) {
    if (number.targetUnitType != UINT32_MAX &&
        number.targetUnitId != UINT32_MAX &&
        event.targetUnitType != UINT32_MAX &&
        event.targetUnitId != UINT32_MAX) {
        return number.targetUnitType == event.targetUnitType &&
            number.targetUnitId == event.targetUnitId;
    }

    if (IsLikelyRemotePointer(number.targetServerUnit) &&
        IsLikelyRemotePointer(event.targetServerUnit) &&
        number.targetServerUnit == event.targetServerUnit) {
        return true;
    }

    if (IsLikelyRemotePointer(number.targetClientUnit) &&
        IsLikelyRemotePointer(event.targetClientUnit) &&
        number.targetClientUnit == event.targetClientUnit) {
        return true;
    }

    return false;
}

int GetStackLaneCount(const OverlayConfig& config) {
    return ClampInt(config.stackLanes, 1, 9);
}

float GetStackLaneOffset(const OverlayConfig& config, int lane) {
    if (lane <= 0) return 0.0f;

    const int distance = (lane + 1) / 2;
    const float direction = (lane % 2) == 1 ? -1.0f : 1.0f;
    return direction * static_cast<float>(distance) * config.stackLaneWidth;
}

int GetNearestStackLane(const OverlayConfig& config, float sideOffset, int laneCount) {
    int bestLane = 0;
    float bestDistance = std::abs(sideOffset - GetStackLaneOffset(config, 0));
    for (int lane = 1; lane < laneCount; ++lane) {
        const float distance = std::abs(sideOffset - GetStackLaneOffset(config, lane));
        if (distance < bestDistance) {
            bestLane = lane;
            bestDistance = distance;
        }
    }
    return bestLane;
}

bool IsStackRelatedNumber(const DamageNumber& number, const DamageEvent& event) {
    const bool eventHasTarget = HasDamageTarget(
        event.targetUnitType,
        event.targetUnitId,
        event.targetServerUnit,
        event.targetClientUnit
    );
    const bool numberHasTarget = HasDamageTarget(
        number.targetUnitType,
        number.targetUnitId,
        number.targetServerUnit,
        number.targetClientUnit
    );

    if (eventHasTarget && numberHasTarget) {
        return IsSameDamageTarget(number, event);
    }

    if (!eventHasTarget && !numberHasTarget) {
        return GetScreenDistance(number.anchorX, number.anchorY, event.screenX, event.screenY) <= 72.0f;
    }

    return false;
}

StackPlacement CalculateStackPlacement(const OverlayConfig& config, const DamageEvent& event) {
    StackPlacement placement{};
    if (!config.useStackLanes) return placement;
    if (IsCoalescibleDamageEvent(config, event)) return placement;

    const int laneCount = GetStackLaneCount(config);
    const float reuseSeconds = max(0.05f, config.stackReuseSeconds);

    std::vector<int> laneCounts(static_cast<size_t>(laneCount), 0);
    std::vector<float> laneOldestAge(static_cast<size_t>(laneCount), -1.0f);
    int relatedCount = 0;

    for (const auto& number : g_numbers) {
        if (number.age >= number.lifetime) continue;
        if (number.age > reuseSeconds) continue;
        if (!IsStackRelatedNumber(number, event)) continue;

        ++relatedCount;
        int lane = number.stackLane >= 0 && number.stackLane < laneCount
            ? number.stackLane
            : GetNearestStackLane(config, number.localOffsetX, laneCount);
        laneCounts[static_cast<size_t>(lane)] += 1;
        laneOldestAge[static_cast<size_t>(lane)] = max(laneOldestAge[static_cast<size_t>(lane)], number.age);
    }

    int bestLane = 0;
    for (int lane = 0; lane < laneCount; ++lane) {
        if (laneCounts[static_cast<size_t>(lane)] == 0) {
            bestLane = lane;
            break;
        }

        const int bestCount = laneCounts[static_cast<size_t>(bestLane)];
        const int laneCountValue = laneCounts[static_cast<size_t>(lane)];
        const float bestAge = laneOldestAge[static_cast<size_t>(bestLane)];
        const float laneAge = laneOldestAge[static_cast<size_t>(lane)];
        if (laneCountValue < bestCount ||
            (laneCountValue == bestCount && laneAge > bestAge)) {
            bestLane = lane;
        }
    }

    const int verticalStepCount = laneCount > 0 ? relatedCount / laneCount : 0;
    placement.lane = bestLane;
    placement.sideOffset = GetStackLaneOffset(config, bestLane);
    placement.verticalOffset = min(
        max(0.0f, config.stackMaxYOffset),
        static_cast<float>(verticalStepCount) * config.stackVerticalStep
    );
    return placement;
}

float GetCoalesceWindowSeconds(const OverlayConfig& config) {
    return static_cast<float>(max(50, config.coalesceWindowMs)) / 1000.0f;
}

void AddCoalesceTickPop(const OverlayConfig& config, DamageNumber& number, const DamageEvent& event) {
    if (!config.coalesceTickPop || event.amount <= 0) return;

    CoalesceTickPop tick{};
    tick.text = L"+" + FormatDamageAmount(event.amount);
    tick.age = 0.0f;
    tick.duration = max(0.05f, config.coalesceTickPopSeconds);

    const float distance = max(0.0f, config.coalesceTickPopDistance);
    const float sideSpread = min(14.0f, distance * 0.22f);
    tick.startOffsetX = RandomFloat(-sideSpread, sideSpread);
    tick.startOffsetY = -RandomFloat(distance * 0.65f, distance * 1.15f);
    tick.endOffsetX = tick.startOffsetX * 0.25f;
    tick.endOffsetY = config.coalesceTickPopMergeOffsetY;

    constexpr size_t maxVisibleTickPops = 8;
    if (number.coalesceTickPops.size() >= maxVisibleTickPops) {
        number.coalesceTickPops.erase(number.coalesceTickPops.begin());
    }

    number.coalesceTickPops.push_back(tick);
}

void RefreshCoalescedNumber(const OverlayConfig& config, DamageNumber& number, const DamageEvent& event) {
    number.coalescedAmount += event.amount;
    number.text = FormatDamageAmount(number.coalescedAmount);
    number.coalesceIdleSeconds = 0.0f;
    number.coalescePulseAge = 0.0f;
    AddCoalesceTickPop(config, number, event);

    number.targetUnitType = event.targetUnitType;
    number.targetUnitId = event.targetUnitId;
    number.targetServerUnit = event.targetServerUnit;
    number.targetClientUnit = event.targetClientUnit;
    if (event.hasTargetWorldPosition) {
        number.hasLastTargetWorldPosition = true;
        number.lastTargetWorldPosition = event.targetWorldPosition;
    }

    const float refreshLifetime = max(0.05f, config.coalesceRefreshLifetime);
    const float fadeStart = ClampFloat(config.fadeStart, 0.05f, 0.95f);
    const float visibleLifetime = (number.age + min(0.12f, refreshLifetime)) / fadeStart;
    number.lifetime = max(number.lifetime, max(number.age + refreshLifetime, visibleLifetime));

    number.vx *= 0.35f;
    number.animatedOffsetY = max(number.animatedOffsetY, -28.0f);
}

bool TryCoalesceDamageEvent(const OverlayConfig& config, const DamageEvent& event) {
    if (!IsCoalescibleDamageEvent(config, event)) return false;

    const float windowSeconds = GetCoalesceWindowSeconds(config);
    for (auto it = g_numbers.rbegin(); it != g_numbers.rend(); ++it) {
        DamageNumber& number = *it;
        if (!number.canCoalesce) continue;
        if (number.age >= number.lifetime) continue;
        if (number.coalesceIdleSeconds > windowSeconds) continue;
        if (!IsSameDamageTarget(number, event)) continue;

        RefreshCoalescedNumber(config, number, event);
        return true;
    }

    return false;
}

void SpawnNumberFromEvent(const OverlayConfig& config, const DamageEvent& event) {
    const StackPlacement placement = CalculateStackPlacement(config, event);

    DamageNumber number{};
    number.text = FormatDamageAmount(event.amount);

    number.anchorX = event.screenX;
    number.anchorY = event.screenY;
    number.localOffsetX = placement.sideOffset;
    number.localOffsetY = config.spawnYOffset - placement.verticalOffset;
    number.animatedOffsetX = 0.0f;
    number.animatedOffsetY = 0.0f;
    number.coalesceIdleSeconds = 0.0f;
    number.coalescePulseAge = max(0.01f, config.coalescePulseSeconds);
    number.x = number.anchorX + number.localOffsetX;
    number.y = number.anchorY + number.localOffsetY;
    number.drawX = number.x;
    number.drawY = number.y;
    number.vx = RandomFloat(-config.horizontalDrift, config.horizontalDrift);
    number.vy = -config.floatSpeed * (IsProminentDamage(event.damageKind) ? 1.05f : 1.0f);
    number.age = 0.0f;
    number.lifetime = max(0.10f, event.durationSeconds);
    number.color = GetDamageColor(config, event.damageKind);
    number.isProminent = IsProminentDamage(event.damageKind);
    number.canCoalesce = IsCoalescibleDamageEvent(config, event);
    number.coalescedAmount = event.amount;
    number.stackLane = placement.lane;
    number.prominenceScale = GetProminenceScale(event.damageKind);
    number.targetUnitType = event.targetUnitType;
    number.targetUnitId = event.targetUnitId;
    number.targetServerUnit = event.targetServerUnit;
    number.targetClientUnit = event.targetClientUnit;
    number.hasLastTargetWorldPosition = event.hasTargetWorldPosition;
    number.lastTargetWorldPosition = event.targetWorldPosition;

    g_numbers.push_back(number);
}

bool IsSameProjectionTarget(
    const ProjectionFrameCacheEntry& entry,
    uint32_t targetUnitType,
    uint32_t targetUnitId,
    uintptr_t targetServerUnit,
    uintptr_t targetClientUnit
) {
    return entry.targetUnitType == targetUnitType &&
        entry.targetUnitId == targetUnitId &&
        entry.targetServerUnit == targetServerUnit &&
        entry.targetClientUnit == targetClientUnit;
}

float CalculateFollowAlpha(const OverlayConfig& config, float dt) {
    const float response = max(1.0f, config.followResponse);
    return ClampFloat(1.0f - std::exp(-response * max(0.0f, dt)), 0.0f, 1.0f);
}

}

const std::vector<DamageNumber>& Numbers() {
    return g_numbers;
}

size_t ActiveCount() {
    return g_numbers.size();
}

size_t PendingCount() {
    return g_pendingEvents.size();
}

void QueueDamageEvent(const DamageEvent& event, bool overlayReady) {
    if (!overlayReady) return;
    g_pendingEvents.push_back(event);
}

void QueueTestDamageAt(
    const OverlayConfig& config,
    bool overlayReady,
    float x,
    float y,
    DamageKind damageKind
) {
    DamageEvent event{};
    event.amount = GenerateTestAmount(damageKind);
    event.screenX = x;
    event.screenY = y;
    event.damageKind = damageKind;
    event.durationSeconds = IsProminentDamage(damageKind) ? config.critLifetimeSeconds : config.lifetimeSeconds;
    event.createdAt = std::chrono::steady_clock::now();

    QueueDamageEvent(event, overlayReady);
}

void QueueTestBurstAt(const OverlayConfig& config, bool overlayReady, float x, float y) {
    for (int i = 0; i < config.burstCount; ++i) {
        const float offsetX = RandomFloat(-config.burstRadius, config.burstRadius);
        const float offsetY = RandomFloat(-config.burstRadius, config.burstRadius);
        QueueTestDamageAt(config, overlayReady, x + offsetX, y + offsetY, RandomBurstDamageKind());
    }
}

void TrimActiveNumbers(const OverlayConfig& config) {
    const size_t maxActive = static_cast<size_t>(config.maxActiveNumbers);
    if (g_numbers.size() <= maxActive) return;

    g_numbers.erase(g_numbers.begin(), g_numbers.begin() + (g_numbers.size() - maxActive));
}

void ProcessDamageEvents(const OverlayConfig& config) {
    if (g_pendingEvents.empty()) return;

    for (const auto& event : g_pendingEvents) {
        DpsTracker::RecordDamageEvent(event);
        if (!TryCoalesceDamageEvent(config, event)) {
            SpawnNumberFromEvent(config, event);
        }
    }

    g_pendingEvents.clear();
    TrimActiveNumbers(config);
}

void UpdateNumbers(const OverlayConfig& config, bool hasGameRect, float dt) {
    const uintptr_t currentGame = g_memorySource.currentSinglePlayerGame;
    const bool screenAnchorReady =
        config.useScreenAnchorCandidates &&
        g_screenAnchorModel.ready &&
        hasGameRect;
    const bool worldCoordProjectionReady =
        config.useWorldPositions &&
        config.useWorldCoordCandidates &&
        config.learnWorldProjection &&
        g_worldCoordModel.ready &&
        hasGameRect;

    WorldPosition playerPosition;
    const bool playerPositionReady =
        config.followDamageTargets &&
        config.useWorldPositions &&
        hasGameRect &&
        TryReadPlayerWorldPosition(currentGame, playerPosition);

    std::vector<ProjectionFrameCacheEntry> projectionCache;
    projectionCache.reserve(g_numbers.size());

    for (auto& number : g_numbers) {
        number.age += dt;
        number.coalesceIdleSeconds += dt;
        number.coalescePulseAge += dt;
        for (auto& tick : number.coalesceTickPops) {
            tick.age += dt;
        }
        number.coalesceTickPops.erase(
            std::remove_if(
                number.coalesceTickPops.begin(),
                number.coalesceTickPops.end(),
                [](const CoalesceTickPop& tick) {
                    return tick.age >= max(0.05f, tick.duration);
                }
            ),
            number.coalesceTickPops.end()
        );

        number.animatedOffsetX += number.vx * dt;
        number.animatedOffsetY += number.vy * dt;

        if (config.followDamageTargets &&
            (playerPositionReady || screenAnchorReady || worldCoordProjectionReady) &&
            number.targetUnitType != UINT32_MAX &&
            number.targetUnitId != UINT32_MAX) {
            float projectedX = 0.0f;
            float projectedY = 0.0f;
            WorldPosition projectedTargetWorldPosition;
            bool hasProjectedTargetWorldPosition = false;
            bool projected = false;

            ProjectionFrameCacheEntry* cached = nullptr;
            for (auto& entry : projectionCache) {
                if (IsSameProjectionTarget(
                    entry,
                    number.targetUnitType,
                    number.targetUnitId,
                    number.targetServerUnit,
                    number.targetClientUnit
                )) {
                    cached = &entry;
                    break;
                }
            }

            if (cached) {
                projected = cached->valid;
                projectedX = cached->screenX;
                projectedY = cached->screenY;
                hasProjectedTargetWorldPosition = cached->hasTargetWorldPosition;
                projectedTargetWorldPosition = cached->targetWorldPosition;
            }
            else {
                ProjectionFrameCacheEntry entry{};
                entry.targetUnitType = number.targetUnitType;
                entry.targetUnitId = number.targetUnitId;
                entry.targetServerUnit = number.targetServerUnit;
                entry.targetClientUnit = number.targetClientUnit;
                if (playerPositionReady) {
                    entry.valid = TryProjectUnitToScreenFromPlayer(
                        currentGame,
                        playerPosition,
                        number.targetUnitType,
                        number.targetUnitId,
                        number.targetServerUnit,
                        number.targetClientUnit,
                        entry.screenX,
                        entry.screenY,
                        &entry.targetWorldPosition,
                        &entry.hasTargetWorldPosition
                    );
                }
                else {
                    entry.valid = TryProjectUnitToScreen(
                        number.targetUnitType,
                        number.targetUnitId,
                        number.targetServerUnit,
                        number.targetClientUnit,
                        entry.screenX,
                        entry.screenY,
                        &entry.targetWorldPosition,
                        &entry.hasTargetWorldPosition
                    );
                }

                projected = entry.valid;
                projectedX = entry.screenX;
                projectedY = entry.screenY;
                hasProjectedTargetWorldPosition = entry.hasTargetWorldPosition;
                projectedTargetWorldPosition = entry.targetWorldPosition;
                projectionCache.push_back(entry);
            }

            if (!projected && playerPositionReady && number.hasLastTargetWorldPosition) {
                projected = ProjectWorldPositionRelativeToPlayer(
                    number.lastTargetWorldPosition,
                    playerPosition,
                    true,
                    projectedX,
                    projectedY
                );
            }

            if (projected) {
                if (IsLargeProjectionOutlier(number, projectedX, projectedY)) {
                    projected = false;
                }
            }

            if (projected) {
                if (hasProjectedTargetWorldPosition) {
                    number.hasLastTargetWorldPosition = true;
                    number.lastTargetWorldPosition = projectedTargetWorldPosition;
                }

                const float dx = projectedX - number.anchorX;
                const float dy = projectedY - number.anchorY;
                const float distance = GetScreenDistance(projectedX, projectedY, number.anchorX, number.anchorY);
                if (number.age <= dt * 1.5f) {
                    number.anchorX = projectedX;
                    number.anchorY = projectedY;
                }
                else if (distance > 0.001f) {
                    const float alpha = CalculateFollowAlpha(config, dt);
                    const float wantedStep = distance * alpha;
                    const float maxStep = max(4.0f, config.followMaxSpeed * max(0.0f, dt));
                    const float step = min(wantedStep, maxStep);
                    number.anchorX += dx * (step / distance);
                    number.anchorY += dy * (step / distance);
                }
                else {
                    number.anchorX = projectedX;
                    number.anchorY = projectedY;
                }
            }
        }

        number.x = number.anchorX + number.localOffsetX + number.animatedOffsetX;
        number.y = number.anchorY + number.localOffsetY + number.animatedOffsetY;
        if (number.age <= dt * 1.5f) {
            number.drawX = number.x;
            number.drawY = number.y;
        }
        else {
            const float drawAlpha = ClampFloat(1.0f - std::exp(-120.0f * max(0.0f, dt)), 0.0f, 1.0f);
            number.drawX += (number.x - number.drawX) * drawAlpha;
            number.drawY += (number.y - number.drawY) * drawAlpha;
        }
    }

    g_numbers.erase(
        std::remove_if(
            g_numbers.begin(),
            g_numbers.end(),
            [](const DamageNumber& number) {
                return number.age >= number.lifetime;
            }
        ),
        g_numbers.end()
    );

    TrimActiveNumbers(config);
}

}
