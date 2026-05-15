#pragma once

#include <string>

#include "DamageModel.h"

namespace DpsTracker {

void RecordDamageEvent(const DamageEvent& event);
void Update(float dt, float windowSeconds);
long long CurrentDps(float windowSeconds);
std::wstring CurrentDpsText(float windowSeconds);
void Reset();

}
