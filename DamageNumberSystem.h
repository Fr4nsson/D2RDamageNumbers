#pragma once

#include <windows.h>

#include <cstddef>
#include <vector>

#include "DamageModel.h"
#include "OverlayConfig.h"

namespace DamageNumberSystem {

const std::vector<DamageNumber>& Numbers();
size_t ActiveCount();
size_t PendingCount();

void QueueDamageEvent(const DamageEvent& event, bool overlayReady);
void QueueTestDamageAt(const OverlayConfig& config, bool overlayReady, float x, float y, DamageKind damageKind);
void QueueTestBurstAt(const OverlayConfig& config, bool overlayReady, float x, float y);

void ProcessDamageEvents(const OverlayConfig& config);
void UpdateNumbers(const OverlayConfig& config, bool hasGameRect, float dt);
void TrimActiveNumbers(const OverlayConfig& config);

}
