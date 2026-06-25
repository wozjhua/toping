#include "pc_lock_runtime.h"
#include "pc_lock_options_dialog.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <windowsx.h>
#include <utility>

#ifndef PC_LOCK_ENABLE_STATS
#  if defined(_DEBUG)
#    define PC_LOCK_ENABLE_STATS 1
#  else
#    define PC_LOCK_ENABLE_STATS 0
#  endif
#endif

#if PC_LOCK_ENABLE_STATS
#  define LOCK_STAT(expr) do { expr; } while (0)
#else
#  define LOCK_STAT(expr) do {} while (0)
#endif

namespace {
    static UINT lockMouseVkFromMessage(UINT msg, WPARAM wp) {
        switch (msg) {
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
            return VK_LBUTTON;
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
            return VK_RBUTTON;
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:
            return VK_MBUTTON;
        case WM_XBUTTONDOWN:
        case WM_XBUTTONUP: {
            const WORD xbtn = GET_XBUTTON_WPARAM(wp);
            return (xbtn == XBUTTON2) ? VK_XBUTTON2 : VK_XBUTTON1;
        }
        default:
            return 0;
        }
    }

    static bool lockMouseMessageIsDown(UINT msg) {
        return msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN || msg == WM_XBUTTONDOWN;
    }

    static bool lockMouseMessageIsUp(UINT msg) {
        return msg == WM_LBUTTONUP || msg == WM_RBUTTONUP || msg == WM_MBUTTONUP || msg == WM_XBUTTONUP;
    }

    static int lockNormToClientCoord(int norm, int size) {
        const int s = (std::max)(1, size);
        const int n = (std::max)(0, (std::min)(1000000, norm));
        const int v = static_cast<int>((static_cast<long long>(n) * (s - 1) + 500000LL) / 1000000LL);
        return (std::max)(0, (std::min)(s - 1, v));
    }
} // namespace

void PcLockRuntime::SetCallbacks(Callbacks cb) {
    callbacks_ = std::move(cb);
}

void PcLockRuntime::SetProfileProvider(std::function<const PcMappingProfile& ()> provider) {
    profileProvider_ = std::move(provider);
    ClearPendingAuxDown();
    activeCfgValid_ = false;
}

void PcLockRuntime::Start(HWND hwnd) {
    hwnd_ = hwnd;
    RegisterRawMouse(true);
}

void PcLockRuntime::Stop() {
    SetLocked(false);
    RegisterRawMouse(false);
    hwnd_ = nullptr;
}

void PcLockRuntime::Reset() {
    vkDown_.fill(false);
    comboWasDown_ = false;
    menuInputSuspended_ = false;
    menuReleasedTouch_ = false;
    SetLocked(false);
    ClearPendingRebuild();
    ClearPendingAuxDown();
    ClearActiveLockConfig();
    debugStats_ = DebugStats{};
}

void PcLockRuntime::SetInputSuspendedByMenu(bool suspended) {
    // 旧版轮盘菜单逻辑：只暂停视角 RawInput，不释放当前锁定触点。
    menuInputSuspended_ = suspended && locked_;
}

void PcLockRuntime::PauseTouchForMenu() {
    SetInputSuspendedByMenu(true);
}

void PcLockRuntime::ResumeTouchAfterMenu() {
    SetInputSuspendedByMenu(false);
}

void PcLockRuntime::EnterMenuFreeCursorMode() {
    // 道具/背包不是“在锁定状态上临时暂停”，而是旧版语义里的模式切换：
    // 触发 UI 点完以后，当前视角三种模式的 slot 必须抬起，且全局视角锁定状态进入自由模式。
    // 同时清掉视角触发键的内部按下态；否则菜单激活期间 KEYUP 被菜单吞掉后，
    // comboWasDown_ 会停留在 true，用户再按视角触发键就无法从自由模式回到锁定模式。
    ClearKeyStateForMenuFreeMode();
    menuInputSuspended_ = false;
    menuReleasedTouch_ = false;
    SetLocked(false);
}

void PcLockRuntime::LeaveMenuFreeCursorMode() {
    // 关闭 UI 点完以后，明确回到视角锁定模式。
    // 不按“进入前是否锁定”恢复，否则道具/背包会把 locked_ 和真实 slot 状态拆开导致状态错乱。
    menuInputSuspended_ = false;
    menuReleasedTouch_ = false;
    SetLocked(true);
}

void PcLockRuntime::SetLockedFromMenu(bool locked) {
    menuInputSuspended_ = false;
    menuReleasedTouch_ = false;
    SetLocked(locked);
}

void PcLockRuntime::ClearKeyStateForMenuFreeMode() {
    vkDown_.fill(false);
    comboWasDown_ = false;
}

bool PcLockRuntime::HasLockConfig() const {
    if (!profileProvider_) return false;
    return !profileProvider_().locks.empty();
}

std::wstring PcLockRuntime::StatusText() const {
    if (!HasLockConfig()) return L"视角=未创建";
    std::wstring s = locked_ ? L"视角=锁定" : L"视角=自由";
    if (profileProvider_ && !profileProvider_().locks.empty()) {
        s += L" | ";
        s += ModeLabel(profileProvider_().locks.front().mode);
    }
    return s;
}

PcLockRuntime::DebugStats PcLockRuntime::SnapshotDebugStats() const {
    DebugStats s = debugStats_;
    s.locked = locked_;
    s.inputSuspended = menuInputSuspended_;
    s.mode = (profileProvider_ && !profileProvider_().locks.empty())
        ? static_cast<int>(profileProvider_().locks.front().mode)
        : 0;
    s.primarySlot = primary_.slot;
    s.primaryDown = primary_.down;
    s.primaryXNorm = primary_.xNorm;
    s.primaryYNorm = primary_.yNorm;
    s.auxSlot = aux_.slot;
    s.auxDown = aux_.down;
    s.auxXNorm = aux_.xNorm;
    s.auxYNorm = aux_.yNorm;
    return s;
}

PcLockRuntime::DebugStats PcLockRuntime::SnapshotAndResetDebugStats() {
    DebugStats s = SnapshotDebugStats();
    debugStats_ = DebugStats{};
    return s;
}

void PcLockRuntime::ReclickLockTouch(bool dismountStyle) {
    if (!locked_) return;
    if (!activeCfgValid_ && !LoadActiveLockConfig()) return;

    ClearPendingRebuild();
    EndTouchLocked();
    // “上车/下车”先做成 PC 端安全重按：释放当前视角触点后重新按下。
    // dismountStyle 预留给后续做更长冻结/延迟；当前不阻塞 UI 线程。
    (void)dismountStyle;
    BeginTouchLocked(activeCfg_);
}

UINT PcLockRuntime::NormalizeVk(UINT vk, LPARAM lp) {
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

int PcLockRuntime::ClampInt(int v, int lo, int hi) {
    return (std::max)(lo, (std::min)(hi, v));
}

bool PcLockRuntime::ComboSatisfied(const PcLockBinding& b, const std::array<bool, 256>& vkDown) {
    if (b.comboTriggerCodes.empty()) return false;
    for (int code : b.comboTriggerCodes) {
        if (code < 0 || code >= static_cast<int>(vkDown.size()) || !vkDown[static_cast<size_t>(code)]) return false;
    }
    return true;
}

std::wstring PcLockRuntime::ModeLabel(PcLockSlideTouchMode mode) {
    switch (mode) {
    case PcLockSlideTouchMode::SingleReanchor: return L"1 单slot";
    case PcLockSlideTouchMode::DualSimultaneous: return L"2 双slot同时";
    case PcLockSlideTouchMode::DualSequential: return L"3 双slot顺序";
    default: return L"视角";
    }
}

void PcLockRuntime::NotifyLockedChanged() {
    if (callbacks_.lockChanged) callbacks_.lockChanged(locked_);
}

void PcLockRuntime::RegisterRawMouse(bool enable) {
    if (!IsWindow(hwnd_)) return;
    RAWINPUTDEVICE rid{};
    rid.usUsagePage = 0x01;
    rid.usUsage = 0x02; // mouse
    rid.dwFlags = enable ? RIDEV_INPUTSINK : RIDEV_REMOVE;
    rid.hwndTarget = enable ? hwnd_ : nullptr;
    RegisterRawInputDevices(&rid, 1, sizeof(rid));
    rawRegistered_ = enable;
}

bool PcLockRuntime::NormToScreenPoint(int xNorm, int yNorm, POINT& out) const {
    if (!IsWindow(hwnd_)) return false;

    RECT rc{};
    if (!GetClientRect(hwnd_, &rc)) return false;
    const int w = (std::max)(1L, rc.right - rc.left);
    const int h = (std::max)(1L, rc.bottom - rc.top);

    POINT pt{
        lockNormToClientCoord(xNorm, w),
        lockNormToClientCoord(yNorm, h)
    };
    if (!ClientToScreen(hwnd_, &pt)) return false;

    out = pt;
    return true;
}

void PcLockRuntime::LockCursorToNormPoint(int xNorm, int yNorm) {
    POINT pt{};
    if (!NormToScreenPoint(xNorm, yNorm, pt)) return;

    lockedCursorScreenPoint_ = pt;
    lockedCursorPointValid_ = true;

    // 锁定视角时，RawInput 提供真实相对位移；系统光标本身不需要移动。
    // 把系统光标夹在 1x1 锚点内，避免隐藏光标跑到窗口边框/标题栏后触发拖拽或缩放。
    SetCursorPos(pt.x, pt.y);
    RECT clip{ pt.x, pt.y, pt.x + 1, pt.y + 1 };
    ClipCursor(&clip);

    if (!cursorHidden_) {
        while (ShowCursor(FALSE) >= 0) {}
        cursorHidden_ = true;
    }
}

void PcLockRuntime::RefreshLockedCursorAnchor() {
    if (!locked_) return;
    if (!activeCfgValid_ && !LoadActiveLockConfig()) return;
    LockCursorToNormPoint(activeCfg_.centerXNorm, activeCfg_.centerYNorm);
}

void PcLockRuntime::ClipMouse(bool enable) {
    if (!IsWindow(hwnd_)) return;
    if (enable) {
        RefreshLockedCursorAnchor();
    }
    else {
        ClipCursor(nullptr);
        lockedCursorPointValid_ = false;
        if (cursorHidden_) {
            while (ShowCursor(TRUE) < 0) {}
            cursorHidden_ = false;
        }
    }
}

bool PcLockRuntime::LoadActiveLockConfig() {
    if (!profileProvider_) {
        ClearActiveLockConfig();
        return false;
    }

    const PcMappingProfile& profile = profileProvider_();
    if (profile.locks.empty()) {
        ClearActiveLockConfig();
        return false;
    }

    activeCfg_ = profile.locks.front();
    activeCfgValid_ = true;
    RebuildRuntimeCache(activeCfg_);
    return true;
}

void PcLockRuntime::ClearActiveLockConfig() {
    activeCfgValid_ = false;
    ClearPendingAuxDown();
}

void PcLockRuntime::RebuildRuntimeCache(const PcLockBinding& cfg) {
    RECT rc{};
    if (IsWindow(hwnd_) && GetClientRect(hwnd_, &rc)) {
        cachedClientW_ = (std::max)(1L, rc.right - rc.left);
        cachedClientH_ = (std::max)(1L, rc.bottom - rc.top);
    }
    else {
        cachedClientW_ = 1000;
        cachedClientH_ = 1000;
    }

    rawScaleX_ = PcLockOptionLimits::ClampSpeedNorm(cfg.speedXNorm) *
        1000000.0 / static_cast<double>((std::max)(1, cachedClientW_ - 1));
    rawScaleY_ = PcLockOptionLimits::ClampSpeedNorm(cfg.speedYNorm) *
        1000000.0 / static_cast<double>((std::max)(1, cachedClientH_ - 1));
}

void PcLockRuntime::SetLocked(bool locked) {
    if (!locked) {
        menuInputSuspended_ = false;
        menuReleasedTouch_ = false;
        ClearPendingRawDelta();
        ClearPendingRebuild();
        ClearPendingAuxDown();
    }
    if (locked_ == locked) return;
    locked_ = locked;
    ClearPendingRawDelta();
    ClearPendingRebuild();
    if (locked_) {
        if (!LoadActiveLockConfig()) {
            locked_ = false;
            return;
        }
        BeginTouchLocked(activeCfg_);
        LockCursorToNormPoint(activeCfg_.centerXNorm, activeCfg_.centerYNorm);
    }
    else {
        EndTouchLocked();
        ClipMouse(false);
        ClearActiveLockConfig();
    }
    NotifyLockedChanged();
}

void PcLockRuntime::BeginTouchLocked(const PcLockBinding& cfg) {
    ClearPendingRawDelta();
    ClearPendingRebuild();
    EndTouchLocked();
    menuInputSuspended_ = false;
    menuReleasedTouch_ = false;
    primary_.slot = cfg.primarySlot;
    aux_.slot = cfg.auxSlot;
    primary_.xNorm = cfg.centerXNorm;
    primary_.yNorm = cfg.centerYNorm;
    primary_.xNormF = static_cast<double>(cfg.centerXNorm);
    primary_.yNormF = static_cast<double>(cfg.centerYNorm);
    aux_.xNorm = cfg.centerXNorm;
    aux_.yNorm = cfg.centerYNorm;
    aux_.xNormF = static_cast<double>(cfg.centerXNorm);
    aux_.yNormF = static_cast<double>(cfg.centerYNorm);
    activeSlotIndex_ = 0;

    if (callbacks_.touchDown) {
        primary_.down = callbacks_.touchDown(primary_.slot, primary_.xNorm, primary_.yNorm);
        if (primary_.down) LOCK_STAT(++debugStats_.touchDowns);
        if (cfg.mode == PcLockSlideTouchMode::DualSequential) {
            // 3 号：先 primary，aux 改为非阻塞延迟按下，避免在消息线程 Sleep。
            if (primary_.down) {
                SchedulePendingAuxDown();
            }
            activeSlotIndex_ = 0;
            return;
        }
        if (cfg.mode == PcLockSlideTouchMode::DualSimultaneous) {
            // 2 号：双 slot 同时语义，两个 slot 连续按下。
            aux_.down = callbacks_.touchDown(aux_.slot, aux_.xNorm, aux_.yNorm);
            if (aux_.down) LOCK_STAT(++debugStats_.touchDowns);
        }
    }
}

void PcLockRuntime::EndTouchLocked() {
    ClearPendingAuxDown();
    if (primary_.down && callbacks_.touchUp) { if (callbacks_.touchUp(primary_.slot)) LOCK_STAT(++debugStats_.touchUps); }
    if (aux_.down && callbacks_.touchUp) { if (callbacks_.touchUp(aux_.slot)) LOCK_STAT(++debugStats_.touchUps); }
    primary_.down = false;
    aux_.down = false;
    ClearPendingRebuild();
}

void PcLockRuntime::ClearPendingRawDelta() {
    pendingRawDeltaActive_ = false;
    pendingRawDx_ = 0;
    pendingRawDy_ = 0;
    pendingRawPackets_ = 0;
}


void PcLockRuntime::ClearPendingRebuild() {
    pendingRebuildMode_ = 0;
    pendingRebuildXNorm_ = 500000;
    pendingRebuildYNorm_ = 500000;
    pendingRebuildDue_ = std::chrono::steady_clock::time_point{};
}

void PcLockRuntime::ClearPendingAuxDown() {
    if (pendingAuxDownActive_ && hwnd_) {
        KillTimer(hwnd_, kPendingAuxDownTimerId);
    }
    pendingAuxDownActive_ = false;
    pendingAuxDownDue_ = std::chrono::steady_clock::time_point{};
}

void PcLockRuntime::SchedulePendingAuxDown() {
    pendingAuxDownActive_ = true;
    pendingAuxDownDue_ = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(PcLockOptionLimits::kSequentialAuxDownDelayMs);

    if (hwnd_) {
        SetTimer(hwnd_, kPendingAuxDownTimerId, PcLockOptionLimits::kSequentialAuxDownDelayMs, nullptr);
    }
}

bool PcLockRuntime::FinishPendingAuxDownIfReady(const PcLockBinding& cfg, bool force) {
    if (!pendingAuxDownActive_) return true;
    if (!force && std::chrono::steady_clock::now() < pendingAuxDownDue_) return false;

    ClearPendingAuxDown();

    if (!locked_ || !callbacks_.touchDown || !primary_.down || aux_.down) {
        return false;
    }

    aux_.slot = cfg.auxSlot;
    aux_.down = callbacks_.touchDown(aux_.slot, aux_.xNorm, aux_.yNorm);
    if (aux_.down) {
        LOCK_STAT(++debugStats_.touchDowns);
        activeSlotIndex_ = 1;
        return true;
    }

    // aux 按下失败时，回滚 primary，避免顺序双 slot 半悬挂。
    if (callbacks_.touchUp && primary_.down) {
        if (callbacks_.touchUp(primary_.slot)) LOCK_STAT(++debugStats_.touchUps);
    }
    primary_.down = false;
    activeSlotIndex_ = 0;
    return false;
}

int PcLockRuntime::ClampRebuildDelayMs(const PcLockBinding& cfg) const {
    return PcLockOptionLimits::ClampRebuildDownDelayMs(cfg.rebuildDownDelayMs);
}

void PcLockRuntime::AccumulateRawDelta(int dx, int dy) {
    const auto now = std::chrono::steady_clock::now();
    if (!pendingRawDeltaActive_) {
        pendingRawDeltaActive_ = true;
        pendingRawDeltaStart_ = now;
    }
    else {
        LOCK_STAT(++debugStats_.coalesceDeferredPackets);
    }
    pendingRawDx_ += static_cast<int64_t>(dx);
    pendingRawDy_ += static_cast<int64_t>(dy);
    ++pendingRawPackets_;
}

bool PcLockRuntime::FlushPendingRawDelta(bool force) {
    if (!pendingRawDeltaActive_ || (pendingRawDx_ == 0 && pendingRawDy_ == 0)) {
        ClearPendingRawDelta();
        return false;
    }
    if (!locked_ || !activeCfgValid_) {
        ClearPendingRawDelta();
        return false;
    }

    const auto now = std::chrono::steady_clock::now();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - pendingRawDeltaStart_).count();
    if (!force && elapsed < PcLockOptionLimits::kRawDeltaCoalesceMs) {
        return false;
    }

    if (pendingRebuildMode_ != 0 && !FinishPendingRebuildIfReady(activeCfg_)) {
        return false;
    }
    if (pendingAuxDownActive_ && !FinishPendingAuxDownIfReady(activeCfg_)) {
        return false;
    }

    const int dx = static_cast<int>((std::max)(
        static_cast<int64_t>((std::numeric_limits<int>::min)()),
        (std::min)(static_cast<int64_t>((std::numeric_limits<int>::max)()), pendingRawDx_)
        ));
    const int dy = static_cast<int>((std::max)(
        static_cast<int64_t>((std::numeric_limits<int>::min)()),
        (std::min)(static_cast<int64_t>((std::numeric_limits<int>::max)()), pendingRawDy_)
        ));

    ClearPendingRawDelta();
    LOCK_STAT(++debugStats_.coalesceFlushes);
    if (force) LOCK_STAT(++debugStats_.coalesceForcedFlushes);
    ApplyRawDelta(activeCfg_, dx, dy);
    return true;
}

void PcLockRuntime::ApplyRawDelta(const PcLockBinding& cfg, int dx, int dy) {
    LOCK_STAT(++debugStats_.applyCalls);
    if (!locked_ || menuInputSuspended_) {
        LOCK_STAT(++debugStats_.applySkippedState);
        return;
    }
    if (dx == 0 && dy == 0) {
        LOCK_STAT(++debugStats_.applySkippedZero);
        return;
    }
    const double dxNorm = static_cast<double>(dx) * rawScaleX_;
    const double dyNorm = static_cast<double>(dy) * rawScaleY_;
    if (std::fabs(dxNorm) < 0.0001 && std::fabs(dyNorm) < 0.0001) {
        LOCK_STAT(++debugStats_.applySkippedTiny);
        return;
    }

    switch (cfg.mode) {
    case PcLockSlideTouchMode::SingleReanchor:
        SingleMove(cfg, dxNorm, dyNorm);
        break;
    case PcLockSlideTouchMode::DualSimultaneous:
        DualMove(cfg, dxNorm, dyNorm, false);
        break;
    case PcLockSlideTouchMode::DualSequential:
    default:
        // 3 号模式：primary 先按下，固定等待 5ms 后按 aux；使用时后按的 aux 先用。
        DualSequentialMove(cfg, dxNorm, dyNorm);
        break;
    }
}


bool PcLockRuntime::MoveSlotClamped(const PcLockBinding& cfg, SlotState& st, double dxNorm, double dyNorm) {
    LOCK_STAT(++debugStats_.moveAttempts);
    if (!st.down || !callbacks_.touchMove) return false;
    const double nxF = (std::max)(static_cast<double>(cfg.leftNorm), (std::min)(static_cast<double>(cfg.rightNorm), st.xNormF + dxNorm));
    const double nyF = (std::max)(static_cast<double>(cfg.topNorm), (std::min)(static_cast<double>(cfg.bottomNorm), st.yNormF + dyNorm));
    const int nx = ClampInt(static_cast<int>(nxF + 0.5), cfg.leftNorm, cfg.rightNorm);
    const int ny = ClampInt(static_cast<int>(nyF + 0.5), cfg.topNorm, cfg.bottomNorm);
    if (nx == st.xNorm && ny == st.yNorm) {
        st.xNormF = nxF;
        st.yNormF = nyF;
        return false;
    }
    st.xNormF = nxF;
    st.yNormF = nyF;
    st.xNorm = nx;
    st.yNorm = ny;
    const bool ok = callbacks_.touchMove(st.slot, st.xNorm, st.yNorm);
    if (ok) LOCK_STAT(++debugStats_.touchMoves);
    return ok;
}

PcLockRuntime::DeltaMoveResult PcLockRuntime::MoveSlotConsumeDelta(const PcLockBinding& cfg, SlotState& st, double dxNorm, double dyNorm) {
    DeltaMoveResult r;
    r.remainingXNorm = dxNorm;
    r.remainingYNorm = dyNorm;

    LOCK_STAT(++debugStats_.moveAttempts);
    if (!st.down || !callbacks_.touchMove) {
        r.blocked = true;
        return r;
    }

    const double oldX = st.xNormF;
    const double oldY = st.yNormF;
    const double targetX = oldX + dxNorm;
    const double targetY = oldY + dyNorm;
    const double nxF = (std::max)(static_cast<double>(cfg.leftNorm), (std::min)(static_cast<double>(cfg.rightNorm), targetX));
    const double nyF = (std::max)(static_cast<double>(cfg.topNorm), (std::min)(static_cast<double>(cfg.bottomNorm), targetY));
    const int nx = ClampInt(static_cast<int>(nxF + 0.5), cfg.leftNorm, cfg.rightNorm);
    const int ny = ClampInt(static_cast<int>(nyF + 0.5), cfg.topNorm, cfg.bottomNorm);

    const double consumedX = nxF - oldX;
    const double consumedY = nyF - oldY;
    r.remainingXNorm = dxNorm - consumedX;
    r.remainingYNorm = dyNorm - consumedY;
    r.blocked = std::fabs(r.remainingXNorm) > 0.25 || std::fabs(r.remainingYNorm) > 0.25;

    if (nx == st.xNorm && ny == st.yNorm) {
        st.xNormF = nxF;
        st.yNormF = nyF;
        r.moved = false;
        if (std::fabs(dxNorm) > 0.25 || std::fabs(dyNorm) > 0.25) {
            r.blocked = true;
        }
        return r;
    }

    st.xNormF = nxF;
    st.yNormF = nyF;
    st.xNorm = nx;
    st.yNorm = ny;
    const bool ok = callbacks_.touchMove(st.slot, st.xNorm, st.yNorm);
    r.moved = ok;
    if (ok) LOCK_STAT(++debugStats_.touchMoves);
    return r;
}

bool PcLockRuntime::LiftSlot(SlotState& st) {
    if (!st.down) return true;
    bool ok = true;
    if (callbacks_.touchUp) {
        ok = callbacks_.touchUp(st.slot);
        if (ok) LOCK_STAT(++debugStats_.touchUps);
    }
    st.down = false;
    return ok;
}

double PcLockRuntime::DistanceToEdgeSqForDelta(const PcLockBinding& cfg, const SlotState& st, double dxNorm, double dyNorm) const {
    double cap = 1.0;
    bool constrained = false;
    if (dxNorm > 0) { cap = (std::min)(cap, double(cfg.rightNorm - st.xNorm) / double(dxNorm)); constrained = true; }
    if (dxNorm < 0) { cap = (std::min)(cap, double(cfg.leftNorm - st.xNorm) / double(dxNorm)); constrained = true; }
    if (dyNorm > 0) { cap = (std::min)(cap, double(cfg.bottomNorm - st.yNorm) / double(dyNorm)); constrained = true; }
    if (dyNorm < 0) { cap = (std::min)(cap, double(cfg.topNorm - st.yNorm) / double(dyNorm)); constrained = true; }
    if (!constrained) return 0.0;
    cap = (std::max)(0.0, (std::min)(1.0, cap));
    const double sx = dxNorm * cap;
    const double sy = dyNorm * cap;
    return sx * sx + sy * sy;
}

PcLockRuntime::SlotState* PcLockRuntime::ChooseSlotForDelta(const PcLockBinding& cfg, double dxNorm, double dyNorm) {
    SlotState* a = primary_.down ? &primary_ : nullptr;
    SlotState* b = aux_.down ? &aux_ : nullptr;
    if (a && b) {
        const double ca = DistanceToEdgeSqForDelta(cfg, *a, dxNorm, dyNorm);
        const double cb = DistanceToEdgeSqForDelta(cfg, *b, dxNorm, dyNorm);
        return (cb > ca) ? b : a;
    }
    return a ? a : b;
}


static int lockRebuildAnchorX(const PcLockBinding& cfg, double preferredXNorm) {
    const int marginX = (std::max)(4000, (cfg.rightNorm - cfg.leftNorm) / 18);
    int nx = cfg.centerXNorm;
    if (preferredXNorm > 0) nx = cfg.leftNorm + marginX;
    if (preferredXNorm < 0) nx = cfg.rightNorm - marginX;
    return (std::max)(cfg.leftNorm, (std::min)(cfg.rightNorm, nx));
}

static int lockRebuildAnchorY(const PcLockBinding& cfg, double preferredYNorm) {
    const int marginY = (std::max)(4000, (cfg.bottomNorm - cfg.topNorm) / 18);
    int ny = cfg.centerYNorm;
    if (preferredYNorm > 0) ny = cfg.topNorm + marginY;
    if (preferredYNorm < 0) ny = cfg.bottomNorm - marginY;
    return (std::max)(cfg.topNorm, (std::min)(cfg.bottomNorm, ny));
}

bool PcLockRuntime::FinishPendingRebuildIfReady(const PcLockBinding& cfg) {
    if (pendingRebuildMode_ == 0) return true;
    if (std::chrono::steady_clock::now() < pendingRebuildDue_) return false;

    const int mode = pendingRebuildMode_;
    const int nx = ClampInt(pendingRebuildXNorm_, cfg.leftNorm, cfg.rightNorm);
    const int ny = ClampInt(pendingRebuildYNorm_, cfg.topNorm, cfg.bottomNorm);
    ClearPendingRebuild();

    primary_.slot = cfg.primarySlot;
    aux_.slot = cfg.auxSlot;
    primary_.xNorm = nx;
    primary_.yNorm = ny;
    primary_.xNormF = static_cast<double>(nx);
    primary_.yNormF = static_cast<double>(ny);
    aux_.xNorm = nx;
    aux_.yNorm = ny;
    aux_.xNormF = static_cast<double>(nx);
    aux_.yNormF = static_cast<double>(ny);

    if (!callbacks_.touchDown) return false;

    if (mode == 1) {
        primary_.down = callbacks_.touchDown(primary_.slot, primary_.xNorm, primary_.yNorm);
        if (primary_.down) LOCK_STAT(++debugStats_.touchDowns);
        activeSlotIndex_ = 0;
        return primary_.down;
    }

    primary_.down = callbacks_.touchDown(primary_.slot, primary_.xNorm, primary_.yNorm);
    if (primary_.down) LOCK_STAT(++debugStats_.touchDowns);
    if (mode == 3) {
        if (primary_.down) {
            SchedulePendingAuxDown();
            activeSlotIndex_ = 0;
            return true;
        }
        return false;
    }

    aux_.down = primary_.down ? callbacks_.touchDown(aux_.slot, aux_.xNorm, aux_.yNorm) : false;
    if (aux_.down) LOCK_STAT(++debugStats_.touchDowns);
    if (!aux_.down) {
        if (primary_.down && callbacks_.touchUp) { if (callbacks_.touchUp(primary_.slot)) LOCK_STAT(++debugStats_.touchUps); }
        primary_.down = false;
        aux_.down = false;
        return false;
    }
    activeSlotIndex_ = 0;
    return true;
}

void PcLockRuntime::ScheduleSingleRebuild(const PcLockBinding& cfg, double preferredXNorm, double preferredYNorm, const wchar_t*) {
    LOCK_STAT(++debugStats_.reanchors);
    pendingRebuildMode_ = 1;
    pendingRebuildXNorm_ = lockRebuildAnchorX(cfg, preferredXNorm);
    pendingRebuildYNorm_ = lockRebuildAnchorY(cfg, preferredYNorm);
    pendingRebuildDue_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(ClampRebuildDelayMs(cfg));

    if (primary_.down && callbacks_.touchUp) { if (callbacks_.touchUp(primary_.slot)) LOCK_STAT(++debugStats_.touchUps); }
    primary_.down = false;
    primary_.xNorm = pendingRebuildXNorm_;
    primary_.yNorm = pendingRebuildYNorm_;
    primary_.xNormF = static_cast<double>(pendingRebuildXNorm_);
    primary_.yNormF = static_cast<double>(pendingRebuildYNorm_);
}

void PcLockRuntime::ScheduleDualRebuild(const PcLockBinding& cfg, double preferredXNorm, double preferredYNorm, int mode, const wchar_t*) {
    LOCK_STAT(++debugStats_.reanchors);
    pendingRebuildMode_ = (mode == 3) ? 3 : 2;
    pendingRebuildXNorm_ = lockRebuildAnchorX(cfg, preferredXNorm);
    pendingRebuildYNorm_ = lockRebuildAnchorY(cfg, preferredYNorm);
    pendingRebuildDue_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(ClampRebuildDelayMs(cfg));

    // 双 slot 重按语义：两个 slot 都已经抬起以后，等待 rebuildDownDelayMs，再重新按下。
    // 3 号顺序模式保持旧顺序：先抬 aux，再抬 primary；重按时 primary -> aux。
    if (pendingRebuildMode_ == 3) {
        if (aux_.down && callbacks_.touchUp) { if (callbacks_.touchUp(aux_.slot)) LOCK_STAT(++debugStats_.touchUps); }
        if (primary_.down && callbacks_.touchUp) { if (callbacks_.touchUp(primary_.slot)) LOCK_STAT(++debugStats_.touchUps); }
    }
    else {
        if (primary_.down && callbacks_.touchUp) { if (callbacks_.touchUp(primary_.slot)) LOCK_STAT(++debugStats_.touchUps); }
        if (aux_.down && callbacks_.touchUp) { if (callbacks_.touchUp(aux_.slot)) LOCK_STAT(++debugStats_.touchUps); }
    }
    primary_.down = false;
    aux_.down = false;

    primary_.xNorm = pendingRebuildXNorm_;
    primary_.yNorm = pendingRebuildYNorm_;
    primary_.xNormF = static_cast<double>(pendingRebuildXNorm_);
    primary_.yNormF = static_cast<double>(pendingRebuildYNorm_);
    aux_.xNorm = pendingRebuildXNorm_;
    aux_.yNorm = pendingRebuildYNorm_;
    aux_.xNormF = static_cast<double>(pendingRebuildXNorm_);
    aux_.yNormF = static_cast<double>(pendingRebuildYNorm_);
}

void PcLockRuntime::ReanchorSlot(const PcLockBinding& cfg, SlotState& st, double preferredXNorm, double preferredYNorm, const wchar_t*) {
    LOCK_STAT(++debugStats_.reanchors);
    const int marginX = (std::max)(4000, (cfg.rightNorm - cfg.leftNorm) / 18);
    const int marginY = (std::max)(4000, (cfg.bottomNorm - cfg.topNorm) / 18);
    int nx = cfg.centerXNorm;
    int ny = cfg.centerYNorm;
    if (preferredXNorm > 0) nx = cfg.leftNorm + marginX;
    if (preferredXNorm < 0) nx = cfg.rightNorm - marginX;
    if (preferredYNorm > 0) ny = cfg.topNorm + marginY;
    if (preferredYNorm < 0) ny = cfg.bottomNorm - marginY;
    nx = ClampInt(nx, cfg.leftNorm, cfg.rightNorm);
    ny = ClampInt(ny, cfg.topNorm, cfg.bottomNorm);

    if (st.down && callbacks_.touchUp) { if (callbacks_.touchUp(st.slot)) LOCK_STAT(++debugStats_.touchUps); }
    st.xNorm = nx;
    st.yNorm = ny;
    st.xNormF = static_cast<double>(nx);
    st.yNormF = static_cast<double>(ny);
    st.down = callbacks_.touchDown ? callbacks_.touchDown(st.slot, st.xNorm, st.yNorm) : false;
    if (st.down) LOCK_STAT(++debugStats_.touchDowns);
}

void PcLockRuntime::SingleMove(const PcLockBinding& cfg, double dxNorm, double dyNorm) {
    if (pendingRebuildMode_ != 0 && !FinishPendingRebuildIfReady(cfg)) return;
    if (!primary_.down && callbacks_.touchDown) {
        primary_.xNorm = cfg.centerXNorm;
        primary_.yNorm = cfg.centerYNorm;
        primary_.xNormF = static_cast<double>(cfg.centerXNorm);
        primary_.yNormF = static_cast<double>(cfg.centerYNorm);
        primary_.down = callbacks_.touchDown(primary_.slot, primary_.xNorm, primary_.yNorm);
        if (primary_.down) LOCK_STAT(++debugStats_.touchDowns);
    }
    if (!primary_.down) return;
    const bool moved = MoveSlotClamped(cfg, primary_, dxNorm, dyNorm);
    const bool nearEdge = primary_.xNorm <= cfg.leftNorm + 2 || primary_.xNorm >= cfg.rightNorm - 2 ||
        primary_.yNorm <= cfg.topNorm + 2 || primary_.yNorm >= cfg.bottomNorm - 2;
    if (!moved || nearEdge) {
        // 单 slot：碰到边界后先抬起；下一次移动到来时，等待 rebuildDownDelayMs 后再按下并移动。
        ScheduleSingleRebuild(cfg, dxNorm, dyNorm, L"single");
    }
}

static int lockSequentialAnchorX(const PcLockBinding& cfg, double preferredXNorm) {
    return lockRebuildAnchorX(cfg, preferredXNorm);
}

static int lockSequentialAnchorY(const PcLockBinding& cfg, double preferredYNorm) {
    return lockRebuildAnchorY(cfg, preferredYNorm);
}

void PcLockRuntime::RebuildSequentialPair(const PcLockBinding& cfg, double preferredXNorm, double preferredYNorm, const wchar_t*) {
    LOCK_STAT(++debugStats_.reanchors);
    const int nx = lockSequentialAnchorX(cfg, preferredXNorm);
    const int ny = lockSequentialAnchorY(cfg, preferredYNorm);

    // 对齐旧版 mouse_slide_rebuild_sequential：先抬 aux，再抬 primary；再 primary -> aux 顺序重按。
    if (aux_.down && callbacks_.touchUp) { if (callbacks_.touchUp(aux_.slot)) LOCK_STAT(++debugStats_.touchUps); }
    if (primary_.down && callbacks_.touchUp) { if (callbacks_.touchUp(primary_.slot)) LOCK_STAT(++debugStats_.touchUps); }
    primary_.down = false;
    aux_.down = false;

    primary_.xNorm = nx;
    primary_.yNorm = ny;
    primary_.xNormF = static_cast<double>(nx);
    primary_.yNormF = static_cast<double>(ny);
    aux_.xNorm = nx;
    aux_.yNorm = ny;
    aux_.xNormF = static_cast<double>(nx);
    aux_.yNormF = static_cast<double>(ny);

    if (callbacks_.touchDown) {
        primary_.down = callbacks_.touchDown(primary_.slot, primary_.xNorm, primary_.yNorm);
        if (primary_.down) {
            LOCK_STAT(++debugStats_.touchDowns);
            SchedulePendingAuxDown();
        }
    }
    activeSlotIndex_ = 0; // aux 到期按下后再切到 1。
}

void PcLockRuntime::DualSequentialMove(const PcLockBinding& cfg, double dxNorm, double dyNorm) {
    // 3 号顺序模式：按下顺序是 primary -> 固定等待 5ms -> aux。
    // 使用顺序是“后按的先用”：先移动 aux；aux 到边界后抬起 aux，再把剩余移动量交给 primary。
    // primary 也到边界后，两个 slot 都处于抬起状态；下一次移动到来时，等待 rebuildDownDelayMs 后重新 primary -> 5ms -> aux。
    if (pendingRebuildMode_ != 0 && !FinishPendingRebuildIfReady(cfg)) return;
    if (pendingAuxDownActive_ && !FinishPendingAuxDownIfReady(cfg)) return;

    if (!primary_.down && !aux_.down) {
        BeginTouchLocked(cfg);
    }
    if (pendingAuxDownActive_ && !FinishPendingAuxDownIfReady(cfg)) return;
    if (!primary_.down && !aux_.down) return;

    // 兼容热更新/旧状态：如果两个 slot 都还按着，3 号模式永远从后按下的 aux 开始使用。
    if (primary_.down && aux_.down) {
        activeSlotIndex_ = 1;
    }

    double rx = dxNorm;
    double ry = dyNorm;

    if (activeSlotIndex_ == 1 && aux_.down) {
        const auto first = MoveSlotConsumeDelta(cfg, aux_, rx, ry);
        if (!first.blocked) return;

        // 后按的 aux 已经用完：抬起 aux，然后把剩余移动量交给 primary。
        LiftSlot(aux_);
        activeSlotIndex_ = 0;
        rx = first.remainingXNorm;
        ry = first.remainingYNorm;
    }

    if (primary_.down) {
        const auto second = MoveSlotConsumeDelta(cfg, primary_, rx, ry);
        if (!second.blocked) return;

        // 先按的 primary 也用完：此时两个 slot 都不可用，下一次移动前按延迟重新按下。
        LiftSlot(primary_);
        activeSlotIndex_ = 1;
        ScheduleDualRebuild(cfg, second.remainingXNorm, second.remainingYNorm, 3, L"seq-both-used");
        return;
    }

    // 防御分支：如果 primary 已经不可用但 aux 还在，就继续消耗 aux，避免状态异常时丢失位移。
    if (aux_.down) {
        const auto fallback = MoveSlotConsumeDelta(cfg, aux_, rx, ry);
        if (!fallback.blocked) return;

        LiftSlot(aux_);
        activeSlotIndex_ = 1;
        ScheduleDualRebuild(cfg, fallback.remainingXNorm, fallback.remainingYNorm, 3, L"seq-aux-only");
        return;
    }

    activeSlotIndex_ = 1;
    ScheduleDualRebuild(cfg, rx, ry, 3, L"seq-none");
}

void PcLockRuntime::DualMove(const PcLockBinding& cfg, double dxNorm, double dyNorm, bool sequential) {
    if (pendingRebuildMode_ != 0 && !FinishPendingRebuildIfReady(cfg)) return;
    if (!primary_.down && !aux_.down) BeginTouchLocked(cfg);

    if (sequential) {
        // 保留旧分支兼容；当前 ApplyRawDelta 的 3 号模式已经走 DualSequentialMove。
        DualSequentialMove(cfg, dxNorm, dyNorm);
        return;
    }

    SlotState* first = ChooseSlotForDelta(cfg, dxNorm, dyNorm);
    if (!first) return;
    SlotState* second = nullptr;
    if (first == &primary_) second = aux_.down ? &aux_ : nullptr;
    else second = primary_.down ? &primary_ : nullptr;

    const auto r1 = MoveSlotConsumeDelta(cfg, *first, dxNorm, dyNorm);
    if (!r1.blocked) return;

    if (second) {
        const auto r2 = MoveSlotConsumeDelta(cfg, *second, r1.remainingXNorm, r1.remainingYNorm);
        if (!r2.blocked) return;
        // 2 号双 slot 同时：只有两个 slot 都无法继续处理剩余移动量时，才进入完整重按周期。
        ScheduleDualRebuild(cfg, r2.remainingXNorm, r2.remainingYNorm, 2, L"dual-both-used");
        return;
    }

    ScheduleDualRebuild(cfg, r1.remainingXNorm, r1.remainingYNorm, 2, L"dual-one-left");
}

bool PcLockRuntime::ConsumeLockedRawMouse(HWND hwnd, LPARAM lp) {
    if (!hwnd_) hwnd_ = hwnd;

    LOCK_STAT(++debugStats_.rawInputMessages);
    if (!locked_ || !activeCfgValid_) {
        LOCK_STAT(++debugStats_.rawInputIgnoredNotReady);
        return false;
    }

    RAWINPUT raw{};
    UINT size = sizeof(raw);
    const UINT got = GetRawInputData(
        reinterpret_cast<HRAWINPUT>(lp),
        RID_INPUT,
        &raw,
        &size,
        sizeof(RAWINPUTHEADER)
    );
    if (got == static_cast<UINT>(-1) || got != size || raw.header.dwType != RIM_TYPEMOUSE) {
        LOCK_STAT(++debugStats_.rawInputBytesRejected);
        return false;
    }

    LOCK_STAT(++debugStats_.rawInputMouse);
    const int dx = raw.data.mouse.lLastX;
    const int dy = raw.data.mouse.lLastY;
    LOCK_STAT(debugStats_.rawDx += dx);
    LOCK_STAT(debugStats_.rawDy += dy);
    AccumulateRawDelta(dx, dy);
    FlushPendingRawDelta(false);
    return true;
}

bool PcLockRuntime::HandleWindowMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LRESULT* outResult) {
    if (outResult) *outResult = 0;
    if (!hwnd_) hwnd_ = hwnd;

    switch (msg) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        const UINT vk = NormalizeVk(static_cast<UINT>(wp), lp);
        const bool repeat = (lp & 0x40000000L) != 0;
        if (vk < vkDown_.size()) vkDown_[vk] = true;
        if (!profileProvider_ || profileProvider_().locks.empty()) return false;
        const auto& cfg = profileProvider_().locks.front();
        const bool nowDown = ComboSatisfied(cfg, vkDown_);
        if (nowDown && !comboWasDown_ && !repeat) {
            comboWasDown_ = true;
            SetLocked(!locked_);
            if (outResult) *outResult = 0;
            return true;
        }
        comboWasDown_ = comboWasDown_ || nowDown;
        return nowDown;
    }
    case WM_KEYUP:
    case WM_SYSKEYUP: {
        const UINT vk = NormalizeVk(static_cast<UINT>(wp), lp);
        if (vk < vkDown_.size()) vkDown_[vk] = false;
        if (profileProvider_ && !profileProvider_().locks.empty()) {
            comboWasDown_ = ComboSatisfied(profileProvider_().locks.front(), vkDown_);
        }
        return false;
    }
    case WM_INPUT:
        return ConsumeLockedRawMouse(hwnd, lp);
    case WM_TIMER:
        if (wp == kPendingAuxDownTimerId) {
            if (locked_ && activeCfgValid_ && pendingAuxDownActive_) {
                FinishPendingAuxDownIfReady(activeCfg_, true);
                FlushPendingRawDelta(true);
            }
            else {
                ClearPendingAuxDown();
            }
            if (outResult) *outResult = 0;
            return true;
        }
        return false;
    case WM_MOVE:
    case WM_SIZE:
        if (locked_ && activeCfgValid_) {
            RebuildRuntimeCache(activeCfg_);
            RefreshLockedCursorAnchor();
        }
        return false;
    case WM_MOUSEMOVE:
        if (locked_) {
            RefreshLockedCursorAnchor();
            return true;
        }
        return false;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP: {
        const UINT mouseVk = lockMouseVkFromMessage(msg, wp);
        const bool isDown = lockMouseMessageIsDown(msg);
        const bool isUp = lockMouseMessageIsUp(msg);

        // 鼠标左键永远不作为视角锁定触发键，保留给普通点击/射击等场景。
        // 右键/中键/侧键可以像键盘一样参与 comboTriggerCodes，支持“右键”或“Ctrl+右键”。
        if (mouseVk != 0 && mouseVk != VK_LBUTTON && mouseVk < vkDown_.size()) {
            vkDown_[static_cast<size_t>(mouseVk)] = isDown ? true : (isUp ? false : vkDown_[static_cast<size_t>(mouseVk)]);

            if (profileProvider_ && !profileProvider_().locks.empty()) {
                const auto& cfg = profileProvider_().locks.front();
                const bool nowDown = ComboSatisfied(cfg, vkDown_);
                if (isDown && nowDown && !comboWasDown_) {
                    comboWasDown_ = true;
                    SetLocked(!locked_);
                    if (outResult) *outResult = 0;
                    return true;
                }
                comboWasDown_ = nowDown;
                if (nowDown || locked_) return true;
            }
        }

        // 锁定模式下禁止普通投屏点击链路接管鼠标；映射 runtime 已在前面处理需要的鼠标绑定。
        return locked_;
    }
    case WM_KILLFOCUS:
    case WM_CANCELMODE:
        vkDown_.fill(false);
        comboWasDown_ = false;
        SetLocked(false);
        return false;
    default:
        return false;
    }
}
