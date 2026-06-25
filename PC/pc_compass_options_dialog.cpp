#include "pc_compass_options_dialog.h"
#include "pc_ui_theme.h"

#include <algorithm>
#include <cwchar>
#include <cmath>
#include <windowsx.h>

namespace {
constexpr wchar_t kClassName[] = L"HuiLangPcCompassOptionsDialog";

enum class DragKind { None, Inner, Outer, SpeedX, SpeedY, SwaySector, SwayDiagSector, SwayMin, SwayMax, SwaySpeed, MouseStep, MouseUpdate, MouseHold };

struct CompassDialogState {
    HINSTANCE inst{};
    HWND owner{};
    HWND hwnd{};
    PcCompassOptions options{};
    bool done = false;
    bool ok = false;
    WPARAM cookie = 0;
    DragKind drag = DragKind::None;
    HFONT titleFont{};
    HFONT font{};
    HFONT smallFont{};
};

static int scalePx(HWND hwnd, int px) {
    HDC dc = GetDC(hwnd);
    const int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSX) : 96;
    if (dc) ReleaseDC(hwnd, dc);
    return MulDiv(px, dpi > 0 ? dpi : 96, 96);
}
static RECT rc(int l, int t, int r, int b) { return RECT{l,t,r,b}; }
static bool ptIn(const RECT& r, int x, int y) { return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom; }
static RECT panelRect(const RECT& c) {
    const int w = c.right - c.left;
    const int h = c.bottom - c.top;
    const int pw = 560;
    const int ph = 530;
    return rc((w-pw)/2, (h-ph)/2, (w+pw)/2, (h+ph)/2);
}
static RECT closeRect(const RECT& p) { return rc(p.right-34,p.top+8,p.right-10,p.top+32); }
static RECT fixedRect(const RECT& p) { return rc(p.left+20,p.top+40,p.left+148,p.top+66); }
static RECT swayRect(const RECT& p) { return rc(p.left+160,p.top+40,p.left+288,p.top+66); }
static RECT reverseRect(const RECT& p) { return rc(p.left+300,p.top+40,p.right-20,p.top+66); }

// 双列紧凑布局。每一行左右各一个滑动条，减少高度占用。
static RECT innerSliderRect(const RECT& p) { return rc(p.left+96,p.top+96,p.left+248,p.top+108); }
static RECT outerSliderRect(const RECT& p) { return rc(p.left+368,p.top+96,p.right-24,p.top+108); }
static RECT speedXSliderRect(const RECT& p) { return rc(p.left+96,p.top+144,p.left+248,p.top+156); }
static RECT speedYSliderRect(const RECT& p) { return rc(p.left+368,p.top+144,p.right-24,p.top+156); }
static RECT swaySectorSliderRect(const RECT& p) { return rc(p.left+96,p.top+212,p.left+248,p.top+224); }
static RECT swayDiagSectorSliderRect(const RECT& p) { return rc(p.left+368,p.top+212,p.right-24,p.top+224); }
static RECT swayMinSliderRect(const RECT& p) { return rc(p.left+96,p.top+260,p.left+248,p.top+272); }
static RECT swayMaxSliderRect(const RECT& p) { return rc(p.left+368,p.top+260,p.right-24,p.top+272); }
static RECT swaySpeedSliderRect(const RECT& p) { return rc(p.left+96,p.top+308,p.left+248,p.top+320); }
static RECT mouseSmallRect(const RECT& p) { return rc(p.left+288,p.top+302,p.right-24,p.top+328); }
static RECT mouseStepSliderRect(const RECT& p) { return rc(p.left+96,p.top+376,p.left+248,p.top+388); }
static RECT mouseUpdateSliderRect(const RECT& p) { return rc(p.left+368,p.top+376,p.right-24,p.top+388); }
static RECT mouseHoldSliderRect(const RECT& p) { return rc(p.left+96,p.top+424,p.left+248,p.top+436); }
static RECT cancelRect(const RECT& p) { return rc(p.left+20,p.bottom-44,p.left+132,p.bottom-14); }
static RECT okRect(const RECT& p) { return rc(p.right-132,p.bottom-44,p.right-20,p.bottom-14); }

static void clampOptions(PcCompassOptions& o) {
    int inner = o.innerRadiusNorm / 1000;
    int outer = o.outerRadiusNorm / 1000;
    inner = (std::max)(PC_COMPASS_INNER_RADIUS_MIN, (std::min)(PC_COMPASS_INNER_RADIUS_MAX, inner));
    outer = (std::max)(PC_COMPASS_OUTER_RADIUS_MIN, (std::min)(PC_COMPASS_OUTER_RADIUS_MAX, outer));
    if (outer < inner + PC_COMPASS_RADIUS_MIN_GAP) {
        outer = (std::min)(PC_COMPASS_OUTER_RADIUS_MAX, inner + PC_COMPASS_RADIUS_MIN_GAP);
        if (outer < inner + PC_COMPASS_RADIUS_MIN_GAP) {
            inner = (std::max)(PC_COMPASS_INNER_RADIUS_MIN, outer - PC_COMPASS_RADIUS_MIN_GAP);
        }
    }
    o.innerRadiusNorm = inner * 1000;
    o.outerRadiusNorm = outer * 1000;
    o.speedXStep = (std::max)(PC_COMPASS_SPEED_STEP_MIN, (std::min)(PC_COMPASS_SPEED_STEP_MAX, o.speedXStep));
    o.speedYStep = (std::max)(PC_COMPASS_SPEED_STEP_MIN, (std::min)(PC_COMPASS_SPEED_STEP_MAX, o.speedYStep));
    o.swaySectorPercent = (std::max)(PC_COMPASS_SWAY_SECTOR_MIN, (std::min)(PC_COMPASS_SWAY_SECTOR_MAX, o.swaySectorPercent));
    o.swayDiagonalSectorPercent = (std::max)(PC_COMPASS_SWAY_DIAG_SECTOR_MIN, (std::min)(PC_COMPASS_SWAY_DIAG_SECTOR_MAX, o.swayDiagonalSectorPercent));
    o.swayStepMinPercent = (std::max)(PC_COMPASS_SWAY_STEP_MIN_MIN, (std::min)(PC_COMPASS_SWAY_STEP_MIN_MAX, o.swayStepMinPercent));
    o.swayStepMaxPercent = (std::max)(PC_COMPASS_SWAY_STEP_MAX_MIN, (std::min)(PC_COMPASS_SWAY_STEP_MAX_MAX, o.swayStepMaxPercent));
    if (o.swayStepMaxPercent < o.swayStepMinPercent + PC_COMPASS_SWAY_STEP_MIN_GAP) o.swayStepMaxPercent = o.swayStepMinPercent + PC_COMPASS_SWAY_STEP_MIN_GAP;
    o.swaySpeedPercent = (std::max)(PC_COMPASS_SWAY_SPEED_MIN, (std::min)(PC_COMPASS_SWAY_SPEED_MAX, o.swaySpeedPercent));
    o.swayMouseButtonStepPercent = (std::max)(PC_COMPASS_MOUSE_STABLE_SECTOR_MIN, (std::min)(PC_COMPASS_MOUSE_STABLE_SECTOR_MAX, o.swayMouseButtonStepPercent));
    o.swayMouseButtonUpdatePercent = (std::max)(PC_COMPASS_MOUSE_STABLE_SPEED_MIN, (std::min)(PC_COMPASS_MOUSE_STABLE_SPEED_MAX, o.swayMouseButtonUpdatePercent));
    o.swayMouseButtonHoldMs = (std::max)(PC_COMPASS_MOUSE_STABLE_HOLD_MS_MIN, (std::min)(PC_COMPASS_MOUSE_STABLE_HOLD_MS_MAX, o.swayMouseButtonHoldMs));
}

static int valueFromSlider(const RECT& track, int x, int minV, int maxV) {
    const int left = static_cast<int>(track.left);
    const int right = static_cast<int>(track.right);
    const int w = (std::max)(1, right - left);
    const int cx = (std::max)(left, (std::min)(right, x));
    const double t = double(cx - left) / double(w);
    return minV + static_cast<int>(std::lround(t * double(maxV - minV)));
}

static void drawChip(HDC dc, HFONT font, const RECT& r, const wchar_t* text, bool selected) {
    const auto& th = PcUi::DefaultTheme();
    HBRUSH fill = PcUi::CreateBrush(selected ? th.accent : th.panel2);
    PcUi::DrawRoundFill(dc, r, 12, fill);
    DeleteObject(fill);
    PcUi::DrawRoundBorder(dc, r, 12, PcUi::Rgb(selected ? th.accent2 : th.stroke), selected ? 2 : 1);
    PcUi::DrawTextCenter(dc, r, text, font, PcUi::Rgb(selected ? th.bg : th.text));
}

static void drawSlider(HDC dc, HFONT font, HFONT smallFont, const RECT& track, const wchar_t* label, int value, int minV, int maxV, const wchar_t* suffix = L"") {
    const auto& th = PcUi::DefaultTheme();

    // 紧凑双列滑条：标签在左侧，数值在滑条下方，滑条高度更小。
    RECT labelRc{track.left - 84, track.top - 4, track.left - 8, track.bottom + 4};
    PcUi::DrawTextLeft(dc, labelRc, label, font, PcUi::Rgb(th.textMuted));

    RECT outer{track.left - 1, track.top - 1, track.right + 1, track.bottom + 1};
    HBRUSH bg = PcUi::CreateBrush(th.panel2);
    PcUi::DrawRoundFill(dc, outer, 7, bg); DeleteObject(bg);
    PcUi::DrawRoundBorder(dc, outer, 7, PcUi::Rgb(th.stroke), 1);

    const int left = static_cast<int>(track.left);
    const int right = static_cast<int>(track.right);
    const int w = (std::max)(1, right - left);
    const int clampedValue = (std::max)(minV, (std::min)(maxV, value));
    const double ratio = double(clampedValue - minV) / double((std::max)(1, maxV - minV));
    const int knobX = left + static_cast<int>(std::lround(ratio * w));

    RECT active{track.left, track.top, knobX, track.bottom};
    HBRUSH af = PcUi::CreateBrush(th.accent);
    PcUi::DrawRoundFill(dc, active, 6, af); DeleteObject(af);

    RECT knob{knobX - 7, track.top - 5, knobX + 7, track.bottom + 5};
    HBRUSH kf = PcUi::CreateBrush(th.text);
    PcUi::DrawRoundFill(dc, knob, 7, kf); DeleteObject(kf);
    PcUi::DrawRoundBorder(dc, knob, 7, PcUi::Rgb(th.accent2), 1);

    wchar_t valueText[96]{};
    std::swprintf(valueText, 96, L"%d%s", value, suffix);
    RECT vr{track.left, track.bottom + 2, track.right, track.bottom + 18};
    PcUi::DrawTextCenter(dc, vr, valueText, smallFont, PcUi::Rgb(th.textMuted));
}

static RECT sliderHitRect(const RECT& r) {
    RECT h = r;
    h.left -= 6;
    h.right += 6;
    h.top -= 10;
    h.bottom += 20;
    return h;
}

static void paintDialog(CompassDialogState* st, HDC dc) {
    RECT client{}; GetClientRect(st->hwnd, &client);
    const auto& th = PcUi::DefaultTheme();
    HBRUSH bg = PcUi::CreateBrush(th.bg);
    FillRect(dc, &client, bg); DeleteObject(bg);
    RECT p = panelRect(client);
    HBRUSH panel = PcUi::CreateBrush(th.panel);
    PcUi::DrawRoundFill(dc, p, 14, panel); DeleteObject(panel);
    PcUi::DrawRoundBorder(dc, p, 14, PcUi::Rgb(th.stroke), 1);

    // 标题压缩，去掉“运行模式”文字，直接显示模式按钮。
    PcUi::DrawTextLeft(dc, rc(p.left+20,p.top+10,p.right-50,p.top+32), L"轮盘设置", st->titleFont, PcUi::Rgb(th.text));
    PcUi::DrawTextCenter(dc, closeRect(p), L"×", st->titleFont, PcUi::Rgb(th.text));

    drawChip(dc, st->font, fixedRect(p), L"固定", st->options.motionMode == PcCompassMotionMode::FixedCenter);
    drawChip(dc, st->font, swayRect(p), L"晃动", st->options.motionMode == PcCompassMotionMode::MouseSway);
    drawChip(dc, st->font, reverseRect(p), st->options.centerOuterReversed ? L"反转开" : L"反转关", st->options.centerOuterReversed);

    PcUi::DrawTextLeft(dc, rc(p.left+20,p.top+76,p.right-20,p.top+92), L"基础", st->smallFont, PcUi::Rgb(th.textMuted));
    drawSlider(dc, st->font, st->smallFont, innerSliderRect(p), L"内半径", st->options.innerRadiusNorm / 1000, PC_COMPASS_INNER_RADIUS_MIN, PC_COMPASS_INNER_RADIUS_MAX, L"");
    drawSlider(dc, st->font, st->smallFont, outerSliderRect(p), L"外半径", st->options.outerRadiusNorm / 1000, PC_COMPASS_OUTER_RADIUS_MIN, PC_COMPASS_OUTER_RADIUS_MAX, L"");
    drawSlider(dc, st->font, st->smallFont, speedXSliderRect(p), L"X速度", st->options.speedXStep, PC_COMPASS_SPEED_STEP_MIN, PC_COMPASS_SPEED_STEP_MAX, L"");
    drawSlider(dc, st->font, st->smallFont, speedYSliderRect(p), L"Y速度", st->options.speedYStep, PC_COMPASS_SPEED_STEP_MIN, PC_COMPASS_SPEED_STEP_MAX, L"");

    PcUi::DrawTextLeft(dc, rc(p.left+20,p.top+188,p.right-20,p.top+204), L"晃动", st->smallFont, PcUi::Rgb(th.textMuted));
    drawSlider(dc, st->font, st->smallFont, swaySectorSliderRect(p), L"正向扇区", st->options.swaySectorPercent, PC_COMPASS_SWAY_SECTOR_MIN, PC_COMPASS_SWAY_SECTOR_MAX, L"%");
    drawSlider(dc, st->font, st->smallFont, swayDiagSectorSliderRect(p), L"斜向扇区", st->options.swayDiagonalSectorPercent, PC_COMPASS_SWAY_DIAG_SECTOR_MIN, PC_COMPASS_SWAY_DIAG_SECTOR_MAX, L"%");
    drawSlider(dc, st->font, st->smallFont, swayMinSliderRect(p), L"最小扇区", st->options.swayStepMinPercent, PC_COMPASS_SWAY_STEP_MIN_MIN, PC_COMPASS_SWAY_STEP_MIN_MAX, L"%");
    drawSlider(dc, st->font, st->smallFont, swayMaxSliderRect(p), L"最大扇区", st->options.swayStepMaxPercent, PC_COMPASS_SWAY_STEP_MAX_MIN, PC_COMPASS_SWAY_STEP_MAX_MAX, L"%");
    drawSlider(dc, st->font, st->smallFont, swaySpeedSliderRect(p), L"扇区速度", st->options.swaySpeedPercent, PC_COMPASS_SWAY_SPEED_MIN, PC_COMPASS_SWAY_SPEED_MAX, L"%");
    drawChip(dc, st->font, mouseSmallRect(p), st->options.swayMouseButtonSmallStep ? L"鼠标稳定：开" : L"鼠标稳定：关", st->options.swayMouseButtonSmallStep);

    PcUi::DrawTextLeft(dc, rc(p.left+20,p.top+352,p.right-20,p.top+368), L"鼠标稳定", st->smallFont, PcUi::Rgb(th.textMuted));
    drawSlider(dc, st->font, st->smallFont, mouseStepSliderRect(p), L"鼠标最大", st->options.swayMouseButtonStepPercent, PC_COMPASS_MOUSE_STABLE_SECTOR_MIN, PC_COMPASS_MOUSE_STABLE_SECTOR_MAX, L"%");
    drawSlider(dc, st->font, st->smallFont, mouseUpdateSliderRect(p), L"鼠标速度", st->options.swayMouseButtonUpdatePercent, PC_COMPASS_MOUSE_STABLE_SPEED_MIN, PC_COMPASS_MOUSE_STABLE_SPEED_MAX, L"%");
    drawSlider(dc, st->font, st->smallFont, mouseHoldSliderRect(p), L"持续秒", st->options.swayMouseButtonHoldMs / 1000, PC_COMPASS_MOUSE_STABLE_HOLD_SEC_MIN, PC_COMPASS_MOUSE_STABLE_HOLD_SEC_MAX, L"s");

    RECT hint{p.left+20,p.bottom-74,p.right-20,p.bottom-52};
    PcUi::DrawTextLeft(dc, hint, L"鼠标左/右键触发后，数秒内回到当前方向并使用小扇区稳定移动。", st->smallFont, PcUi::Rgb(th.textMuted));

    RECT c = cancelRect(p);
    HBRUSH cf = PcUi::CreateBrush(th.panel2); PcUi::DrawRoundFill(dc, c, 10, cf); DeleteObject(cf);
    PcUi::DrawRoundBorder(dc, c, 10, PcUi::Rgb(th.stroke), 1);
    PcUi::DrawTextCenter(dc, c, L"取消", st->font, PcUi::Rgb(th.text));
    RECT ok = okRect(p);
    HBRUSH of = PcUi::CreateBrush(th.accent); PcUi::DrawRoundFill(dc, ok, 10, of); DeleteObject(of);
    PcUi::DrawRoundBorder(dc, ok, 10, PcUi::Rgb(th.accent2), 1);
    PcUi::DrawTextCenter(dc, ok, L"确定", st->font, PcUi::Rgb(th.bg));
}

static void updateSliderValue(CompassDialogState* st, DragKind kind, int x) {
    if (!st) return;
    RECT client{}; GetClientRect(st->hwnd, &client); RECT p = panelRect(client);
    switch (kind) {
        case DragKind::Inner:
            st->options.innerRadiusNorm = valueFromSlider(innerSliderRect(p), x, PC_COMPASS_INNER_RADIUS_MIN, PC_COMPASS_INNER_RADIUS_MAX) * 1000;
            break;
        case DragKind::Outer:
            st->options.outerRadiusNorm = valueFromSlider(outerSliderRect(p), x, PC_COMPASS_OUTER_RADIUS_MIN, PC_COMPASS_OUTER_RADIUS_MAX) * 1000;
            break;
        case DragKind::SpeedX:
            st->options.speedXStep = valueFromSlider(speedXSliderRect(p), x, PC_COMPASS_SPEED_STEP_MIN, PC_COMPASS_SPEED_STEP_MAX);
            break;
        case DragKind::SpeedY:
            st->options.speedYStep = valueFromSlider(speedYSliderRect(p), x, PC_COMPASS_SPEED_STEP_MIN, PC_COMPASS_SPEED_STEP_MAX);
            break;
        case DragKind::SwaySector:
            st->options.swaySectorPercent = valueFromSlider(swaySectorSliderRect(p), x, PC_COMPASS_SWAY_SECTOR_MIN, PC_COMPASS_SWAY_SECTOR_MAX);
            break;
        case DragKind::SwayDiagSector:
            st->options.swayDiagonalSectorPercent = valueFromSlider(swayDiagSectorSliderRect(p), x, PC_COMPASS_SWAY_DIAG_SECTOR_MIN, PC_COMPASS_SWAY_DIAG_SECTOR_MAX);
            break;
        case DragKind::SwayMin:
            st->options.swayStepMinPercent = valueFromSlider(swayMinSliderRect(p), x, PC_COMPASS_SWAY_STEP_MIN_MIN, PC_COMPASS_SWAY_STEP_MIN_MAX);
            break;
        case DragKind::SwayMax:
            st->options.swayStepMaxPercent = valueFromSlider(swayMaxSliderRect(p), x, PC_COMPASS_SWAY_STEP_MAX_MIN, PC_COMPASS_SWAY_STEP_MAX_MAX);
            break;
        case DragKind::SwaySpeed:
            st->options.swaySpeedPercent = valueFromSlider(swaySpeedSliderRect(p), x, PC_COMPASS_SWAY_SPEED_MIN, PC_COMPASS_SWAY_SPEED_MAX);
            break;
        case DragKind::MouseStep:
            st->options.swayMouseButtonStepPercent = valueFromSlider(mouseStepSliderRect(p), x, PC_COMPASS_MOUSE_STABLE_SECTOR_MIN, PC_COMPASS_MOUSE_STABLE_SECTOR_MAX);
            break;
        case DragKind::MouseUpdate:
            st->options.swayMouseButtonUpdatePercent = valueFromSlider(mouseUpdateSliderRect(p), x, PC_COMPASS_MOUSE_STABLE_SPEED_MIN, PC_COMPASS_MOUSE_STABLE_SPEED_MAX);
            break;
        case DragKind::MouseHold:
            st->options.swayMouseButtonHoldMs = valueFromSlider(mouseHoldSliderRect(p), x, PC_COMPASS_MOUSE_STABLE_HOLD_SEC_MIN, PC_COMPASS_MOUSE_STABLE_HOLD_SEC_MAX) * 1000;
            break;
        default:
            break;
    }
    clampOptions(st->options);
    InvalidateRect(st->hwnd, nullptr, FALSE);
}

static void finish(CompassDialogState* st, bool ok) {
    if (!st || st->done) return;
    st->done = true;
    st->ok = ok;
    if (st->hwnd) DestroyWindow(st->hwnd);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = reinterpret_cast<CompassDialogState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        st = reinterpret_cast<CompassDialogState*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    if (!st) return DefWindowProcW(hwnd, msg, wp, lp);

    switch (msg) {
        case WM_CREATE:
            st->titleFont = PcUi::CreateUiFont(12, true);
            st->font = PcUi::CreateUiFont(9, true);
            st->smallFont = PcUi::CreateUiFont(8, false);
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps{}; HDC dc = BeginPaint(hwnd, &ps); paintDialog(st, dc); EndPaint(hwnd, &ps); return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_LBUTTONDOWN: {
            RECT client{}; GetClientRect(hwnd, &client); RECT p = panelRect(client);
            const int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
            if (ptIn(sliderHitRect(innerSliderRect(p)), x, y)) { st->drag = DragKind::Inner; SetCapture(hwnd); updateSliderValue(st, st->drag, x); return 0; }
            if (ptIn(sliderHitRect(outerSliderRect(p)), x, y)) { st->drag = DragKind::Outer; SetCapture(hwnd); updateSliderValue(st, st->drag, x); return 0; }
            if (ptIn(sliderHitRect(speedXSliderRect(p)), x, y)) { st->drag = DragKind::SpeedX; SetCapture(hwnd); updateSliderValue(st, st->drag, x); return 0; }
            if (ptIn(sliderHitRect(speedYSliderRect(p)), x, y)) { st->drag = DragKind::SpeedY; SetCapture(hwnd); updateSliderValue(st, st->drag, x); return 0; }
            if (ptIn(sliderHitRect(swaySectorSliderRect(p)), x, y)) { st->drag = DragKind::SwaySector; SetCapture(hwnd); updateSliderValue(st, st->drag, x); return 0; }
            if (ptIn(sliderHitRect(swayDiagSectorSliderRect(p)), x, y)) { st->drag = DragKind::SwayDiagSector; SetCapture(hwnd); updateSliderValue(st, st->drag, x); return 0; }
            if (ptIn(sliderHitRect(swayMinSliderRect(p)), x, y)) { st->drag = DragKind::SwayMin; SetCapture(hwnd); updateSliderValue(st, st->drag, x); return 0; }
            if (ptIn(sliderHitRect(swayMaxSliderRect(p)), x, y)) { st->drag = DragKind::SwayMax; SetCapture(hwnd); updateSliderValue(st, st->drag, x); return 0; }
            if (ptIn(sliderHitRect(swaySpeedSliderRect(p)), x, y)) { st->drag = DragKind::SwaySpeed; SetCapture(hwnd); updateSliderValue(st, st->drag, x); return 0; }
            if (ptIn(sliderHitRect(mouseStepSliderRect(p)), x, y)) { st->drag = DragKind::MouseStep; SetCapture(hwnd); updateSliderValue(st, st->drag, x); return 0; }
            if (ptIn(sliderHitRect(mouseUpdateSliderRect(p)), x, y)) { st->drag = DragKind::MouseUpdate; SetCapture(hwnd); updateSliderValue(st, st->drag, x); return 0; }
            if (ptIn(sliderHitRect(mouseHoldSliderRect(p)), x, y)) { st->drag = DragKind::MouseHold; SetCapture(hwnd); updateSliderValue(st, st->drag, x); return 0; }
            return 0;
        }
        case WM_MOUSEMOVE:
            if (st->drag != DragKind::None && GetCapture() == hwnd) {
                updateSliderValue(st, st->drag, GET_X_LPARAM(lp));
                return 0;
            }
            break;
        case WM_LBUTTONUP: {
            if (st->drag != DragKind::None) {
                updateSliderValue(st, st->drag, GET_X_LPARAM(lp));
                st->drag = DragKind::None;
                if (GetCapture() == hwnd) ReleaseCapture();
                return 0;
            }
            RECT client{}; GetClientRect(hwnd, &client); RECT p = panelRect(client);
            const int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
            if (ptIn(closeRect(p),x,y) || ptIn(cancelRect(p),x,y)) { finish(st,false); return 0; }
            if (ptIn(okRect(p),x,y)) { clampOptions(st->options); finish(st,true); return 0; }
            if (ptIn(fixedRect(p),x,y)) { st->options.motionMode = PcCompassMotionMode::FixedCenter; InvalidateRect(hwnd,nullptr,FALSE); return 0; }
            if (ptIn(swayRect(p),x,y)) { st->options.motionMode = PcCompassMotionMode::MouseSway; InvalidateRect(hwnd,nullptr,FALSE); return 0; }
            if (ptIn(reverseRect(p),x,y)) { st->options.centerOuterReversed = !st->options.centerOuterReversed; InvalidateRect(hwnd,nullptr,FALSE); return 0; }
            if (ptIn(mouseSmallRect(p),x,y)) { st->options.swayMouseButtonSmallStep = !st->options.swayMouseButtonSmallStep; InvalidateRect(hwnd,nullptr,FALSE); return 0; }
            return 0;
        }
        case WM_CANCELMODE:
        case WM_KILLFOCUS:
            if (st->drag != DragKind::None) { st->drag = DragKind::None; if (GetCapture() == hwnd) ReleaseCapture(); }
            break;
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) { finish(st,false); return 0; }
            if (wp == VK_RETURN) { clampOptions(st->options); finish(st,true); return 0; }
            break;
        case WM_CLOSE: finish(st,false); return 0;
        case WM_DESTROY: {
            PcCompassOptions* result = nullptr;
            if (st->ok) result = new PcCompassOptions(st->options);
            if (st->owner) PostMessageW(st->owner, WM_PC_COMPASS_OPTIONS_DONE, st->cookie, reinterpret_cast<LPARAM>(result));
            if (st->titleFont) DeleteObject(st->titleFont);
            if (st->font) DeleteObject(st->font);
            if (st->smallFont) DeleteObject(st->smallFont);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            delete st;
            return 0;
        }
    }
    return DefWindowProcW(hwnd,msg,wp,lp);
}

static bool registerClass(HINSTANCE inst) {
    WNDCLASSW wc{}; wc.lpfnWndProc = WndProc; wc.hInstance = inst; wc.lpszClassName = kClassName; wc.hCursor = LoadCursor(nullptr, IDC_ARROW); wc.hbrBackground = nullptr; RegisterClassW(&wc); return true;
}
} // namespace

bool OpenCompassOptionsDialog(HINSTANCE inst, HWND ownerHwnd, const PcCompassOptions& options, WPARAM cookie) {
    registerClass(inst);
    RECT owner{}; GetWindowRect(ownerHwnd, &owner);
    const int w = scalePx(ownerHwnd, 600);
    const int h = scalePx(ownerHwnd, 620);
    const int x = owner.left + ((owner.right-owner.left)-w)/2;
    const int y = owner.top + ((owner.bottom-owner.top)-h)/2;
    auto* st = new CompassDialogState();
    st->inst = inst; st->owner = ownerHwnd; st->options = options; st->cookie = cookie;
    clampOptions(st->options);
    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kClassName, L"轮盘设置", WS_POPUP | WS_CAPTION | WS_SYSMENU, x,y,w,h, ownerHwnd, nullptr, inst, st);
    if (!hwnd) { delete st; return false; }
    st->hwnd = hwnd;
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
    return true;
}
