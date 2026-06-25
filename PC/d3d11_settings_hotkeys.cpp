#include "d3d11_renderer.h"

// trimKeyText
std::wstring D3D11Renderer::trimKeyText(std::wstring s) {
        while (!s.empty() && std::iswspace(s.front())) s.erase(s.begin());
        while (!s.empty() && std::iswspace(s.back())) s.pop_back();
        return s;
    }

// upperKeyText
std::wstring D3D11Renderer::upperKeyText(std::wstring s) {
        for (wchar_t& ch : s) ch = static_cast<wchar_t>(std::towupper(ch));
        return s;
    }

// parseVirtualKey
UINT D3D11Renderer::parseVirtualKey(const std::wstring& raw) {
        std::wstring s = upperKeyText(trimKeyText(raw));
        if (s.empty()) return 0;
        if (s == HLW(L"ENTER") || s == HLW(L"RETURN") || s == HLW(L"回车")) return VK_RETURN;
        if (s == HLW(L"ESC") || s == HLW(L"ESCAPE") || s == HLW(L"退出")) return VK_ESCAPE;
        if (s == HLW(L"SPACE") || s == HLW(L"空格")) return VK_SPACE;
        if (s == HLW(L"TAB")) return VK_TAB;
        if (s == HLW(L"BACKSPACE") || s == HLW(L"退格")) return VK_BACK;
        if (s == HLW(L"DELETE") || s == HLW(L"DEL")) return VK_DELETE;
        if (s == HLW(L"LEFT") || s == HLW(L"左")) return VK_LEFT;
        if (s == HLW(L"RIGHT") || s == HLW(L"右")) return VK_RIGHT;
        if (s == HLW(L"UP") || s == HLW(L"上")) return VK_UP;
        if (s == HLW(L"DOWN") || s == HLW(L"下")) return VK_DOWN;
        if (s.size() >= 2 && s[0] == L'F') {
            int n = _wtoi(s.c_str() + 1);
            if (n >= 1 && n <= 24) return VK_F1 + static_cast<UINT>(n - 1);
        }
        if (s.size() == 1) {
            wchar_t ch = s[0];
            if (ch >= L'A' && ch <= L'Z') return static_cast<UINT>(ch);
            if (ch >= L'0' && ch <= L'9') return static_cast<UINT>(ch);
        }
        return 0;
    }

// keyName
std::wstring D3D11Renderer::keyName(UINT vk) {
        if (vk >= VK_F1 && vk <= VK_F24) {
            wchar_t buf[16]{};
            std::swprintf(buf, 16, HLW(L"F%u"), unsigned(vk - VK_F1 + 1));
            return buf;
        }
        switch (vk) {
        case VK_RETURN: return HLW(L"Enter");
        case VK_ESCAPE: return HLW(L"Esc");
        case VK_SPACE: return HLW(L"Space");
        case VK_TAB: return HLW(L"Tab");
        case VK_BACK: return HLW(L"Backspace");
        case VK_DELETE: return HLW(L"Delete");
        case VK_LEFT: return HLW(L"Left");
        case VK_RIGHT: return HLW(L"Right");
        case VK_UP: return HLW(L"Up");
        case VK_DOWN: return HLW(L"Down");
        default:
            if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) {
                wchar_t buf[2]{ static_cast<wchar_t>(vk), 0 };
                return buf;
            }
            return L"";
        }
    }

// supportedHotkeyKeys
const UINT* D3D11Renderer::supportedHotkeyKeys(size_t& count) {
        static const UINT keys[] = {
            VK_F1, VK_F2, VK_F3, VK_F4, VK_F5, VK_F6, VK_F7, VK_F8,
            VK_F9, VK_F10, VK_F11, VK_F12, VK_F13, VK_F14, VK_F15, VK_F16,
            VK_F17, VK_F18, VK_F19, VK_F20, VK_F21, VK_F22, VK_F23, VK_F24,
            VK_RETURN, VK_ESCAPE, VK_SPACE, VK_TAB, VK_BACK, VK_DELETE,
            VK_LEFT, VK_RIGHT, VK_UP, VK_DOWN,
            'A','B','C','D','E','F','G','H','I','J','K','L','M',
            'N','O','P','Q','R','S','T','U','V','W','X','Y','Z',
            '0','1','2','3','4','5','6','7','8','9'
        };
        count = sizeof(keys) / sizeof(keys[0]);
        return keys;
    }

// isAnySupportedHotkeyDown
bool D3D11Renderer::isAnySupportedHotkeyDown() {
        size_t count = 0;
        const UINT* keys = supportedHotkeyKeys(count);
        for (size_t i = 0; i < count; ++i) {
            if ((GetAsyncKeyState(static_cast<int>(keys[i])) & 0x8000) != 0) {
                return true;
            }
        }
        return false;
    }

// isSupportedHotkey
bool D3D11Renderer::isSupportedHotkey(UINT vk) {
        size_t count = 0;
        const UINT* keys = supportedHotkeyKeys(count);
        for (size_t i = 0; i < count; ++i) {
            if (keys[i] == vk) return true;
        }
        return false;
    }

// setHotkeyTextForControl
void D3D11Renderer::setHotkeyTextForControl(HWND edit, UINT vk) {
        if (!edit || !isSupportedHotkey(vk)) return;
        const std::wstring name = keyName(vk);
        if (name.empty()) return;
        SetWindowTextW(edit, name.c_str());
        activeHotkeyEditId_ = GetDlgCtrlID(edit);
        settingsHotkeyCaptureArmed_ = false;
        if (settingsHwnd_) SetTimer(settingsHwnd_, IDC_TIMER_HOTKEY_CAPTURE, 50, nullptr);
        SetFocus(edit);
    }

// HotkeyEditProc
LRESULT CALLBACK D3D11Renderer::HotkeyEditProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<D3D11Renderer*>(GetPropW(hwnd, L"HuiLangHotkeySelf"));
    auto oldProc = reinterpret_cast<WNDPROC>(GetPropW(hwnd, L"HuiLangHotkeyOldProc"));
    if (!oldProc) return DefWindowProcW(hwnd, msg, wp, lp);

    switch (msg) {
    case WM_SETFOCUS:
        if (self) {
            self->activeHotkeyEditId_ = GetDlgCtrlID(hwnd);
            self->settingsHotkeyCaptureArmed_ = false;
            if (self->settingsHwnd_) SetTimer(self->settingsHwnd_, IDC_TIMER_HOTKEY_CAPTURE, 50, nullptr);
        }
        break;
    case WM_KILLFOCUS:
        if (self && self->activeHotkeyEditId_ == GetDlgCtrlID(hwnd)) {
            // 不清空文本，只停止“当前编辑框”状态；窗口内切换焦点时 WM_SETFOCUS 会重设。
            self->activeHotkeyEditId_ = 0;
        }
        break;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        const UINT vk = static_cast<UINT>(wp);
        if (self && isSupportedHotkey(vk)) {
            self->setHotkeyTextForControl(hwnd, vk);
            return 0;
        }
        break;
    }
    case WM_CHAR:
    case WM_SYSCHAR:
        // 热键框只捕获虚拟键，不让字符输入污染文本。
        return 0;
    case WM_NCDESTROY: {
        RemovePropW(hwnd, L"HuiLangHotkeySelf");
        RemovePropW(hwnd, L"HuiLangHotkeyOldProc");
        break;
    }
    default:
        break;
    }
    return CallWindowProcW(oldProc, hwnd, msg, wp, lp);
}

// focusedHotkeyEditId
int D3D11Renderer::focusedHotkeyEditId(HWND settingsHwnd) {
    HWND focus = GetFocus();
    if (!focus || !settingsHwnd) return 0;
    if (GetAncestor(focus, GA_ROOT) != settingsHwnd) return 0;
    const int id = GetDlgCtrlID(focus);
    switch (id) {
    case IDC_EDIT_HUD_KEY:
    case IDC_EDIT_FULLSCREEN_KEY1:
    case IDC_EDIT_FULLSCREEN_KEY2:
    case IDC_EDIT_SETTINGS_KEY:
    case IDC_EDIT_EXIT_KEY:
        return id;
    default:
        return 0;
    }
}

// armSettingsHotkeyCapture
void D3D11Renderer::armSettingsHotkeyCapture(HWND hwnd) {
    settingsHotkeyCaptureArmed_ = false;
    SetTimer(hwnd, IDC_TIMER_HOTKEY_CAPTURE, 50, nullptr);
}

// stopSettingsHotkeyCapture
void D3D11Renderer::stopSettingsHotkeyCapture(HWND hwnd) {
    KillTimer(hwnd, IDC_TIMER_HOTKEY_CAPTURE);
    settingsHotkeyCaptureArmed_ = false;
    activeHotkeyEditId_ = 0;
}

// captureHotkeyByTimer
void D3D11Renderer::captureHotkeyByTimer() {
    if (!settingsHwnd_ || !IsWindowVisible(settingsHwnd_)) return;

    const bool anyDown = isAnySupportedHotkeyDown();
    if (!settingsHotkeyCaptureArmed_) {
        // 避免打开设置窗口时，把还没松开的 F2 直接捕获到输入框。
        if (!anyDown) settingsHotkeyCaptureArmed_ = true;
        return;
    }

    int editId = activeHotkeyEditId_;
    if (editId == 0) editId = focusedHotkeyEditId(settingsHwnd_);
    if (editId == 0) return;

    size_t count = 0;
    const UINT* keys = supportedHotkeyKeys(count);
    for (size_t i = 0; i < count; ++i) {
        const UINT vk = keys[i];
        if ((GetAsyncKeyState(static_cast<int>(vk)) & 0x8000) == 0) continue;

        const std::wstring name = keyName(vk);
        if (!name.empty()) {
            SetWindowTextW(GetDlgItem(settingsHwnd_, editId), name.c_str());
            settingsHotkeyCaptureArmed_ = false; // 等这次按键松开后再捕获下一次。
            SetFocus(GetDlgItem(settingsHwnd_, editId));
        }
        return;
    }
}

