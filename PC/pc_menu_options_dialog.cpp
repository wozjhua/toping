#include "pc_menu_options_dialog.h"
#include "pc_ui_theme.h"

#include <algorithm>
#include <cwchar>
#include <windowsx.h>

namespace {
constexpr wchar_t kClassName[] = L"HuiLangPcMenuOptionsDialog";

struct MenuDialogState {
    HINSTANCE inst{};
    HWND owner{};
    HWND hwnd{};
    WPARAM cookie{};
    PcMenuOptions options{};
    HFONT titleFont{};
    HFONT font{};
    HFONT smallFont{};
    bool done = false;
    bool ok = false;
};

static int scalePx(HWND hwnd, int px) {
    HDC dc = GetDC(hwnd);
    const int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSX) : 96;
    if (dc) ReleaseDC(hwnd, dc);
    return MulDiv(px, dpi > 0 ? dpi : 96, 96);
}

static int clampi(int v, int lo, int hi) { return (std::max)(lo, (std::min)(hi, v)); }
static double clampd(double v, double lo, double hi) { return (std::max)(lo, (std::min)(hi, v)); }
static RECT rc(int l, int t, int r, int b) { return RECT{l,t,r,b}; }
static bool ptIn(const RECT& r, int x, int y) { return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom; }

static bool isFreeCursorOptions(PcMenuCategory cat) {
    return cat == PcMenuCategory::MenuItemOperation ||
           cat == PcMenuCategory::MenuBagOperation ||
           cat == PcMenuCategory::MenuHorizontal ||
           cat == PcMenuCategory::MenuVertical;
}

static RECT panelRect(const RECT& c, const PcMenuOptions& options) {
    const int w = c.right - c.left;
    const int h = c.bottom - c.top;
    const int pw = isFreeCursorOptions(options.category) ? 500 : 460;
    const int ph = isFreeCursorOptions(options.category) ? 438 : 390;
    return rc((w-pw)/2, (h-ph)/2, (w+pw)/2, (h+ph)/2);
}

static RECT closeRect(const RECT& p) { return rc(p.right-36,p.top+10,p.right-10,p.top+36); }
static RECT cancelRect(const RECT& p) { return rc(p.left+24,p.bottom-48,p.left+136,p.bottom-14); }
static RECT okRect(const RECT& p) { return rc(p.right-136,p.bottom-48,p.right-24,p.bottom-14); }

static RECT minusRect(const RECT& p, int rowTop) { return rc(p.left+24,rowTop,p.left+64,rowTop+32); }
static RECT valueRect(const RECT& p, int rowTop) { return rc(p.left+72,rowTop,p.right-72,rowTop+32); }
static RECT plusRect(const RECT& p, int rowTop) { return rc(p.right-64,rowTop,p.right-24,rowTop+32); }
static RECT twoChipRect(const RECT& p, int rowTop, int index) {
    const int left = p.left + 24;
    const int right = p.right - 24;
    const int gap = 10;
    const int chipW = ((right - left) - gap) / 2;
    const int x = left + index * (chipW + gap);
    return rc(x, rowTop, x + chipW, rowTop + 32);
}

static int radiusRow(const RECT& p) { return p.top + 80; }
static int speedXRow(const RECT& p) { return p.top + 154; }
static int speedYRow(const RECT& p) { return p.top + 212; }
static int wheelToggleRow(const RECT& p) { return p.top + 154; }
static int wheelSpeedRow(const RECT& p) { return p.top + 212; }
static int wheelDistanceRow(const RECT& p) { return p.top + 270; }
static int wheelStepRow(const RECT& p) { return p.top + 328; }

static const wchar_t* categoryTitle(PcMenuCategory cat) {
    switch (cat) {
        case PcMenuCategory::MenuRadial: return L"轮盘菜单设置";
        case PcMenuCategory::MenuBagOperation: return L"背包操作设置";
        case PcMenuCategory::MenuItemOperation: return L"道具操作设置";
        case PcMenuCategory::MenuHorizontal:
        case PcMenuCategory::MenuVertical: return L"道具操作设置";
        default: return L"菜单设置";
    }
}

static int clampMenuRadiusNorm(int v) {
    return clampi(v, PC_MENU_BUTTON_RADIUS_MIN_NORM, PC_MENU_BUTTON_RADIUS_MAX_NORM);
}

static int radiusToDisplay(int radiusNorm) {
    return clampMenuRadiusNorm(radiusNorm) / PC_MENU_BUTTON_RADIUS_UNIT;
}

static void clampOptions(PcMenuOptions& o) {
    o.relativeSpeedX = clampd(o.relativeSpeedX, 0.4, 1.2);
    o.relativeSpeedY = clampd(o.relativeSpeedY, 0.4, 1.2);
    o.radiusNorm = clampMenuRadiusNorm(o.radiusNorm);
    o.itemWheelScrollSpeed = clampi(o.itemWheelScrollSpeed, PC_MENU_ITEM_WHEEL_SPEED_MIN, PC_MENU_ITEM_WHEEL_SPEED_MAX);
    o.itemWheelMaxDistancePx = clampi(o.itemWheelMaxDistancePx, PC_MENU_ITEM_WHEEL_DISTANCE_MIN, PC_MENU_ITEM_WHEEL_DISTANCE_MAX);
    o.itemWheelStepPx = clampi(o.itemWheelStepPx, PC_MENU_ITEM_WHEEL_STEP_MIN, PC_MENU_ITEM_WHEEL_STEP_MAX);
}

static void drawChip(HDC dc, HFONT font, const RECT& r, const wchar_t* text, bool selected) {
    const auto& th = PcUi::DefaultTheme();
    HBRUSH fill = PcUi::CreateBrush(selected ? th.accent : th.panel2);
    PcUi::DrawRoundFill(dc, r, 12, fill);
    DeleteObject(fill);
    PcUi::DrawRoundBorder(dc, r, 12, PcUi::Rgb(selected ? th.accent2 : th.stroke), selected ? 2 : 1);
    PcUi::DrawTextCenter(dc, r, text, font, PcUi::Rgb(selected ? th.bg : th.text));
}

static void drawNumericRow(HDC dc, HFONT font, const RECT& p, int rowTop, const wchar_t* valueText) {
    drawChip(dc, font, minusRect(p, rowTop), L"-", false);
    drawChip(dc, font, plusRect(p, rowTop), L"+", false);
    drawChip(dc, font, valueRect(p, rowTop), valueText, true);
}

static void paintDialog(MenuDialogState* st, HDC dc) {
    RECT client{}; GetClientRect(st->hwnd, &client);
    const bool freeMode = isFreeCursorOptions(st->options.category);
    const auto& th = PcUi::DefaultTheme();
    HBRUSH bg = PcUi::CreateBrush(th.bg);
    FillRect(dc, &client, bg); DeleteObject(bg);
    RECT p = panelRect(client, st->options);
    HBRUSH panel = PcUi::CreateBrush(th.panel);
    PcUi::DrawRoundFill(dc, p, 18, panel); DeleteObject(panel);
    PcUi::DrawRoundBorder(dc, p, 18, PcUi::Rgb(th.stroke), 1);

    PcUi::DrawTextLeft(dc, rc(p.left+22,p.top+14,p.right-60,p.top+44), categoryTitle(st->options.category), st->titleFont, PcUi::Rgb(th.text));
    PcUi::DrawTextCenter(dc, closeRect(p), L"×", st->titleFont, PcUi::Rgb(th.text));

    PcUi::DrawTextLeft(dc, rc(p.left+24,p.top+56,p.right-24,p.top+78), L"圆形按钮半径 / 随机点击范围", st->font, PcUi::Rgb(th.textMuted));
    wchar_t radiusText[96]{}; std::swprintf(radiusText, 96, L"半径 %d", radiusToDisplay(st->options.radiusNorm));
    drawNumericRow(dc, st->font, p, radiusRow(p), radiusText);

    if (freeMode) {
        PcUi::DrawTextLeft(dc, rc(p.left+24,p.top+130,p.right-24,p.top+152), L"滚轮滑动", st->font, PcUi::Rgb(th.textMuted));
        drawChip(dc, st->font, twoChipRect(p, wheelToggleRow(p), 0), st->options.itemWheelScrollEnabled ? L"滚轮：开" : L"滚轮：关", st->options.itemWheelScrollEnabled);
        drawChip(dc, st->font, twoChipRect(p, wheelToggleRow(p), 1), st->options.itemWheelInvert ? L"方向：反向" : L"方向：正常", st->options.itemWheelInvert);

        PcUi::DrawTextLeft(dc, rc(p.left+24,p.top+188,p.right-24,p.top+210), L"滚轮滑动速度", st->font, PcUi::Rgb(th.textMuted));
        wchar_t ws[96]{}; std::swprintf(ws, 96, L"速度 %d", st->options.itemWheelScrollSpeed);
        drawNumericRow(dc, st->font, p, wheelSpeedRow(p), ws);

        PcUi::DrawTextLeft(dc, rc(p.left+24,p.top+246,p.right-24,p.top+268), L"滚轮最大滑动距离", st->font, PcUi::Rgb(th.textMuted));
        wchar_t wd[96]{}; std::swprintf(wd, 96, L"距离 %d px", st->options.itemWheelMaxDistancePx);
        drawNumericRow(dc, st->font, p, wheelDistanceRow(p), wd);

        PcUi::DrawTextLeft(dc, rc(p.left+24,p.top+304,p.right-24,p.top+326), L"滚轮每格步长", st->font, PcUi::Rgb(th.textMuted));
        wchar_t wp[96]{}; std::swprintf(wp, 96, L"步长 %d px", st->options.itemWheelStepPx);
        drawNumericRow(dc, st->font, p, wheelStepRow(p), wp);

    } else {
        PcUi::DrawTextLeft(dc, rc(p.left+24,p.top+130,p.right-24,p.top+152), L"鼠标 X 相对移动倍率", st->font, PcUi::Rgb(th.textMuted));
        wchar_t sx[96]{}; std::swprintf(sx, 96, L"X %.1f", st->options.relativeSpeedX);
        drawNumericRow(dc, st->font, p, speedXRow(p), sx);

        PcUi::DrawTextLeft(dc, rc(p.left+24,p.top+188,p.right-24,p.top+210), L"鼠标 Y 相对移动倍率", st->font, PcUi::Rgb(th.textMuted));
        wchar_t sy[96]{}; std::swprintf(sy, 96, L"Y %.1f", st->options.relativeSpeedY);
        drawNumericRow(dc, st->font, p, speedYRow(p), sy);

        PcUi::DrawTextLeft(dc, rc(p.left+24,p.top+250,p.right-24,p.top+276), L"半径范围：20~60，默认 30。", st->smallFont, PcUi::Rgb(th.textMuted));
    }

    RECT c = cancelRect(p);
    HBRUSH cf = PcUi::CreateBrush(th.panel2); PcUi::DrawRoundFill(dc, c, 12, cf); DeleteObject(cf);
    PcUi::DrawRoundBorder(dc, c, 12, PcUi::Rgb(th.stroke), 1);
    PcUi::DrawTextCenter(dc, c, L"取消", st->font, PcUi::Rgb(th.text));

    RECT ok = okRect(p);
    HBRUSH of = PcUi::CreateBrush(th.accent); PcUi::DrawRoundFill(dc, ok, 12, of); DeleteObject(of);
    PcUi::DrawRoundBorder(dc, ok, 12, PcUi::Rgb(th.accent2), 1);
    PcUi::DrawTextCenter(dc, ok, L"确定", st->font, PcUi::Rgb(th.bg));
}

static void finish(MenuDialogState* st, bool ok) {
    if (!st || st->done) return;
    st->done = true;
    st->ok = ok;
    if (st->hwnd) DestroyWindow(st->hwnd);
}

static void adjustValue(MenuDialogState* st, int row, int deltaSign) {
    if (!st) return;
    switch (row) {
        case 0: st->options.radiusNorm += PC_MENU_BUTTON_RADIUS_UNIT * deltaSign; break;
        case 1: st->options.relativeSpeedX += 0.1 * deltaSign; break;
        case 2: st->options.relativeSpeedY += 0.1 * deltaSign; break;
        case 3: st->options.itemWheelScrollSpeed += deltaSign; break;
        case 4: st->options.itemWheelMaxDistancePx += 20 * deltaSign; break;
        case 5: st->options.itemWheelStepPx += 5 * deltaSign; break;
        default: break;
    }
    clampOptions(st->options);
    InvalidateRect(st->hwnd, nullptr, FALSE);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = reinterpret_cast<MenuDialogState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        st = reinterpret_cast<MenuDialogState*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    if (!st) return DefWindowProcW(hwnd, msg, wp, lp);
    switch (msg) {
        case WM_CREATE:
            st->titleFont = PcUi::CreateUiFont(15, true);
            st->font = PcUi::CreateUiFont(10, true);
            st->smallFont = PcUi::CreateUiFont(9, false);
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps{}; HDC dc = BeginPaint(hwnd,&ps); paintDialog(st,dc); EndPaint(hwnd,&ps); return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_LBUTTONUP: {
            RECT client{}; GetClientRect(hwnd,&client); RECT p = panelRect(client, st->options);
            const int x = GET_X_LPARAM(lp); const int y = GET_Y_LPARAM(lp);
            if (ptIn(closeRect(p),x,y) || ptIn(cancelRect(p),x,y)) { finish(st,false); return 0; }
            if (ptIn(okRect(p),x,y)) { clampOptions(st->options); finish(st,true); return 0; }
            const bool freeMode = isFreeCursorOptions(st->options.category);
            if (ptIn(minusRect(p, radiusRow(p)),x,y)) { adjustValue(st, 0, -1); return 0; }
            if (ptIn(plusRect(p, radiusRow(p)),x,y)) { adjustValue(st, 0, 1); return 0; }
            if (freeMode) {
                if (ptIn(twoChipRect(p, wheelToggleRow(p), 0), x, y)) { st->options.itemWheelScrollEnabled = !st->options.itemWheelScrollEnabled; InvalidateRect(hwnd, nullptr, FALSE); return 0; }
                if (ptIn(twoChipRect(p, wheelToggleRow(p), 1), x, y)) { st->options.itemWheelInvert = !st->options.itemWheelInvert; InvalidateRect(hwnd, nullptr, FALSE); return 0; }
                if (ptIn(minusRect(p, wheelSpeedRow(p)),x,y)) { adjustValue(st, 3, -1); return 0; }
                if (ptIn(plusRect(p, wheelSpeedRow(p)),x,y)) { adjustValue(st, 3, 1); return 0; }
                if (ptIn(minusRect(p, wheelDistanceRow(p)),x,y)) { adjustValue(st, 4, -1); return 0; }
                if (ptIn(plusRect(p, wheelDistanceRow(p)),x,y)) { adjustValue(st, 4, 1); return 0; }
                if (ptIn(minusRect(p, wheelStepRow(p)),x,y)) { adjustValue(st, 5, -1); return 0; }
                if (ptIn(plusRect(p, wheelStepRow(p)),x,y)) { adjustValue(st, 5, 1); return 0; }
            } else {
                if (ptIn(minusRect(p, speedXRow(p)),x,y)) { adjustValue(st, 1, -1); return 0; }
                if (ptIn(plusRect(p, speedXRow(p)),x,y)) { adjustValue(st, 1, 1); return 0; }
                if (ptIn(minusRect(p, speedYRow(p)),x,y)) { adjustValue(st, 2, -1); return 0; }
                if (ptIn(plusRect(p, speedYRow(p)),x,y)) { adjustValue(st, 2, 1); return 0; }
            }
            return 0;
        }
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) { finish(st,false); return 0; }
            if (wp == VK_RETURN) { clampOptions(st->options); finish(st,true); return 0; }
            break;
        case WM_CLOSE: finish(st,false); return 0;
        case WM_DESTROY: {
            PcMenuOptions* result = nullptr;
            if (st->ok) result = new PcMenuOptions(st->options);
            if (st->owner) PostMessageW(st->owner, WM_PC_MENU_OPTIONS_DONE, st->cookie, reinterpret_cast<LPARAM>(result));
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
    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = kClassName;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    RegisterClassW(&wc);
    return true;
}
} // namespace

bool OpenMenuOptionsDialog(HINSTANCE inst, HWND ownerHwnd, const PcMenuOptions& options, WPARAM cookie) {
    registerClass(inst);
    RECT owner{}; GetWindowRect(ownerHwnd, &owner);
    const bool freeMode = isFreeCursorOptions(options.category);
    const int w = scalePx(ownerHwnd, freeMode ? 560 : 520);
    const int h = scalePx(ownerHwnd, freeMode ? 500 : 450);
    const int x = owner.left + ((owner.right-owner.left)-w)/2;
    const int y = owner.top + ((owner.bottom-owner.top)-h)/2;
    auto* st = new MenuDialogState();
    st->inst = inst; st->owner = ownerHwnd; st->options = options; st->cookie = cookie;
    clampOptions(st->options);
    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kClassName, L"菜单设置", WS_POPUP | WS_CAPTION | WS_SYSMENU, x,y,w,h, ownerHwnd, nullptr, inst, st);
    if (!hwnd) { delete st; return false; }
    st->hwnd = hwnd;
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
    return true;
}
