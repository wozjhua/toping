#include "pc_lock_options_dialog.h"
#include "pc_ui_theme.h"

#include <algorithm>
#include <cwchar>
#include <windowsx.h>

namespace {
    constexpr wchar_t kClassName[] = L"HuiLangPcLockOptionsDialog";

    struct LockDialogState {
        HINSTANCE inst{};
        HWND owner{};
        HWND hwnd{};
        PcLockOptions options{};
        bool done = false;
        bool ok = false;
        WPARAM cookie = 0;
        HFONT font{};
        HFONT smallFont{};
    };

    static int scalePx(HWND hwnd, int px) {
        HDC dc = GetDC(hwnd);
        const int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSX) : 96;
        if (dc) ReleaseDC(hwnd, dc);
        return MulDiv(px, dpi > 0 ? dpi : 96, 96);
    }
    static RECT rc(int l, int t, int r, int b) { return RECT{ l,t,r,b }; }
    static bool ptIn(const RECT& r, int x, int y) { return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom; }
    static RECT panelRect(const RECT& c) {
        // 客户区内不再额外留一圈外边距；设置面板直接铺满窗口客户区。
        return rc(c.left, c.top, c.right, c.bottom);
    }
    static RECT mode1Rect(const RECT& p) { return rc(p.left + 24, p.top + 38, p.left + 136, p.top + 72); }
    static RECT mode2Rect(const RECT& p) { return rc(p.left + 150, p.top + 38, p.left + 306, p.top + 72); }
    static RECT mode3Rect(const RECT& p) { return rc(p.left + 320, p.top + 38, p.right - 24, p.top + 72); }
    static RECT speedXMinusRect(const RECT& p) { return rc(p.left + 24, p.top + 114, p.left + 64, p.top + 146); }
    static RECT speedXValueRect(const RECT& p) { return rc(p.left + 72, p.top + 114, p.right - 72, p.top + 146); }
    static RECT speedXPlusRect(const RECT& p) { return rc(p.right - 64, p.top + 114, p.right - 24, p.top + 146); }
    static RECT speedYMinusRect(const RECT& p) { return rc(p.left + 24, p.top + 184, p.left + 64, p.top + 216); }
    static RECT speedYValueRect(const RECT& p) { return rc(p.left + 72, p.top + 184, p.right - 72, p.top + 216); }
    static RECT speedYPlusRect(const RECT& p) { return rc(p.right - 64, p.top + 184, p.right - 24, p.top + 216); }
    static RECT rebuildMinusRect(const RECT& p) { return rc(p.left + 24, p.top + 254, p.left + 64, p.top + 286); }
    static RECT rebuildValueRect(const RECT& p) { return rc(p.left + 72, p.top + 254, p.right - 72, p.top + 286); }
    static RECT rebuildPlusRect(const RECT& p) { return rc(p.right - 64, p.top + 254, p.right - 24, p.top + 286); }
    static RECT cancelRect(const RECT& p) { return rc(p.left + 24, p.bottom - 48, p.left + 136, p.bottom - 14); }
    static RECT okRect(const RECT& p) { return rc(p.right - 136, p.bottom - 48, p.right - 24, p.bottom - 14); }

    static void drawChip(HDC dc, HFONT font, const RECT& r, const wchar_t* text, bool selected) {
        const auto& th = PcUi::DefaultTheme();
        HBRUSH fill = PcUi::CreateBrush(selected ? th.accent : th.panel2);
        PcUi::DrawRoundFill(dc, r, 12, fill);
        DeleteObject(fill);
        PcUi::DrawRoundBorder(dc, r, 12, PcUi::Rgb(selected ? th.accent2 : th.stroke), selected ? 2 : 1);
        PcUi::DrawTextCenter(dc, r, text, font, PcUi::Rgb(selected ? th.bg : th.text));
    }

    static void clampOptions(PcLockOptions& o) {
        o.speedXNorm = PcLockOptionLimits::ClampSpeedNorm(o.speedXNorm);
        o.speedYNorm = PcLockOptionLimits::ClampSpeedNorm(o.speedYNorm);
        o.rebuildDownDelayMs = PcLockOptionLimits::ClampRebuildDownDelayMs(o.rebuildDownDelayMs);
    }

    static void paintDialog(LockDialogState* st, HDC dc) {
        RECT client{}; GetClientRect(st->hwnd, &client);
        const auto& th = PcUi::DefaultTheme();
        HBRUSH bg = PcUi::CreateBrush(th.bg);
        FillRect(dc, &client, bg); DeleteObject(bg);
        RECT p = panelRect(client);
        HBRUSH panel = PcUi::CreateBrush(th.panel);
        PcUi::DrawRoundFill(dc, p, 18, panel); DeleteObject(panel);
        PcUi::DrawRoundBorder(dc, p, 18, PcUi::Rgb(th.stroke), 1);

        // PcUi::DrawTextLeft(dc, rc(p.left+24,p.top+14,p.right-24,p.top+36), L"触摸模式", st->font, PcUi::Rgb(th.textMuted));
        drawChip(dc, st->font, mode1Rect(p), L"其他", st->options.mode == PcLockSlideTouchMode::SingleReanchor);
        drawChip(dc, st->font, mode2Rect(p), L"三角|高能|使命|暗区", st->options.mode == PcLockSlideTouchMode::DualSimultaneous);
        drawChip(dc, st->font, mode3Rect(p), L"和平|无畏", st->options.mode == PcLockSlideTouchMode::DualSequential);

        PcUi::DrawTextLeft(dc, rc(p.left + 24, p.top + 90, p.right - 24, p.top + 112), L"鼠标 X 移动速度", st->font, PcUi::Rgb(th.textMuted));
        drawChip(dc, st->font, speedXMinusRect(p), L"-", false);
        drawChip(dc, st->font, speedXPlusRect(p), L"+", false);
        wchar_t sx[96]{}; std::swprintf(sx, 96, L"X %.1f", st->options.speedXNorm);
        drawChip(dc, st->font, speedXValueRect(p), sx, true);

        PcUi::DrawTextLeft(dc, rc(p.left + 24, p.top + 160, p.right - 24, p.top + 182), L"鼠标 Y 移动速度", st->font, PcUi::Rgb(th.textMuted));
        drawChip(dc, st->font, speedYMinusRect(p), L"-", false);
        drawChip(dc, st->font, speedYPlusRect(p), L"+", false);
        wchar_t sy[96]{}; std::swprintf(sy, 96, L"Y %.1f", st->options.speedYNorm);
        drawChip(dc, st->font, speedYValueRect(p), sy, true);

        PcUi::DrawTextLeft(dc, rc(p.left + 24, p.top + 230, p.right - 24, p.top + 252), L"边界重按延迟", st->font, PcUi::Rgb(th.textMuted));
        drawChip(dc, st->font, rebuildMinusRect(p), L"-", false);
        drawChip(dc, st->font, rebuildPlusRect(p), L"+", false);
        wchar_t rd[96]{}; std::swprintf(rd, 96, L"重按 %dms", st->options.rebuildDownDelayMs);
        drawChip(dc, st->font, rebuildValueRect(p), rd, true);

        PcUi::DrawTextLeft(dc, rc(p.left + 24, p.top + 300, p.right - 24, p.top + 336), L"倍率默认0.5", st->smallFont, PcUi::Rgb(th.textMuted), DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);

        RECT c = cancelRect(p);
        HBRUSH cf = PcUi::CreateBrush(th.panel2); PcUi::DrawRoundFill(dc, c, 12, cf); DeleteObject(cf);
        PcUi::DrawRoundBorder(dc, c, 12, PcUi::Rgb(th.stroke), 1);
        PcUi::DrawTextCenter(dc, c, L"取消", st->font, PcUi::Rgb(th.text));

        RECT ok = okRect(p);
        HBRUSH of = PcUi::CreateBrush(th.accent); PcUi::DrawRoundFill(dc, ok, 12, of); DeleteObject(of);
        PcUi::DrawRoundBorder(dc, ok, 12, PcUi::Rgb(th.accent2), 1);
        PcUi::DrawTextCenter(dc, ok, L"确定", st->font, PcUi::Rgb(th.bg));
    }

    static void finish(LockDialogState* st, bool ok) {
        if (!st || st->done) return;
        st->done = true;
        st->ok = ok;
        if (st->hwnd) DestroyWindow(st->hwnd);
    }

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        auto* st = reinterpret_cast<LockDialogState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (msg == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            st = reinterpret_cast<LockDialogState*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));
            return DefWindowProcW(hwnd, msg, wp, lp);
        }
        if (!st) return DefWindowProcW(hwnd, msg, wp, lp);
        switch (msg) {
        case WM_CREATE:
            st->font = PcUi::CreateUiFont(10, true);
            st->smallFont = PcUi::CreateUiFont(9, false);
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps{}; HDC dc = BeginPaint(hwnd, &ps); paintDialog(st, dc); EndPaint(hwnd, &ps); return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_LBUTTONUP: {
            RECT client{}; GetClientRect(hwnd, &client); RECT p = panelRect(client);
            const int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
            if (ptIn(cancelRect(p), x, y)) { finish(st, false); return 0; }
            if (ptIn(okRect(p), x, y)) { clampOptions(st->options); finish(st, true); return 0; }
            if (ptIn(mode1Rect(p), x, y)) { st->options.mode = PcLockSlideTouchMode::SingleReanchor; InvalidateRect(hwnd, nullptr, FALSE); return 0; }
            if (ptIn(mode2Rect(p), x, y)) { st->options.mode = PcLockSlideTouchMode::DualSimultaneous; InvalidateRect(hwnd, nullptr, FALSE); return 0; }
            if (ptIn(mode3Rect(p), x, y)) { st->options.mode = PcLockSlideTouchMode::DualSequential; InvalidateRect(hwnd, nullptr, FALSE); return 0; }
            if (ptIn(speedXMinusRect(p), x, y)) {
                st->options.speedXNorm -= PcLockOptionLimits::kSpeedNormStep;
                clampOptions(st->options);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (ptIn(speedXPlusRect(p), x, y)) {
                st->options.speedXNorm += PcLockOptionLimits::kSpeedNormStep;
                clampOptions(st->options);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (ptIn(speedYMinusRect(p), x, y)) {
                st->options.speedYNorm -= PcLockOptionLimits::kSpeedNormStep;
                clampOptions(st->options);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (ptIn(speedYPlusRect(p), x, y)) {
                st->options.speedYNorm += PcLockOptionLimits::kSpeedNormStep;
                clampOptions(st->options);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (ptIn(rebuildMinusRect(p), x, y)) {
                st->options.rebuildDownDelayMs -= PcLockOptionLimits::kRebuildDownDelayStepMs;
                clampOptions(st->options);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }

            if (ptIn(rebuildPlusRect(p), x, y)) {
                st->options.rebuildDownDelayMs += PcLockOptionLimits::kRebuildDownDelayStepMs;
                clampOptions(st->options);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            return 0;
        }
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) { finish(st, false); return 0; }
            if (wp == VK_RETURN) { clampOptions(st->options); finish(st, true); return 0; }
            break;
        case WM_CLOSE: finish(st, false); return 0;
        case WM_DESTROY: {
            PcLockOptions* result = nullptr;
            if (st->ok) result = new PcLockOptions(st->options);
            if (st->owner) PostMessageW(st->owner, WM_PC_LOCK_OPTIONS_DONE, st->cookie, reinterpret_cast<LPARAM>(result));
            if (st->font) DeleteObject(st->font);
            if (st->smallFont) DeleteObject(st->smallFont);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            delete st;
            return 0;
        }
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    static bool registerClass(HINSTANCE inst) {
        WNDCLASSW wc{}; wc.lpfnWndProc = WndProc; wc.hInstance = inst; wc.lpszClassName = kClassName; wc.hCursor = LoadCursor(nullptr, IDC_ARROW); wc.hbrBackground = nullptr; RegisterClassW(&wc); return true;
    }
} // namespace

bool OpenLockOptionsDialog(HINSTANCE inst, HWND ownerHwnd, const PcLockOptions& options, WPARAM cookie) {
    registerClass(inst);
    RECT owner{}; GetWindowRect(ownerHwnd, &owner);
    const int w = scalePx(ownerHwnd, 480);
    const int h = scalePx(ownerHwnd, 430);
    const int x = owner.left + ((owner.right - owner.left) - w) / 2;
    const int y = owner.top + ((owner.bottom - owner.top) - h) / 2;
    auto* st = new LockDialogState();
    st->inst = inst; st->owner = ownerHwnd; st->options = options; st->cookie = cookie;
    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kClassName, L"视角设置", WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, w, h, ownerHwnd, nullptr, inst, st);
    if (!hwnd) { delete st; return false; }
    st->hwnd = hwnd;
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
    return true;
}
