#pragma once

#include <windows.h>

#include <string>
#include <vector>

#include "DamageModel.h"
#include "OverlayConfig.h"
#include "OverlayDebugModel.h"

struct OverlayRenderContext {
    HWND hwnd = nullptr;
    const OverlayConfig* config = nullptr;
    const std::vector<DamageNumber>* numbers = nullptr;
    const std::vector<PositionDebugMarker>* positionDebugMarkers = nullptr;
    std::wstring debugStatusText;
    std::wstring dpsText;
    bool gameRectReady = false;
    bool statusNoticeActive = false;
};

bool ReloadOverlayRendererConfig(const OverlayConfig& config);
void ClearOverlayRendererFontCache();
void ReleaseOverlayRendererResources();
void UnloadOverlayRendererFont();
void RenderOverlay(const OverlayRenderContext& context);
