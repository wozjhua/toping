#include "pc_bind_dialog.h"
#include "pc_ui_theme.h"
#include <windowsx.h>

#include <algorithm>
#include <array>

namespace {
    constexpr wchar_t kCaptureClassName[] = L"HuiLangPcBindDialogV2";
    constexpr UINT WM_BIND_UPDATE = WM_APP + 310;
    constexpr UINT WM_BIND_DONE = WM_APP + 311;
    constexpr int kMouseWheelUpCode = 6;
    constexpr int kMouseWheelDownCode = 7;

    enum class Hit { None, Capture, Ok, Cancel, Close };

    struct CaptureState {
        HINSTANCE inst{};
        HWND owner{};
        HWND hwnd{};
        HHOOK keyHook{};
        HHOOK mouseHook{};
        HFONT titleFont{};
        HFONT bodyFont{};
        HFONT bigFont{};
        HFONT smallFont{};
        HBRUSH bgBrush{};
        HBRUSH panelBrush{};
        HBRUSH cardBrush{};
        HBRUSH accentBrush{};
        HBRUSH dangerBrush{};
        std::vector<int> captured;
        bool keyDown[256]{};
        int mouseButtonCode = 0;
        bool captureMouse = false;
        bool done = false;
        bool ok = false;
        Hit pressHit = Hit::None;
    };

    static CaptureState* g_capture = nullptr;

    static RECT rectLTRB(int l, int t, int r, int b) { RECT rc{ l, t, r, b }; return rc; }
    static RECT offsetRect(RECT rc, int dx, int dy) { OffsetRect(&rc, dx, dy); return rc; }

    static RECT panelRect(HWND hwnd) {
        RECT rc{};
        GetClientRect(hwnd, &rc);
        const int w = 420;
        const int h = 292;
        const int x = (rc.right - rc.left - w) / 2;
        const int y = (rc.bottom - rc.top - h) / 2;
        return rectLTRB(x, y, x + w, y + h);
    }
    static RECT closeRect(HWND hwnd) { RECT p = panelRect(hwnd); return rectLTRB(p.right - 42, p.top + 14, p.right - 16, p.top + 40); }
    static RECT candidateRect(HWND hwnd) { RECT p = panelRect(hwnd); return rectLTRB(p.left + 30, p.top + 58, p.right - 30, p.top + 132); }
    static RECT captureRect(HWND hwnd) { RECT p = panelRect(hwnd); return rectLTRB(p.left + 30, p.top + 150, p.right - 30, p.top + 190); }
    static RECT cancelRect(HWND hwnd) { RECT p = panelRect(hwnd); return rectLTRB(p.left + 30, p.bottom - 50, p.left + 150, p.bottom - 12); }
    static RECT okRect(HWND hwnd) { RECT p = panelRect(hwnd); return rectLTRB(p.right - 150, p.bottom - 50, p.right - 30, p.bottom - 12); }

    static bool ptIn(const RECT& rc, int x, int y) {
        return x >= rc.left && x < rc.right && y >= rc.top && y < rc.bottom;
    }

    static Hit hitAt(HWND hwnd, int x, int y) {
        if (ptIn(closeRect(hwnd), x, y)) return Hit::Close;
        if (ptIn(captureRect(hwnd), x, y)) return Hit::Capture;
        if (ptIn(cancelRect(hwnd), x, y)) return Hit::Cancel;
        if (ptIn(okRect(hwnd), x, y)) return Hit::Ok;
        return Hit::None;
    }

    static Hit hitAtScreen(CaptureState* st, LONG screenX, LONG screenY) {
        if (!st || !st->hwnd) return Hit::None;
        POINT pt{ screenX, screenY };
        ScreenToClient(st->hwnd, &pt);
        return hitAt(st->hwnd, pt.x, pt.y);
    }

    static bool isDialogCommandHit(Hit h) {
        return h == Hit::Capture || h == Hit::Ok || h == Hit::Cancel || h == Hit::Close;
    }

    static void resetCapturedCombo(CaptureState* st) {
        if (!st) return;
        st->captured.clear();
        std::fill(std::begin(st->keyDown), std::end(st->keyDown), false);
        st->mouseButtonCode = 0;
    }

    static bool isModifierVk(int vk) {
        return vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL ||
            vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT ||
            vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU ||
            vk == VK_LWIN || vk == VK_RWIN;
    }

    static UINT normalizeLowLevelVk(UINT vk, const KBDLLHOOKSTRUCT* info) {
        const bool extended = info && ((info->flags & LLKHF_EXTENDED) != 0);
        if (vk == VK_SHIFT) {
            UINT mapped = info ? MapVirtualKeyW(info->scanCode, MAPVK_VSC_TO_VK_EX) : 0;
            if (mapped == VK_LSHIFT || mapped == VK_RSHIFT) return mapped;
            return VK_LSHIFT;
        }
        if (vk == VK_CONTROL) return extended ? VK_RCONTROL : VK_LCONTROL;
        if (vk == VK_MENU) return extended ? VK_RMENU : VK_LMENU;
        return vk;
    }

    static void addCapturedVk(CaptureState* st, int vk) {
        if (!st || vk <= 0 || vk >= 256) return;
        if (std::find(st->captured.begin(), st->captured.end(), vk) == st->captured.end()) {
            st->captured.push_back(vk);
        }
    }

    static void snapshotPressedKeys(CaptureState* st) {
        if (!st) return;
        for (int vk = 0; vk < 256; ++vk) {
            if (st->keyDown[vk]) addCapturedVk(st, vk);
        }
    }

    static std::wstring mouseButtonLabel(int code) {
        switch (code) {
        case 1: return L"鼠标左键";
        case 2: return L"鼠标右键";
        case 3: return L"鼠标中键";
        case 4: return L"鼠标侧键1";
        case 5: return L"鼠标侧键2";
        case kMouseWheelUpCode: return L"鼠标滚轮上";
        case kMouseWheelDownCode: return L"鼠标滚轮下";
        default: return L"";
        }
    }

    static std::wstring comboLabel(const std::vector<int>& codes, int mouseButtonCode = 0) {
        std::vector<int> ordered = codes;
        std::stable_sort(ordered.begin(), ordered.end(), [](int a, int b) {
            auto rank = [](int vk) {
                if (vk == VK_LCONTROL || vk == VK_RCONTROL || vk == VK_CONTROL) return 0;
                if (vk == VK_LSHIFT || vk == VK_RSHIFT || vk == VK_SHIFT) return 1;
                if (vk == VK_LMENU || vk == VK_RMENU || vk == VK_MENU) return 2;
                if (vk == VK_LWIN || vk == VK_RWIN) return 3;
                return 10;
                };
            const int ra = rank(a), rb = rank(b);
            return ra == rb ? a < b : ra < rb;
            });

        std::wstring out;
        for (int vk : ordered) {
            if (!out.empty()) out += L"+";
            out += PcVkToDisplayName(vk);
        }
        if (mouseButtonCode > 0) {
            if (!out.empty()) out += L"+";
            out += mouseButtonLabel(mouseButtonCode);
        }
        return out.empty() ? L"未选择" : out;
    }

    static void finishCapture(CaptureState* st, bool ok) {
        if (!st) return;
        if (ok && st->captured.empty() && st->mouseButtonCode <= 0) return;
        st->done = true;
        st->ok = ok;
        if (st->hwnd) PostMessageW(st->hwnd, WM_BIND_DONE, ok ? 1 : 0, 0);
    }

    static int mouseButtonCodeFromHook(WPARAM wp, const MSLLHOOKSTRUCT* info) {
        switch (wp) {
        case WM_LBUTTONDOWN: return 1;
        case WM_RBUTTONDOWN: return 2;
        case WM_MBUTTONDOWN: return 3;
        case WM_XBUTTONDOWN: {
            const DWORD xbtn = info ? HIWORD(info->mouseData) : XBUTTON1;
            return (xbtn == XBUTTON2) ? 5 : 4;
        }
        case WM_MOUSEWHEEL: {
            const int delta = info ? static_cast<SHORT>(HIWORD(info->mouseData)) : 0;
            if (delta > 0) return kMouseWheelUpCode;
            if (delta < 0) return kMouseWheelDownCode;
            return 0;
        }
        default: return 0;
        }
    }

    static LRESULT CALLBACK KeyboardHookProc(int code, WPARAM wp, LPARAM lp) {
        CaptureState* st = g_capture;
        if (code < 0 || !st || st->done) return CallNextHookEx(nullptr, code, wp, lp);
        const auto* info = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lp);
        if (!info) return CallNextHookEx(nullptr, code, wp, lp);

        const bool down = (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN);
        const bool up = (wp == WM_KEYUP || wp == WM_SYSKEYUP);
        if (!down && !up) return CallNextHookEx(nullptr, code, wp, lp);

        const UINT vk = normalizeLowLevelVk(static_cast<UINT>(info->vkCode), info);
        if (down && vk == VK_ESCAPE) {
            finishCapture(st, false);
            return 1;
        }
        if (down && vk == VK_RETURN) {
            finishCapture(st, true);
            return 1;
        }
        if (vk > 0 && vk < 256) st->keyDown[vk] = down;
        if (down && vk > 0) {
            addCapturedVk(st, static_cast<int>(vk));
            if (st->hwnd) PostMessageW(st->hwnd, WM_BIND_UPDATE, 0, 0);
            return 1;
        }
        if (up) return 1;
        return 1;
    }

    static LRESULT CALLBACK MouseHookProc(int code, WPARAM wp, LPARAM lp) {
        CaptureState* st = g_capture;
        if (code < 0 || !st || st->done) return CallNextHookEx(nullptr, code, wp, lp);
        if (!st->captureMouse) return CallNextHookEx(nullptr, code, wp, lp);

        const auto* info = reinterpret_cast<const MSLLHOOKSTRUCT*>(lp);

        // 捕获鼠标模式下，点击弹窗自己的按钮应该继续交给弹窗处理，
        // 不能被当成“鼠标左键”绑定。否则用户点击 OK/取消 时会误记录左键。
        if (info && (wp == WM_LBUTTONDOWN || wp == WM_LBUTTONUP)) {
            const Hit h = hitAtScreen(st, info->pt.x, info->pt.y);
            if (isDialogCommandHit(h)) {
                return CallNextHookEx(nullptr, code, wp, lp);
            }
        }

        const int button = mouseButtonCodeFromHook(wp, info);
        if (button > 0) {
            snapshotPressedKeys(st);
            st->mouseButtonCode = button;
            st->captureMouse = false;
            if (st->hwnd) PostMessageW(st->hwnd, WM_BIND_UPDATE, 0, 0);
            return 1;
        }
        if (wp == WM_LBUTTONUP || wp == WM_RBUTTONUP || wp == WM_MBUTTONUP || wp == WM_XBUTTONUP) return 1;
        return CallNextHookEx(nullptr, code, wp, lp);
    }

    static void paintDialog(HWND hwnd, CaptureState* st) {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd, &ps);
        if (!dc) return;

        RECT client{};
        GetClientRect(hwnd, &client);
        const auto& theme = PcUi::DefaultTheme();
        HBRUSH scrim = CreateSolidBrush(RGB(0, 0, 0));
        FillRect(dc, &client, scrim);
        DeleteObject(scrim);

        RECT panel = panelRect(hwnd);
        PcUi::DrawRoundFill(dc, panel, 24, st->panelBrush);
        PcUi::DrawRoundBorder(dc, panel, 24, PcUi::Rgb(theme.stroke), 1);

        RECT titleRc = rectLTRB(panel.left + 26, panel.top + 18, panel.right - 54, panel.top + 46);
        PcUi::DrawTextLeft(dc, titleRc, L"绑定按键", st->titleFont, PcUi::Rgb(theme.text));
        PcUi::DrawTextCenter(dc, closeRect(hwnd), L"×", st->titleFont, PcUi::Rgb(theme.textMuted));

        RECT cand = candidateRect(hwnd);
        PcUi::DrawRoundFill(dc, cand, 20, st->cardBrush);
        PcUi::DrawRoundBorder(dc, cand, 20,
            (st->captured.empty() && st->mouseButtonCode <= 0) ? PcUi::Rgb(theme.stroke) : PcUi::Rgb(theme.accent), 2);
        std::wstring candidate = comboLabel(st->captured, st->mouseButtonCode);
        if (candidate == L"未选择" && st->captureMouse) candidate = L"等待键鼠";
        PcUi::DrawTextCenter(dc, cand, candidate.c_str(), st->bigFont, PcUi::Rgb(theme.text));

        RECT cap = captureRect(hwnd);
        PcUi::DrawRoundFill(dc, cap, 14, st->captureMouse ? st->accentBrush : st->dangerBrush);
        PcUi::DrawRoundBorder(dc, cap, 14, st->captureMouse ? PcUi::Rgb(theme.accent2) : PcUi::Rgb(theme.accent), 1);
        PcUi::DrawTextCenter(dc, cap,
            st->captureMouse ? L"请按键盘 + 鼠标/滚轮组合..." : L"捕获鼠标/滚轮 / 键鼠组合",
            st->bodyFont,
            RGB(255, 255, 255));

        RECT hint = rectLTRB(panel.left + 24, cap.bottom + 12, panel.right - 24, cap.bottom + 42);
        PcUi::DrawTextCenter(dc, hint,
            L"直接按键盘组合，或点击捕获鼠标/滚轮；Enter 确认，Esc 取消",
            st->smallFont,
            PcUi::Rgb(theme.textMuted),
            DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        RECT cancel = cancelRect(hwnd);
        PcUi::DrawRoundFill(dc, cancel, 12, st->cardBrush);
        PcUi::DrawRoundBorder(dc, cancel, 12, PcUi::Rgb(theme.stroke), 1);
        PcUi::DrawTextCenter(dc, cancel, L"取消", st->bodyFont, PcUi::Rgb(theme.text));

        RECT ok = okRect(hwnd);
        PcUi::DrawRoundFill(dc, ok, 12, st->accentBrush);
        PcUi::DrawRoundBorder(dc, ok, 12, PcUi::Rgb(theme.accent2), 1);
        PcUi::DrawTextCenter(dc, ok, L"OK", st->bodyFont, RGB(255, 255, 255));

        EndPaint(hwnd, &ps);
    }

    static LRESULT CALLBACK DialogWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        auto* st = reinterpret_cast<CaptureState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (msg == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            st = reinterpret_cast<CaptureState*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));
            return TRUE;
        }
        if (!st) return DefWindowProcW(hwnd, msg, wp, lp);

        switch (msg) {
        case WM_CREATE: {
            const auto& theme = PcUi::DefaultTheme();
            st->bgBrush = PcUi::CreateBrush(theme.bg);
            st->panelBrush = PcUi::CreateBrush(theme.panel);
            st->cardBrush = PcUi::CreateBrush(theme.panel2);
            st->accentBrush = PcUi::CreateBrush(theme.accent);
            st->dangerBrush = PcUi::CreateBrush(theme.danger);
            st->titleFont = PcUi::CreateUiFont(15, true);
            st->bodyFont = PcUi::CreateUiFont(10, true);
            st->bigFont = PcUi::CreateUiFont(22, true);
            st->smallFont = PcUi::CreateUiFont(9, false);
            return 0;
        }
        case WM_PAINT:
            paintDialog(hwnd, st);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_BIND_UPDATE:
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_BIND_DONE:
            st->done = true;
            st->ok = (wp != 0);
            DestroyWindow(hwnd);
            return 0;
        case WM_LBUTTONDOWN: {
            st->pressHit = hitAt(hwnd, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            return 0;
        }
        case WM_LBUTTONUP: {
            const Hit h = hitAt(hwnd, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            if (h == st->pressHit) {
                switch (h) {
                case Hit::Capture:
                    // 重新进入鼠标/键鼠组合捕获时，必须清空上一次候选。
                    // 否则会把刚才直接键盘捕获到的按键保留下来，导致组合键错误。
                    resetCapturedCombo(st);
                    st->captureMouse = true;
                    InvalidateRect(hwnd, nullptr, FALSE);
                    break;
                case Hit::Ok:
                    finishCapture(st, true);
                    break;
                case Hit::Cancel:
                case Hit::Close:
                    finishCapture(st, false);
                    break;
                default:
                    break;
                }
            }
            st->pressHit = Hit::None;
            return 0;
        }
        case WM_CLOSE:
            finishCapture(st, false);
            return 0;
        case WM_DESTROY:
            if (st->titleFont) { DeleteObject(st->titleFont); st->titleFont = nullptr; }
            if (st->bodyFont) { DeleteObject(st->bodyFont); st->bodyFont = nullptr; }
            if (st->bigFont) { DeleteObject(st->bigFont); st->bigFont = nullptr; }
            if (st->smallFont) { DeleteObject(st->smallFont); st->smallFont = nullptr; }
            if (st->bgBrush) { DeleteObject(st->bgBrush); st->bgBrush = nullptr; }
            if (st->panelBrush) { DeleteObject(st->panelBrush); st->panelBrush = nullptr; }
            if (st->cardBrush) { DeleteObject(st->cardBrush); st->cardBrush = nullptr; }
            if (st->accentBrush) { DeleteObject(st->accentBrush); st->accentBrush = nullptr; }
            if (st->dangerBrush) { DeleteObject(st->dangerBrush); st->dangerBrush = nullptr; }
            if (!st->done) {
                st->done = true;
                st->ok = false;
            }
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wp, lp);
        }
    }

} // namespace

std::wstring PcVkToDisplayName(int vk) {
    switch (vk) {
    case VK_LCONTROL: return L"LCtrl";
    case VK_RCONTROL: return L"RCtrl";
    case VK_CONTROL: return L"Ctrl";
    case VK_LSHIFT: return L"LShift";
    case VK_RSHIFT: return L"RShift";
    case VK_SHIFT: return L"Shift";
    case VK_LMENU: return L"LAlt";
    case VK_RMENU: return L"RAlt";
    case VK_MENU: return L"Alt";
    case VK_LWIN: return L"LWin";
    case VK_RWIN: return L"RWin";
    case VK_ESCAPE: return L"Esc";
    case VK_SPACE: return L"Space";
    case VK_RETURN: return L"Enter";
    case VK_TAB: return L"Tab";
    case VK_BACK: return L"Backspace";
    case VK_UP: return L"Up";
    case VK_DOWN: return L"Down";
    case VK_LEFT: return L"Left";
    case VK_RIGHT: return L"Right";
    default: break;
    }
    if (vk >= VK_F1 && vk <= VK_F24) return L"F" + std::to_wstring(vk - VK_F1 + 1);
    if (vk >= 'A' && vk <= 'Z') return std::wstring(1, static_cast<wchar_t>(vk));
    if (vk >= '0' && vk <= '9') return std::wstring(1, static_cast<wchar_t>(vk));

    wchar_t name[96]{};
    UINT scan = MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_VSC);
    LONG lParam = static_cast<LONG>(scan << 16);
    if (vk == VK_LEFT || vk == VK_UP || vk == VK_RIGHT || vk == VK_DOWN ||
        vk == VK_INSERT || vk == VK_DELETE || vk == VK_HOME || vk == VK_END ||
        vk == VK_PRIOR || vk == VK_NEXT) {
        lParam |= 0x01000000L;
    }
    if (GetKeyNameTextW(lParam, name, 96) > 0) return name;
    return L"VK" + std::to_wstring(vk);
}

bool CaptureKeyComboDialog(HINSTANCE inst, HWND ownerHwnd, PcCapturedKeyCombo& outCombo) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = DialogWndProc;
    wc.hInstance = inst;
    wc.lpszClassName = kCaptureClassName;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    RegisterClassW(&wc);

    RECT owner{};
    GetWindowRect(ownerHwnd, &owner);
    const int w = 460;
    const int h = 340;
    const int x = owner.left + ((owner.right - owner.left) - w) / 2;
    const int y = owner.top + ((owner.bottom - owner.top) - h) / 2;

    CaptureState state{};
    state.inst = inst;
    state.owner = ownerHwnd;

    HWND hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
        kCaptureClassName,
        L"绑定按键",
        WS_POPUP,
        x, y, w, h,
        ownerHwnd,
        nullptr,
        inst,
        &state
    );
    if (!hwnd) return false;
    state.hwnd = hwnd;

    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    UpdateWindow(hwnd);

    g_capture = &state;
    state.keyHook = SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardHookProc, GetModuleHandleW(nullptr), 0);
    state.mouseHook = SetWindowsHookExW(WH_MOUSE_LL, MouseHookProc, GetModuleHandleW(nullptr), 0);
    if (!state.keyHook || !state.mouseHook) {
        if (state.keyHook) UnhookWindowsHookEx(state.keyHook);
        if (state.mouseHook) UnhookWindowsHookEx(state.mouseHook);
        if (g_capture == &state) g_capture = nullptr;
        DestroyWindow(hwnd);
        return false;
    }

    MSG msg{};
    while (!state.done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (state.keyHook) UnhookWindowsHookEx(state.keyHook);
    if (state.mouseHook) UnhookWindowsHookEx(state.mouseHook);
    if (g_capture == &state) g_capture = nullptr;

    if (!state.ok || (state.captured.empty() && state.mouseButtonCode <= 0)) return false;
    outCombo.vkCodes = state.captured;
    outCombo.mouseButtonCode = state.mouseButtonCode;
    outCombo.label = comboLabel(state.captured, state.mouseButtonCode);
    return true;
}
