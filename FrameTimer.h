#pragma once

#include <windows.h>

namespace FrameTimer {

constexpr UINT kFrameTickMessage = WM_APP + 2;
constexpr UINT_PTR kFallbackTimerId = 1;

void Restart(HWND hwnd, int renderFps);
void Stop(HWND hwnd);
void EndTimerResolution();
void MarkFrameMessageHandled();
bool IsFallbackTimer(WPARAM wParam);

}
