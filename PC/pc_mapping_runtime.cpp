#include "pc_mapping_runtime.h"
#include "pc_normal_key_options_dialog.h"

#include <algorithm>
#include <cmath>
#include <windowsx.h>
#include <utility>

PcMappingRuntime::PcMappingRuntime() {
    runtimeSlotOwner_.fill(kNoRuntimeSlotOwner);
}

bool PcMappingRuntime::IsRuntimeTouchSlot(int slot) {
    return slot >= 0 && slot < kRuntimeTouchSlotCount;
}

bool PcMappingRuntime::IsReservedRuntimeTouchSlot(int slot) const {
    // Android 常见设备最大 10 个触点，对应 slot 0..9。
    // 普通按键只从 3..8 动态借用物理 slot，避免和投屏点击、轮盘、视角锁定触点抢 slot。
    if (!IsRuntimeTouchSlot(slot)) return true;
    if (slot == 0) return true; // 投屏直接点击
    if (slot == 1) return true; // 轮盘
    if (slot == 2) return true; // 视角锁定 primary
    if (slot == 9) return true; // 视角锁定 aux
    return false;
}

bool PcMappingRuntime::RuntimeSlotOwnedBy(size_t bindingIndex, int slot) const {
    if (!IsRuntimeTouchSlot(slot)) return false;
    return runtimeSlotOwner_[static_cast<size_t>(slot)] == static_cast<int>(bindingIndex);
}

int PcMappingRuntime::AllocateRuntimeSlot(size_t bindingIndex) {
    const auto now = std::chrono::steady_clock::now();
    for (int slot = 0; slot < kRuntimeTouchSlotCount; ++slot) {
        if (IsReservedRuntimeTouchSlot(slot)) continue;
        const size_t index = static_cast<size_t>(slot);
        if (runtimeSlotOwner_[index] != kNoRuntimeSlotOwner) continue;
        if (now < slotCooldownUntil_[index]) continue;

        runtimeSlotOwner_[index] = static_cast<int>(bindingIndex);
        return slot;
    }
    return -1;
}

void PcMappingRuntime::ReleaseRuntimeSlot(size_t bindingIndex, int slot, bool startCooldown) {
    if (!IsRuntimeTouchSlot(slot)) return;
    const size_t index = static_cast<size_t>(slot);
    if (runtimeSlotOwner_[index] != static_cast<int>(bindingIndex)) return;

    if (startCooldown) {
        slotCooldownUntil_[index] = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(PcNormalKeyOptionLimits::kSlotReuseCooldownMs);
    }
    runtimeSlotOwner_[index] = kNoRuntimeSlotOwner;
}

void PcMappingRuntime::SetCallbacks(Callbacks cb) {
    callbacks_ = std::move(cb);
}

void PcMappingRuntime::SetProfile(const PcMappingProfile& profile) {
    Reset();
    profile_ = profile;
    bindingActive_.assign(profile_.bindings.size(), false);
    activeTouches_.assign(profile_.bindings.size(), ActiveTouch{});
    RebuildBindingBuckets();
}

const PcMappingProfile& PcMappingRuntime::GetProfile() const {
    return profile_;
}

void PcMappingRuntime::SetEnabled(bool enabled) {
    if (enabled_ == enabled) return;
    if (!enabled) Reset();
    enabled_ = enabled;
}

bool PcMappingRuntime::IsEnabled() const {
    return enabled_;
}

void PcMappingRuntime::SetLockActiveProvider(std::function<bool()> provider) {
    lockActiveProvider_ = std::move(provider);
}

bool PcMappingRuntime::IsLockActive() const {
    return lockActiveProvider_ ? lockActiveProvider_() : false;
}

void PcMappingRuntime::RandomPointInRadius(int radius, int& dx, int& dy) {
    dx = 0;
    dy = 0;

    radius = (std::max)(0, (std::min)(120000, radius));
    if (radius <= 0) return;

    // UI 圆边缘和运行时整数取整之间会有轻微误差。
    // 随机点不贴着可视圆边缘采样，避免看起来超出 UI 圆。
    constexpr int kRandomPointInsetMinNorm = 1000;
    constexpr int kRandomPointInsetDivisor = 12; // 约内缩 8.3%。
    const int inset = (std::max)(kRandomPointInsetMinNorm, radius / kRandomPointInsetDivisor);
    const int safeRadius = (std::max)(0, radius - inset);
    if (safeRadius <= 0) return;

    std::uniform_real_distribution<double> angleDist(0.0, 6.2831853071795864769);
    std::uniform_real_distribution<double> radiusDist(0.0, 1.0);
    const double a = angleDist(rng_);
    const double r = std::sqrt(radiusDist(rng_)) * static_cast<double>(safeRadius);

    const double ux = std::cos(a) * r;
    const double uy = std::sin(a) * r;

    // UI 圆通常按窗口短边换算半径；而触摸坐标的 X/Y Norm 分别按宽/高换算。
    // 横屏下如果直接 dx=ux、dy=uy，X 方向会比可视圆更宽。
    // 这里按当前窗口宽高把短边半径换算成 X/Y 各自的 Norm 偏移。
    const int w = clientWidthPx_;
    const int h = clientHeightPx_;
    if (w > 0 && h > 0) {
        const double ref = static_cast<double>((std::min)(w, h));
        const double xScale = ref / static_cast<double>(w);
        const double yScale = ref / static_cast<double>(h);
        dx = static_cast<int>(std::lround(ux * xScale));
        dy = static_cast<int>(std::lround(uy * yScale));

        // 取整后再按 UI 像素圆的度量严格裁剪一次：
        // (dx * width / ref)^2 + (dy * height / ref)^2 <= safeRadius^2
        const double uiDx = static_cast<double>(dx) * static_cast<double>(w) / ref;
        const double uiDy = static_cast<double>(dy) * static_cast<double>(h) / ref;
        const double dist = std::hypot(uiDx, uiDy);
        if (dist > static_cast<double>(safeRadius) && dist > 0.0) {
            const double scale = static_cast<double>(safeRadius) / dist;
            dx = static_cast<int>(std::trunc(static_cast<double>(dx) * scale));
            dy = static_cast<int>(std::trunc(static_cast<double>(dy) * scale));
        }
        return;
    }

    // 未拿到窗口尺寸时退回到普通 Norm 圆，但仍然使用安全内缩和严格圆内裁剪。
    dx = static_cast<int>(std::lround(ux));
    dy = static_cast<int>(std::lround(uy));

    const long long limitSq = static_cast<long long>(safeRadius) * static_cast<long long>(safeRadius);
    long long distSq = static_cast<long long>(dx) * static_cast<long long>(dx) +
        static_cast<long long>(dy) * static_cast<long long>(dy);
    if (distSq > limitSq && distSq > 0) {
        const double scale = static_cast<double>(safeRadius) / std::sqrt(static_cast<double>(distSq));
        dx = static_cast<int>(std::trunc(static_cast<double>(dx) * scale));
        dy = static_cast<int>(std::trunc(static_cast<double>(dy) * scale));
    }
}

void PcMappingRuntime::ClampPair(int& x, int& y) const {
    x = PcMappingProfile::ClampNorm(x);
    y = PcMappingProfile::ClampNorm(y);
}

void PcMappingRuntime::UpdateClientSize(HWND hwnd) {
    if (!hwnd) return;

    RECT rc{};
    if (!GetClientRect(hwnd, &rc)) return;

    const int w = (std::max)(0, static_cast<int>(rc.right - rc.left));
    const int h = (std::max)(0, static_cast<int>(rc.bottom - rc.top));
    if (w > 0 && h > 0) {
        clientWidthPx_ = w;
        clientHeightPx_ = h;
    }
}

void PcMappingRuntime::Tick() {
    if (!enabled_) return;

    FinishPendingDownTouches();
    if (!callbacks_.touchMove) return;

    const auto now = std::chrono::steady_clock::now();
    constexpr int kMoveFrameIntervalMs = 16;    // 约 60fps，只控制输出帧率，不控制移动速度。
    constexpr int kMinSmoothDurationMs = 400;  // 最短滑动时间，单位是毫秒
    constexpr int kMaxSmoothDurationMs = 1500;  //最长滑动时间

    const size_t n = (std::min)(profile_.bindings.size(), activeTouches_.size());
    for (size_t i = 0; i < n; ++i) {
        auto& t = activeTouches_[i];
        if (!t.active) continue;

        const auto& b = profile_.bindings[i];
        if (b.actionType != PcMappingActionType::TouchHold) continue;
        if (b.touchMode != PcKeyTouchMode::RandomMove) continue;

        const int uiRadius = PcMappingProfile::ClampButtonRadiusNorm(b.radiusNorm);
        if (uiRadius <= 0) continue;

        if (!t.hasMoveTarget) {
            int targetX = t.curXNorm;
            int targetY = t.curYNorm;
            const int minTravel = (std::max)(1, uiRadius / 8);
            const long long minTravelSq = static_cast<long long>(minTravel) * static_cast<long long>(minTravel);

            // 避免频繁抽到离当前位置太近的点，看起来像原地抖动。
            for (int attempt = 0; attempt < 6; ++attempt) {
                int dx = 0, dy = 0;
                RandomPointInRadius(uiRadius, dx, dy);
                int candidateX = t.baseXNorm + dx;
                int candidateY = t.baseYNorm + dy;
                ClampPair(candidateX, candidateY);

                const long long distX = static_cast<long long>(candidateX) - static_cast<long long>(t.curXNorm);
                const long long distY = static_cast<long long>(candidateY) - static_cast<long long>(t.curYNorm);
                const long long distSq = distX * distX + distY * distY;
                targetX = candidateX;
                targetY = candidateY;
                if (distSq >= minTravelSq || attempt == 5) break;
            }

            const double distance = std::hypot(
                static_cast<double>(targetX - t.curXNorm),
                static_cast<double>(targetY - t.curYNorm));
            const double distanceRatio = (std::min)(1.0, distance / static_cast<double>((std::max)(1, uiRadius)));
            const int interval = (std::max)(8, (std::min)(200, b.randomMoveIntervalMs));
            const int configuredDelay = interval * 8;
            // 据当前点到目标点的距离动态算出来的滑动时间
            const int distanceDelay = 700 + static_cast<int>(std::lround(distanceRatio * 1800.0));
            const int jitter = std::uniform_int_distribution<int>(0, 500)(rng_);

            t.moveStartXNorm = t.curXNorm;
            t.moveStartYNorm = t.curYNorm;
            t.moveTargetXNorm = targetX;
            t.moveTargetYNorm = targetY;
            t.moveDurationMs = (std::max)(kMinSmoothDurationMs, (std::min)(kMaxSmoothDurationMs, configuredDelay + distanceDelay + jitter));
            t.moveStart = now;
            t.hasMoveTarget = true;
        }

        const int duration = (std::max)(1, t.moveDurationMs);
        const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - t.moveStart).count();
        const double linear = (std::max)(0.0, (std::min)(1.0, static_cast<double>(elapsedMs) / static_cast<double>(duration)));

        // smootherstep，比 smoothstep 加速度变化更柔，接近手指拖动的起停。
        const double smooth = linear * linear * linear * (linear * (linear * 6.0 - 15.0) + 10.0);

        int nx = static_cast<int>(std::lround(
            static_cast<double>(t.moveStartXNorm) +
            static_cast<double>(t.moveTargetXNorm - t.moveStartXNorm) * smooth));
        int ny = static_cast<int>(std::lround(
            static_cast<double>(t.moveStartYNorm) +
            static_cast<double>(t.moveTargetYNorm - t.moveStartYNorm) * smooth));
        ClampPair(nx, ny);

        const bool reachedTarget = linear >= 1.0;
        if (!reachedTarget && t.lastMove.time_since_epoch().count() != 0) {
            const auto frameElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - t.lastMove).count();
            if (frameElapsed < kMoveFrameIntervalMs) continue;
        }

        if (nx == t.curXNorm && ny == t.curYNorm) {
            if (reachedTarget) t.hasMoveTarget = false;
            continue;
        }

        if (!RuntimeSlotOwnedBy(i, t.slot)) {
            t = ActiveTouch{};
            continue;
        }

        if (callbacks_.touchMove(t.slot, nx, ny)) {
            t.curXNorm = nx;
            t.curYNorm = ny;
            t.lastMove = now;
            if (reachedTarget) t.hasMoveTarget = false;
        }
    }
}

bool PcMappingRuntime::BindingBelongsToLockedLeftBox(const PcMappingBinding& b) const {
    return b.triggerType == PcMappingTriggerType::MouseButton && b.triggerCode == 1;
}

void PcMappingRuntime::RebuildBindingBuckets() {
    normalBindingIndices_.clear();
    lockedLeftBindingIndices_.clear();
    for (size_t i = 0; i < profile_.bindings.size(); ++i) {
        if (BindingBelongsToLockedLeftBox(profile_.bindings[i])) {
            lockedLeftBindingIndices_.push_back(i);
        }
        else {
            normalBindingIndices_.push_back(i);
        }
    }
}

void PcMappingRuntime::Reset() {
    StopPendingDownTimer();
    for (auto& t : activeTouches_) {
        if (t.active && IsRuntimeTouchSlot(t.slot) && callbacks_.touchUp) {
            callbacks_.touchUp(t.slot);
        }
        t = ActiveTouch{};
    }
    runtimeSlotOwner_.fill(kNoRuntimeSlotOwner);
    slotCooldownUntil_.fill(std::chrono::steady_clock::time_point{});
    std::fill(bindingActive_.begin(), bindingActive_.end(), false);
    vkDown_.fill(false);
    mouseDown_.fill(false);
}

std::wstring PcMappingRuntime::StatusText() const {
    std::wstring s = enabled_ ? L"PC映射=启用" : L"PC映射=关闭";
    s += L" | 绑定 ";
    s += std::to_wstring(static_cast<unsigned long long>(profile_.bindings.size()));
    return s;
}

UINT PcMappingRuntime::NormalizeVk(UINT vk, LPARAM lp) {
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

int PcMappingRuntime::MouseButtonCodeFromMessage(UINT msg, WPARAM wp) {
    switch (msg) {
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
        return 1;
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
        return 2;
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
        return 3;
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
        return GET_XBUTTON_WPARAM(wp) == XBUTTON2 ? 5 : 4;
    case WM_MOUSEWHEEL: {
        const int delta = GET_WHEEL_DELTA_WPARAM(wp);
        if (delta > 0) return 6; // 鼠标滚轮上
        if (delta < 0) return 7; // 鼠标滚轮下
        return 0;
    }
    default:
        return 0;
    }
}

bool PcMappingRuntime::HandleWindowMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LRESULT* outResult) {
    if (outResult) *outResult = 0;
    if (hwnd) hwnd_ = hwnd;

    if (msg == WM_TIMER && wp == kPendingDownTimerId) {
        FinishPendingDownTouches();
        return true;
    }

    if (!enabled_) return false;
    UpdateClientSize(hwnd);

    switch (msg) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        const UINT vk = NormalizeVk(static_cast<UINT>(wp), lp);
        const bool repeat = (lp & 0x40000000L) != 0;
        if (vk < vkDown_.size()) vkDown_[vk] = true;
        return HandleTriggerStateChanged(PcMappingTriggerType::KeyboardVk, static_cast<int>(vk), true, repeat);
    }
    case WM_KEYUP:
    case WM_SYSKEYUP: {
        const UINT vk = NormalizeVk(static_cast<UINT>(wp), lp);
        if (vk < vkDown_.size()) vkDown_[vk] = false;
        return HandleTriggerStateChanged(PcMappingTriggerType::KeyboardVk, static_cast<int>(vk), false, false);
    }
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_XBUTTONDOWN: {
        const int code = MouseButtonCodeFromMessage(msg, wp);
        if (code > 0 && code < static_cast<int>(mouseDown_.size())) mouseDown_[code] = true;
        return code > 0 && HandleTriggerStateChanged(PcMappingTriggerType::MouseButton, code, true, false);
    }
    case WM_MOUSEWHEEL: {
        const int code = MouseButtonCodeFromMessage(msg, wp);
        if (code <= 0 || code >= static_cast<int>(mouseDown_.size())) return false;

        // 滚轮没有按下/抬起状态，这里模拟一次瞬时按下再抬起。
        // 这样 TouchTap、TouchHold、SendLinuxKey 都能获得完整的 down/up 生命周期。
        mouseDown_[code] = true;
        const bool consumedDown = HandleTriggerStateChanged(PcMappingTriggerType::MouseButton, code, true, false);
        mouseDown_[code] = false;
        const bool consumedUp = HandleTriggerStateChanged(PcMappingTriggerType::MouseButton, code, false, false);
        return consumedDown || consumedUp;
    }
    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
    case WM_MBUTTONUP:
    case WM_XBUTTONUP: {
        const int code = MouseButtonCodeFromMessage(msg, wp);
        if (code > 0 && code < static_cast<int>(mouseDown_.size())) mouseDown_[code] = false;
        return code > 0 && HandleTriggerStateChanged(PcMappingTriggerType::MouseButton, code, false, false);
    }
    case WM_KILLFOCUS:
    case WM_CANCELMODE:
        Reset();
        return false;
    default:
        return false;
    }
}

bool PcMappingRuntime::IsBindingComboSatisfied(const PcMappingBinding& b) const {
    if (b.triggerType == PcMappingTriggerType::KeyboardVk) {
        if (!b.comboTriggerCodes.empty()) {
            for (int code : b.comboTriggerCodes) {
                if (code < 0 || code >= static_cast<int>(vkDown_.size()) || !vkDown_[static_cast<size_t>(code)]) {
                    return false;
                }
            }
            return true;
        }
        return b.triggerCode >= 0 && b.triggerCode < static_cast<int>(vkDown_.size()) && vkDown_[static_cast<size_t>(b.triggerCode)];
    }
    if (b.triggerType == PcMappingTriggerType::MouseButton) {
        const bool mouseOk = b.triggerCode > 0 &&
            b.triggerCode < static_cast<int>(mouseDown_.size()) &&
            mouseDown_[static_cast<size_t>(b.triggerCode)];
        if (!mouseOk) return false;
        // 鼠标绑定也允许附带键盘组合，例如 Ctrl+鼠标右键。
        for (int code : b.comboTriggerCodes) {
            if (code < 0 || code >= static_cast<int>(vkDown_.size()) || !vkDown_[static_cast<size_t>(code)]) {
                return false;
            }
        }
        return true;
    }
    return false;
}

bool PcMappingRuntime::HandleTriggerStateChanged(PcMappingTriggerType type, int code, bool down, bool repeat) {
    // 鼠标左键映射放入“锁定专用盒子”：自由模式直接跳过，锁定模式才扫描。
    // 这样自由模式不会为了鼠标左键映射一直走复杂判断，也不会抢走普通投屏点击。
    if (type == PcMappingTriggerType::MouseButton && code == 1) {
        if (!IsLockActive()) return false;
        return HandleTriggerStateChangedInList(lockedLeftBindingIndices_, type, code, down, repeat);
    }

    bool consumed = HandleTriggerStateChangedInList(normalBindingIndices_, type, code, down, repeat);

    // 键盘组合变化时，如果当前处于锁定模式，还要更新“Ctrl/Shift + 鼠标左键”这类锁定专用绑定。
    if (type == PcMappingTriggerType::KeyboardVk && IsLockActive()) {
        consumed = HandleTriggerStateChangedInList(lockedLeftBindingIndices_, type, code, down, repeat) || consumed;
    }
    return consumed;
}

bool PcMappingRuntime::HandleTriggerStateChangedInList(const std::vector<size_t>& indices, PcMappingTriggerType type, int code, bool down, bool repeat) {
    bool consumed = false;
    for (size_t i : indices) {
        if (i >= profile_.bindings.size()) continue;
        const auto& b = profile_.bindings[i];
        if (b.actionType == PcMappingActionType::None) continue;
        // 鼠标+键盘组合下，键盘状态变化也要重新计算鼠标绑定是否仍满足。
        if (b.triggerType != type) {
            const bool keyboardUpdateForMouseCombo =
                (type == PcMappingTriggerType::KeyboardVk &&
                    b.triggerType == PcMappingTriggerType::MouseButton &&
                    !b.comboTriggerCodes.empty());
            if (!keyboardUpdateForMouseCombo) continue;
        }

        bool related = false;
        if (b.triggerType == PcMappingTriggerType::KeyboardVk) {
            if (!b.comboTriggerCodes.empty()) {
                related = std::find(b.comboTriggerCodes.begin(), b.comboTriggerCodes.end(), code) != b.comboTriggerCodes.end();
            }
            else {
                related = b.triggerCode == code;
            }
        }
        else if (b.triggerType == PcMappingTriggerType::MouseButton) {
            // 鼠标绑定：鼠标按钮本身、或附带键盘组合中的任一按键变化，都需要重新计算触发状态。
            related = (b.triggerCode == code) ||
                std::find(b.comboTriggerCodes.begin(), b.comboTriggerCodes.end(), code) != b.comboTriggerCodes.end();
        }
        if (!related) continue;

        const bool nowSatisfied = IsBindingComboSatisfied(b);
        if (bindingActive_.size() < profile_.bindings.size()) bindingActive_.resize(profile_.bindings.size(), false);
        const bool wasSatisfied = bindingActive_[i];

        if (nowSatisfied && !wasSatisfied) {
            bindingActive_[i] = true;
            if (ExecuteBindingAtIndex(i, true, repeat) && b.consumeEvent) consumed = true;
        }
        else if (!nowSatisfied && wasSatisfied) {
            bindingActive_[i] = false;
            if (ExecuteBindingAtIndex(i, false, false) && b.consumeEvent) consumed = true;
        }
        else if (nowSatisfied && wasSatisfied && down && repeat) {
            if (b.consumeEvent) consumed = true;
        }
        else if (b.consumeEvent) {
            consumed = true;
        }
    }
    return consumed;
}

bool PcMappingRuntime::ExecuteBindingAtIndex(size_t bindingIndex, bool down, bool repeat) {
    if (bindingIndex >= profile_.bindings.size()) return false;
    const auto& b = profile_.bindings[bindingIndex];
    if (activeTouches_.size() < profile_.bindings.size()) activeTouches_.resize(profile_.bindings.size());

    switch (b.actionType) {
    case PcMappingActionType::TouchTap: {
        if (!down || repeat || !callbacks_.touchDown || !callbacks_.touchUp) return true;

        const int slot = AllocateRuntimeSlot(bindingIndex);
        if (slot < 0) {
            // 没有空闲物理触点时，宁可丢弃本次 tap，也不能复用 busy slot 形成跨按键滑动。
            return true;
        }

        const int x = PcMappingProfile::ClampNorm(b.xNorm);
        const int y = PcMappingProfile::ClampNorm(b.yNorm);
        const bool downOk = callbacks_.touchDown(slot, x, y);
        if (downOk) {
            callbacks_.touchUp(slot);
        }
        ReleaseRuntimeSlot(bindingIndex, slot, downOk);
        return true;
    }

    // 固定模式
    case PcMappingActionType::TouchHold: {
        if (down) {
            auto& activeTouch = activeTouches_[bindingIndex];
            if (repeat || activeTouch.active || activeTouch.pendingDown || !callbacks_.touchDown) return true;
            if (b.specialAction != PcKeySpecialAction::None && callbacks_.specialAction) {
                callbacks_.specialAction(b.specialAction);
            }

            int downX = PcMappingProfile::ClampNorm(b.xNorm);
            int downY = PcMappingProfile::ClampNorm(b.yNorm);
            const int uiRadius = PcMappingProfile::ClampButtonRadiusNorm(b.radiusNorm);
            if (uiRadius > 0) {
                int dx = 0, dy = 0;
                RandomPointInRadius(uiRadius, dx, dy);
                downX += dx;
                downY += dy;
                ClampPair(downX, downY);
            }

            const uint64_t nextGeneration = activeTouch.generation + 1;
            activeTouch = ActiveTouch{};
            activeTouch.generation = nextGeneration;
            activeTouch.slot = -1; // 物理 slot 在真正 TOUCH_DOWN 时从运行时池动态分配。
            activeTouch.baseXNorm = PcMappingProfile::ClampNorm(b.xNorm);
            activeTouch.baseYNorm = PcMappingProfile::ClampNorm(b.yNorm);
            activeTouch.curXNorm = downX;
            activeTouch.curYNorm = downY;
            activeTouch.hasMoveTarget = false;
            activeTouch.moveStartXNorm = downX;
            activeTouch.moveStartYNorm = downY;
            activeTouch.moveTargetXNorm = downX;
            activeTouch.moveTargetYNorm = downY;
            activeTouch.moveDurationMs = 0;
            activeTouch.moveStart = std::chrono::steady_clock::time_point{};
            activeTouch.lastMove = std::chrono::steady_clock::time_point{};

            const bool instantWheelTrigger =
                b.triggerType == PcMappingTriggerType::MouseButton &&
                (b.triggerCode == 6 || b.triggerCode == 7);

            // 滚轮没有真实“按住”生命周期，WM_MOUSEWHEEL 会在同一条消息里模拟 down+up。
            // 因此滚轮触发的 TouchHold 立即借 slot、立即 down，随后由模拟 up 释放。
            if (instantWheelTrigger) {
                const int slot = AllocateRuntimeSlot(bindingIndex);
                if (slot < 0) {
                    activeTouch = ActiveTouch{};
                    return true;
                }
                activeTouch.slot = slot;

                if (callbacks_.touchDown(activeTouch.slot, activeTouch.curXNorm, activeTouch.curYNorm)) {
                    activeTouch.active = true;
                    activeTouch.lastMove = std::chrono::steady_clock::now();
                }
                else {
                    ReleaseRuntimeSlot(bindingIndex, activeTouch.slot, false);
                    activeTouch = ActiveTouch{};
                }
                return true;
            }

            activeTouch.pendingDown = true;
            activeTouch.pendingDownDue = std::chrono::steady_clock::now() +
                std::chrono::milliseconds(PcNormalKeyOptionLimits::kPendingDownDelayMs);
            SchedulePendingDownTimer(hwnd_);
        }
        else {
            ReleaseBindingTouch(bindingIndex);
        }
        return true;
    }

    case PcMappingActionType::SendLinuxKey:
        if (!callbacks_.key || b.linuxKeyCode <= 0) return true;
        callbacks_.key(b.linuxKeyCode, down);
        return true;

    case PcMappingActionType::None:
    default:
        return false;
    }
}

void PcMappingRuntime::SchedulePendingDownTimer(HWND hwnd) {
    if (!hwnd || pendingDownTimerActive_) return;
    SetTimer(hwnd, kPendingDownTimerId, 1, nullptr);
    pendingDownTimerActive_ = true;
}

void PcMappingRuntime::StopPendingDownTimer() {
    if (!pendingDownTimerActive_) return;
    if (hwnd_) KillTimer(hwnd_, kPendingDownTimerId);
    pendingDownTimerActive_ = false;
}

bool PcMappingRuntime::HasPendingDownTouches() const {
    for (const auto& t : activeTouches_) {
        if (t.pendingDown) return true;
    }
    return false;
}

void PcMappingRuntime::FinishPendingDownTouches() {
    if (activeTouches_.empty()) {
        StopPendingDownTimer();
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    const size_t n = (std::min)(profile_.bindings.size(), activeTouches_.size());
    for (size_t i = 0; i < n; ++i) {
        auto& t = activeTouches_[i];
        if (!t.pendingDown) continue;
        if (now < t.pendingDownDue) continue;

        const bool stillSatisfied = i < bindingActive_.size() && bindingActive_[i];
        const auto& b = profile_.bindings[i];
        if (!enabled_ || !stillSatisfied || b.actionType != PcMappingActionType::TouchHold || !callbacks_.touchDown) {
            t = ActiveTouch{};
            continue;
        }

        const int slot = AllocateRuntimeSlot(i);
        if (slot < 0) {
            // 当前没有空闲物理触点，继续保持 pending；松开触发键时会被 ReleaseBindingTouch 取消。
            continue;
        }
        t.slot = slot;

        if (callbacks_.touchDown(t.slot, t.curXNorm, t.curYNorm)) {
            t.pendingDown = false;
            t.active = true;
            t.hasMoveTarget = false;
            t.moveStart = std::chrono::steady_clock::time_point{};
            t.lastMove = now;
        }
        else {
            ReleaseRuntimeSlot(i, t.slot, false);
            t = ActiveTouch{};
        }
    }

    if (!HasPendingDownTouches()) {
        StopPendingDownTimer();
    }
}

void PcMappingRuntime::ReleaseBindingTouch(size_t bindingIndex) {
    if (bindingIndex >= activeTouches_.size()) return;
    auto& t = activeTouches_[bindingIndex];

    if (t.pendingDown) {
        t = ActiveTouch{};
        if (!HasPendingDownTouches()) StopPendingDownTimer();
        return;
    }

    if (t.active && RuntimeSlotOwnedBy(bindingIndex, t.slot) && callbacks_.touchUp) {
        callbacks_.touchUp(t.slot);
        ReleaseRuntimeSlot(bindingIndex, t.slot, true);
    }
    else if (IsRuntimeTouchSlot(t.slot)) {
        // 防御：如果状态异常导致 owner 不一致，不能 touchUp 别人的 slot，只清理本 binding 状态。
        ReleaseRuntimeSlot(bindingIndex, t.slot, false);
    }
    t = ActiveTouch{};
}
