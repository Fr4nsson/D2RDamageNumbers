#pragma once

#include <windows.h>

#include <string>

struct PositionDebugMarker {
    std::wstring label;
    int x = 0;
    int y = 0;
    COLORREF color = RGB(255, 255, 255);
};
