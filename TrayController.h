#pragma once

#include <windows.h>

#include <cstddef>
#include <string>
#include <vector>

#include "OverlayConfig.h"
#include "OverlayDebugModel.h"

namespace TrayController {

constexpr UINT kTrayIconMessage = WM_APP + 1;

struct Context {
    OverlayConfig* config = nullptr;
    std::vector<PositionDebugMarker>* positionDebugMarkers = nullptr;
    bool hasGameRect = false;
    bool memoryConnected = false;
    std::wstring memoryStatus;
    size_t activeNumberCount = 0;
};

struct Callbacks {
    void (*appendLogBlock)(const std::wstring& text) = nullptr;
    void (*showStatusNotice)(const wchar_t* message) = nullptr;
    void (*reloadConfig)() = nullptr;
    void (*restartFrameTimer)(HWND hwnd) = nullptr;
    void (*trimActiveNumbers)() = nullptr;
    void (*queueTestBurstAtGameCenter)() = nullptr;
};

void AddTrayIcon(HWND hwnd, const Context& context);
void RemoveTrayIcon(HWND hwnd);
void UpdateTrayIconTooltip(HWND hwnd, const Context& context);
bool HandleTrayIconMessage(HWND hwnd, LPARAM lParam, Context& context, const Callbacks& callbacks);
bool HandleCommand(HWND hwnd, UINT commandId, Context& context, const Callbacks& callbacks);

}
