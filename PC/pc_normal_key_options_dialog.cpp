#include "pc_normal_key_options_dialog.h"
#include "pc_ui_theme.h"

#include <algorithm>
#include <cwchar>
#include <windowsx.h>

namespace {
constexpr wchar_t kClassName[] = L"HuiLangPcNormalKeyOptionsDialog";

struct OptionDialogState {
    HINSTANCE inst{};
    HWND owner{};
    HWND hwnd{};
    PcNormalKeyOptions options{};
    bool done = false;
    bool ok = false;
    bool modeless = false;
    WPARAM cookie = 0;
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

static RECT rc(int l, int t, int r, int b) { return RECT{l, t, r, b}; }
static bool ptIn(const RECT& r, int x, int y) { return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom; }

static RECT panelRect(const RECT& client) {
    const int w = client.right - client.left;
    const int h = client.bottom - client.top;
    const int pw = 440;
    const int ph = 352;
    return rc((w - pw) / 2, (h - ph) / 2, (w + pw) / 2, (h + ph) / 2);
}
static RECT closeRect(const RECT& p) { return rc(p.right - 36, p.top + 10, p.right - 10, p.top + 36); }
static RECT randomMoveRect(const RECT& p) { return rc(p.left + 24, p.top + 74, p.left + 206, p.top + 108); }
static RECT noRandomRect(const RECT& p) { return rc(p.left + 218, p.top + 74, p.right - 24, p.top + 108); }
static RECT radiusMinusRect(const RECT& p) { return rc(p.left + 24, p.top + 138, p.left + 64, p.top + 170); }
static RECT radiusValueRect(const RECT& p) { return rc(p.left + 72, p.top + 138, p.right - 72, p.top + 170); }
static RECT radiusPlusRect(const RECT& p) { return rc(p.right - 64, p.top + 138, p.right - 24, p.top + 170); }
static RECT actionNoneRect(const RECT& p) { return rc(p.left + 24, p.top + 212, p.left + 120, p.top + 246); }
static RECT actionRideRect(const RECT& p) { return rc(p.left + 132, p.top + 212, p.left + 248, p.top + 246); }
static RECT actionDismountRect(const RECT& p) { return rc(p.left + 260, p.top + 212, p.right - 24, p.top + 246); }
static RECT cancelRect(const RECT& p) { return rc(p.left + 24, p.bottom - 48, p.left + 136, p.bottom - 14); }
static RECT okRect(const RECT& p) { return rc(p.right - 136, p.bottom - 48, p.right - 24, p.bottom - 14); }

static int radiusToDisplayPx(int radiusNorm) {
    // 显示一个稳定、可理解的近似值；实际运行使用归一化半径，不依赖当前投屏尺寸。
    return std::max(PC_BUTTON_RADIUS_MIN, std::min(PC_BUTTON_RADIUS_MAX, radiusNorm / PC_BUTTON_RADIUS_UNIT));
}

static void drawChip(HDC dc, HFONT font, const RECT& r, const wchar_t* text, bool selected) {
    const auto& th = PcUi::DefaultTheme();
    HBRUSH fill = PcUi::CreateBrush(selected ? th.accent : th.panel2);
    PcUi::DrawRoundFill(dc, r, 12, fill);
    DeleteObject(fill);
    PcUi::DrawRoundBorder(dc, r, 12, PcUi::Rgb(selected ? th.accent2 : th.stroke), selected ? 2 : 1);
    PcUi::DrawTextCenter(dc, r, text, font, PcUi::Rgb(selected ? th.bg : th.text));
}

static void paintDialog(OptionDialogState* st, HDC dc) {
    if (!st) return;
    RECT client{};
    GetClientRect(st->hwnd, &client);
    const auto& th = PcUi::DefaultTheme();

    HBRUSH bg = PcUi::CreateBrush(th.bg);
    FillRect(dc, &client, bg);
    DeleteObject(bg);

    RECT p = panelRect(client);
    HBRUSH panel = PcUi::CreateBrush(th.panel);
    PcUi::DrawRoundFill(dc, p, 18, panel);
    DeleteObject(panel);
    PcUi::DrawRoundBorder(dc, p, 18, PcUi::Rgb(th.stroke), 1);

    RECT title{p.left + 22, p.top + 14, p.right - 60, p.top + 44};
    PcUi::DrawTextLeft(dc, title, L"普通按键选项", st->titleFont, PcUi::Rgb(th.text));
    PcUi::DrawTextCenter(dc, closeRect(p), L"×", st->titleFont, PcUi::Rgb(th.text));

    RECT sec1{p.left + 24, p.top + 48, p.right - 24, p.top + 70};
    PcUi::DrawTextLeft(dc, sec1, L"按住效果", st->font, PcUi::Rgb(th.textMuted));
    drawChip(dc, st->font, randomMoveRect(p), L"随机移动", st->options.touchMode == PcKeyTouchMode::RandomMove);
    drawChip(dc, st->font, noRandomRect(p), L"不随机移动", st->options.touchMode == PcKeyTouchMode::RandomDownUp);

    RECT hint1{p.left + 24, p.top + 114, p.right - 24, p.top + 134};
    const wchar_t* cur = st->options.touchMode == PcKeyTouchMode::RandomMove
        ? L"随机移动：按下点和移动点都限制在按钮圆形范围内"
        : L"不随机移动：固定中心按下，松开抬起，范围只用于编辑/命中";
    PcUi::DrawTextLeft(dc, hint1, cur, st->smallFont, PcUi::Rgb(th.textMuted));

    RECT secRadius{p.left + 24, p.top + 120, p.right - 24, p.top + 138};
    PcUi::DrawTextLeft(dc, secRadius, L"按钮半径 / 触发范围", st->font, PcUi::Rgb(th.textMuted));
    drawChip(dc, st->font, radiusMinusRect(p), L"-", false);
    drawChip(dc, st->font, radiusPlusRect(p), L"+", false);
    wchar_t radiusText[128]{};
    std::swprintf(radiusText, 128, L"半径 %d", radiusToDisplayPx(st->options.radiusNorm));
    drawChip(dc, st->font, radiusValueRect(p), radiusText, true);

    RECT hintRadius{p.left + 24, p.top + 176, p.right - 24, p.top + 198};
    PcUi::DrawTextLeft(dc, hintRadius, L"范围：20~60，默认 30；随机按下/随机移动不会超过该圆形范围。", st->smallFont, PcUi::Rgb(th.textMuted));

    RECT sec2{p.left + 24, p.top + 188, p.right - 24, p.top + 210};
    PcUi::DrawTextLeft(dc, sec2, L"特殊动作", st->font, PcUi::Rgb(th.textMuted));
    drawChip(dc, st->font, actionNoneRect(p), L"无", st->options.specialAction == PcKeySpecialAction::None);
    drawChip(dc, st->font, actionRideRect(p), L"上车", st->options.specialAction == PcKeySpecialAction::ReclickLockOnKeyDown);
    drawChip(dc, st->font, actionDismountRect(p), L"下车", st->options.specialAction == PcKeySpecialAction::DismountReclickLockOnKeyDown);

    RECT hint2{p.left + 24, p.top + 254, p.right - 24, p.top + 280};
    PcUi::DrawTextLeft(dc, hint2, L"上车/下车：锁定模式下触发视角触点安全重按。", st->smallFont, PcUi::Rgb(th.textMuted));

    RECT c = cancelRect(p);
    HBRUSH cf = PcUi::CreateBrush(th.panel2);
    PcUi::DrawRoundFill(dc, c, 12, cf);
    DeleteObject(cf);
    PcUi::DrawRoundBorder(dc, c, 12, PcUi::Rgb(th.stroke), 1);
    PcUi::DrawTextCenter(dc, c, L"取消", st->font, PcUi::Rgb(th.text));

    RECT ok = okRect(p);
    HBRUSH of = PcUi::CreateBrush(th.accent);
    PcUi::DrawRoundFill(dc, ok, 12, of);
    DeleteObject(of);
    PcUi::DrawRoundBorder(dc, ok, 12, PcUi::Rgb(th.accent2), 1);
    PcUi::DrawTextCenter(dc, ok, L"确定", st->font, PcUi::Rgb(th.bg));
}

static void finish(OptionDialogState* st, bool ok) {
    if (!st || st->done) return;
    st->done = true;
    st->ok = ok;
    if (st->hwnd) DestroyWindow(st->hwnd);
}

static void notifyModelessResult(OptionDialogState* st) {
    if (!st || !st->modeless || !st->owner) return;
    PcNormalKeyOptions* result = nullptr;
    if (st->ok) result = new PcNormalKeyOptions(st->options);
    PostMessageW(st->owner, WM_PC_NORMAL_KEY_OPTIONS_DONE, st->cookie, reinterpret_cast<LPARAM>(result));
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = reinterpret_cast<OptionDialogState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        st = reinterpret_cast<OptionDialogState*>(cs->lpCreateParams);
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
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(hwnd, &ps);
            paintDialog(st, dc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_LBUTTONUP: {
            RECT client{}; GetClientRect(hwnd, &client);
            RECT p = panelRect(client);
            const int x = GET_X_LPARAM(lp);
            const int y = GET_Y_LPARAM(lp);
            if (ptIn(closeRect(p), x, y) || ptIn(cancelRect(p), x, y)) { finish(st, false); return 0; }
            if (ptIn(okRect(p), x, y)) { finish(st, true); return 0; }
            if (ptIn(randomMoveRect(p), x, y)) { st->options.touchMode = PcKeyTouchMode::RandomMove; InvalidateRect(hwnd, nullptr, FALSE); return 0; }
            if (ptIn(noRandomRect(p), x, y)) { st->options.touchMode = PcKeyTouchMode::RandomDownUp; InvalidateRect(hwnd, nullptr, FALSE); return 0; }
            if (ptIn(radiusMinusRect(p), x, y)) {
                st->options.radiusNorm = PcMappingProfile::ClampButtonRadiusNorm(st->options.radiusNorm - PC_BUTTON_RADIUS_UNIT);
                st->options.randomRadiusNorm = (std::min)(st->options.randomRadiusNorm, st->options.radiusNorm);
                st->options.randomMoveRadiusNorm = (std::min)(st->options.randomMoveRadiusNorm, st->options.radiusNorm);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (ptIn(radiusPlusRect(p), x, y)) {
                st->options.radiusNorm = PcMappingProfile::ClampButtonRadiusNorm(st->options.radiusNorm + PC_BUTTON_RADIUS_UNIT);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (ptIn(actionNoneRect(p), x, y)) { st->options.specialAction = PcKeySpecialAction::None; InvalidateRect(hwnd, nullptr, FALSE); return 0; }
            if (ptIn(actionRideRect(p), x, y)) { st->options.specialAction = PcKeySpecialAction::ReclickLockOnKeyDown; InvalidateRect(hwnd, nullptr, FALSE); return 0; }
            if (ptIn(actionDismountRect(p), x, y)) { st->options.specialAction = PcKeySpecialAction::DismountReclickLockOnKeyDown; InvalidateRect(hwnd, nullptr, FALSE); return 0; }
            return 0;
        }
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) { finish(st, false); return 0; }
            if (wp == VK_RETURN) { finish(st, true); return 0; }
            break;
        case WM_CLOSE:
            finish(st, false);
            return 0;
        case WM_DESTROY:
            notifyModelessResult(st);
            if (st->titleFont) { DeleteObject(st->titleFont); st->titleFont = nullptr; }
            if (st->font) { DeleteObject(st->font); st->font = nullptr; }
            if (st->smallFont) { DeleteObject(st->smallFont); st->smallFont = nullptr; }
            if (!st->done) { st->done = true; st->ok = false; }
            if (st->modeless) {
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
                delete st;
            }
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
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

static HWND createDialogWindow(HINSTANCE inst, HWND ownerHwnd, OptionDialogState* st, bool activate) {
    registerClass(inst);
    RECT owner{};
    GetWindowRect(ownerHwnd, &owner);
    const int w = scalePx(ownerHwnd, 480);
    const int h = scalePx(ownerHwnd, 400);
    const int x = owner.left + ((owner.right - owner.left) - w) / 2;
    const int y = owner.top + ((owner.bottom - owner.top) - h) / 2;
    HWND hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
        kClassName,
        L"普通按键选项",
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        x, y, w, h,
        ownerHwnd,
        nullptr,
        inst,
        st
    );
    if (!hwnd) return nullptr;
    st->hwnd = hwnd;
    ShowWindow(hwnd, activate ? SW_SHOW : SW_SHOWNOACTIVATE);
    if (activate) SetForegroundWindow(hwnd);
    return hwnd;
}
} // namespace

bool ShowNormalKeyOptionsDialog(HINSTANCE inst, HWND ownerHwnd, PcNormalKeyOptions& options) {
    OptionDialogState st{};
    st.inst = inst;
    st.owner = ownerHwnd;
    st.options = options;
    st.modeless = false;
    if (!createDialogWindow(inst, ownerHwnd, &st, true)) return false;

    MSG msg{};
    while (!st.done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (!st.ok) return false;
    options = st.options;
    return true;
}

bool OpenNormalKeyOptionsDialog(HINSTANCE inst, HWND ownerHwnd, const PcNormalKeyOptions& options, WPARAM cookie) {
    auto* st = new OptionDialogState();
    st->inst = inst;
    st->owner = ownerHwnd;
    st->options = options;
    st->modeless = true;
    st->cookie = cookie;
    if (!createDialogWindow(inst, ownerHwnd, st, true)) {
        delete st;
        return false;
    }
    return true;
}
