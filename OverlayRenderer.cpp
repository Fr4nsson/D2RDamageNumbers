#include "OverlayRenderer.h"

#include <cmath>

namespace {

struct CachedRenderFont {
    int fontSize = 0;
    int fontWeight = 0;
    HFONT font = nullptr;
};

HDC g_renderDc = nullptr;
HBITMAP g_renderBitmap = nullptr;
HBITMAP g_renderOldBitmap = nullptr;
void* g_renderBits = nullptr;
int g_renderWidth = 0;
int g_renderHeight = 0;
std::vector<CachedRenderFont> g_renderFontCache;
bool g_privateFontLoaded = false;
std::wstring g_loadedFontPath;

int MaxInt(int lhs, int rhs) {
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

void ClearRenderFontCache() {
    for (const auto& entry : g_renderFontCache) {
        if (entry.font) {
            DeleteObject(entry.font);
        }
    }
    g_renderFontCache.clear();
}

void ReleaseRenderSurface() {
    if (g_renderDc && g_renderOldBitmap) {
        SelectObject(g_renderDc, g_renderOldBitmap);
    }

    if (g_renderBitmap) {
        DeleteObject(g_renderBitmap);
        g_renderBitmap = nullptr;
    }

    if (g_renderDc) {
        DeleteDC(g_renderDc);
        g_renderDc = nullptr;
    }

    g_renderOldBitmap = nullptr;
    g_renderBits = nullptr;
    g_renderWidth = 0;
    g_renderHeight = 0;
}

bool EnsureRenderSurface(HDC screenDc, int width, int height) {
    if (g_renderDc && g_renderBitmap && g_renderWidth == width && g_renderHeight == height) {
        return true;
    }

    ReleaseRenderSurface();

    g_renderDc = CreateCompatibleDC(screenDc);
    if (!g_renderDc) return false;

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    g_renderBitmap = CreateDIBSection(screenDc, &bmi, DIB_RGB_COLORS, &g_renderBits, nullptr, 0);
    if (!g_renderBitmap) {
        ReleaseRenderSurface();
        return false;
    }

    g_renderOldBitmap = static_cast<HBITMAP>(SelectObject(g_renderDc, g_renderBitmap));
    g_renderWidth = width;
    g_renderHeight = height;
    return true;
}

HFONT GetCachedRenderFont(const OverlayConfig& config, int fontSize, int fontWeight) {
    for (const auto& entry : g_renderFontCache) {
        if (entry.fontSize == fontSize && entry.fontWeight == fontWeight) {
            return entry.font;
        }
    }

    CachedRenderFont entry{};
    entry.fontSize = fontSize;
    entry.fontWeight = fontWeight;
    entry.font = CreateFontW(
        fontSize,
        0,
        0,
        0,
        fontWeight,
        FALSE,
        FALSE,
        FALSE,
        DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE,
        config.fontFace.c_str()
    );
    g_renderFontCache.push_back(entry);
    return entry.font;
}

void UnloadPrivateFont() {
    if (!g_privateFontLoaded || g_loadedFontPath.empty()) return;

    ClearRenderFontCache();
    RemoveFontResourceExW(g_loadedFontPath.c_str(), FR_PRIVATE, nullptr);
    g_privateFontLoaded = false;
    g_loadedFontPath.clear();
}

bool LoadPrivateFontFile(const std::wstring& fontFile) {
    UnloadPrivateFont();

    if (fontFile.empty()) {
        return true;
    }

    const std::wstring fontPath = ResolveConfigRelativePath(fontFile);
    DWORD attributes = GetFileAttributesW(fontPath.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY)) {
        return false;
    }

    int loadedCount = AddFontResourceExW(fontPath.c_str(), FR_PRIVATE, nullptr);
    if (loadedCount <= 0) {
        return false;
    }

    g_privateFontLoaded = true;
    g_loadedFontPath = fontPath;
    return true;
}

COLORREF ScaleColor(COLORREF color, float scale) {
    scale = ClampFloat(scale, 0.0f, 1.0f);
    return RGB(
        static_cast<int>(GetRValue(color) * scale),
        static_cast<int>(GetGValue(color) * scale),
        static_cast<int>(GetBValue(color) * scale)
    );
}

void RenderDebugStatus(HDC dc, const OverlayRenderContext& context) {
    const OverlayConfig& config = *context.config;
    if (!config.showDebugStatus && !context.statusNoticeActive) return;
    if (context.debugStatusText.empty()) return;

    HFONT debugFont = CreateFontW(
        16,
        0,
        0,
        0,
        FW_SEMIBOLD,
        FALSE,
        FALSE,
        FALSE,
        DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE,
        L"Segoe UI"
    );

    HFONT oldFont = static_cast<HFONT>(SelectObject(dc, debugFont));

    RECT shadowRc{ 13, 13, 760, 230 };
    SetTextColor(dc, RGB(1, 1, 1));
    DrawTextW(dc, context.debugStatusText.c_str(), -1, &shadowRc, DT_LEFT | DT_TOP | DT_NOCLIP);

    RECT textRc{ 12, 12, 760, 230 };
    SetTextColor(dc, context.gameRectReady ? RGB(180, 255, 190) : RGB(255, 210, 120));
    DrawTextW(dc, context.debugStatusText.c_str(), -1, &textRc, DT_LEFT | DT_TOP | DT_NOCLIP);

    SelectObject(dc, oldFont);
    DeleteObject(debugFont);
}

void RenderPositionDebugMarkers(
    HDC dc,
    const OverlayConfig& config,
    const std::vector<PositionDebugMarker>& positionDebugMarkers
) {
    if (!config.showPositionCandidates || positionDebugMarkers.empty()) return;

    HFONT markerFont = CreateFontW(
        14,
        0,
        0,
        0,
        FW_SEMIBOLD,
        FALSE,
        FALSE,
        FALSE,
        DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE,
        L"Segoe UI"
    );

    HFONT oldFont = static_cast<HFONT>(SelectObject(dc, markerFont));
    SetBkMode(dc, TRANSPARENT);

    for (const auto& marker : positionDebugMarkers) {
        const bool isCursor = marker.label == L"cursor";
        const int radius = isCursor ? 3 : 5;
        const int cross = isCursor ? 8 : 10;
        const int penWidth = isCursor ? 1 : 2;

        HPEN pen = CreatePen(PS_SOLID, penWidth, marker.color);
        HBRUSH brush = CreateSolidBrush(marker.color);
        HPEN oldPen = static_cast<HPEN>(SelectObject(dc, pen));
        HBRUSH oldBrush = static_cast<HBRUSH>(SelectObject(dc, brush));

        Ellipse(dc, marker.x - radius, marker.y - radius, marker.x + radius + 1, marker.y + radius + 1);
        MoveToEx(dc, marker.x - cross, marker.y, nullptr);
        LineTo(dc, marker.x + cross + 1, marker.y);
        MoveToEx(dc, marker.x, marker.y - cross, nullptr);
        LineTo(dc, marker.x, marker.y + cross + 1);

        SelectObject(dc, oldBrush);
        SelectObject(dc, oldPen);
        DeleteObject(brush);
        DeleteObject(pen);

        RECT shadowRc{ marker.x + 10, marker.y - 13, marker.x + 280, marker.y + 12 };
        SetTextColor(dc, RGB(1, 1, 1));
        DrawTextW(dc, marker.label.c_str(), -1, &shadowRc, DT_LEFT | DT_TOP | DT_NOCLIP);

        RECT textRc = shadowRc;
        OffsetRect(&textRc, -1, -1);
        SetTextColor(dc, marker.color);
        DrawTextW(dc, marker.label.c_str(), -1, &textRc, DT_LEFT | DT_TOP | DT_NOCLIP);
    }

    SelectObject(dc, oldFont);
    DeleteObject(markerFont);
}

float CalculateFade(const OverlayConfig& config, float age, float lifetime) {
    if (lifetime <= 0.0f) return 0.0f;

    const float t = ClampFloat(age / lifetime, 0.0f, 1.0f);
    if (t <= config.fadeStart) {
        return 1.0f;
    }

    return ClampFloat((1.0f - t) / MaxFloat(0.05f, 1.0f - config.fadeStart), 0.0f, 1.0f);
}

float CalculatePopScale(const OverlayConfig& config, float age) {
    const float popInSeconds = MaxFloat(0.01f, config.popInSeconds);
    const float popSettleSeconds = MaxFloat(0.01f, config.popSettleSeconds);

    if (age < popInSeconds) {
        const float t = age / popInSeconds;
        return config.popStartScale + t * (config.popOvershootScale - config.popStartScale);
    }

    if (age < popInSeconds + popSettleSeconds) {
        const float t = (age - popInSeconds) / popSettleSeconds;
        return config.popOvershootScale + t * (1.0f - config.popOvershootScale);
    }

    return 1.0f;
}

float CalculateCoalescePulseScale(const OverlayConfig& config, const DamageNumber& number) {
    if (!number.canCoalesce) return 1.0f;

    const float pulseSeconds = MaxFloat(0.01f, config.coalescePulseSeconds);
    if (number.coalescePulseAge >= pulseSeconds) return 1.0f;

    const float t = ClampFloat(number.coalescePulseAge / pulseSeconds, 0.0f, 1.0f);
    const float eased = (1.0f - t) * (1.0f - t);
    return 1.0f + ((MaxFloat(1.0f, config.coalescePulseScale) - 1.0f) * eased);
}

void DrawTextOffsetWithFormat(
    HDC dc,
    const std::wstring& text,
    const RECT& textRc,
    int offsetX,
    int offsetY,
    UINT format
) {
    RECT rc = textRc;
    OffsetRect(&rc, offsetX, offsetY);
    DrawTextW(dc, text.c_str(), -1, &rc, format);
}

void DrawTextOffset(HDC dc, const std::wstring& text, const RECT& textRc, int offsetX, int offsetY) {
    DrawTextOffsetWithFormat(dc, text, textRc, offsetX, offsetY, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void DrawStyledText(
    HDC dc,
    const OverlayConfig& config,
    const std::wstring& text,
    COLORREF color,
    float fade,
    const RECT& textRc,
    UINT format
) {
    const int outlineThickness = config.outlineThickness;

    if (config.shadowOffsetX != 0 || config.shadowOffsetY != 0) {
        SetTextColor(dc, ScaleColor(config.shadowColor, fade));
        DrawTextOffsetWithFormat(dc, text, textRc, config.shadowOffsetX, config.shadowOffsetY, format);
    }

    if (outlineThickness > 0) {
        SetTextColor(dc, ScaleColor(config.outlineColor, fade));

        for (int y = -outlineThickness; y <= outlineThickness; ++y) {
            for (int x = -outlineThickness; x <= outlineThickness; ++x) {
                if (x == 0 && y == 0) continue;
                if ((x * x) + (y * y) > outlineThickness * outlineThickness + outlineThickness) continue;

                DrawTextOffsetWithFormat(dc, text, textRc, x, y, format);
            }
        }
    }

    SetTextColor(dc, ScaleColor(color, fade));
    DrawTextOffsetWithFormat(dc, text, textRc, 0, 0, format);
}

void DrawStyledDamageText(
    HDC dc,
    const OverlayConfig& config,
    const DamageNumber& number,
    const RECT& textRc
) {
    DrawStyledText(
        dc,
        config,
        number.text,
        number.color,
        CalculateFade(config, number.age, number.lifetime),
        textRc,
        DT_CENTER | DT_VCENTER | DT_SINGLELINE
    );
}

void RenderDamageTextAt(
    HDC dc,
    const OverlayConfig& config,
    HFONT oldFont,
    const XFORM& identity,
    const std::wstring& text,
    COLORREF color,
    float age,
    float lifetime,
    float drawX,
    float drawY,
    int fontSize,
    int fontWeight,
    int minHalfWidth,
    int minHalfHeight
) {
    if (text.empty()) return;

    const int textHalfWidth =
        static_cast<int>(fontSize * MaxInt(4, static_cast<int>(text.length())) * 0.42f) +
        config.outlineThickness +
        16;
    const int textHalfHeight = fontSize + config.outlineThickness + 12;
    const int halfWidth = MaxInt(minHalfWidth, textHalfWidth);
    const int halfHeight = MaxInt(minHalfHeight, textHalfHeight);

    HFONT numberFont = GetCachedRenderFont(config, fontSize, fontWeight);
    SelectObject(dc, numberFont);

    const float baseX = std::floor(drawX);
    const float baseY = std::floor(drawY);
    const XFORM subpixelTransform{ 1.0f, 0.0f, 0.0f, 1.0f, drawX - baseX, drawY - baseY };
    SetWorldTransform(dc, &subpixelTransform);

    RECT textRc{};
    textRc.left = static_cast<LONG>(baseX) - halfWidth;
    textRc.top = static_cast<LONG>(baseY) - halfHeight;
    textRc.right = static_cast<LONG>(baseX) + halfWidth;
    textRc.bottom = static_cast<LONG>(baseY) + halfHeight;

    DamageNumber drawNumber{};
    drawNumber.text = text;
    drawNumber.age = age;
    drawNumber.lifetime = lifetime;
    drawNumber.color = color;

    DrawStyledDamageText(dc, config, drawNumber, textRc);

    SetWorldTransform(dc, &identity);
    SelectObject(dc, oldFont);
}

void RenderCoalesceTickPop(
    HDC dc,
    const OverlayConfig& config,
    HFONT oldFont,
    const XFORM& identity,
    const DamageNumber& number,
    const CoalesceTickPop& tick,
    int baseFontSize
) {
    if (!config.coalesceTickPop) return;

    const float duration = MaxFloat(0.05f, tick.duration);
    if (tick.age >= duration) return;

    const float t = ClampFloat(tick.age / duration, 0.0f, 1.0f);
    const float moveT = 1.0f - std::pow(1.0f - t, 3.0f);
    const float drawX = number.drawX + tick.startOffsetX + ((tick.endOffsetX - tick.startOffsetX) * moveT);
    const float drawY = number.drawY + tick.startOffsetY + ((tick.endOffsetY - tick.startOffsetY) * moveT);

    const float popT = t < 0.30f
        ? t / 0.30f
        : ClampFloat(1.0f - ((t - 0.30f) / 0.70f), 0.0f, 1.0f);
    const float tickScale = MaxFloat(0.25f, config.coalesceTickPopScale) * (1.0f + (0.20f * popT));
    const int fontSize = MaxInt(10, static_cast<int>(baseFontSize * number.prominenceScale * tickScale));

    RenderDamageTextAt(
        dc,
        config,
        oldFont,
        identity,
        tick.text,
        number.color,
        tick.age,
        duration,
        drawX,
        drawY,
        fontSize,
        config.fontWeight,
        56,
        22
    );
}

void RenderDpsNumber(HDC dc, const OverlayConfig& config, const std::wstring& text, int width, int height) {
    if (!config.showDpsNumber || text.empty()) return;

    const int fontSize = static_cast<int>(ClampFloat(static_cast<float>(config.fontSize - 6), 20.0f, 40.0f));
    HFONT dpsFont = GetCachedRenderFont(config, fontSize, config.fontWeight);
    SelectObject(dc, dpsFont);

    SIZE textSize{};
    GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.length()), &textSize);

    const float xPercent = ClampFloat(config.dpsNumberXPercent, 0.0f, 100.0f);
    const float yPercent = ClampFloat(config.dpsNumberYPercent, 0.0f, 100.0f);
    const int anchorX = static_cast<int>((static_cast<float>(width) * xPercent / 100.0f) + 0.5f);
    const int anchorY = static_cast<int>((static_cast<float>(height) * yPercent / 100.0f) + 0.5f);
    const int padX = config.outlineThickness + 10;
    const int padY = config.outlineThickness + 6;
    const int boxWidth = textSize.cx + (padX * 2);
    const int boxHeight = MaxInt(fontSize + (padY * 2), textSize.cy + (padY * 2));

    RECT textRc{};
    UINT format = DT_VCENTER | DT_SINGLELINE;

    if (xPercent <= 10.0f) {
        textRc.left = anchorX;
        textRc.right = anchorX + boxWidth;
        format |= DT_LEFT;
    }
    else if (xPercent >= 90.0f) {
        textRc.left = anchorX - boxWidth;
        textRc.right = anchorX;
        format |= DT_RIGHT;
    }
    else {
        textRc.left = anchorX - (boxWidth / 2);
        textRc.right = textRc.left + boxWidth;
        format |= DT_CENTER;
    }

    if (yPercent <= 10.0f) {
        textRc.top = anchorY;
        textRc.bottom = anchorY + boxHeight;
    }
    else if (yPercent >= 90.0f) {
        textRc.top = anchorY - boxHeight;
        textRc.bottom = anchorY;
    }
    else {
        textRc.top = anchorY - (boxHeight / 2);
        textRc.bottom = textRc.top + boxHeight;
    }

    if (textRc.left < 0) {
        OffsetRect(&textRc, -textRc.left, 0);
    }
    if (textRc.right > width) {
        OffsetRect(&textRc, width - textRc.right, 0);
    }
    if (textRc.top < 0) {
        OffsetRect(&textRc, 0, -textRc.top);
    }
    if (textRc.bottom > height) {
        OffsetRect(&textRc, 0, height - textRc.bottom);
    }

    DrawStyledText(dc, config, text, config.normalColor, 1.0f, textRc, format);
}

}

bool ReloadOverlayRendererConfig(const OverlayConfig& config) {
    const bool loaded = LoadPrivateFontFile(config.fontFile);
    ClearRenderFontCache();
    return loaded;
}

void ClearOverlayRendererFontCache() {
    ClearRenderFontCache();
}

void ReleaseOverlayRendererResources() {
    ReleaseRenderSurface();
    ClearRenderFontCache();
    UnloadPrivateFont();
}

void UnloadOverlayRendererFont() {
    UnloadPrivateFont();
}

void RenderOverlay(const OverlayRenderContext& context) {
    if (!context.hwnd || !context.config || !context.numbers || !context.positionDebugMarkers) return;

    const OverlayConfig& config = *context.config;
    const std::vector<DamageNumber>& numbers = *context.numbers;
    const std::vector<PositionDebugMarker>& positionDebugMarkers = *context.positionDebugMarkers;

    RECT rc{};
    GetClientRect(context.hwnd, &rc);

    int width = rc.right - rc.left;
    int height = rc.bottom - rc.top;
    if (width <= 0 || height <= 0) return;

    HDC screenDc = GetDC(nullptr);
    if (!screenDc) return;
    if (!EnsureRenderSurface(screenDc, width, height)) {
        ReleaseDC(nullptr, screenDc);
        return;
    }

    HBRUSH clearBrush = CreateSolidBrush(RGB(0, 0, 0));
    FillRect(g_renderDc, &rc, clearBrush);
    DeleteObject(clearBrush);

    SetBkMode(g_renderDc, TRANSPARENT);
    SetGraphicsMode(g_renderDc, GM_ADVANCED);

    HFONT oldFont = static_cast<HFONT>(GetCurrentObject(g_renderDc, OBJ_FONT));
    const XFORM identity{ 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f };

    for (const auto& number : numbers) {
        const int baseFontSize = number.isProminent ? config.critFontSize : config.fontSize;
        for (const auto& tick : number.coalesceTickPops) {
            RenderCoalesceTickPop(g_renderDc, config, oldFont, identity, number, tick, baseFontSize);
        }
    }

    for (const auto& number : numbers) {
        const int baseFontSize = number.isProminent ? config.critFontSize : config.fontSize;
        const float popScale = MaxFloat(
            CalculatePopScale(config, number.age),
            CalculateCoalescePulseScale(config, number)
        );
        const int fontSize = MaxInt(12, static_cast<int>(baseFontSize * number.prominenceScale * popScale));
        const int minHalfWidth = number.isProminent ? 132 : 112;
        const int minHalfHeight = number.isProminent ? 42 : 34;
        RenderDamageTextAt(
            g_renderDc,
            config,
            oldFont,
            identity,
            number.text,
            number.color,
            number.age,
            number.lifetime,
            number.drawX,
            number.drawY,
            fontSize,
            number.isProminent ? config.critFontWeight : config.fontWeight,
            minHalfWidth,
            minHalfHeight
        );
    }

    SetWorldTransform(g_renderDc, &identity);
    RenderPositionDebugMarkers(g_renderDc, config, positionDebugMarkers);
    RenderDpsNumber(g_renderDc, config, context.dpsText, width, height);
    RenderDebugStatus(g_renderDc, context);

    POINT srcPoint{ 0, 0 };
    SIZE size{ width, height };

    POINT dstPoint{};
    RECT wndRect{};
    GetWindowRect(context.hwnd, &wndRect);
    dstPoint.x = wndRect.left;
    dstPoint.y = wndRect.top;

    BLENDFUNCTION blend{};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = 0;

    UpdateLayeredWindow(
        context.hwnd,
        screenDc,
        &dstPoint,
        &size,
        g_renderDc,
        &srcPoint,
        RGB(0, 0, 0),
        &blend,
        ULW_COLORKEY
    );

    SelectObject(g_renderDc, oldFont);
    ReleaseDC(nullptr, screenDc);
}
