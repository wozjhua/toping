#include "pc_mapping_overlay.h"
#include "pc_ui_theme.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>
#include <string>

namespace {
static const std::vector<PcMacroRuntime::Binding>* g_macroSource = nullptr;
static int g_selectedMacroIndex = -1;
static int g_selectedMacroStepId = 0;
static bool g_macroStepEditorVisible = false;

static int clampi(int v, int lo, int hi) { return (std::max)(lo, (std::min)(hi, v)); }

static int normToPx(int norm, int size) {
    const int s = (std::max)(1, size);
    return clampi(static_cast<int>((static_cast<long long>(PcMappingProfile::ClampNorm(norm)) * (s - 1) + 500000LL) / 1000000LL), 0, s - 1);
}

static int radiusNormToPx(int radiusNorm, int width, int height) {
    const int base = (std::max)(1, (std::min)(width, height));
    const int r = static_cast<int>((static_cast<long long>(PcMappingProfile::ClampButtonRadiusNorm(radiusNorm)) * base + 500000LL) / 1000000LL);
    return clampi(r, PC_BUTTON_RADIUS_MIN, PC_BUTTON_RADIUS_MAX);
}

static bool ptIn(const RECT& r, int x, int y) { return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom; }
static bool ptInCircle(int cx, int cy, int r, int x, int y) {
    const int dx = x - cx;
    const int dy = y - cy;
    return dx * dx + dy * dy <= r * r;
}

static std::wstring fallbackLabel(const PcMappingBinding& b) {
    if (!b.triggerLabel.empty()) return b.triggerLabel;
    if (!b.comboTriggerCodes.empty()) return L"Key";
    if (b.triggerCode > 0) return L"Key" + std::to_wstring(b.triggerCode);
    return L"映射";
}

static std::wstring lockModeShort(PcLockSlideTouchMode mode) {
    switch (mode) {
        case PcLockSlideTouchMode::SingleReanchor: return L"LOCK-1";
        case PcLockSlideTouchMode::DualSimultaneous: return L"LOCK-2";
        case PcLockSlideTouchMode::DualSequential: return L"LOCK-3";
        default: return L"LOCK";
    }
}

static void drawLine(HDC dc, int x1, int y1, int x2, int y2, COLORREF color, int width = 2) {
    HPEN pen = CreatePen(PS_SOLID, width, color);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    MoveToEx(dc, x1, y1, nullptr);
    LineTo(dc, x2, y2);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
}

static void drawEllipseFillBorder(HDC dc, const RECT& rc, COLORREF fill, COLORREF border, int borderWidth) {
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, borderWidth, border);
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    Ellipse(dc, rc.left, rc.top, rc.right, rc.bottom);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(pen);
    DeleteObject(brush);
}

static void drawHandle(HDC dc, int x, int y, COLORREF fill, COLORREF border) {
    RECT r{x - 6, y - 6, x + 7, y + 7};
    HBRUSH b = CreateSolidBrush(fill);
    PcUi::DrawRoundFill(dc, r, 6, b);
    DeleteObject(b);
    PcUi::DrawRoundBorder(dc, r, 6, border, 2);
}

static void drawCornerBrackets(HDC dc, const RECT& r, COLORREF color) {
    const int len = 26;
    drawLine(dc, r.left, r.top + len, r.left, r.top, color, 2);
    drawLine(dc, r.left, r.top, r.left + len, r.top, color, 2);
    drawLine(dc, r.right - len, r.top, r.right, r.top, color, 2);
    drawLine(dc, r.right, r.top, r.right, r.top + len, color, 2);
    drawLine(dc, r.left, r.bottom - len, r.left, r.bottom, color, 2);
    drawLine(dc, r.left, r.bottom, r.left + len, r.bottom, color, 2);
    drawLine(dc, r.right - len, r.bottom, r.right, r.bottom, color, 2);
    drawLine(dc, r.right, r.bottom - len, r.right, r.bottom, color, 2);
}

static void lockRangeRect(const PcLockBinding& l, int width, int height, RECT& range) {
    range = RECT{ normToPx(l.leftNorm, width), normToPx(l.topNorm, height), normToPx(l.rightNorm, width), normToPx(l.bottomNorm, height) };
    if (range.left > range.right) std::swap(range.left, range.right);
    if (range.top > range.bottom) std::swap(range.top, range.bottom);
}

static void lockModeChipRects(const RECT& range, int height, RECT& c1, RECT& c2, RECT& c3) {
    const int chipY = static_cast<int>((std::min<LONG>)(static_cast<LONG>(height - 30), range.bottom + 10));
    const int chipW = 54;
    c1 = RECT{range.left, chipY, range.left + chipW, chipY + 24};
    c2 = RECT{range.left + chipW + 6, chipY, range.left + chipW * 2 + 6, chipY + 24};
    c3 = RECT{range.left + chipW * 2 + 12, chipY, range.left + chipW * 3 + 12, chipY + 24};
}

static void lockActionRects(const PcLockBinding& l, int width, int height, RECT& options, RECT& rebind) {
    const int cx = normToPx(l.centerXNorm, width);
    const int cy = normToPx(l.centerYNorm, height);
    options = RECT{cx - 96, cy - 18, cx - 38, cy + 12};
    rebind = RECT{cx - 96, cy + 18, cx - 38, cy + 48};
    if (options.left < 4) {
        options = RECT{cx + 38, cy - 18, cx + 96, cy + 12};
        rebind = RECT{cx + 38, cy + 18, cx + 96, cy + 48};
    }
}

static void drawModeChip(HDC dc, HFONT font, RECT rc, const wchar_t* text, bool selected) {
    const auto& theme = PcUi::DefaultTheme();
    HBRUSH fill = PcUi::CreateBrush(selected ? theme.accent : theme.panel);
    PcUi::DrawRoundFill(dc, rc, 12, fill);
    DeleteObject(fill);
    PcUi::DrawRoundBorder(dc, rc, 12, PcUi::Rgb(selected ? theme.accent2 : theme.stroke), 1);
    PcUi::DrawTextCenter(dc, rc, text, font, PcUi::Rgb(selected ? theme.bg : theme.text));
}

static void drawLockNode(HDC dc, HFONT font, const PcLockBinding& l, int width, int height, bool editMode) {
    const auto& theme = PcUi::DefaultTheme();
    RECT range{};
    lockRangeRect(l, width, height, range);
    PcUi::DrawRoundBorder(dc, range, 18, PcUi::Rgb(theme.accent), editMode ? 2 : 1);
    drawCornerBrackets(dc, range, PcUi::Rgb(theme.accent2));

    if (editMode) {
        const COLORREF accent = PcUi::Rgb(theme.accent);
        const COLORREF accent2 = PcUi::Rgb(theme.accent2);
        drawHandle(dc, range.left, range.top, accent2, accent);
        drawHandle(dc, (range.left + range.right) / 2, range.top, accent2, accent);
        drawHandle(dc, range.right, range.top, accent2, accent);
        drawHandle(dc, range.left, (range.top + range.bottom) / 2, accent2, accent);
        drawHandle(dc, range.right, (range.top + range.bottom) / 2, accent2, accent);
        drawHandle(dc, range.left, range.bottom, accent2, accent);
        drawHandle(dc, (range.left + range.right) / 2, range.bottom, accent2, accent);
        drawHandle(dc, range.right, range.bottom, accent2, accent);

        RECT c1{}, c2{}, c3{};
        lockModeChipRects(range, height, c1, c2, c3);
        drawModeChip(dc, font, c1, L"1 单", l.mode == PcLockSlideTouchMode::SingleReanchor);
        drawModeChip(dc, font, c2, L"2 同", l.mode == PcLockSlideTouchMode::DualSimultaneous);
        drawModeChip(dc, font, c3, L"3 顺", l.mode == PcLockSlideTouchMode::DualSequential);
    }

    const int cx = normToPx(l.centerXNorm, width);
    const int cy = normToPx(l.centerYNorm, height);
    const int r = 24;
    RECT node{cx - r, cy - r, cx + r, cy + r};
    drawEllipseFillBorder(dc, node, PcUi::Rgb(theme.panel2), PcUi::Rgb(theme.accent2), 2);
    drawLine(dc, cx - 12, cy, cx + 12, cy, PcUi::Rgb(theme.textMuted), 2);
    drawLine(dc, cx, cy - 12, cx, cy + 12, PcUi::Rgb(theme.textMuted), 2);

    RECT badge{cx - 44, cy + r - 2, cx + 44, cy + r + 22};
    HBRUSH badgeFill = PcUi::CreateBrush(theme.panel);
    PcUi::DrawRoundFill(dc, badge, 12, badgeFill);
    DeleteObject(badgeFill);
    PcUi::DrawRoundBorder(dc, badge, 12, PcUi::Rgb(theme.stroke), 1);
    PcUi::DrawTextCenter(dc, badge, lockModeShort(l.mode).c_str(), font, PcUi::Rgb(theme.text));

    if (editMode) {
        RECT opt{}, reb{};
        lockActionRects(l, width, height, opt, reb);
        HBRUSH optFill = PcUi::CreateBrush(theme.accent);
        PcUi::DrawRoundFill(dc, opt, 12, optFill); DeleteObject(optFill);
        PcUi::DrawRoundBorder(dc, opt, 12, PcUi::Rgb(theme.accent2), 1);
        PcUi::DrawTextCenter(dc, opt, L"设置", font, PcUi::Rgb(theme.bg));

        HBRUSH rebFill = PcUi::CreateBrush(theme.panel);
        PcUi::DrawRoundFill(dc, reb, 12, rebFill); DeleteObject(rebFill);
        PcUi::DrawRoundBorder(dc, reb, 12, PcUi::Rgb(theme.accent), 1);
        PcUi::DrawTextCenter(dc, reb, L"重绑", font, PcUi::Rgb(theme.text));
    }

    if (!l.triggerLabel.empty()) {
        RECT label{cx - 78, cy - r - 30, cx + 78, cy - r - 6};
        HBRUSH labelFill = PcUi::CreateBrush(theme.panel);
        PcUi::DrawRoundFill(dc, label, 12, labelFill); DeleteObject(labelFill);
        PcUi::DrawRoundBorder(dc, label, 12, PcUi::Rgb(theme.accent), 1);
        PcUi::DrawTextCenter(dc, label, l.triggerLabel.c_str(), font, PcUi::Rgb(theme.text));
    }
}


static int compassRadiusPx(int radiusNorm, int width, int height) {
    (void)width;
    (void)height;
    // 轮盘半径滑条值直接按 PC 像素/DP 使用：80 -> 80px，240 -> 240px。
    const int r = PcMappingProfile::ClampRadiusNorm(radiusNorm) / PC_COMPASS_RADIUS_UNIT;
    return (std::max)(20, r);
}

static std::wstring compactCompassLabel(const PcCompassButtonBinding& b, const wchar_t* fallback) {
    std::wstring label = b.triggerLabel.empty() ? fallback : b.triggerLabel;
    if (label.size() > 5) label = label.substr(0, 4) + L"…";
    return label;
}

static void compassButtonRects(const PcCompassBinding& c, int width, int height, RECT& up, RECT& left, RECT& down, RECT& right, RECT& center) {
    const int cx = normToPx(c.centerXNorm, width);
    const int cy = normToPx(c.centerYNorm, height);
    const int inner = compassRadiusPx(c.innerRadiusNorm, width, height);
    const int chipW = 54;
    const int chipH = 24;
    up = RECT{cx - chipW / 2, cy - inner - chipH / 2, cx + chipW / 2, cy - inner + chipH / 2};
    left = RECT{cx - inner - chipW / 2, cy - chipH / 2, cx - inner + chipW / 2, cy + chipH / 2};
    down = RECT{cx - chipW / 2, cy + inner - chipH / 2, cx + chipW / 2, cy + inner + chipH / 2};
    right = RECT{cx + inner - chipW / 2, cy - chipH / 2, cx + inner + chipW / 2, cy + chipH / 2};
    center = RECT{cx - 28, cy - 14, cx + 28, cy + 14};
}

static void compassActionRects(const PcCompassBinding& c, int width, int height, RECT& fixed, RECT& sway, RECT& reverse) {
    const int cx = normToPx(c.centerXNorm, width);
    const int cy = normToPx(c.centerYNorm, height);
    const int outer = compassRadiusPx(c.outerRadiusNorm, width, height);
    const int y = cy + outer + 12;
    fixed = RECT{cx - 86, y, cx - 30, y + 24};
    sway = RECT{cx - 24, y, cx + 32, y + 24};
    reverse = RECT{cx + 38, y, cx + 110, y + 24};
}

static RECT compassOptionsRect(const PcCompassBinding& c, int width, int height) {
    const int cx = normToPx(c.centerXNorm, width);
    const int cy = normToPx(c.centerYNorm, height);
    const int outer = compassRadiusPx(c.outerRadiusNorm, width, height);
    int y = cy + outer + 12;
    if (y + 64 > height) y = cy - outer - 76;
    return RECT{cx - 46, y, cx + 46, y + 26};
}

static RECT compassDeleteRect(const PcCompassBinding& c, int width, int height) {
    RECT opt = compassOptionsRect(c, width, height);
    return RECT{opt.left, opt.bottom + 8, opt.right, opt.bottom + 34};
}


static RECT compassDiagHandleRect(const PcCompassBinding& c, int width, int height, int slot) {
    const int cx = normToPx(c.centerXNorm, width);
    const int cy = normToPx(c.centerYNorm, height);
    int nx = -1;
    int ny = -1;
    switch (slot) {
        case 0: nx = c.diagLeftUpXNorm; ny = c.diagLeftUpYNorm; break;
        case 1: nx = c.diagRightUpXNorm; ny = c.diagRightUpYNorm; break;
        case 2: nx = c.diagLeftDownXNorm; ny = c.diagLeftDownYNorm; break;
        case 3: nx = c.diagRightDownXNorm; ny = c.diagRightDownYNorm; break;
        default: break;
    }
    if (nx < 0 || ny < 0) {
        const int r = (compassRadiusPx(c.innerRadiusNorm, width, height) + compassRadiusPx(c.outerRadiusNorm, width, height)) / 2;
        const int d = static_cast<int>(r * 0.70710678);
        switch (slot) {
            case 0: return RECT{cx - d - 16, cy - d - 16, cx - d + 16, cy - d + 16};
            case 1: return RECT{cx + d - 16, cy - d - 16, cx + d + 16, cy - d + 16};
            case 2: return RECT{cx - d - 16, cy + d - 16, cx - d + 16, cy + d + 16};
            case 3: return RECT{cx + d - 16, cy + d - 16, cx + d + 16, cy + d + 16};
            default: return RECT{cx - 16, cy - 16, cx + 16, cy + 16};
        }
    }
    const int x = normToPx(nx, width);
    const int y = normToPx(ny, height);
    return RECT{x - 16, y - 16, x + 16, y + 16};
}

static void drawCompassDiagAnchors(HDC dc, HFONT font, const PcCompassBinding& c, int width, int height) {
    const auto& theme = PcUi::DefaultTheme();
    const int cx = normToPx(c.centerXNorm, width);
    const int cy = normToPx(c.centerYNorm, height);
    const wchar_t* labels[4] = {L"LU", L"RU", L"LD", L"RD"};
    for (int i = 0; i < 4; ++i) {
        RECT r = compassDiagHandleRect(c, width, height, i);
        const int hx = (r.left + r.right) / 2;
        const int hy = (r.top + r.bottom) / 2;
        drawLine(dc, cx, cy, hx, hy, PcUi::Rgb(theme.stroke), 1);
        drawEllipseFillBorder(dc, RECT{hx - 12, hy - 12, hx + 13, hy + 13}, RGB(5, 72, 116), RGB(34, 173, 244), 2);
        PcUi::DrawTextCenter(dc, RECT{hx - 15, hy - 10, hx + 15, hy + 10}, labels[i], font, PcUi::Rgb(theme.text));
    }
}

static void drawCompassChip(HDC dc, HFONT font, const RECT& rc, const wchar_t* text, bool selected, COLORREF customFill = CLR_INVALID, COLORREF customBorder = CLR_INVALID) {
    const auto& theme = PcUi::DefaultTheme();
    const COLORREF fillColor = (customFill != CLR_INVALID) ? customFill : PcUi::Rgb(selected ? theme.accent : theme.panel);
    const COLORREF borderColor = (customBorder != CLR_INVALID) ? customBorder : PcUi::Rgb(selected ? theme.accent2 : theme.stroke);
    const COLORREF textColor = selected ? PcUi::Rgb(theme.bg) : PcUi::Rgb(theme.text);
    HBRUSH fill = CreateSolidBrush(fillColor);
    PcUi::DrawRoundFill(dc, rc, 12, fill);
    DeleteObject(fill);
    PcUi::DrawRoundBorder(dc, rc, 12, borderColor, selected ? 2 : 1);
    PcUi::DrawTextCenter(dc, rc, text, font, textColor);
}


static constexpr double kPiOverlay = 3.14159265358979323846;

static double degToRad(double deg) { return deg * kPiOverlay / 180.0; }

static double normAngleDiffOverlay(double a, double b) {
    double d = a - b;
    while (d > kPiOverlay) d -= 2.0 * kPiOverlay;
    while (d < -kPiOverlay) d += 2.0 * kPiOverlay;
    return d;
}

static int clampSectorPercentOverlay(int v) {
    return (std::max)(25, (std::min)(140, v));
}

static int clampDiagonalSectorPercentOverlay(int v) {
    return (std::max)(25, (std::min)(90, v));
}

static int compassDiagSectorPercent(const PcCompassBinding& c, int slot) {
    switch (slot) {
        case 0: return clampDiagonalSectorPercentOverlay(c.swayDiagLeftUpSectorPercent);
        case 1: return clampDiagonalSectorPercentOverlay(c.swayDiagRightUpSectorPercent);
        case 2: return clampDiagonalSectorPercentOverlay(c.swayDiagLeftDownSectorPercent);
        case 3: return clampDiagonalSectorPercentOverlay(c.swayDiagRightDownSectorPercent);
        default: return clampDiagonalSectorPercentOverlay(c.swayDiagonalSectorPercent);
    }
}

static double compassDiagCenterAnglePx(const PcCompassBinding& c, int width, int height, int slot) {
    const int cx = normToPx(c.centerXNorm, width);
    const int cy = normToPx(c.centerYNorm, height);
    RECT r = compassDiagHandleRect(c, width, height, slot);
    const int hx = (r.left + r.right) / 2;
    const int hy = (r.top + r.bottom) / 2;
    if (hx == cx && hy == cy) {
        static const double fallback[4] = {degToRad(-135.0), degToRad(-45.0), degToRad(135.0), degToRad(45.0)};
        return fallback[(std::max)(0, (std::min)(3, slot))];
    }
    return std::atan2(static_cast<double>(hy - cy), static_cast<double>(hx - cx));
}

static void drawSectorWedge(HDC dc, int cx, int cy, int inner, int outer, double centerRad, double halfRad, COLORREF fill, COLORREF border) {
    const int steps = 18;
    POINT pts[40]{};
    int n = 0;
    const double a0 = centerRad - halfRad;
    const double a1 = centerRad + halfRad;
    for (int i = 0; i <= steps; ++i) {
        const double t = static_cast<double>(i) / static_cast<double>(steps);
        const double a = a0 + (a1 - a0) * t;
        pts[n++] = POINT{cx + static_cast<LONG>(std::lround(std::cos(a) * outer)), cy + static_cast<LONG>(std::lround(std::sin(a) * outer))};
    }
    for (int i = steps; i >= 0; --i) {
        const double t = static_cast<double>(i) / static_cast<double>(steps);
        const double a = a0 + (a1 - a0) * t;
        pts[n++] = POINT{cx + static_cast<LONG>(std::lround(std::cos(a) * inner)), cy + static_cast<LONG>(std::lround(std::sin(a) * inner))};
    }
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ oldB = SelectObject(dc, brush);
    HGDIOBJ oldP = SelectObject(dc, pen);
    Polygon(dc, pts, n);
    SelectObject(dc, oldP);
    SelectObject(dc, oldB);
    DeleteObject(pen);
    DeleteObject(brush);
}

static void drawSectorBoundary(HDC dc, int cx, int cy, int inner, int outer, double centerRad, double halfRad, COLORREF color) {
    HPEN pen = CreatePen(PS_SOLID, 2, color);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    const double angles[2] = {centerRad - halfRad, centerRad + halfRad};
    for (double a : angles) {
        const int x1 = cx + static_cast<int>(std::lround(std::cos(a) * inner));
        const int y1 = cy + static_cast<int>(std::lround(std::sin(a) * inner));
        const int x2 = cx + static_cast<int>(std::lround(std::cos(a) * outer));
        const int y2 = cy + static_cast<int>(std::lround(std::sin(a) * outer));
        MoveToEx(dc, x1, y1, nullptr);
        LineTo(dc, x2, y2);
    }
    SelectObject(dc, oldPen);
    DeleteObject(pen);
}

static RECT sectorHandleRectAt(int cx, int cy, int outer, double angleRad) {
    const int x = cx + static_cast<int>(std::lround(std::cos(angleRad) * outer));
    const int y = cy + static_cast<int>(std::lround(std::sin(angleRad) * outer));
    return RECT{x - 7, y - 7, x + 8, y + 8};
}

static void drawSectorHandle(HDC dc, const RECT& r, COLORREF fill, COLORREF border) {
    HBRUSH b = CreateSolidBrush(fill);
    HPEN p = CreatePen(PS_SOLID, 2, border);
    HGDIOBJ oldB = SelectObject(dc, b);
    HGDIOBJ oldP = SelectObject(dc, p);
    Ellipse(dc, r.left, r.top, r.right, r.bottom);
    SelectObject(dc, oldP);
    SelectObject(dc, oldB);
    DeleteObject(p);
    DeleteObject(b);
}

static void drawCompassSectorRanges(HDC dc, const PcCompassBinding& c, int width, int height) {
    const int cx = normToPx(c.centerXNorm, width);
    const int cy = normToPx(c.centerYNorm, height);
    const int inner = compassRadiusPx(c.innerRadiusNorm, width, height);
    const int outer = compassRadiusPx(c.outerRadiusNorm, width, height);
    const COLORREF purple = RGB(190, 86, 238);
    const COLORREF purpleFill = RGB(74, 26, 104);
    const COLORREF blue = RGB(34, 173, 244);
    const COLORREF blueFill = RGB(5, 72, 116);
    const double baseHalf = degToRad(22.5);
    const double cardHalf = baseHalf * clampSectorPercentOverlay(c.swaySectorPercent) / 100.0;
    const double cardCenters[4] = {degToRad(-90.0), degToRad(180.0), degToRad(90.0), degToRad(0.0)}; // W A S D

    for (double a : cardCenters) {
        drawSectorWedge(dc, cx, cy, inner, outer, a, cardHalf, purpleFill, purple);
        drawSectorBoundary(dc, cx, cy, inner, outer, a, cardHalf, purple);
        drawSectorHandle(dc, sectorHandleRectAt(cx, cy, outer, a - cardHalf), RGB(66, 18, 86), purple);
        drawSectorHandle(dc, sectorHandleRectAt(cx, cy, outer, a + cardHalf), RGB(66, 18, 86), purple);
    }
    for (int i = 0; i < 4; ++i) {
        const double a = compassDiagCenterAnglePx(c, width, height, i);
        const double h = baseHalf * compassDiagSectorPercent(c, i) / 100.0;
        drawSectorWedge(dc, cx, cy, inner, outer, a, h, blueFill, blue);
        drawSectorBoundary(dc, cx, cy, inner, outer, a, h, blue);
        drawSectorHandle(dc, sectorHandleRectAt(cx, cy, outer, a - h), RGB(5, 47, 82), blue);
        drawSectorHandle(dc, sectorHandleRectAt(cx, cy, outer, a + h), RGB(5, 47, 82), blue);
    }
}

static bool hitCompassSectorHandle(const PcCompassBinding& c, int width, int height, int x, int y, int& group, int& index) {
    const int cx = normToPx(c.centerXNorm, width);
    const int cy = normToPx(c.centerYNorm, height);
    const int outer = compassRadiusPx(c.outerRadiusNorm, width, height);
    const double baseHalf = degToRad(22.5);
    const double cardHalf = baseHalf * clampSectorPercentOverlay(c.swaySectorPercent) / 100.0;
    const double cardCenters[4] = {degToRad(-90.0), degToRad(180.0), degToRad(90.0), degToRad(0.0)};
    for (int i = 0; i < 4; ++i) {
        const double a = cardCenters[i];
        if (ptIn(sectorHandleRectAt(cx, cy, outer, a - cardHalf), x, y) || ptIn(sectorHandleRectAt(cx, cy, outer, a + cardHalf), x, y)) {
            group = 0;
            index = i;
            return true;
        }
    }
    for (int i = 0; i < 4; ++i) {
        const double a = compassDiagCenterAnglePx(c, width, height, i);
        const double h = baseHalf * compassDiagSectorPercent(c, i) / 100.0;
        if (ptIn(sectorHandleRectAt(cx, cy, outer, a - h), x, y) || ptIn(sectorHandleRectAt(cx, cy, outer, a + h), x, y)) {
            group = 1;
            index = i;
            return true;
        }
    }
    return false;
}

static void drawCompassNode(HDC dc, HFONT font, const PcCompassBinding& c, int width, int height, bool editMode, bool selected) {
    const auto& theme = PcUi::DefaultTheme();
    const int cx = normToPx(c.centerXNorm, width);
    const int cy = normToPx(c.centerYNorm, height);
    const int inner = compassRadiusPx(c.innerRadiusNorm, width, height);
    const int outer = compassRadiusPx(c.outerRadiusNorm, width, height);
    RECT outerRc{cx - outer, cy - outer, cx + outer, cy + outer};
    RECT innerRc{cx - inner, cy - inner, cx + inner, cy + inner};
    HBRUSH transparent = reinterpret_cast<HBRUSH>(GetStockObject(NULL_BRUSH));
    HPEN outerPen = CreatePen(PS_SOLID, editMode ? 3 : 2, PcUi::Rgb(theme.accent));
    HGDIOBJ oldBrush = SelectObject(dc, transparent);
    HGDIOBJ oldPen = SelectObject(dc, outerPen);
    Ellipse(dc, outerRc.left, outerRc.top, outerRc.right, outerRc.bottom);
    SelectObject(dc, oldPen);
    DeleteObject(outerPen);
    HPEN innerPen = CreatePen(PS_SOLID, 2, PcUi::Rgb(theme.accent2));
    oldPen = SelectObject(dc, innerPen);
    Ellipse(dc, innerRc.left, innerRc.top, innerRc.right, innerRc.bottom);
    SelectObject(dc, oldPen);
    DeleteObject(innerPen);
    SelectObject(dc, oldBrush);

    drawLine(dc, cx - outer + 8, cy, cx + outer - 8, cy, PcUi::Rgb(theme.stroke), 1);
    drawLine(dc, cx, cy - outer + 8, cx, cy + outer - 8, PcUi::Rgb(theme.stroke), 1);
    drawLine(dc, cx - inner + 6, cy, cx + inner - 6, cy, PcUi::Rgb(theme.accent2), 2);
    drawLine(dc, cx, cy - inner + 6, cx, cy + inner - 6, PcUi::Rgb(theme.accent2), 2);

    if (editMode && c.motionMode == PcCompassMotionMode::MouseSway) {
        drawCompassSectorRanges(dc, c, width, height);
    }

    RECT up{}, left{}, down{}, right{}, center{};
    compassButtonRects(c, width, height, up, left, down, right, center);
    const COLORREF cardFill = RGB(74, 26, 104);
    const COLORREF cardBorder = RGB(190, 86, 238);
    drawCompassChip(dc, font, up, compactCompassLabel(c.up, L"W").c_str(), false, cardFill, cardBorder);
    drawCompassChip(dc, font, left, compactCompassLabel(c.left, L"A").c_str(), false, cardFill, cardBorder);
    drawCompassChip(dc, font, down, compactCompassLabel(c.down, L"S").c_str(), false, cardFill, cardBorder);
    drawCompassChip(dc, font, right, compactCompassLabel(c.right, L"D").c_str(), false, cardFill, cardBorder);
    drawCompassChip(dc, font, center, compactCompassLabel(c.center, L"Shift").c_str(), true);

    if (editMode) {
        drawCompassDiagAnchors(dc, font, c, width, height);

        if (selected) {
            RECT opt = compassOptionsRect(c, width, height);
            RECT del = compassDeleteRect(c, width, height);
            drawCompassChip(dc, font, opt, L"设置", true);
            drawCompassChip(dc, font, del, L"删除", false);
            const std::wstring modeText = std::wstring(c.motionMode == PcCompassMotionMode::MouseSway ? L"晃动" : L"固定") + (c.centerOuterReversed ? L" / 反转" : L"");
            RECT info{opt.left - 76, del.bottom + 4, opt.right + 76, del.bottom + 24};
            PcUi::DrawTextCenter(dc, info, modeText.c_str(), font, PcUi::Rgb(theme.textMuted));
        }
    }
}

static void setVisibleAlpha(uint8_t* data, size_t countPixels, float alphaScale) {
    alphaScale = (std::max)(0.0f, (std::min)(1.0f, alphaScale));
    for (size_t i = 0; i < countPixels; ++i) {
        uint8_t* p = data + i * 4u;
        if (p[0] || p[1] || p[2]) {
            const int lum = int(p[0]) + int(p[1]) + int(p[2]);
            int base = lum > 620 ? 255 : 224;
            int a = static_cast<int>(std::lround(base * alphaScale));
            p[3] = static_cast<uint8_t>(clampi(a, 0, 255));
        } else {
            p[3] = 0;
        }
    }
}

static void bindingRects(const PcMappingBinding& b, int cx, int cy, int width, int height, RECT& circle, RECT& options, RECT& rebind, RECT& del) {
    const int r = radiusNormToPx(b.radiusNorm, width, height);
    circle = RECT{cx - r, cy - r, cx + r, cy + r};
    options = RECT{circle.left - 58, cy - 54, circle.left - 8, cy - 24};
    rebind = RECT{circle.left - 58, cy - 14, circle.left - 8, cy + 16};
    del = RECT{circle.left - 58, cy + 26, circle.left - 8, cy + 56};
    if (options.left < 4) {
        options = RECT{circle.right + 8, cy - 54, circle.right + 58, cy - 24};
        rebind = RECT{circle.right + 8, cy - 14, circle.right + 58, cy + 16};
        del = RECT{circle.right + 8, cy + 26, circle.right + 58, cy + 56};
    }
}

static void drawActionButton(HDC dc, HFONT font, const RECT& rc, const wchar_t* text, bool primary) {
    const auto& theme = PcUi::DefaultTheme();
    HBRUSH fill = PcUi::CreateBrush(primary ? theme.accent : theme.panel);
    PcUi::DrawRoundFill(dc, rc, 12, fill); DeleteObject(fill);
    PcUi::DrawRoundBorder(dc, rc, 12, PcUi::Rgb(primary ? theme.accent2 : theme.accent), 1);
    PcUi::DrawTextCenter(dc, rc, text, font, PcUi::Rgb(primary ? theme.bg : theme.text));
}

static void drawBindingCircle(HDC dc, HFONT font, const PcMappingBinding& b, int cx, int cy, int width, int height, bool selected, bool editMode) {
    const auto& theme = PcUi::DefaultTheme();
    std::wstring label = fallbackLabel(b);
    if (label.size() > 8) label = label.substr(0, 7) + L"…";

    RECT circle{}, opt{}, reb{}, del{};
    bindingRects(b, cx, cy, width, height, circle, opt, reb, del);
    drawEllipseFillBorder(dc, circle, PcUi::Rgb(selected ? theme.panel : theme.panel2), PcUi::Rgb(selected ? theme.accent2 : theme.accent), selected ? 3 : 2);

    const int r = (circle.right - circle.left) / 2;
    RECT inner{cx - r * 7 / 10, cy - r * 7 / 10, cx + r * 7 / 10, cy + r * 7 / 10};
    drawEllipseFillBorder(dc, inner, PcUi::Rgb(theme.accent), PcUi::Rgb(theme.stroke), 1);
    RECT textRc{circle.left + 4, circle.top + 4, circle.right - 4, circle.bottom - 4};
    PcUi::DrawTextCenter(dc, textRc, label.c_str(), font, PcUi::Rgb(theme.text));

    if (editMode && selected) {
        drawActionButton(dc, font, opt, L"设置", true);
        drawActionButton(dc, font, reb, L"重绑", false);
        drawActionButton(dc, font, del, L"删除", false);
    }
}


static std::wstring macroConditionText(PcMacroRuntime::TriggerCondition c) {
    return c == PcMacroRuntime::TriggerCondition::Hold ? L"按住" : L"按下";
}

static std::wstring macroLabel(const PcMacroRuntime::Binding& m) {
    std::wstring label = m.triggerLabel.empty() ? L"未绑" : m.triggerLabel;
    if (label.size() > 6) label = label.substr(0, 5) + L"…";
    return label;
}

static RECT macroMainRect(const PcMacroRuntime::Binding& m, int width, int height) {
    const int cx = normToPx(m.xNorm, width);
    const int cy = normToPx(m.yNorm, height);
    return RECT{cx - 42, cy - 18, cx + 42, cy + 18};
}

static RECT macroBindRect(const PcMacroRuntime::Binding& m, int width, int height) {
    RECT r = macroMainRect(m, width, height);
    return RECT{r.left, r.top - 28, r.left + 34, r.top - 6};
}

static RECT macroDeleteRect(const PcMacroRuntime::Binding& m, int width, int height) {
    RECT r = macroMainRect(m, width, height);
    return RECT{r.right - 34, r.top - 28, r.right, r.top - 6};
}

static RECT macroConditionRect(const PcMacroRuntime::Binding& m, int width, int height) {
    RECT r = macroMainRect(m, width, height);
    const int totalW = 50 + 8 + 34;
    int left = ((r.left + r.right) / 2) - totalW / 2;
    left = clampi(left, 6, (std::max)(6, width - totalW - 6));
    return RECT{left, r.bottom + 8, left + 50, r.bottom + 32};
}

static RECT macroAddRect(const PcMacroRuntime::Binding& m, int width, int height) {
    RECT c = macroConditionRect(m, width, height);
    return RECT{c.right + 8, c.top, c.right + 42, c.bottom};
}

static RECT macroStepRect(const PcMacroRuntime::Step& st, int width, int height) {
    const int cx = normToPx(st.xNorm, width);
    const int cy = normToPx(st.yNorm, height);
    return RECT{cx - 20, cy - 20, cx + 20, cy + 20};
}

static int macroStepRandomRadiusPx(const PcMacroRuntime::Step& st, int width, int height) {
    const int base = (std::max)(1, (std::min)(width, height));
    return (std::max)(0, static_cast<int>((static_cast<long long>((std::max)(0, (std::min)(120000, st.randomRadiusNorm))) * base + 500000LL) / 1000000LL));
}

static RECT macroStepRangeHandleRect(const PcMacroRuntime::Step& st, int width, int height) {
    const int cx = normToPx(st.xNorm, width);
    const int cy = normToPx(st.yNorm, height);
    const int r = (std::max)(28, macroStepRandomRadiusPx(st, width, height));

    // “范”手柄放到随机范围圈上方，避免和步骤右侧的“编”按钮重叠。
    const int handleW = 24;
    const int handleH = 18;
    int left = cx - handleW / 2;
    int top = cy - r - handleH - 8;
    left = clampi(left, 4, (std::max)(4, width - handleW - 4));
    top = clampi(top, 4, (std::max)(4, height - handleH - 4));
    return RECT{left, top, left + handleW, top + handleH};
}

static RECT macroStepConfigRect(const PcMacroRuntime::Step& st, int width, int height) {
    RECT r = macroStepRect(st, width, height);
    return RECT{r.right + 6, r.top, r.right + 34, r.top + 26};
}

static RECT macroSwipeEndRect(const PcMacroRuntime::Step& st, int width, int height) {
    const int ex = normToPx(st.endXNorm, width);
    const int ey = normToPx(st.endYNorm, height);
    return RECT{ex - 14, ey - 14, ex + 14, ey + 14};
}

static std::wstring macroStepActionText(PcMacroRuntime::StepActionType t) {
    switch (t) {
        case PcMacroRuntime::StepActionType::Tap: return L"动作: 点击";
        case PcMacroRuntime::StepActionType::Swipe: return L"动作: 滑动";
        case PcMacroRuntime::StepActionType::Wait: return L"动作: 等待";
        case PcMacroRuntime::StepActionType::Key: return L"动作: 按键";
        case PcMacroRuntime::StepActionType::Wheel: return L"动作: 滚轮";
        default: return L"动作";
    }
}

static int macroStepDisplayOrder(const PcMacroRuntime::Binding& m, int stepId) {
    int index = 1;
    int current = m.startStepId;
    bool seen[256]{};
    while (current > 0 && index < 256) {
        if (current >= 0 && current < 256) {
            if (seen[current]) break;
            seen[current] = true;
        }
        const auto it = std::find_if(m.steps.begin(), m.steps.end(), [current](const PcMacroRuntime::Step& s) { return s.id == current; });
        if (it == m.steps.end()) break;
        if (it->id == stepId) return index;
        if (it->nextStepId > 0) {
            current = it->nextStepId;
        } else {
            const auto next = std::next(it);
            if (next == m.steps.end()) break;
            current = next->id;
        }
        ++index;
    }
    const auto it = std::find_if(m.steps.begin(), m.steps.end(), [stepId](const PcMacroRuntime::Step& s) { return s.id == stepId; });
    return it == m.steps.end() ? 0 : (std::max)(1, it->order);
}

static void drawMacroHint(HDC dc, HFONT font, const PcMacroRuntime::Binding& m, int width, int height) {
    const auto& theme = PcUi::DefaultTheme();
    RECT main = macroMainRect(m, width, height);
    RECT chip{main.left - 64, main.top - 104, main.right + 64, main.top - 36};
    const int chipW = chip.right - chip.left;
    if (chip.left < 8) { chip.left = 8; chip.right = chip.left + chipW; }
    if (chip.right > width - 8) { chip.right = width - 8; chip.left = chip.right - chipW; }
    if (chip.top < 8) { chip.top = main.bottom + 40; chip.bottom = chip.top + 68; }
    HBRUSH fill = PcUi::CreateBrush(theme.panel);
    PcUi::DrawRoundFill(dc, chip, 12, fill); DeleteObject(fill);
    PcUi::DrawRoundBorder(dc, chip, 12, PcUi::Rgb(theme.accent), 1);
    RECT l1{chip.left + 6, chip.top + 4, chip.right - 6, chip.top + 20};
    RECT l2{chip.left + 6, chip.top + 20, chip.right - 6, chip.top + 36};
    RECT l3{chip.left + 6, chip.top + 36, chip.right - 6, chip.top + 52};
    RECT l4{chip.left + 6, chip.top + 52, chip.right - 6, chip.top + 68};
    PcUi::DrawTextCenter(dc, l1, L"多种触发方式：按下 / 按住", font, PcUi::Rgb(theme.textMuted));
    PcUi::DrawTextCenter(dc, l2, L"按住后循环触发，松开后停止", font, PcUi::Rgb(theme.textMuted));
    PcUi::DrawTextCenter(dc, l3, L"点击 + 创建新的点击位置", font, PcUi::Rgb(theme.textMuted));
    PcUi::DrawTextCenter(dc, l4, L"点击 绑 设置触发键", font, PcUi::Rgb(theme.textMuted));
}

static void drawMacroNode(HDC dc, HFONT font, const PcMacroRuntime::Binding& m, int width, int height, bool selected, int selectedStepId, bool editMode) {
    const auto& theme = PcUi::DefaultTheme();
    const int mcx = normToPx(m.xNorm, width);
    const int mcy = normToPx(m.yNorm, height);

    for (const auto& st : m.steps) {
        int nextId = st.nextStepId;
        if (nextId <= 0) {
            auto sorted = m.steps;
            std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.order < b.order; });
            auto it = std::find_if(sorted.begin(), sorted.end(), [&st](const auto& s) { return s.id == st.id; });
            if (it != sorted.end() && std::next(it) != sorted.end()) nextId = std::next(it)->id;
        }
        if (nextId > 0) {
            const auto it = std::find_if(m.steps.begin(), m.steps.end(), [nextId](const auto& s) { return s.id == nextId; });
            if (it != m.steps.end()) {
                drawLine(dc, normToPx(st.xNorm, width), normToPx(st.yNorm, height), normToPx(it->xNorm, width), normToPx(it->yNorm, height), RGB(255, 59, 48), 3);
            }
        }
    }
    if (m.startStepId > 0) {
        const auto it = std::find_if(m.steps.begin(), m.steps.end(), [&m](const auto& s) { return s.id == m.startStepId; });
        if (it != m.steps.end()) drawLine(dc, mcx, mcy, normToPx(it->xNorm, width), normToPx(it->yNorm, height), RGB(255, 59, 48), 3);
    }

    RECT main = macroMainRect(m, width, height);
    HBRUSH fill = CreateSolidBrush(selected ? RGB(34, 48, 72) : RGB(28, 38, 55));
    PcUi::DrawRoundFill(dc, main, 10, fill); DeleteObject(fill);
    PcUi::DrawRoundBorder(dc, main, 10, selected ? RGB(51, 255, 255) : RGB(160, 190, 220), selected ? 2 : 1);
    PcUi::DrawTextCenter(dc, main, macroLabel(m).c_str(), font, PcUi::Rgb(theme.text));

    if (editMode && selected) {
        drawMacroHint(dc, font, m, width, height);
        drawActionButton(dc, font, macroBindRect(m, width, height), m.triggerLabel.empty() ? L"绑" : macroLabel(m).c_str(), false);
        drawActionButton(dc, font, macroDeleteRect(m, width, height), L"删", false);
        drawActionButton(dc, font, macroConditionRect(m, width, height), macroConditionText(m.triggerCondition).c_str(), false);
        drawActionButton(dc, font, macroAddRect(m, width, height), L"+", false);
    }

    for (const auto& st : m.steps) {
        RECT sr = macroStepRect(st, width, height);
        const bool selStep = selected && selectedStepId == st.id;
        if (selected) {
            const int cx = (sr.left + sr.right) / 2;
            const int cy = (sr.top + sr.bottom) / 2;
            const int rr = macroStepRandomRadiusPx(st, width, height);
            if (rr > 0 || selStep) {
                const int drawR = (std::max)(rr, 28);
                HPEN rangePen = CreatePen(PS_DOT, 1, RGB(51, 255, 255));
                HGDIOBJ oldPen = SelectObject(dc, rangePen);
                HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
                Ellipse(dc, cx - drawR, cy - drawR, cx + drawR, cy + drawR);
                SelectObject(dc, oldBrush);
                SelectObject(dc, oldPen);
                DeleteObject(rangePen);
                const RECT hr = macroStepRangeHandleRect(st, width, height);
                drawEllipseFillBorder(dc, hr, RGB(16, 24, 32), RGB(51, 255, 255), 1);
                PcUi::DrawTextCenter(dc, hr, L"范", font, PcUi::Rgb(theme.text));
            }
        }
        drawEllipseFillBorder(dc, sr, RGB(23, 36, 51), selStep ? RGB(51, 255, 255) : RGB(180, 205, 230), selStep ? 2 : 1);
        wchar_t orderText[16]{};
        const int ord = macroStepDisplayOrder(m, st.id);
        if (ord > 0) std::swprintf(orderText, 16, L"%d", ord); else std::swprintf(orderText, 16, L"·");
        PcUi::DrawTextCenter(dc, sr, orderText, font, PcUi::Rgb(theme.text));
        RECT act{sr.left - 44, sr.bottom + 3, sr.right + 44, sr.bottom + 21};
        PcUi::DrawTextCenter(dc, act, macroStepActionText(st.actionType).c_str(), font, PcUi::Rgb(theme.textMuted));
        if (selected) {
            drawActionButton(dc, font, macroStepConfigRect(st, width, height), L"编", false);
        }
        if (st.actionType == PcMacroRuntime::StepActionType::Swipe) {
            drawLine(dc, normToPx(st.xNorm, width), normToPx(st.yNorm, height), normToPx(st.endXNorm, width), normToPx(st.endYNorm, height), RGB(255, 59, 48), 2);
            RECT er = macroSwipeEndRect(st, width, height);
            drawEllipseFillBorder(dc, er, RGB(16, 24, 32), RGB(51, 255, 255), 1);
            PcUi::DrawTextCenter(dc, er, L"尾", font, PcUi::Rgb(theme.text));
        }
    }
}

static RECT macroEditorPanelRect(const PcMacroRuntime::Binding& m, const PcMacroRuntime::Step& step, int width, int height) {
    int x = normToPx(step.xNorm, width) + 48;
    int y = normToPx(step.yNorm, height) - 24;
    const int w = 250;
    const int h = 310;
    x = clampi(x, 8, (std::max)(8, width - w - 8));
    y = clampi(y, 8, (std::max)(8, height - h - 8));
    return RECT{x, y, x + w, y + h};
}

struct MacroEditorRectsNative {
    RECT panel, close, del, action, durationMinus, durationPlus, delayMinus, delayPlus, repeatMinus, repeatPlus, randomMinus, randomPlus, addAfter;
};

static MacroEditorRectsNative macroEditorRects(const PcMacroRuntime::Binding& m, const PcMacroRuntime::Step& step, int width, int height) {
    MacroEditorRectsNative r{};
    r.panel = macroEditorPanelRect(m, step, width, height);
    const RECT& p = r.panel;
    r.close = RECT{p.right - 40, p.top + 8, p.right - 10, p.top + 32};
    r.del = RECT{p.left + 10, p.top + 42, p.left + 62, p.top + 68};
    r.action = RECT{p.left + 72, p.top + 42, p.right - 10, p.top + 68};
    auto row = [&](int y, RECT& minus, RECT& plus) {
        minus = RECT{p.left + 16, y, p.left + 48, y + 24};
        plus = RECT{p.right - 48, y, p.right - 16, y + 24};
    };
    row(p.top + 96, r.durationMinus, r.durationPlus);
    row(p.top + 138, r.delayMinus, r.delayPlus);
    row(p.top + 180, r.repeatMinus, r.repeatPlus);
    row(p.top + 222, r.randomMinus, r.randomPlus);
    r.addAfter = RECT{p.left + 38, p.bottom - 38, p.right - 38, p.bottom - 12};
    return r;
}

static void drawMacroStepEditor(HDC dc, HFONT font, const PcMacroRuntime::Binding& m, const PcMacroRuntime::Step& step, int width, int height) {
    const auto& theme = PcUi::DefaultTheme();
    const auto r = macroEditorRects(m, step, width, height);
    HBRUSH bg = CreateSolidBrush(RGB(15, 22, 34));
    PcUi::DrawRoundFill(dc, r.panel, 14, bg); DeleteObject(bg);
    PcUi::DrawRoundBorder(dc, r.panel, 14, RGB(51, 255, 255), 1);
    RECT title{r.panel.left, r.panel.top + 6, r.panel.right, r.panel.top + 28};
    std::wstring titleText = L"步骤 " + std::to_wstring((std::max)(1, step.order));
    PcUi::DrawTextCenter(dc, title, titleText.c_str(), font, PcUi::Rgb(theme.text));
    drawActionButton(dc, font, r.close, L"关", false);
    drawActionButton(dc, font, r.del, L"删步", false);
    drawActionButton(dc, font, r.action, macroStepActionText(step.actionType).c_str(), false);
    auto drawRow = [&](const wchar_t* name, const std::wstring& value, const RECT& minus, const RECT& plus, int y) {
        RECT t{r.panel.left, y, r.panel.right, y + 16};
        RECT v{r.panel.left, y + 16, r.panel.right, y + 34};
        PcUi::DrawTextCenter(dc, t, name, font, PcUi::Rgb(theme.textMuted));
        PcUi::DrawTextCenter(dc, v, value.c_str(), font, PcUi::Rgb(theme.text));
        drawActionButton(dc, font, minus, L"-", false);
        drawActionButton(dc, font, plus, L"+", false);
    };
    const bool swipeStep = step.actionType == PcMacroRuntime::StepActionType::Swipe;
    const int shownDurationMs = swipeStep ? (std::max)(20, (std::min)(200, step.durationMs)) : step.durationMs;
    drawRow(swipeStep ? L"滑动时间 ms" : L"持续按下的时间 ms", std::to_wstring(shownDurationMs), r.durationMinus, r.durationPlus, r.panel.top + 78);
    drawRow(L"下一个动作等待时间 ms", std::to_wstring(step.delayAfterMs), r.delayMinus, r.delayPlus, r.panel.top + 120);
    drawRow(L"重复次数", std::to_wstring(step.repeatCount), r.repeatMinus, r.repeatPlus, r.panel.top + 162);
    drawRow(L"随机半径", std::to_wstring(step.randomRadiusNorm / 1000), r.randomMinus, r.randomPlus, r.panel.top + 204);
    if (swipeStep) {
        RECT hint{r.panel.left, r.panel.top + 248, r.panel.right, r.panel.top + 266};
        PcUi::DrawTextCenter(dc, hint, L"拖动尾点设置滑动终点", font, PcUi::Rgb(theme.textMuted));
    }
    drawActionButton(dc, font, r.addAfter, L"在后面新增一步", false);
}

static bool isMenuRadialCategory(PcMenuCategory cat) {
    return cat == PcMenuCategory::MenuRadial;
}

static bool isMenuItemCategory(PcMenuCategory cat) {
    return cat == PcMenuCategory::MenuItemOperation ||
           cat == PcMenuCategory::MenuHorizontal ||
           cat == PcMenuCategory::MenuVertical;
}

static bool isMenuBagCategory(PcMenuCategory cat) {
    return cat == PcMenuCategory::MenuBagOperation;
}

static bool isMenuFreeCursorCategory(PcMenuCategory cat) {
    return isMenuItemCategory(cat) || isMenuBagCategory(cat);
}

static std::wstring menuLabel(const PcMenuBinding& m) {
    switch (m.category) {
        case PcMenuCategory::MenuRadial: return L"轮盘菜单";
        case PcMenuCategory::MenuItemOperation:
        case PcMenuCategory::MenuHorizontal:
        case PcMenuCategory::MenuVertical: return L"道具操作";
        case PcMenuCategory::MenuBagOperation: return L"背包操作";
        default: return L"菜单";
    }
}

static std::wstring menuKindTag(const PcMenuBinding& m) {
    switch (m.category) {
        case PcMenuCategory::MenuRadial: return L"轮盘";
        case PcMenuCategory::MenuBagOperation: return L"背包";
        case PcMenuCategory::MenuItemOperation:
        case PcMenuCategory::MenuHorizontal:
        case PcMenuCategory::MenuVertical: return L"道具";
        default: return L"菜单";
    }
}

static void menuPalette(const PcMenuBinding& m, COLORREF& fill, COLORREF& border, COLORREF& tagFill, COLORREF& tagText) {
    if (isMenuItemCategory(m.category)) {
        // 道具操作：三个圆形 UI 统一紫色。
        fill = RGB(72, 42, 118);
        border = RGB(190, 130, 255);
        tagFill = RGB(190, 130, 255);
        tagText = RGB(18, 10, 28);
    } else if (isMenuBagCategory(m.category)) {
        // 背包操作：三个圆形 UI 统一蓝色。
        fill = RGB(34, 70, 128);
        border = RGB(88, 178, 255);
        tagFill = RGB(88, 178, 255);
        tagText = RGB(8, 18, 32);
    } else {
        fill = RGB(82, 54, 22);
        border = RGB(247, 179, 83);
        tagFill = RGB(247, 179, 83);
        tagText = RGB(22, 18, 12);
    }
}

static int menuPointRadiusPx(const PcMenuBinding& m, int width, int height) {
    // 菜单按钮 UI 的半径统一限制在 20~60。道具/背包的触发点、鼠标落点、关闭点
    // 共用这个半径；轮盘菜单的可见圆也使用同一个设置值。
    const int r = radiusNormToPx(m.radiusNorm, width, height);
    return clampi(r, PC_MENU_BUTTON_RADIUS_MIN, PC_MENU_BUTTON_RADIUS_MAX);
}

static RECT menuPointRectAt(int xNorm, int yNorm, int radiusPx, int width, int height) {
    const int cx = normToPx(xNorm, width);
    const int cy = normToPx(yNorm, height);
    return RECT{cx - radiusPx, cy - radiusPx, cx + radiusPx, cy + radiusPx};
}

static RECT unionRect2(const RECT& a, const RECT& b) {
    return RECT{(std::min)(a.left, b.left), (std::min)(a.top, b.top), (std::max)(a.right, b.right), (std::max)(a.bottom, b.bottom)};
}

static RECT menuTriggerPointRect(const PcMenuBinding& m, int width, int height) {
    const int r = menuPointRadiusPx(m, width, height);
    if (isMenuFreeCursorCategory(m.category)) {
        return menuPointRectAt(m.triggerXNorm, m.triggerYNorm, r, width, height);
    }
    return menuPointRectAt(m.centerXNorm, m.centerYNorm, r, width, height);
}

static RECT menuLandingPointRect(const PcMenuBinding& m, int width, int height) {
    const int r = menuPointRadiusPx(m, width, height);
    return menuPointRectAt(m.freeCursorXNorm, m.freeCursorYNorm, r, width, height);
}

static RECT menuClosePointRect(const PcMenuBinding& m, int width, int height) {
    const int r = menuPointRadiusPx(m, width, height);
    return menuPointRectAt(m.closeXNorm, m.closeYNorm, r, width, height);
}

static void menuRects(const PcMenuBinding& m, int width, int height, RECT& body, RECT& options, RECT& rebind, RECT& del) {
    if (isMenuFreeCursorCategory(m.category)) {
        body = menuTriggerPointRect(m, width, height);
        const int cy = (body.top + body.bottom) / 2;
        // 设置/重绑/删除只跟随“触发键圆点”，不再挂在触发点+落点+关闭点的整体外接框上。
        options = RECT{body.right + 8, cy - 54, body.right + 58, cy - 24};
        rebind = RECT{body.right + 8, cy - 14, body.right + 58, cy + 16};
        del = RECT{body.right + 8, cy + 26, body.right + 58, cy + 56};
        if (del.right > width - 4) {
            options = RECT{body.left - 58, cy - 54, body.left - 8, cy - 24};
            rebind = RECT{body.left - 58, cy - 14, body.left - 8, cy + 16};
            del = RECT{body.left - 58, cy + 26, body.left - 8, cy + 56};
        }
        return;
    }

    const int cx = normToPx(m.centerXNorm, width);
    const int cy = normToPx(m.centerYNorm, height);
    const int r = menuPointRadiusPx(m, width, height);
    body = RECT{cx - r, cy - r, cx + r, cy + r};
    options = RECT{body.right + 8, cy - 54, body.right + 58, cy - 24};
    rebind = RECT{body.right + 8, cy - 14, body.right + 58, cy + 16};
    del = RECT{body.right + 8, cy + 26, body.right + 58, cy + 56};
    if (del.right > width - 4) {
        options = RECT{body.left - 58, cy - 54, body.left - 8, cy - 24};
        rebind = RECT{body.left - 58, cy - 14, body.left - 8, cy + 16};
        del = RECT{body.left - 58, cy + 26, body.left - 8, cy + 56};
    }
}

static void drawMenuTag(HDC dc, HFONT font, const RECT& rc, const std::wstring& text, COLORREF fill, COLORREF textColor) {
    HBRUSH b = CreateSolidBrush(fill);
    PcUi::DrawRoundFill(dc, rc, 12, b);
    DeleteObject(b);
    PcUi::DrawRoundBorder(dc, rc, 12, RGB(255, 255, 255), 1);
    PcUi::DrawTextCenter(dc, rc, text.c_str(), font, textColor);
}

static std::wstring truncateMenuText(std::wstring text, size_t limit) {
    if (text.size() > limit) text = text.substr(0, limit > 0 ? limit - 1 : 0) + L"…";
    return text;
}

static void drawFreeMenuPoint(HDC dc, HFONT font, const RECT& circle, const wchar_t* title, const wchar_t* sub, COLORREF fill, COLORREF border) {
    const auto& theme = PcUi::DefaultTheme();
    const int cx = (circle.left + circle.right) / 2;
    const int cy = (circle.top + circle.bottom) / 2;
    drawEllipseFillBorder(dc, circle, fill, border, 3);
    RECT titleRc{circle.left + 3, cy - 20, circle.right - 3, cy - 2};
    PcUi::DrawTextCenter(dc, titleRc, title, font, PcUi::Rgb(theme.text));
    RECT subRc{circle.left + 3, cy + 2, circle.right - 3, cy + 20};
    PcUi::DrawTextCenter(dc, subRc, sub, font, PcUi::Rgb(theme.textMuted));
}

static void drawMenuNode(HDC dc, HFONT font, const PcMenuBinding& m, int width, int height, bool editMode, bool selected) {
    const auto& theme = PcUi::DefaultTheme();
    RECT body{}, opt{}, reb{}, del{};
    menuRects(m, width, height, body, opt, reb, del);
    const int cx = (body.left + body.right) / 2;
    const int cy = (body.top + body.bottom) / 2;
    COLORREF fill{}, border{}, tagFill{}, tagText{};
    menuPalette(m, fill, border, tagFill, tagText);

    if (isMenuRadialCategory(m.category)) {
        drawEllipseFillBorder(dc, body, fill, border, editMode ? 3 : 2);
        const int r = (body.right - body.left) / 2;
        const int count = clampi(m.segmentCount, 2, 12);
        constexpr double kPi = 3.14159265358979323846;
        for (int i = 0; i < count; ++i) {
            const double a = (-90.0 + 360.0 * static_cast<double>(i) / static_cast<double>(count)) * kPi / 180.0;
            const int ex = cx + static_cast<int>(std::lround(std::cos(a) * (r - 8)));
            const int ey = cy + static_cast<int>(std::lround(std::sin(a) * (r - 8)));
            drawLine(dc, cx, cy, ex, ey, RGB(255, 238, 210), 1);
        }
        RECT inner{cx - r * 42 / 100, cy - r * 42 / 100, cx + r * 42 / 100, cy + r * 42 / 100};
        drawEllipseFillBorder(dc, inner, RGB(28, 32, 40), RGB(255, 238, 210), 1);
        RECT text{inner.left + 2, inner.top + 2, inner.right - 2, inner.bottom - 2};
        std::wstring label = truncateMenuText(m.triggerLabel.empty() ? menuLabel(m) : m.triggerLabel, 7);
        PcUi::DrawTextCenter(dc, text, label.c_str(), font, PcUi::Rgb(theme.text));
        RECT tag{body.left + 10, body.top + 8, body.left + 58, body.top + 30};
        drawMenuTag(dc, font, tag, menuKindTag(m), tagFill, tagText);
        RECT sub{body.left + 4, cy + r * 38 / 100, body.right - 4, cy + r * 38 / 100 + 22};
        PcUi::DrawTextCenter(dc, sub, L"按住选择", font, PcUi::Rgb(theme.textMuted));
    } else {
        const RECT trigger = body;
        const RECT landing = menuLandingPointRect(m, width, height);
        const RECT close = menuClosePointRect(m, width, height);
        const RECT all = unionRect2(unionRect2(trigger, landing), close);
        const int tx = (trigger.left + trigger.right) / 2;
        const int ty = (trigger.top + trigger.bottom) / 2;
        const int lx = (landing.left + landing.right) / 2;
        const int ly = (landing.top + landing.bottom) / 2;
        const int cx2 = (close.left + close.right) / 2;
        const int cy2 = (close.top + close.bottom) / 2;
        drawLine(dc, tx, ty, lx, ly, border, 2);
        drawLine(dc, lx, ly, cx2, cy2, border, 2);

        const std::wstring triggerText = truncateMenuText(m.triggerLabel.empty() ? L"未绑" : m.triggerLabel, 4);
        const bool bag = isMenuBagCategory(m.category);
        drawFreeMenuPoint(dc, font, trigger, triggerText.c_str(), bag ? L"按住触发" : L"按下触发", fill, border);
        drawFreeMenuPoint(dc, font, landing, L"鼠标", L"落点", fill, border);
        drawFreeMenuPoint(dc, font, close, L"隐藏", bag ? L"松开" : L"右键", fill, border);
    }

    if (editMode && selected) {
        drawActionButton(dc, font, opt, L"设置", true);
        drawActionButton(dc, font, reb, L"重绑", false);
        drawActionButton(dc, font, del, L"删除", false);
    }
}

} // namespace

void SetPcMappingOverlayMacroSource(const std::vector<PcMacroRuntime::Binding>* macros, int selectedMacroIndex, int selectedMacroStepId, bool macroStepEditorVisible) {
    g_macroSource = macros;
    g_selectedMacroIndex = selectedMacroIndex;
    g_selectedMacroStepId = selectedMacroStepId;
    g_macroStepEditorVisible = macroStepEditorVisible;
}

PcMappingOverlayHit HitTestPcMappingOverlay(int width, int height, const PcMappingProfile& profile, const PcMappingOverlayOptions& options, int x, int y) {
    PcMappingOverlayHit hit;
    if (!options.editMode || width <= 1 || height <= 1) return hit;

    const auto* macros = options.macros ? options.macros : g_macroSource;
    const int selectedMacroIndex = options.selectedMacroIndex >= 0 ? options.selectedMacroIndex : g_selectedMacroIndex;
    const int selectedMacroStepId = options.selectedMacroStepId > 0 ? options.selectedMacroStepId : g_selectedMacroStepId;
    const bool macroStepEditorVisible = options.macroStepEditorVisible || g_macroStepEditorVisible;
    if (macros) {
        if (macroStepEditorVisible && selectedMacroIndex >= 0 &&
            selectedMacroIndex < static_cast<int>(macros->size())) {
            const auto& m = (*macros)[static_cast<size_t>(selectedMacroIndex)];
            const auto stIt = std::find_if(m.steps.begin(), m.steps.end(), [sid = selectedMacroStepId](const auto& st) { return st.id == sid; });
            if (stIt != m.steps.end()) {
                const auto er = macroEditorRects(m, *stIt, width, height);
                if (ptIn(er.close, x, y)) { hit.kind = PcMappingOverlayHit::Kind::MacroStepEditorClose; hit.macroIndex = static_cast<size_t>(selectedMacroIndex); hit.macroStepId = stIt->id; return hit; }
                if (ptIn(er.del, x, y)) { hit.kind = PcMappingOverlayHit::Kind::MacroStepEditorDelete; hit.macroIndex = static_cast<size_t>(selectedMacroIndex); hit.macroStepId = stIt->id; return hit; }
                if (ptIn(er.action, x, y)) { hit.kind = PcMappingOverlayHit::Kind::MacroStepEditorAction; hit.macroIndex = static_cast<size_t>(selectedMacroIndex); hit.macroStepId = stIt->id; return hit; }
                if (ptIn(er.durationMinus, x, y)) { hit.kind = PcMappingOverlayHit::Kind::MacroStepEditorDurationMinus; hit.macroIndex = static_cast<size_t>(selectedMacroIndex); hit.macroStepId = stIt->id; return hit; }
                if (ptIn(er.durationPlus, x, y)) { hit.kind = PcMappingOverlayHit::Kind::MacroStepEditorDurationPlus; hit.macroIndex = static_cast<size_t>(selectedMacroIndex); hit.macroStepId = stIt->id; return hit; }
                if (ptIn(er.delayMinus, x, y)) { hit.kind = PcMappingOverlayHit::Kind::MacroStepEditorDelayMinus; hit.macroIndex = static_cast<size_t>(selectedMacroIndex); hit.macroStepId = stIt->id; return hit; }
                if (ptIn(er.delayPlus, x, y)) { hit.kind = PcMappingOverlayHit::Kind::MacroStepEditorDelayPlus; hit.macroIndex = static_cast<size_t>(selectedMacroIndex); hit.macroStepId = stIt->id; return hit; }
                if (ptIn(er.repeatMinus, x, y)) { hit.kind = PcMappingOverlayHit::Kind::MacroStepEditorRepeatMinus; hit.macroIndex = static_cast<size_t>(selectedMacroIndex); hit.macroStepId = stIt->id; return hit; }
                if (ptIn(er.repeatPlus, x, y)) { hit.kind = PcMappingOverlayHit::Kind::MacroStepEditorRepeatPlus; hit.macroIndex = static_cast<size_t>(selectedMacroIndex); hit.macroStepId = stIt->id; return hit; }
                if (ptIn(er.randomMinus, x, y)) { hit.kind = PcMappingOverlayHit::Kind::MacroStepEditorRandomMinus; hit.macroIndex = static_cast<size_t>(selectedMacroIndex); hit.macroStepId = stIt->id; return hit; }
                if (ptIn(er.randomPlus, x, y)) { hit.kind = PcMappingOverlayHit::Kind::MacroStepEditorRandomPlus; hit.macroIndex = static_cast<size_t>(selectedMacroIndex); hit.macroStepId = stIt->id; return hit; }
                if (ptIn(er.addAfter, x, y)) { hit.kind = PcMappingOverlayHit::Kind::MacroStepEditorAddAfter; hit.macroIndex = static_cast<size_t>(selectedMacroIndex); hit.macroStepId = stIt->id; return hit; }
            }
        }

        for (int i = static_cast<int>(macros->size()) - 1; i >= 0; --i) {
            const auto& m = (*macros)[static_cast<size_t>(i)];
            const bool selected = selectedMacroIndex == i;
            if (selected) {
                if (ptIn(macroBindRect(m, width, height), x, y)) { hit.kind = PcMappingOverlayHit::Kind::MacroBind; hit.macroIndex = static_cast<size_t>(i); return hit; }
                if (ptIn(macroDeleteRect(m, width, height), x, y)) { hit.kind = PcMappingOverlayHit::Kind::MacroDelete; hit.macroIndex = static_cast<size_t>(i); return hit; }
                if (ptIn(macroConditionRect(m, width, height), x, y)) { hit.kind = PcMappingOverlayHit::Kind::MacroTriggerCondition; hit.macroIndex = static_cast<size_t>(i); return hit; }
                if (ptIn(macroAddRect(m, width, height), x, y)) { hit.kind = PcMappingOverlayHit::Kind::MacroAddStep; hit.macroIndex = static_cast<size_t>(i); return hit; }
                for (int si = static_cast<int>(m.steps.size()) - 1; si >= 0; --si) {
                    const auto& st = m.steps[static_cast<size_t>(si)];
                    if (ptIn(macroStepRangeHandleRect(st, width, height), x, y)) { hit.kind = PcMappingOverlayHit::Kind::MacroStepRangeHandle; hit.macroIndex = static_cast<size_t>(i); hit.macroStepId = st.id; return hit; }
                    if (st.actionType == PcMacroRuntime::StepActionType::Swipe && ptIn(macroSwipeEndRect(st, width, height), x, y)) { hit.kind = PcMappingOverlayHit::Kind::MacroStepSwipeEndHandle; hit.macroIndex = static_cast<size_t>(i); hit.macroStepId = st.id; return hit; }
                    if (ptIn(macroStepConfigRect(st, width, height), x, y)) { hit.kind = PcMappingOverlayHit::Kind::MacroStepConfig; hit.macroIndex = static_cast<size_t>(i); hit.macroStepId = st.id; return hit; }
                }
            }
            for (int si = static_cast<int>(m.steps.size()) - 1; si >= 0; --si) {
                const auto& st = m.steps[static_cast<size_t>(si)];
                RECT sr = macroStepRect(st, width, height);
                const int cx = (sr.left + sr.right) / 2;
                const int cy = (sr.top + sr.bottom) / 2;
                const int rr = (sr.right - sr.left) / 2;
                if (ptInCircle(cx, cy, rr, x, y)) { hit.kind = PcMappingOverlayHit::Kind::MacroStepBody; hit.macroIndex = static_cast<size_t>(i); hit.macroStepId = st.id; return hit; }
            }
            if (ptIn(macroMainRect(m, width, height), x, y)) { hit.kind = PcMappingOverlayHit::Kind::MacroBody; hit.macroIndex = static_cast<size_t>(i); return hit; }
        }
    }

    for (int i = static_cast<int>(profile.menus.size()) - 1; i >= 0; --i) {
        const auto& m = profile.menus[static_cast<size_t>(i)];
        RECT body{}, opt{}, reb{}, del{};
        menuRects(m, width, height, body, opt, reb, del);
        if (options.selectedMenuIndex == i && ptIn(opt, x, y)) { hit.kind = PcMappingOverlayHit::Kind::MenuOptions; hit.menuIndex = static_cast<size_t>(i); return hit; }
        if (options.selectedMenuIndex == i && ptIn(reb, x, y)) { hit.kind = PcMappingOverlayHit::Kind::MenuRebind; hit.menuIndex = static_cast<size_t>(i); return hit; }
        if (options.selectedMenuIndex == i && ptIn(del, x, y)) { hit.kind = PcMappingOverlayHit::Kind::MenuDelete; hit.menuIndex = static_cast<size_t>(i); return hit; }
        if (isMenuFreeCursorCategory(m.category)) {
            const RECT trigger = menuTriggerPointRect(m, width, height);
            const RECT landing = menuLandingPointRect(m, width, height);
            const RECT close = menuClosePointRect(m, width, height);
            const int ccx = (close.left + close.right) / 2;
            const int ccy = (close.top + close.bottom) / 2;
            const int cr = (close.right - close.left) / 2;
            if (ptInCircle(ccx, ccy, cr, x, y)) { hit.kind = PcMappingOverlayHit::Kind::MenuCloseAnchor; hit.menuIndex = static_cast<size_t>(i); return hit; }
            const int lcx = (landing.left + landing.right) / 2;
            const int lcy = (landing.top + landing.bottom) / 2;
            const int lr = (landing.right - landing.left) / 2;
            if (ptInCircle(lcx, lcy, lr, x, y)) { hit.kind = PcMappingOverlayHit::Kind::MenuLandingAnchor; hit.menuIndex = static_cast<size_t>(i); return hit; }
            const int tcx = (trigger.left + trigger.right) / 2;
            const int tcy = (trigger.top + trigger.bottom) / 2;
            const int tr = (trigger.right - trigger.left) / 2;
            if (ptInCircle(tcx, tcy, tr, x, y)) { hit.kind = PcMappingOverlayHit::Kind::MenuTriggerAnchor; hit.menuIndex = static_cast<size_t>(i); return hit; }
        } else {
            const int cx = (body.left + body.right) / 2;
            const int cy = (body.top + body.bottom) / 2;
            const int r = (body.right - body.left) / 2;
            if (ptInCircle(cx, cy, r, x, y)) { hit.kind = PcMappingOverlayHit::Kind::MenuBody; hit.menuIndex = static_cast<size_t>(i); return hit; }
        }
    }

    if (!profile.compasses.empty()) {
        const auto& c = profile.compasses.front();
        RECT opt = compassOptionsRect(c, width, height);
        RECT del = compassDeleteRect(c, width, height);
        if (options.selectedCompass && ptIn(opt, x, y)) { hit.kind = PcMappingOverlayHit::Kind::CompassOptions; return hit; }
        if (options.selectedCompass && ptIn(del, x, y)) { hit.kind = PcMappingOverlayHit::Kind::CompassDelete; return hit; }
        int sectorGroup = -1;
        int sectorIndex = -1;
        if (hitCompassSectorHandle(c, width, height, x, y, sectorGroup, sectorIndex)) {
            hit.kind = PcMappingOverlayHit::Kind::CompassSectorHandle;
            hit.compassSectorGroup = sectorGroup;
            hit.compassSectorIndex = sectorIndex;
            return hit;
        }
        for (int i = 0; i < 4; ++i) {
            RECT dr = compassDiagHandleRect(c, width, height, i);
            if (ptIn(dr, x, y)) { hit.kind = PcMappingOverlayHit::Kind::CompassDiagAnchor; hit.compassDiagSlot = i; return hit; }
        }
        RECT up{}, left{}, down{}, right{}, center{};
        compassButtonRects(c, width, height, up, left, down, right, center);
        if (ptIn(up, x, y)) { hit.kind = PcMappingOverlayHit::Kind::CompassButtonRebind; hit.compassButton = 0; return hit; }
        if (ptIn(left, x, y)) { hit.kind = PcMappingOverlayHit::Kind::CompassButtonRebind; hit.compassButton = 1; return hit; }
        if (ptIn(down, x, y)) { hit.kind = PcMappingOverlayHit::Kind::CompassButtonRebind; hit.compassButton = 2; return hit; }
        if (ptIn(right, x, y)) { hit.kind = PcMappingOverlayHit::Kind::CompassButtonRebind; hit.compassButton = 3; return hit; }
        if (ptIn(center, x, y)) { hit.kind = PcMappingOverlayHit::Kind::CompassButtonRebind; hit.compassButton = 4; return hit; }
        const int cx = normToPx(c.centerXNorm, width);
        const int cy = normToPx(c.centerYNorm, height);
        const int r = compassRadiusPx(c.outerRadiusNorm, width, height);
        if (ptInCircle(cx, cy, r, x, y)) { hit.kind = PcMappingOverlayHit::Kind::CompassBody; return hit; }
    }

    if (!profile.locks.empty()) {
        const auto& l = profile.locks.front();
        RECT range{}; lockRangeRect(l, width, height, range);
        RECT c1{}, c2{}, c3{}; lockModeChipRects(range, height, c1, c2, c3);
        if (ptIn(c1, x, y)) { hit.kind = PcMappingOverlayHit::Kind::LockModeChip; hit.lockMode = 1; return hit; }
        if (ptIn(c2, x, y)) { hit.kind = PcMappingOverlayHit::Kind::LockModeChip; hit.lockMode = 2; return hit; }
        if (ptIn(c3, x, y)) { hit.kind = PcMappingOverlayHit::Kind::LockModeChip; hit.lockMode = 3; return hit; }
        RECT opt{}, reb{}; lockActionRects(l, width, height, opt, reb);
        if (ptIn(opt, x, y)) { hit.kind = PcMappingOverlayHit::Kind::LockOptions; return hit; }
        if (ptIn(reb, x, y)) { hit.kind = PcMappingOverlayHit::Kind::LockRebind; return hit; }
    }

    for (int i = static_cast<int>(profile.bindings.size()) - 1; i >= 0; --i) {
        const auto& b = profile.bindings[static_cast<size_t>(i)];
        if (b.actionType == PcMappingActionType::None && !(options.editMode && b.triggerLabel == L"未绑定")) continue;
        const int cx = normToPx(b.xNorm, width);
        const int cy = normToPx(b.yNorm, height);
        RECT circle{}, opt{}, reb{}, del{};
        bindingRects(b, cx, cy, width, height, circle, opt, reb, del);
        if (options.selectedBindingIndex == i && ptIn(opt, x, y)) { hit.kind = PcMappingOverlayHit::Kind::BindingOptions; hit.bindingIndex = static_cast<size_t>(i); return hit; }
        if (options.selectedBindingIndex == i && ptIn(reb, x, y)) { hit.kind = PcMappingOverlayHit::Kind::BindingRebind; hit.bindingIndex = static_cast<size_t>(i); return hit; }
        if (options.selectedBindingIndex == i && ptIn(del, x, y)) { hit.kind = PcMappingOverlayHit::Kind::BindingDelete; hit.bindingIndex = static_cast<size_t>(i); return hit; }
        const int r = (circle.right - circle.left) / 2;
        if (ptInCircle(cx, cy, r, x, y)) { hit.kind = PcMappingOverlayHit::Kind::BindingChip; hit.bindingIndex = static_cast<size_t>(i); return hit; }
    }
    return hit;
}

bool BuildPcMappingOverlayFrame(int width, int height, const PcMappingProfile& profile, const PcMappingOverlayOptions& options, PcMappingOverlayFrame& outFrame) {
    outFrame.width = width;
    outFrame.height = height;
    outFrame.visible = false;
    if (width <= 1 || height <= 1 || (profile.bindings.empty() && profile.locks.empty() && profile.compasses.empty() && profile.menus.empty() && (!options.macros || options.macros->empty()) && (!g_macroSource || g_macroSource->empty()))) { outFrame.bgra.clear(); return false; }
    const size_t bytes = static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;
    outFrame.bgra.assign(bytes, 0);
    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) return false;
    BITMAPINFO bmi{}; bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); bmi.bmiHeader.biWidth = width; bmi.bmiHeader.biHeight = -height; bmi.bmiHeader.biPlanes = 1; bmi.bmiHeader.biBitCount = 32; bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp || !bits) { if (bmp) DeleteObject(bmp); DeleteDC(dc); return false; }
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    std::fill_n(static_cast<uint8_t*>(bits), bytes, 0);

    HFONT font = PcUi::CreateUiFont(10, true);
    HFONT oldFont = font ? static_cast<HFONT>(SelectObject(dc, font)) : nullptr;
    SetBkMode(dc, TRANSPARENT);

    const auto* buildMacros = options.macros ? options.macros : g_macroSource;
    const int buildSelectedMacroIndex = options.selectedMacroIndex >= 0 ? options.selectedMacroIndex : g_selectedMacroIndex;
    const int buildSelectedMacroStepId = options.selectedMacroStepId > 0 ? options.selectedMacroStepId : g_selectedMacroStepId;
    const bool buildMacroStepEditorVisible = options.macroStepEditorVisible || g_macroStepEditorVisible;

    if (buildMacros) {
        for (size_t i = 0; i < buildMacros->size(); ++i) {
            const auto& m = (*buildMacros)[i];
            const bool selected = options.editMode && static_cast<int>(i) == buildSelectedMacroIndex;
            drawMacroNode(dc, font, m, width, height, selected, buildSelectedMacroStepId, options.editMode);
            outFrame.visible = true;
        }
        if (options.editMode && buildMacroStepEditorVisible && buildSelectedMacroIndex >= 0 &&
            buildSelectedMacroIndex < static_cast<int>(buildMacros->size())) {
            const auto& m = (*buildMacros)[static_cast<size_t>(buildSelectedMacroIndex)];
            const auto stIt = std::find_if(m.steps.begin(), m.steps.end(), [sid = buildSelectedMacroStepId](const auto& st) { return st.id == sid; });
            if (stIt != m.steps.end()) {
                drawMacroStepEditor(dc, font, m, *stIt, width, height);
                outFrame.visible = true;
            }
        }
    }

    for (size_t i = 0; i < profile.menus.size(); ++i) { drawMenuNode(dc, font, profile.menus[i], width, height, options.editMode, static_cast<int>(i) == options.selectedMenuIndex); outFrame.visible = true; }
    for (const auto& c : profile.compasses) { drawCompassNode(dc, font, c, width, height, options.editMode, options.selectedCompass); outFrame.visible = true; }
    for (const auto& l : profile.locks) { drawLockNode(dc, font, l, width, height, options.editMode); outFrame.visible = true; }
    for (size_t i = 0; i < profile.bindings.size(); ++i) {
        const auto& b = profile.bindings[i];
        if (b.actionType == PcMappingActionType::None && !(options.editMode && b.triggerLabel == L"未绑定")) continue;
        const int x = normToPx(b.xNorm, width);
        const int y = normToPx(b.yNorm, height);
        drawBindingCircle(dc, font, b, x, y, width, height, options.editMode && static_cast<int>(i) == options.selectedBindingIndex, options.editMode);
        outFrame.visible = true;
    }

    if (oldFont) SelectObject(dc, oldFont);
    if (font) DeleteObject(font);
    std::memcpy(outFrame.bgra.data(), bits, bytes);
    const float alpha = options.editMode ? 1.0f : options.collapsedAlpha;
    setVisibleAlpha(outFrame.bgra.data(), static_cast<size_t>(width) * static_cast<size_t>(height), alpha);
    SelectObject(dc, oldBmp); DeleteObject(bmp); DeleteDC(dc);
    return outFrame.visible;
}
