#include "pc_menu_runtime.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <utility>

namespace {
static int clampi(int v, int lo, int hi) { return (std::max)(lo, (std::min)(hi, v)); }
static double clampd(double v, double lo, double hi) { return (std::max)(lo, (std::min)(hi, v)); }
static double clampSpeed(double v) { return (std::max)(0.4, (std::min)(1.2, v)); }
static int clampMenuButtonRadiusNorm(int v) { return PcMappingProfile::ClampButtonRadiusNorm(v); }
static constexpr long long BAG_HOLD_TRIGGER_MS = 50;

static bool validRange(int l, int t, int r, int b) {
    return l >= 0 && t >= 0 && r >= 0 && b >= 0 && r > l && b > t;
}

static void resolveMenuRange(const PcMenuBinding& m, bool radialMenu, int& l, int& t, int& r, int& b) {
    l = m.rangeLeftNorm;
    t = m.rangeTopNorm;
    r = m.rangeRightNorm;
    b = m.rangeBottomNorm;

    if (!validRange(l, t, r, b)) {
        if (radialMenu) {
            const int cx = PcMappingProfile::ClampNorm(m.centerXNorm);
            const int cy = PcMappingProfile::ClampNorm(m.centerYNorm);
            const int radius = clampMenuButtonRadiusNorm(m.radiusNorm);
            l = PcMappingProfile::ClampNorm(cx - radius);
            t = PcMappingProfile::ClampNorm(cy - radius);
            r = PcMappingProfile::ClampNorm(cx + radius);
            b = PcMappingProfile::ClampNorm(cy + radius);
            if (r <= l) r = PcMappingProfile::ClampNorm(l + 1);
            if (b <= t) b = PcMappingProfile::ClampNorm(t + 1);
        } else {
            l = 0; t = 0; r = 1000000; b = 1000000;
        }
    } else {
        l = PcMappingProfile::ClampNorm(l);
        t = PcMappingProfile::ClampNorm(t);
        r = PcMappingProfile::ClampNorm(r);
        b = PcMappingProfile::ClampNorm(b);
        if (r <= l) r = PcMappingProfile::ClampNorm(l + 1);
        if (b <= t) b = PcMappingProfile::ClampNorm(t + 1);
    }

    if (radialMenu) {
        const int cx = PcMappingProfile::ClampNorm(m.centerXNorm);
        const int cy = PcMappingProfile::ClampNorm(m.centerYNorm);
        l = (std::min)(l, cx);
        r = (std::max)(r, cx);
        t = (std::min)(t, cy);
        b = (std::max)(b, cy);
    }
}
}

void PcMenuRuntime::SetCallbacks(Callbacks cb) { callbacks_ = std::move(cb); }

void PcMenuRuntime::SetProfileProvider(std::function<const PcMappingProfile&()> provider) {
    profileProvider_ = std::move(provider);
}

long long PcMenuRuntime::NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

void PcMenuRuntime::Reset() {
    EndMenu(true);
    vkDown_.fill(false);
    suppressReopenId_.clear();
    leftDown_ = false;
    triggerWasDown_ = false;
    pendingRawDx_ = 0;
    pendingRawDy_ = 0;
    pendingRawActive_ = false;
    debugStats_ = DebugStats{};
}

std::wstring PcMenuRuntime::StatusText() const {
    if (!profileProvider_ || profileProvider_().menus.empty()) return L"菜单=未创建";
    if (!active_) return L"菜单=就绪";
    if (stage_ == Stage::TriggerHeld) return activeCategory_ == PcMenuCategory::MenuBagOperation ? L"背包=按住触发" : L"菜单=触发";
    if (stage_ == Stage::SecondaryFree) {
        if (activeCategory_ == PcMenuCategory::MenuBagOperation) return L"背包=松开隐藏";
        if (activeCategory_ == PcMenuCategory::MenuItemOperation || activeCategory_ == PcMenuCategory::MenuHorizontal || activeCategory_ == PcMenuCategory::MenuVertical) return L"道具=右键隐藏";
        return L"菜单=自由鼠标";
    }
    return L"菜单=轮盘";
}

UINT PcMenuRuntime::NormalizeVk(UINT vk, LPARAM lp) {
    const UINT scanCode = static_cast<UINT>((lp >> 16) & 0xFFu);
    const bool extended = (lp & 0x01000000L) != 0;
    if (vk == VK_SHIFT) {
        UINT mapped = MapVirtualKeyW(scanCode, MAPVK_VSC_TO_VK_EX);
        if (mapped == VK_LSHIFT || mapped == VK_RSHIFT) return mapped;
        return VK_LSHIFT;
    }
    if (vk == VK_CONTROL) return extended ? VK_RCONTROL : VK_LCONTROL;
    if (vk == VK_MENU) return extended ? VK_RMENU : VK_LMENU;
    return vk;
}

int PcMenuRuntime::ClampNorm(int v) { return PcMappingProfile::ClampNorm(v); }
int PcMenuRuntime::ClampRadiusNorm(int v) { return PcMappingProfile::ClampRadiusNorm(v); }
int PcMenuRuntime::RandomAnchorRadiusNorm(const PcMenuBinding& m) {
    // 道具/背包三个圆形 UI 的运行时随机范围要和画面 UI 一致：20~60，默认 30。
    return clampMenuButtonRadiusNorm(m.radiusNorm);
}

bool PcMenuRuntime::ComboSatisfied(const PcMenuBinding& m, const std::array<bool, 256>& vkDown) {
    if (!m.enabled || m.comboTriggerCodes.empty()) return false;
    for (int code : m.comboTriggerCodes) {
        if (code <= 0 || code >= static_cast<int>(vkDown.size())) return false;
        if (code == VK_SHIFT) {
            if (!vkDown[VK_SHIFT] && !vkDown[VK_LSHIFT] && !vkDown[VK_RSHIFT]) return false;
            continue;
        }
        if (code == VK_CONTROL) {
            if (!vkDown[VK_CONTROL] && !vkDown[VK_LCONTROL] && !vkDown[VK_RCONTROL]) return false;
            continue;
        }
        if (code == VK_MENU) {
            if (!vkDown[VK_MENU] && !vkDown[VK_LMENU] && !vkDown[VK_RMENU]) return false;
            continue;
        }
        if (!vkDown[static_cast<size_t>(code)]) return false;
    }
    return true;
}

bool PcMenuRuntime::IsRelatedKey(const PcMenuBinding& m, int vk) {
    for (int c : m.comboTriggerCodes) {
        if (c == vk) return true;
        if ((c == VK_SHIFT || c == VK_LSHIFT || c == VK_RSHIFT) && (vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT)) return true;
        if ((c == VK_CONTROL || c == VK_LCONTROL || c == VK_RCONTROL) && (vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL)) return true;
        if ((c == VK_MENU || c == VK_LMENU || c == VK_RMENU) && (vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU)) return true;
    }
    return false;
}

bool PcMenuRuntime::IsFreeCursorMenu(const PcMenuBinding& m) const {
    return m.category == PcMenuCategory::MenuItemOperation ||
           m.category == PcMenuCategory::MenuBagOperation ||
           m.category == PcMenuCategory::MenuHorizontal ||
           m.category == PcMenuCategory::MenuVertical ||
           m.triggerMode == PcMenuTriggerMode::HoldFreeCursor;
}

bool PcMenuRuntime::IsRadialMenu(const PcMenuBinding& m) const {
    return !IsFreeCursorMenu(m);
}

bool PcMenuRuntime::IsBagMenu(const PcMenuBinding& m) const {
    return m.category == PcMenuCategory::MenuBagOperation;
}

const PcMenuBinding* PcMenuRuntime::FindActiveMenu() const {
    if (!profileProvider_ || !active_) return nullptr;
    const auto& p = profileProvider_();
    for (const auto& m : p.menus) {
        if (m.id == activeId_) return &m;
    }
    return nullptr;
}

const PcMenuBinding* PcMenuRuntime::FindPressedMenu() const {
    if (!profileProvider_) return nullptr;
    const auto& p = profileProvider_();
    for (const auto& m : p.menus) {
        if (!m.enabled) continue;
        if (ComboSatisfied(m, vkDown_)) return &m;
    }
    return nullptr;
}

void PcMenuRuntime::ClampToRadial(int& x, int& y) const {
    const int ox = x - centerXNorm_;
    const int oy = y - centerYNorm_;
    const double len = std::sqrt(static_cast<double>(ox) * ox + static_cast<double>(oy) * oy);
    const double maxR = static_cast<double>(radiusNorm_);
    if (len > maxR && len > 0.0001) {
        const double s = maxR / len;
        x = centerXNorm_ + static_cast<int>(std::lround(ox * s));
        y = centerYNorm_ + static_cast<int>(std::lround(oy * s));
    }
}

void PcMenuRuntime::BeginMenu(const PcMenuBinding& m) {
    EndMenu(false);
    pendingRawDx_ = 0;
    pendingRawDy_ = 0;
    pendingRawActive_ = false;

    active_ = true;
    activeId_ = m.id;
    activeCategory_ = m.category;
    activeSlot_ = clampi(m.slot, 1, 15);
    centerXNorm_ = ClampNorm(m.centerXNorm);
    centerYNorm_ = ClampNorm(m.centerYNorm);
    triggerXNorm_ = ClampNorm(m.triggerXNorm);
    triggerYNorm_ = ClampNorm(m.triggerYNorm);
    closeXNorm_ = ClampNorm(m.closeXNorm);
    closeYNorm_ = ClampNorm(m.closeYNorm);
    radiusNorm_ = clampMenuButtonRadiusNorm(m.radiusNorm);
    resolveMenuRange(m, IsRadialMenu(m), rangeLeftNorm_, rangeTopNorm_, rangeRightNorm_, rangeBottomNorm_);
    speedX_ = clampSpeed(m.relativeSpeedX);
    speedY_ = clampSpeed(m.relativeSpeedY);
    cursorXNormF_ = static_cast<double>(centerXNorm_);
    cursorYNormF_ = static_cast<double>(centerYNorm_);
    if (IsRadialMenu(m)) ClampToRadialVisible(cursorXNormF_, cursorYNormF_);
    ClampToMenuRange(cursorXNormF_, cursorYNormF_);
    CommitCursor(cursorXNormF_, cursorYNormF_);
    touchActive_ = false;
    leftDown_ = false;
    triggerWasDown_ = true;
    triggerDownMs_ = NowMs();

    menuBeginNotified_ = false;

    if (IsRadialMenu(m)) {
        if (callbacks_.menuBegin) {
            callbacks_.menuBegin(activeCategory_);
            menuBeginNotified_ = true;
        }
        stage_ = Stage::RadialActive;
        if (callbacks_.touchDown) {
            touchActive_ = callbacks_.touchDown(activeSlot_, cursorXNorm_, cursorYNorm_);
        }
    } else if (IsBagMenu(m)) {
        // 背包操作：必须长按 50ms 后才真正触发。短按释放只取消，不点击触发 UI，
        // 也不切换全局视角锁定状态。
        stage_ = Stage::TriggerHeld;
    } else {
        // 道具操作：触发键按下后立刻在触发键圆形 UI 范围内做一次临时 slot 点击，
        // 然后进入全局自由模式并把鼠标落到“鼠标落点”圆形 UI。
        TapTriggerPoint(m);
        if (callbacks_.menuBegin) {
            callbacks_.menuBegin(activeCategory_);
            menuBeginNotified_ = true;
        }
        EnterSecondaryFree(m);
    }
}

void PcMenuRuntime::TapTriggerPoint(const PcMenuBinding& m) {
    int triggerX = ClampNorm(m.triggerXNorm);
    int triggerY = ClampNorm(m.triggerYNorm);
    PickRandomPointInAnchor(triggerX, triggerY, RandomAnchorRadiusNorm(m), triggerX, triggerY);
    if (callbacks_.tapTransient) {
        callbacks_.tapTransient(triggerX, triggerY, activeSlot_);
        return;
    }
    if (callbacks_.touchDown) {
        const bool down = callbacks_.touchDown(activeSlot_, triggerX, triggerY);
        if (down && callbacks_.touchUp) callbacks_.touchUp(activeSlot_);
    }
}

void PcMenuRuntime::EnterSecondaryFree(const PcMenuBinding& m) {
    if (!active_) return;
    if (touchActive_ && callbacks_.touchUp) callbacks_.touchUp(activeSlot_);
    touchActive_ = false;
    stage_ = Stage::SecondaryFree;
    int landingX = ClampNorm(m.freeCursorXNorm);
    int landingY = ClampNorm(m.freeCursorYNorm);
    PickRandomPointInAnchor(landingX, landingY, RandomAnchorRadiusNorm(m), landingX, landingY);
    cursorXNormF_ = static_cast<double>(landingX);
    cursorYNormF_ = static_cast<double>(landingY);
    ClampToMenuRange(cursorXNormF_, cursorYNormF_);
    CommitCursor(cursorXNormF_, cursorYNormF_);
    if (callbacks_.freeCursorMove) callbacks_.freeCursorMove(cursorXNorm_, cursorYNorm_);
    leftDown_ = false;
}

void PcMenuRuntime::TapClosePoint(const PcMenuBinding& m) {
    if (touchActive_ && callbacks_.touchUp) callbacks_.touchUp(activeSlot_);
    touchActive_ = false;
    leftDown_ = false;

    int closeX = ClampNorm(m.closeXNorm);
    int closeY = ClampNorm(m.closeYNorm);
    PickRandomPointInAnchor(closeX, closeY, RandomAnchorRadiusNorm(m), closeX, closeY);
    if (callbacks_.tapTransient) {
        callbacks_.tapTransient(closeX, closeY, activeSlot_);
        return;
    }
    if (callbacks_.touchDown) {
        const bool down = callbacks_.touchDown(activeSlot_, closeX, closeY);
        if (down && callbacks_.touchUp) callbacks_.touchUp(activeSlot_);
    }
}

void PcMenuRuntime::EndMenu(bool notifyEnd) {
    FlushPendingRawDelta(true);
    const bool wasActive = active_;
    const PcMenuCategory oldCategory = activeCategory_;

    if (touchActive_ && callbacks_.touchUp) callbacks_.touchUp(activeSlot_);
    const bool shouldNotifyEnd = wasActive && menuBeginNotified_;

    active_ = false;
    activeId_.clear();
    stage_ = Stage::None;
    touchActive_ = false;
    leftDown_ = false;
    triggerWasDown_ = false;
    menuBeginNotified_ = false;
    triggerDownMs_ = 0;

    if (shouldNotifyEnd && notifyEnd && callbacks_.menuEnd) callbacks_.menuEnd(oldCategory);
}

void PcMenuRuntime::ApplyRelative(int dx, int dy) {
    if (!active_) return;
    const PcMenuBinding* m = FindActiveMenu();
    if (!m) { EndMenu(); return; }

    if (IsFreeCursorMenu(*m)) {
        if (stage_ != Stage::SecondaryFree) return;
        const double nxDelta = static_cast<double>(dx) * 1000000.0 / static_cast<double>((std::max)(1, clientWidth_ - 1));
        const double nyDelta = static_cast<double>(dy) * 1000000.0 / static_cast<double>((std::max)(1, clientHeight_ - 1));
        double nx = cursorXNormF_ + nxDelta;
        double ny = cursorYNormF_ + nyDelta;
        ClampToMenuRange(nx, ny);
        CommitCursor(nx, ny);
        if (callbacks_.freeCursorMove) callbacks_.freeCursorMove(cursorXNorm_, cursorYNorm_);
        if (leftDown_ && touchActive_ && callbacks_.touchMove) callbacks_.touchMove(activeSlot_, cursorXNorm_, cursorYNorm_);
        return;
    }

    if (stage_ != Stage::RadialActive) return;
    double nx = cursorXNormF_ + RawDeltaToNormX(*m, dx);
    double ny = cursorYNormF_ + RawDeltaToNormY(*m, dy);
    // 先做矩形范围，再按“实际屏幕圆形”投影，最后再迭代一次矩形+圆形，避免矩形 clamp 把点推到圆外。
    for (int i = 0; i < 3; ++i) {
        ClampToMenuRange(nx, ny);
        ClampToRadialVisible(nx, ny);
    }
    CommitCursor(nx, ny);
    if (touchActive_ && callbacks_.touchMove) callbacks_.touchMove(activeSlot_, cursorXNorm_, cursorYNorm_);
}

void PcMenuRuntime::Tick() {
    FlushPendingRawDelta(false);

    if (!active_ && !suppressReopenId_.empty()) {
        bool stillSuppressed = false;
        if (profileProvider_) {
            for (const auto& m : profileProvider_().menus) {
                if (m.id == suppressReopenId_ && ComboSatisfied(m, vkDown_)) {
                    stillSuppressed = true;
                    break;
                }
            }
        }
        if (stillSuppressed) return;
        suppressReopenId_.clear();
    }

    const PcMenuBinding* pressed = FindPressedMenu();
    if (!active_) {
        if (pressed) BeginMenu(*pressed);
        return;
    }

    const PcMenuBinding* m = FindActiveMenu();
    if (!m || !m->enabled) { EndMenu(); return; }
    const bool stillPressed = ComboSatisfied(*m, vkDown_);

    if (IsRadialMenu(*m)) {
        if (!stillPressed) EndMenu();
        return;
    }

    if (stage_ == Stage::TriggerHeld) {
        if (!stillPressed) {
            // 背包长按不足 50ms 就释放：取消，不点击触发 UI，不切模式。
            EndMenu(false);
            return;
        }

        if (IsBagMenu(*m)) {
            const long long heldMs = NowMs() - triggerDownMs_;
            if (heldMs < BAG_HOLD_TRIGGER_MS) {
                triggerWasDown_ = true;
                return;
            }
            TapTriggerPoint(*m);
            if (callbacks_.menuBegin) {
                callbacks_.menuBegin(activeCategory_);
                menuBeginNotified_ = true;
            }
            EnterSecondaryFree(*m);
            triggerWasDown_ = true;
            return;
        }

        // 兼容其它 HoldFreeCursor 菜单。
        if (callbacks_.menuBegin && !menuBeginNotified_) {
            callbacks_.menuBegin(activeCategory_);
            menuBeginNotified_ = true;
        }
        EnterSecondaryFree(*m);
        triggerWasDown_ = stillPressed;
        return;
    }

    if (stage_ == Stage::SecondaryFree) {
        if (IsBagMenu(*m) && !stillPressed) {
            // 背包操作：松开触发键就是关闭。关闭时同样用临时 slot 点击关闭 UI，
            // 点击完成后 menuEnd 统一进入全局视角锁定模式。
            TapClosePoint(*m);
            suppressReopenId_ = activeId_;
            EndMenu();
            return;
        }
        triggerWasDown_ = stillPressed;
    }
}

void PcMenuRuntime::UpdateClientSize(HWND hwnd) {
    RECT rc{};
    if (IsWindow(hwnd) && GetClientRect(hwnd, &rc)) {
        clientWidth_ = (std::max)(1L, rc.right - rc.left);
        clientHeight_ = (std::max)(1L, rc.bottom - rc.top);
    }
}

double PcMenuRuntime::RawDeltaToNormX(const PcMenuBinding& m, int dx) const {
    const int w = (std::max)(1, clientWidth_);
    return static_cast<double>(dx) * clampSpeed(m.relativeSpeedX) * 1000000.0 / static_cast<double>((std::max)(1, w - 1));
}

double PcMenuRuntime::RawDeltaToNormY(const PcMenuBinding& m, int dy) const {
    const int h = (std::max)(1, clientHeight_);
    return static_cast<double>(dy) * clampSpeed(m.relativeSpeedY) * 1000000.0 / static_cast<double>((std::max)(1, h - 1));
}

void PcMenuRuntime::ClampToMenuRange(double& x, double& y) const {
    x = clampd(x, static_cast<double>(rangeLeftNorm_), static_cast<double>(rangeRightNorm_));
    y = clampd(y, static_cast<double>(rangeTopNorm_), static_cast<double>(rangeBottomNorm_));
}

void PcMenuRuntime::ClampToRadialVisible(double& x, double& y) const {
    const double w = static_cast<double>((std::max)(1, clientWidth_));
    const double h = static_cast<double>((std::max)(1, clientHeight_));
    const double base = (std::max)(1.0, (std::min)(w, h));
    const double rx = (std::max)(1.0, static_cast<double>(radiusNorm_) * base / w);
    const double ry = (std::max)(1.0, static_cast<double>(radiusNorm_) * base / h);
    const double ox = x - static_cast<double>(centerXNorm_);
    const double oy = y - static_cast<double>(centerYNorm_);
    const double v = (ox * ox) / (rx * rx) + (oy * oy) / (ry * ry);
    if (v > 1.0) {
        const double scale = 1.0 / std::sqrt(v);
        x = static_cast<double>(centerXNorm_) + ox * scale;
        y = static_cast<double>(centerYNorm_) + oy * scale;
    }
}


void PcMenuRuntime::PickRandomPointInAnchor(int centerXNorm, int centerYNorm, int radiusNorm, int& outXNorm, int& outYNorm) const {
    // UI 上的道具/背包圆点半径是按 min(width,height) 换算出的“屏幕像素圆”。
    // 旧写法直接把同一个 norm 半径加到 X/Y，会在宽屏窗口里变成椭圆随机区，
    // 触发点/落点/关闭点的真实点击会和屏幕上的圆形 UI 发生横向或纵向偏移。
    const double w = static_cast<double>((std::max)(1, clientWidth_));
    const double h = static_cast<double>((std::max)(1, clientHeight_));
    const double base = (std::max)(1.0, (std::min)(w, h));
    const double radiusPx = static_cast<double>((std::max)(1, radiusNorm)) * base / 1000000.0;

    const double a = (static_cast<double>(std::rand()) / static_cast<double>(RAND_MAX)) * 6.28318530717958647692;
    const double u = static_cast<double>(std::rand()) / static_cast<double>(RAND_MAX);
    const double distPx = std::sqrt(u) * radiusPx;

    const double dxNorm = std::cos(a) * distPx * 1000000.0 / (std::max)(1.0, w - 1.0);
    const double dyNorm = std::sin(a) * distPx * 1000000.0 / (std::max)(1.0, h - 1.0);

    double x = static_cast<double>(centerXNorm) + dxNorm;
    double y = static_cast<double>(centerYNorm) + dyNorm;
    ClampToMenuRange(x, y);
    outXNorm = ClampNorm(static_cast<int>(std::lround(x)));
    outYNorm = ClampNorm(static_cast<int>(std::lround(y)));
}

void PcMenuRuntime::CommitCursor(double x, double y) {
    cursorXNormF_ = clampd(x, 0.0, 1000000.0);
    cursorYNormF_ = clampd(y, 0.0, 1000000.0);
    cursorXNorm_ = ClampNorm(static_cast<int>(std::lround(cursorXNormF_)));
    cursorYNorm_ = ClampNorm(static_cast<int>(std::lround(cursorYNormF_)));
}


PcMenuRuntime::DebugStats PcMenuRuntime::SnapshotDebugStats() const {
    DebugStats st = debugStats_;
    st.active = active_;
    st.secondaryFree = active_ && stage_ == Stage::SecondaryFree;
    st.cursorXNorm = cursorXNorm_;
    st.cursorYNorm = cursorYNorm_;
    return st;
}

PcMenuRuntime::DebugStats PcMenuRuntime::SnapshotAndResetDebugStats() {
    DebugStats st = SnapshotDebugStats();
    debugStats_ = DebugStats{};
    return st;
}

bool PcMenuRuntime::ReadRawMouseDelta(HWND hwnd, LPARAM lp, int& dx, int& dy) {
    dx = 0;
    dy = 0;
    if (!active_) {
        ++debugStats_.rawInputIgnoredNotReady;
        return false;
    }

    UpdateClientSize(hwnd);
    UINT size = 0;
    GetRawInputData(reinterpret_cast<HRAWINPUT>(lp), RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER));
    if (size == 0 || size > 4096) {
        ++debugStats_.rawInputBytesRejected;
        return false;
    }

    BYTE buffer[4096];
    if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lp), RID_INPUT, buffer, &size, sizeof(RAWINPUTHEADER)) != size) {
        ++debugStats_.rawInputBytesRejected;
        return false;
    }

    RAWINPUT* raw = reinterpret_cast<RAWINPUT*>(buffer);
    if (raw->header.dwType != RIM_TYPEMOUSE) return false;

    dx = raw->data.mouse.lLastX;
    dy = raw->data.mouse.lLastY;
    ++debugStats_.rawInputMouse;
    debugStats_.rawDx += dx;
    debugStats_.rawDy += dy;
    return true;
}

void PcMenuRuntime::QueueRawDelta(int dx, int dy) {
    if (dx == 0 && dy == 0) return;

    const auto now = std::chrono::steady_clock::now();
    if (!pendingRawActive_) {
        pendingRawActive_ = true;
        pendingRawFirstAt_ = now;
    }
    pendingRawLastAt_ = now;
    pendingRawDx_ += dx;
    pendingRawDy_ += dy;

    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - pendingRawFirstAt_).count();
    if (elapsedMs >= kRawDeltaCoalesceMs) {
        ApplyPendingRawDelta(false);
    }
}

void PcMenuRuntime::ApplyPendingRawDelta(bool forced) {
    if (!pendingRawActive_) return;

    const int dx = pendingRawDx_;
    const int dy = pendingRawDy_;
    pendingRawDx_ = 0;
    pendingRawDy_ = 0;
    pendingRawActive_ = false;

    if (dx == 0 && dy == 0) return;
    if (!active_) return;

    ++debugStats_.applyCalls;
    ++debugStats_.coalesceFlushes;
    if (forced) ++debugStats_.coalesceForcedFlushes;
    ApplyRelative(dx, dy);
}

void PcMenuRuntime::FlushPendingRawDelta(bool force) {
    if (!pendingRawActive_) return;

    if (force) {
        ApplyPendingRawDelta(true);
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - pendingRawFirstAt_).count();
    if (elapsedMs >= kRawDeltaCoalesceMs) {
        ApplyPendingRawDelta(false);
    }
}

bool PcMenuRuntime::ConsumeActiveRawMouse(HWND hwnd, LPARAM lp) {
    int dx = 0;
    int dy = 0;
    if (!ReadRawMouseDelta(hwnd, lp, dx, dy)) return false;
    QueueRawDelta(dx, dy);
    return true;
}

void PcMenuRuntime::AddRawMouseDelta(HWND hwnd, LPARAM lp) {
    ConsumeActiveRawMouse(hwnd, lp);
}

bool PcMenuRuntime::HandleMouseButton(UINT msg, WPARAM, LPARAM) {
    if (!active_) return false;
    const PcMenuBinding* m = FindActiveMenu();
    if (!m) { EndMenu(); return false; }

    if (IsRadialMenu(*m)) {
        // 轮盘菜单运行期间鼠标按钮不再传给视角/普通点击，避免误触。
        return true;
    }

    if (stage_ != Stage::SecondaryFree) {
        return true;
    }

    switch (msg) {
        case WM_LBUTTONDOWN:
            leftDown_ = true;
            if (!touchActive_ && callbacks_.touchDown) touchActive_ = callbacks_.touchDown(activeSlot_, cursorXNorm_, cursorYNorm_);
            return true;
        case WM_LBUTTONUP:
            leftDown_ = false;
            if (touchActive_ && callbacks_.touchUp) callbacks_.touchUp(activeSlot_);
            touchActive_ = false;
            return true;
        case WM_RBUTTONDOWN:
            TapClosePoint(*m);
            suppressReopenId_ = activeId_;
            EndMenu();
            return true;
        case WM_RBUTTONUP:
            return true;
        default:
            return true;
    }
}

bool PcMenuRuntime::HandleWindowMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LRESULT* outResult) {
    if (outResult) *outResult = 0;
    UpdateClientSize(hwnd);
    switch (msg) {
        case WM_INPUT:
            if (active_) { AddRawMouseDelta(hwnd, lp); return true; }
            return false;
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN: {
            const UINT vk = NormalizeVk(static_cast<UINT>(wp), lp);
            if (vk < vkDown_.size()) vkDown_[vk] = true;

            if (active_) {
                // 菜单 active 时不能吞掉所有键盘消息。
                // 道具/背包/轮盘菜单只应该独占自己的触发键；W/A/S/D 等摇杆键必须继续传给
                // PcCompassRuntime，否则菜单显示鼠标后 WASD 的 WM_KEYUP 会被这里吃掉，摇杆就不抬起。
                const PcMenuBinding* activeMenu = FindActiveMenu();
                return activeMenu ? IsRelatedKey(*activeMenu, static_cast<int>(vk)) : true;
            }

            if (profileProvider_) {
                for (const auto& m : profileProvider_().menus) {
                    if (IsRelatedKey(m, static_cast<int>(vk))) return true;
                }
            }
            return false;
        }
        case WM_KEYUP:
        case WM_SYSKEYUP: {
            const UINT vk = NormalizeVk(static_cast<UINT>(wp), lp);
            if (vk < vkDown_.size()) vkDown_[vk] = false;

            if (active_) {
                // 同 KEYDOWN：只消费当前菜单自己的触发键释放。
                // 其它键释放必须放行，保证 WASD 摇杆运行时能收到 KEYUP 并按真实按键状态释放触摸。
                const PcMenuBinding* activeMenu = FindActiveMenu();
                return activeMenu ? IsRelatedKey(*activeMenu, static_cast<int>(vk)) : true;
            }

            if (profileProvider_) {
                for (const auto& m : profileProvider_().menus) {
                    if (IsRelatedKey(m, static_cast<int>(vk))) return true;
                }
            }
            return false;
        }
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:
        case WM_XBUTTONDOWN:
        case WM_XBUTTONUP:
        case WM_MOUSEWHEEL:
            return HandleMouseButton(msg, wp, lp);
        case WM_KILLFOCUS:
        case WM_CANCELMODE:
            Reset();
            return false;
        default:
            return false;
    }
}
