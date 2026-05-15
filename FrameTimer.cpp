#include "FrameTimer.h"

#include <mmsystem.h>

#pragma comment(lib, "winmm.lib")

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace FrameTimer {
namespace {

bool g_timerResolutionActive = false;
HANDLE g_frameThread = nullptr;
HANDLE g_frameStopEvent = nullptr;
volatile LONG g_frameMessagePending = 0;
volatile LONG g_renderFps = 120;
HWND g_targetHwnd = nullptr;

int ClampRenderFps(int renderFps) {
    if (renderFps < 30) return 30;
    if (renderFps > 240) return 240;
    return renderFps;
}

UINT GetFrameTimerIntervalMs() {
    const int renderFps = ClampRenderFps(static_cast<int>(g_renderFps));
    const int intervalMs = (1000 + (renderFps / 2)) / renderFps;
    return static_cast<UINT>(intervalMs > 1 ? intervalMs : 1);
}

double GetFrameIntervalSeconds() {
    const int renderFps = ClampRenderFps(static_cast<int>(g_renderFps));
    return 1.0 / static_cast<double>(renderFps);
}

double GetPerformanceSeconds() {
    static LARGE_INTEGER frequency = []() {
        LARGE_INTEGER value{};
        QueryPerformanceFrequency(&value);
        return value;
    }();

    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    return static_cast<double>(counter.QuadPart) / static_cast<double>(frequency.QuadPart);
}

void BeginFrameTimerResolution() {
    if (g_timerResolutionActive) return;
    g_timerResolutionActive = timeBeginPeriod(1) == TIMERR_NOERROR;
}

DWORD WINAPI FrameThreadProc(LPVOID) {
    HANDLE timer = CreateWaitableTimerExW(
        nullptr,
        nullptr,
        CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
        TIMER_ALL_ACCESS
    );
    if (!timer) {
        timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    }
    if (!timer) return 1;

    HANDLE handles[2]{ g_frameStopEvent, timer };
    double nextFrameSeconds = GetPerformanceSeconds() + GetFrameIntervalSeconds();

    for (;;) {
        const double nowSeconds = GetPerformanceSeconds();
        double delaySeconds = nextFrameSeconds - nowSeconds;
        if (delaySeconds < 0.0001) {
            delaySeconds = 0.0001;
        }

        LARGE_INTEGER dueTime{};
        dueTime.QuadPart = -static_cast<LONGLONG>(delaySeconds * 10000000.0);
        if (dueTime.QuadPart == 0) {
            dueTime.QuadPart = -1;
        }

        if (!SetWaitableTimer(timer, &dueTime, 0, nullptr, nullptr, FALSE)) {
            CloseHandle(timer);
            return 1;
        }

        const DWORD waitResult = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
        CancelWaitableTimer(timer);

        if (waitResult == WAIT_OBJECT_0) {
            CloseHandle(timer);
            return 0;
        }
        if (waitResult != WAIT_OBJECT_0 + 1) {
            CloseHandle(timer);
            return 1;
        }

        if (InterlockedExchange(&g_frameMessagePending, 1) == 0) {
            HWND hwnd = g_targetHwnd;
            if (hwnd) {
                PostMessageW(hwnd, kFrameTickMessage, 0, 0);
            }
        }

        const double intervalSeconds = GetFrameIntervalSeconds();
        nextFrameSeconds += intervalSeconds;

        const double afterPostSeconds = GetPerformanceSeconds();
        if (nextFrameSeconds < afterPostSeconds - (intervalSeconds * 2.0)) {
            nextFrameSeconds = afterPostSeconds + intervalSeconds;
        }
    }
}

}

void Restart(HWND hwnd, int renderFps) {
    Stop(hwnd);
    g_targetHwnd = hwnd;
    InterlockedExchange(&g_renderFps, ClampRenderFps(renderFps));
    BeginFrameTimerResolution();

    g_frameStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (g_frameStopEvent) {
        g_frameThread = CreateThread(nullptr, 0, FrameThreadProc, nullptr, 0, nullptr);
        if (!g_frameThread) {
            CloseHandle(g_frameStopEvent);
            g_frameStopEvent = nullptr;
        }
        else {
            SetThreadPriority(g_frameThread, THREAD_PRIORITY_ABOVE_NORMAL);
        }
    }

    if (!g_frameThread) {
        SetTimer(hwnd, kFallbackTimerId, GetFrameTimerIntervalMs(), nullptr);
    }
}

void Stop(HWND hwnd) {
    if (g_frameStopEvent) {
        SetEvent(g_frameStopEvent);
    }

    if (g_frameThread) {
        WaitForSingleObject(g_frameThread, 2000);
        CloseHandle(g_frameThread);
        g_frameThread = nullptr;
    }

    if (g_frameStopEvent) {
        CloseHandle(g_frameStopEvent);
        g_frameStopEvent = nullptr;
    }

    KillTimer(hwnd, kFallbackTimerId);
    InterlockedExchange(&g_frameMessagePending, 0);
}

void EndTimerResolution() {
    if (!g_timerResolutionActive) return;
    timeEndPeriod(1);
    g_timerResolutionActive = false;
}

void MarkFrameMessageHandled() {
    InterlockedExchange(&g_frameMessagePending, 0);
}

bool IsFallbackTimer(WPARAM wParam) {
    return wParam == kFallbackTimerId;
}

}
