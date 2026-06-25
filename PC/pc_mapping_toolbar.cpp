#include "pc_mapping_toolbar.h"
#include "pc_ui_theme.h"

#include <algorithm>
#include <utility>

namespace {
constexpr wchar_t kToolbarClassName[] = L"HuiLangPcMappingToolbarPanel";
constexpr UINT_PTR kRefreshTimerId = 1;
constexpr UINT kRefreshMs = 50; // 20 FPS

// 面板按“常用创建 / 管理 / 存档 / 状态”重新排版。
// 保持现有 HWND 成员和回调不变，只调整文案、尺寸和布局。
constexpr int kToolbarCollapsedWidth = 100; //折叠状态的窗口宽度。
constexpr int kToolbarCollapsedHeight = 54; //折叠状态的窗口高度。
constexpr int kToolbarExpandedWidth = 200;//展开状态的窗口宽度
constexpr int kToolbarExpandedHeight = 548;//展开状态的窗口高度。
constexpr int kToolbarStatusMinHeight = 96;//底部状态文本区域的最小高度

constexpr int IDC_MAP_TOGGLE = 4100;
constexpr int IDC_MAP_KEY = 4101;
constexpr int IDC_MAP_COMPASS = 4102;
constexpr int IDC_MAP_LOCK = 4103;
constexpr int IDC_MAP_MENU_RADIAL = 4104;
constexpr int IDC_MAP_MENU_ITEM = 4105;
constexpr int IDC_MAP_MENU_BAG = 4106;
constexpr int IDC_MAP_MACRO = 4107;
constexpr int IDC_MAP_PROFILE = 4108;
constexpr int IDC_MAP_HIDE_UI = 4109;
constexpr int IDC_MAP_LOAD1 = 4110;
constexpr int IDC_MAP_SAVE1 = 4111;
constexpr int IDC_MAP_LOAD2 = 4112;
constexpr int IDC_MAP_SAVE2 = 4113;
constexpr int IDC_MAP_LOAD3 = 4114;
constexpr int IDC_MAP_SAVE3 = 4115;
constexpr int IDC_MAP_STATUS = 4116;
constexpr int IDC_MAP_COUNT = 4117;

static int dpiScale(HWND hwnd, int px) {
    HDC dc = GetDC(hwnd);
    int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSX) : 96;
    if (dc) ReleaseDC(hwnd, dc);
    return MulDiv(px, dpi > 0 ? dpi : 96, 96);
}
}

bool PcMappingToolbarPanel::Start(HINSTANCE inst, HWND ownerHwnd, Callbacks callbacks) {
    Stop();
    inst_ = inst;
    ownerHwnd_ = ownerHwnd;
    callbacks_ = std::move(callbacks);
    expanded_ = callbacks_.isEditMode ? callbacks_.isEditMode() : false;

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst_;
    wc.lpszClassName = kToolbarClassName;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    RegisterClassW(&wc);

    hwnd_ = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        kToolbarClassName,
        L"PC映射",
        WS_POPUP,
        100, 100, expanded_ ? kToolbarExpandedWidth : kToolbarCollapsedWidth, expanded_ ? kToolbarExpandedHeight : kToolbarCollapsedHeight,
        ownerHwnd_,
        nullptr,
        inst_,
        this
    );
    if (!hwnd_) return false;

    const auto& theme = PcUi::DefaultTheme();
    font_ = PcUi::CreateUiFont(10, true);
    titleFont_ = PcUi::CreateUiFont(11, true);
    bgBrush_ = PcUi::CreateBrush(theme.bg);
    cardBrush_ = PcUi::CreateBrush(theme.panel2);

    CreateControls();
    LayoutControls();
    RefreshText();
    UpdateOwnerPosition();
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    SetTimer(hwnd_, kRefreshTimerId, kRefreshMs, nullptr);
    ApplyEditMode(expanded_);
    return true;
}

void PcMappingToolbarPanel::Stop() {
    if (hwnd_) {
        KillTimer(hwnd_, kRefreshTimerId);
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    if (font_) { DeleteObject(font_); font_ = nullptr; }
    if (titleFont_) { DeleteObject(titleFont_); titleFont_ = nullptr; }
    if (bgBrush_) { DeleteObject(bgBrush_); bgBrush_ = nullptr; }
    if (cardBrush_) { DeleteObject(cardBrush_); cardBrush_ = nullptr; }
}

void PcMappingToolbarPanel::SetVisible(bool visible) {
    if (hwnd_) ShowWindow(hwnd_, visible ? SW_SHOWNOACTIVATE : SW_HIDE);
}

void PcMappingToolbarPanel::SetExpanded(bool expanded) {
    if (expanded_ == expanded) {
        RefreshText();
        return;
    }
    expanded_ = expanded;
    ApplyEditMode(expanded_);
    LayoutControls();
    UpdateOwnerPosition();
    InvalidateRect(hwnd_, nullptr, TRUE);
}

void PcMappingToolbarPanel::ApplyEditMode(bool editMode) {
    if (callbacks_.setEditMode) callbacks_.setEditMode(editMode);
    ShowExpandedControls(editMode);
    RefreshText();
}

void PcMappingToolbarPanel::ShowExpandedControls(bool show) {
    const int cmd = show ? SW_SHOWNOACTIVATE : SW_HIDE;
    HWND controls[] = {btnKey_, btnCompass_, btnLock_, btnMenuRadial_, btnMenuItem_, btnMenuBag_, btnMacro_, btnProfile_, btnHideUi_, btnLoad1_, btnSave1_, btnLoad2_, btnSave2_, btnLoad3_, btnSave3_, count_, status_};
    for (HWND h : controls) {
        if (h) ShowWindow(h, cmd);
    }
}

void PcMappingToolbarPanel::UpdateOwnerPosition() {
    if (!hwnd_ || !IsWindow(ownerHwnd_)) return;
    RECT rc{};
    GetWindowRect(ownerHwnd_, &rc);
    const int width = dpiScale(ownerHwnd_, expanded_ ? kToolbarExpandedWidth : kToolbarCollapsedWidth);
    const int height = dpiScale(ownerHwnd_, expanded_ ? kToolbarExpandedHeight : kToolbarCollapsedHeight);
    const int gap = dpiScale(ownerHwnd_, 8);
    SetWindowPos(hwnd_, HWND_TOPMOST, rc.right + gap, rc.top, width, height, SWP_NOACTIVATE);
}

LRESULT CALLBACK PcMappingToolbarPanel::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<PcMappingToolbarPanel*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = reinterpret_cast<PcMappingToolbarPanel*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        return TRUE;
    }
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    return self->HandleMessage(msg, wp, lp);
}

LRESULT PcMappingToolbarPanel::HandleMessage(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_COMMAND: {
            switch (LOWORD(wp)) {
                case IDC_MAP_TOGGLE:
                    SetExpanded(!expanded_);
                    return 0;
                case IDC_MAP_KEY:
                    if (callbacks_.beginCreateKey) callbacks_.beginCreateKey();
                    return 0;
                case IDC_MAP_COMPASS:
                    if (callbacks_.beginCreateCompass) callbacks_.beginCreateCompass();
                    return 0;
                case IDC_MAP_LOCK:
                    if (callbacks_.beginCreateLock) callbacks_.beginCreateLock();
                    return 0;
                case IDC_MAP_MENU_RADIAL:
                    if (callbacks_.beginCreateMenuRadial) callbacks_.beginCreateMenuRadial();
                    return 0;
                case IDC_MAP_MENU_ITEM:
                    if (callbacks_.beginCreateMenuItem) callbacks_.beginCreateMenuItem();
                    return 0;
                case IDC_MAP_MENU_BAG:
                    if (callbacks_.beginCreateMenuBag) callbacks_.beginCreateMenuBag();
                    return 0;
                case IDC_MAP_MACRO:
                    if (callbacks_.beginCreateMacro) callbacks_.beginCreateMacro();
                    return 0;
                case IDC_MAP_PROFILE:
                    if (callbacks_.showProfiles) callbacks_.showProfiles();
                    return 0;
                case IDC_MAP_HIDE_UI:
                    if (callbacks_.toggleMappingUi) callbacks_.toggleMappingUi();
                    RefreshText();
                    return 0;
                case IDC_MAP_LOAD1:
                    if (callbacks_.loadMappings) callbacks_.loadMappings(1);
                    RefreshText();
                    return 0;
                case IDC_MAP_SAVE1:
                    if (callbacks_.saveMappings) callbacks_.saveMappings(1);
                    RefreshText();
                    return 0;
                case IDC_MAP_LOAD2:
                    if (callbacks_.loadMappings) callbacks_.loadMappings(2);
                    RefreshText();
                    return 0;
                case IDC_MAP_SAVE2:
                    if (callbacks_.saveMappings) callbacks_.saveMappings(2);
                    RefreshText();
                    return 0;
                case IDC_MAP_LOAD3:
                    if (callbacks_.loadMappings) callbacks_.loadMappings(3);
                    RefreshText();
                    return 0;
                case IDC_MAP_SAVE3:
                    if (callbacks_.saveMappings) callbacks_.saveMappings(3);
                    RefreshText();
                    return 0;
                default:
                    break;
            }
            break;
        }
        case WM_TIMER:
            if (wp == kRefreshTimerId) {
                RefreshText();
                return 0;
            }
            break;
        case WM_CTLCOLORSTATIC:
            SetTextColor(reinterpret_cast<HDC>(wp), PcUi::Rgb(PcUi::DefaultTheme().text));
            SetBkMode(reinterpret_cast<HDC>(wp), TRANSPARENT);
            return reinterpret_cast<LRESULT>(bgBrush_);
        case WM_CTLCOLORBTN:
            SetBkMode(reinterpret_cast<HDC>(wp), TRANSPARENT);
            return reinterpret_cast<LRESULT>(cardBrush_);
        case WM_ERASEBKGND:
            PaintBackground(reinterpret_cast<HDC>(wp));
            return 1;
        case WM_SIZE:
            LayoutControls();
            InvalidateRect(hwnd_, nullptr, TRUE);
            return 0;
        case WM_DESTROY:
            hwnd_ = nullptr;
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

void PcMappingToolbarPanel::PaintBackground(HDC dc) {
    if (!hwnd_ || !dc) return;
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    FillRect(dc, &rc, bgBrush_);
}

void PcMappingToolbarPanel::CreateControls() {
    // 文案尽量使用“动作 + 对象”，降低用户理解成本。
    toggle_ = CreateButton(L"PC 映射", IDC_MAP_TOGGLE);

    btnKey_ = CreateButton(L"新建按键", IDC_MAP_KEY);
    btnCompass_ = CreateButton(L"WASD 摇杆", IDC_MAP_COMPASS);
    btnLock_ = CreateButton(L"视角锁定", IDC_MAP_LOCK);
    btnMenuRadial_ = CreateButton(L"轮盘操作", IDC_MAP_MENU_RADIAL);
    btnMenuItem_ = CreateButton(L"道具操作", IDC_MAP_MENU_ITEM);
    btnMenuBag_ = CreateButton(L"背包操作", IDC_MAP_MENU_BAG);
    btnMacro_ = CreateButton(L"新建宏", IDC_MAP_MACRO);

    btnProfile_ = CreateButton(L"映射方案", IDC_MAP_PROFILE);
    btnHideUi_ = CreateButton(L"隐藏 UI", IDC_MAP_HIDE_UI);

    btnLoad1_ = CreateButton(L"读取 1", IDC_MAP_LOAD1);
    btnSave1_ = CreateButton(L"保存 1", IDC_MAP_SAVE1);
    btnLoad2_ = CreateButton(L"读取 2", IDC_MAP_LOAD2);
    btnSave2_ = CreateButton(L"保存 2", IDC_MAP_SAVE2);
    btnLoad3_ = CreateButton(L"读取 3", IDC_MAP_LOAD3);
    btnSave3_ = CreateButton(L"保存 3", IDC_MAP_SAVE3);

    count_ = CreateStatic(L"当前绑定：0 个", IDC_MAP_COUNT);
    status_ = CreateStatic(L"状态：就绪", IDC_MAP_STATUS);
}

HWND PcMappingToolbarPanel::CreateButton(const wchar_t* text, int id) {
    HWND h = CreateWindowW(
        L"BUTTON", text,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPSIBLINGS | BS_PUSHBUTTON | BS_FLAT,
        0, 0, 1, 1,
        hwnd_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        inst_,
        nullptr
    );
    if (h && font_) {
        SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(id == IDC_MAP_TOGGLE && titleFont_ ? titleFont_ : font_), TRUE);
    }
    return h;
}

HWND PcMappingToolbarPanel::CreateStatic(const wchar_t* text, int id) {
    DWORD style = WS_CHILD | WS_VISIBLE | SS_CENTER;
    if (id == IDC_MAP_STATUS) {
        // 诊断日志是长文本/多字段，左对齐并允许自动换行，避免底部状态被截断。
        style = WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX;
    }

    HWND h = CreateWindowW(
        L"STATIC", text,
        style,
        0, 0, 1, 1,
        hwnd_,
        id >= 0 ? reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)) : nullptr,
        inst_,
        nullptr
    );
    if (h && font_) SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    return h;
}

void PcMappingToolbarPanel::LayoutControls() {
    if (!hwnd_) return;

    RECT rc{};
    GetClientRect(hwnd_, &rc);

    const int pad = 12;
    const int gap = 8;
    const int btnH = 34;
    const int smallH = 28;
    const int clientW = static_cast<int>(rc.right - rc.left);
    const int clientH = static_cast<int>(rc.bottom - rc.top);
    const int fullW = (std::max)(1, clientW - pad * 2);
    const int halfW = (std::max)(1, (fullW - gap) / 2);
    const int rightX = pad + halfW + gap;

    int y = 10;
    SetWindowPos(toggle_, nullptr, pad, y, fullW, 34, SWP_NOZORDER);

    if (!expanded_) {
        ShowExpandedControls(false);
        return;
    }

    ShowExpandedControls(true);

    // 顶部先给用户一个“当前处于什么状态”的反馈，而不是把状态塞到底部。
    y += 42;
    SetWindowPos(count_, nullptr, pad, y, fullW, 22, SWP_NOZORDER);

    // 创建类操作：两列排列，减少纵向滚动感。
    y += 32;
    SetWindowPos(btnKey_, nullptr, pad, y, halfW, btnH, SWP_NOZORDER);
    SetWindowPos(btnCompass_, nullptr, rightX, y, fullW - halfW - gap, btnH, SWP_NOZORDER);

    y += btnH + gap;
    SetWindowPos(btnLock_, nullptr, pad, y, halfW, btnH, SWP_NOZORDER);
    SetWindowPos(btnMacro_, nullptr, rightX, y, fullW - halfW - gap, btnH, SWP_NOZORDER);

    y += btnH + gap;
    SetWindowPos(btnMenuRadial_, nullptr, pad, y, halfW, btnH, SWP_NOZORDER);
    SetWindowPos(btnMenuItem_, nullptr, rightX, y, fullW - halfW - gap, btnH, SWP_NOZORDER);

    y += btnH + gap;
    SetWindowPos(btnMenuBag_, nullptr, pad, y, fullW, btnH, SWP_NOZORDER);

    // 管理类操作：和创建类操作拉开间距。
    y += btnH + 14;
    SetWindowPos(btnProfile_, nullptr, pad, y, halfW, btnH, SWP_NOZORDER);
    SetWindowPos(btnHideUi_, nullptr, rightX, y, fullW - halfW - gap, btnH, SWP_NOZORDER);

    // 方案槽位：读取/保存成对出现，用户更容易建立对应关系。
    y += btnH + 14;
    SetWindowPos(btnLoad1_, nullptr, pad, y, halfW, smallH, SWP_NOZORDER);
    SetWindowPos(btnSave1_, nullptr, rightX, y, fullW - halfW - gap, smallH, SWP_NOZORDER);

    y += smallH + gap;
    SetWindowPos(btnLoad2_, nullptr, pad, y, halfW, smallH, SWP_NOZORDER);
    SetWindowPos(btnSave2_, nullptr, rightX, y, fullW - halfW - gap, smallH, SWP_NOZORDER);

    y += smallH + gap;
    SetWindowPos(btnLoad3_, nullptr, pad, y, halfW, smallH, SWP_NOZORDER);
    SetWindowPos(btnSave3_, nullptr, rightX, y, fullW - halfW - gap, smallH, SWP_NOZORDER);

    // 状态区始终贴底，给长诊断文本留空间。
    y += smallH + 14;
    const int statusH = (std::max)(kToolbarStatusMinHeight, clientH - y - pad);
    SetWindowPos(status_, nullptr, pad, y, fullW, statusH, SWP_NOZORDER);
}

void PcMappingToolbarPanel::RefreshText() {
    if (!hwnd_) return;

    if (toggle_) {
        SetWindowTextW(toggle_, expanded_ ? L"收起编辑面板" : L"PC 映射");
    }

    if (btnHideUi_) {
        const bool hidden = callbacks_.isMappingUiHidden ? callbacks_.isMappingUiHidden() : false;
        SetWindowTextW(btnHideUi_, hidden ? L"显示 UI" : L"隐藏 UI");
    }

    const int n = callbacks_.mappingCount ? callbacks_.mappingCount() : 0;
    std::wstring c = L"当前绑定：";
    c += std::to_wstring(n);
    c += L" 个";
    if (count_) SetWindowTextW(count_, c.c_str());

    std::wstring s = callbacks_.statusText ? callbacks_.statusText() : L"就绪";
    if (s.empty()) s = expanded_ ? L"编辑模式：选择一个工具后，在画面上放置映射点。" : L"运行模式：UI 透明，可触发映射。";

    if (!expanded_) {
        s = L"运行模式：UI 透明，可触发映射。";
    } else if (s == L"就绪") {
        s = L"状态：就绪。选择上方工具后，在画面上放置映射点。";
    } else if (s.rfind(L"状态：", 0) != 0) {
        s = L"状态：" + s;
    }

    if (status_) SetWindowTextW(status_, s.c_str());
}
