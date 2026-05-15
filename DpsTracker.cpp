#include "DpsTracker.h"

#include <cwchar>
#include <deque>

namespace DpsTracker {
namespace {

struct DpsSample {
    int amount = 0;
    float ageSeconds = 0.0f;
};

constexpr float kMinDpsWindowSeconds = 0.25f;
constexpr float kMaxDpsWindowSeconds = 60.0f;

std::deque<DpsSample> g_samples;
long long g_rollingDamage = 0;

long long MaxLongLong(long long lhs, long long rhs) {
    return lhs > rhs ? lhs : rhs;
}

float MaxFloat(float lhs, float rhs) {
    return lhs > rhs ? lhs : rhs;
}

float ClampFloat(float value, float minValue, float maxValue) {
    if (value < minValue) return minValue;
    if (value > maxValue) return maxValue;
    return value;
}

float ClampDpsWindowSeconds(float windowSeconds) {
    return ClampFloat(windowSeconds, kMinDpsWindowSeconds, kMaxDpsWindowSeconds);
}

std::wstring FormatWholeWithThousands(long long amount) {
    std::wstring raw = std::to_wstring(MaxLongLong(0LL, amount));
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

std::wstring FormatDpsAmount(long long amount) {
    static const wchar_t* suffixes[] = { L"", L"k", L"m", L"b", L"t" };

    const long long clampedAmount = MaxLongLong(0LL, amount);
    double value = static_cast<double>(clampedAmount);
    int suffixIndex = 0;

    while (value >= 10000.0 && suffixIndex < 4) {
        value /= 1000.0;
        ++suffixIndex;
    }

    if (suffixIndex == 0) {
        return FormatWholeWithThousands(clampedAmount);
    }

    long long roundedTenths = static_cast<long long>((value * 10.0) + 0.5);
    if (roundedTenths >= 10000 && suffixIndex < 4) {
        value /= 1000.0;
        ++suffixIndex;
        roundedTenths = static_cast<long long>((value * 10.0) + 0.5);
    }

    wchar_t text[32]{};

    if (roundedTenths < 1000 && roundedTenths % 10 != 0) {
        swprintf_s(text, L"%lld,%lld%s", roundedTenths / 10, roundedTenths % 10, suffixes[suffixIndex]);
    }
    else {
        swprintf_s(text, L"%lld%s", (roundedTenths + 5) / 10, suffixes[suffixIndex]);
    }

    return text;
}

}

void RecordDamageEvent(const DamageEvent& event) {
    if (event.amount <= 0) return;

    DpsSample sample{};
    sample.amount = event.amount;
    g_samples.push_back(sample);
    g_rollingDamage += event.amount;
}

void Update(float dt, float windowSeconds) {
    const float window = ClampDpsWindowSeconds(windowSeconds);
    const float elapsed = MaxFloat(0.0f, dt);
    for (auto& sample : g_samples) {
        sample.ageSeconds += elapsed;
    }

    while (!g_samples.empty() && g_samples.front().ageSeconds > window) {
        g_rollingDamage -= g_samples.front().amount;
        g_samples.pop_front();
    }

    if (g_rollingDamage < 0) {
        g_rollingDamage = 0;
    }
}

long long CurrentDps(float windowSeconds) {
    const float window = ClampDpsWindowSeconds(windowSeconds);
    return static_cast<long long>(static_cast<double>(g_rollingDamage) / static_cast<double>(window) + 0.5);
}

std::wstring CurrentDpsText(float windowSeconds) {
    return L"DPS " + FormatDpsAmount(CurrentDps(windowSeconds));
}

void Reset() {
    g_samples.clear();
    g_rollingDamage = 0;
}

}
