#include "pc_compass_runtime.h"

#include <algorithm>
#include <cmath>
#include <windowsx.h>
#include <utility>

namespace {
    static constexpr double kPi = 3.14159265358979323846;
    static long long nowMs() {
        using namespace std::chrono;
        return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
    }
    static long long nowUs() {
        using namespace std::chrono;
        return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
    }

    static double compassAngleDiff(double a, double b) {
        double d = a - b;
        while (d > kPi) d -= 2.0 * kPi;
        while (d < -kPi) d += 2.0 * kPi;
        return d;
    }

    static int clampSectorPercent(int v) {
        return (std::max)(PC_COMPASS_SWAY_SECTOR_MIN, (std::min)(PC_COMPASS_SWAY_SECTOR_MAX, v));
    }

    static int clampDiagonalSectorPercent(int v) {
        return (std::max)(PC_COMPASS_SWAY_DIAG_SECTOR_MIN, (std::min)(PC_COMPASS_SWAY_DIAG_SECTOR_MAX, v));
    }

    static double diagAnchorAngle(const PcCompassBinding& c, int slot) {
        int ax = -1;
        int ay = -1;
        switch (slot) {
        case 0: ax = c.diagLeftUpXNorm; ay = c.diagLeftUpYNorm; break;
        case 1: ax = c.diagRightUpXNorm; ay = c.diagRightUpYNorm; break;
        case 2: ax = c.diagLeftDownXNorm; ay = c.diagLeftDownYNorm; break;
        case 3: ax = c.diagRightDownXNorm; ay = c.diagRightDownYNorm; break;
        default: break;
        }
        if (ax >= 0 && ay >= 0) {
            const double vx = static_cast<double>(PcMappingProfile::ClampNorm(ax) - c.centerXNorm);
            const double vy = static_cast<double>(PcMappingProfile::ClampNorm(ay) - c.centerYNorm);
            if (std::hypot(vx, vy) > 0.0001) return std::atan2(vy, vx);
        }
        static const double fallback[4] = { -3.0 * kPi / 4.0, -kPi / 4.0, 3.0 * kPi / 4.0, kPi / 4.0 };
        return fallback[(std::max)(0, (std::min)(3, slot))];
    }

    static int diagSectorPercentForDirection(const PcCompassBinding& c, double baseX, double baseY) {
        const double a = std::atan2(baseY, baseX);
        int bestSlot = 0;
        double best = 100.0;
        for (int i = 0; i < 4; ++i) {
            const double d = std::fabs(compassAngleDiff(a, diagAnchorAngle(c, i)));
            if (d < best) { best = d; bestSlot = i; }
        }
        switch (bestSlot) {
        case 0: return clampDiagonalSectorPercent(c.swayDiagLeftUpSectorPercent);
        case 1: return clampDiagonalSectorPercent(c.swayDiagRightUpSectorPercent);
        case 2: return clampDiagonalSectorPercent(c.swayDiagLeftDownSectorPercent);
        case 3: return clampDiagonalSectorPercent(c.swayDiagRightDownSectorPercent);
        default: return clampDiagonalSectorPercent(c.swayDiagonalSectorPercent);
        }
    }

    static int compassSectorPercentForDirection(const PcCompassBinding& c, double baseX, double baseY, int keyCount) {
        return (keyCount >= 2) ? diagSectorPercentForDirection(c, baseX, baseY) : clampSectorPercent(c.swaySectorPercent);
    }

    static bool fixedDiagonalDirection(const PcCompassBinding& c, int dx, int dy, double& outX, double& outY) {
        int ax = -1;
        int ay = -1;
        if (dx < 0 && dy < 0) {
            ax = c.diagLeftUpXNorm;
            ay = c.diagLeftUpYNorm;
        }
        else if (dx > 0 && dy < 0) {
            ax = c.diagRightUpXNorm;
            ay = c.diagRightUpYNorm;
        }
        else if (dx < 0 && dy > 0) {
            ax = c.diagLeftDownXNorm;
            ay = c.diagLeftDownYNorm;
        }
        else if (dx > 0 && dy > 0) {
            ax = c.diagRightDownXNorm;
            ay = c.diagRightDownYNorm;
        }
        else {
            return false;
        }
        if (ax < 0 || ay < 0) return false;
        const int x = PcMappingProfile::ClampNorm(ax);
        const int y = PcMappingProfile::ClampNorm(ay);
        const double vx = static_cast<double>(x - c.centerXNorm);
        const double vy = static_cast<double>(y - c.centerYNorm);
        const double len = std::hypot(vx, vy);
        if (len <= 0.0001) return false;
        outX = vx / len;
        outY = vy / len;
        return true;
    }


    static bool physicalVkDown(int vk) {
        if (vk <= 0 || vk >= 256) return false;
        return (GetAsyncKeyState(vk) & 0x8000) != 0;
    }

    static void syncModifierPhysicalState(UINT genericVk, UINT leftVk, UINT rightVk, std::array<bool, 256>& vkDown) {
        const bool leftDown = physicalVkDown(static_cast<int>(leftVk));
        const bool rightDown = physicalVkDown(static_cast<int>(rightVk));
        const bool genericDown = physicalVkDown(static_cast<int>(genericVk)) || leftDown || rightDown;
        vkDown[genericVk] = genericDown;
        vkDown[leftVk] = leftDown;
        vkDown[rightVk] = rightDown;
    }

    static void syncOneCompassVkPhysical(int code, std::array<bool, 256>& vkDown) {
        if (code <= 0 || code >= static_cast<int>(vkDown.size())) return;
        if (code == VK_SHIFT || code == VK_LSHIFT || code == VK_RSHIFT) {
            syncModifierPhysicalState(VK_SHIFT, VK_LSHIFT, VK_RSHIFT, vkDown);
            return;
        }
        if (code == VK_CONTROL || code == VK_LCONTROL || code == VK_RCONTROL) {
            syncModifierPhysicalState(VK_CONTROL, VK_LCONTROL, VK_RCONTROL, vkDown);
            return;
        }
        if (code == VK_MENU || code == VK_LMENU || code == VK_RMENU) {
            syncModifierPhysicalState(VK_MENU, VK_LMENU, VK_RMENU, vkDown);
            return;
        }
        vkDown[static_cast<size_t>(code)] = physicalVkDown(code);
    }

    static void syncCompassButtonPhysicalState(const PcCompassButtonBinding& b, std::array<bool, 256>& vkDown) {
        for (int code : b.comboTriggerCodes) {
            syncOneCompassVkPhysical(code, vkDown);
        }
    }

    static void syncCompassKeyboardPhysicalState(const PcCompassBinding& c, std::array<bool, 256>& vkDown) {
        // 菜单/道具/背包 active 时，键盘消息可能被菜单优先处理或被系统吞掉。
        // WASD 摇杆的真实状态必须以物理按键为准，不能只依赖 WM_KEYUP。
        syncCompassButtonPhysicalState(c.up, vkDown);
        syncCompassButtonPhysicalState(c.left, vkDown);
        syncCompassButtonPhysicalState(c.down, vkDown);
        syncCompassButtonPhysicalState(c.right, vkDown);
        syncCompassButtonPhysicalState(c.center, vkDown);
    }

}

void PcCompassRuntime::SetCallbacks(Callbacks cb) {
    callbacks_ = std::move(cb);
}

void PcCompassRuntime::SetProfileProvider(std::function<const PcMappingProfile& ()> provider) {
    profileProvider_ = std::move(provider);
}

void PcCompassRuntime::Reset() {
    ReleaseTouch();
    vkDown_.fill(false);
    upPressed_ = leftPressed_ = downPressed_ = rightPressed_ = centerPressed_ = false;
    swayBiasRad_ = 0.0;
    lastMouseDx_ = lastMouseDy_ = 0;
    mouseLeftDown_ = mouseRightDown_ = false;
    mouseFilteredDx_ = mouseFilteredDy_ = 0.0;
    lastMouseMs_ = 0;
    lastTouchFrameUs_ = 0;
    zeroSinceMs_ = -1;
    ResetParticleState();
}

std::wstring PcCompassRuntime::StatusText() const {
    if (!profileProvider_ || profileProvider_().compasses.empty()) return L"轮盘=未创建";
    return active_ ? L"轮盘=触发" : L"轮盘=就绪";
}

UINT PcCompassRuntime::NormalizeVk(UINT vk, LPARAM lp) {
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

int PcCompassRuntime::ClampInt(int v, int lo, int hi) {
    return (std::max)(lo, (std::min)(hi, v));
}

double PcCompassRuntime::ClampDouble(double v, double lo, double hi) {
    return (std::max)(lo, (std::min)(hi, v));
}

bool PcCompassRuntime::ComboSatisfied(const PcCompassButtonBinding& b, const std::array<bool, 256>& vkDown) {
    if (b.comboTriggerCodes.empty()) return false;
    for (int code : b.comboTriggerCodes) {
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

bool PcCompassRuntime::IsRelatedKey(const PcCompassButtonBinding& b, int vk) {
    for (int c : b.comboTriggerCodes) {
        if (c == vk) return true;
        if ((c == VK_SHIFT || c == VK_LSHIFT || c == VK_RSHIFT) && (vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT)) return true;
        if ((c == VK_CONTROL || c == VK_LCONTROL || c == VK_RCONTROL) && (vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL)) return true;
        if ((c == VK_MENU || c == VK_LMENU || c == VK_RMENU) && (vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU)) return true;
    }
    return false;
}

bool PcCompassRuntime::AnyCompassKeyRelated(int vk) const {
    if (!profileProvider_ || profileProvider_().compasses.empty()) return false;
    const auto& c = profileProvider_().compasses.front();
    return IsRelatedKey(c.up, vk) || IsRelatedKey(c.left, vk) || IsRelatedKey(c.down, vk) || IsRelatedKey(c.right, vk) || IsRelatedKey(c.center, vk);
}

bool PcCompassRuntime::UpdatePressedStates(const PcCompassBinding& c) {
    const bool oldAny = upPressed_ || leftPressed_ || downPressed_ || rightPressed_ || centerPressed_;
    upPressed_ = ComboSatisfied(c.up, vkDown_);
    leftPressed_ = ComboSatisfied(c.left, vkDown_);
    downPressed_ = ComboSatisfied(c.down, vkDown_);
    rightPressed_ = ComboSatisfied(c.right, vkDown_);
    centerPressed_ = ComboSatisfied(c.center, vkDown_);
    const bool newAny = upPressed_ || leftPressed_ || downPressed_ || rightPressed_ || centerPressed_;
    return oldAny || newAny;
}

int PcCompassRuntime::RadiusNormToAxisNorm(int radiusNorm, bool) {
    return PcMappingProfile::ClampRadiusNorm(radiusNorm);
}

bool PcCompassRuntime::UseOuterBand(const PcCompassBinding& c) const {
    return c.centerOuterReversed ? !centerPressed_ : centerPressed_;
}

int PcCompassRuntime::ActiveRadiusNorm(const PcCompassBinding& c) const {
    return (std::max)(1, RadiusNormToAxisNorm(UseOuterBand(c) ? c.outerRadiusNorm : c.innerRadiusNorm, true));
}

int PcCompassRuntime::FixedRawActiveRadiusNorm(const PcCompassBinding& c) const {
    return (std::max)(1, RadiusNormToAxisNorm(UseOuterBand(c) ? c.outerRadiusNorm : c.innerRadiusNorm, true));
}

bool PcCompassRuntime::CurrentClientSize(int& outW, int& outH) const {
    outW = 0;
    outH = 0;
    if (lastHwnd_ && IsWindow(lastHwnd_)) {
        RECT rc{};
        if (GetClientRect(lastHwnd_, &rc)) {
            outW = static_cast<int>(rc.right - rc.left);
            outH = static_cast<int>(rc.bottom - rc.top);
        }
    }
    if (outW > 1 && outH > 1) return true;

    // 兜底用固定横屏触摸面尺寸。摇杆半径现在是“屏幕像素半径”，
    // 不能在 lastHwnd_ 还没建立/失效时退回到裸 norm 半径，否则 1920x1080 下
    // X/Y 半径会被当成同一个 norm 值，首帧随机点可能跑出屏幕上看到的内圈。
    outW = 1920;
    outH = 1080;
    return true;
}

int PcCompassRuntime::VisualCircleRadiusPx(int radiusNorm) const {
    // 轮盘半径滑条的显示值直接按 PC 像素/DP 使用：
    // 80 -> 80px，240 -> 240px。保存字段仍然沿用 Norm 存储，即 240 保存为 240000。
    // 不再按 min(width,height) 缩放，否则 1920x800 窗口下 240 会被显示成 192px。
    const int radiusPx = PcMappingProfile::ClampRadiusNorm(radiusNorm) / PC_COMPASS_RADIUS_UNIT;
    return (std::max)(20, radiusPx);
}

double PcCompassRuntime::FixedVisualMaxNormRadiusForDirection(const PcCompassBinding& c, double dirX, double dirY) const {
    const int rawRadius = UseOuterBand(c) ? c.outerRadiusNorm : c.innerRadiusNorm;
    const int radiusPx = VisualCircleRadiusPx(rawRadius);
    if (radiusPx <= 0) return static_cast<double>(FixedRawActiveRadiusNorm(c));

    int w = 0;
    int h = 0;
    if (!CurrentClientSize(w, h)) return static_cast<double>(FixedRawActiveRadiusNorm(c));

    const double len = std::hypot(dirX, dirY);
    if (len <= 0.0001) return static_cast<double>(FixedRawActiveRadiusNorm(c));
    dirX /= len;
    dirY /= len;

    // norm 半径 r 在窗口像素里对应 (r*dirX*w/1e6, r*dirY*h/1e6)。
    // 反推让像素距离刚好等于 UI 圆半径的 r，得到当前方向可用的最大 norm 半径。
    const double denom = std::sqrt((dirX * static_cast<double>(w)) * (dirX * static_cast<double>(w)) +
                                   (dirY * static_cast<double>(h)) * (dirY * static_cast<double>(h)));
    if (denom <= 0.0001) return static_cast<double>(FixedRawActiveRadiusNorm(c));
    return (static_cast<double>(radiusPx) * 1000000.0) / denom;
}

void PcCompassRuntime::ClampPointToVisualCircleRadiusNorm(int radiusNorm, double centerX, double centerY, double& x, double& y) const {
    const int radiusPx = VisualCircleRadiusPx(radiusNorm);
    if (radiusPx <= 0) return;

    int w = 0;
    int h = 0;
    if (!CurrentClientSize(w, h)) return;

    const double dxNorm = x - centerX;
    const double dyNorm = y - centerY;
    const double dxPx = dxNorm * static_cast<double>(w) / 1000000.0;
    const double dyPx = dyNorm * static_cast<double>(h) / 1000000.0;
    const double distPx = std::hypot(dxPx, dyPx);
    if (distPx <= static_cast<double>(radiusPx) || distPx <= 0.0001) return;

    const double scale = static_cast<double>(radiusPx) / distPx;
    x = centerX + dxNorm * scale;
    y = centerY + dyNorm * scale;
}

void PcCompassRuntime::ClampFixedPointToVisualCircle(const PcCompassBinding& c, double centerX, double centerY, double& x, double& y) const {
    const int rawRadius = UseOuterBand(c) ? c.outerRadiusNorm : c.innerRadiusNorm;
    ClampPointToVisualCircleRadiusNorm(rawRadius, centerX, centerY, x, y);
}

int PcCompassRuntime::FixedActiveRadiusNorm(const PcCompassBinding& c) const {
    // 给固定模式的速度/目标间距使用一个保守标量半径；真正边界由 ClampFixedPointToVisualCircle
    // 按屏幕 UI 圆做像素级限制。
    const int raw = FixedRawActiveRadiusNorm(c);
    const double rx = FixedVisualMaxNormRadiusForDirection(c, 1.0, 0.0);
    const double ry = FixedVisualMaxNormRadiusForDirection(c, 0.0, 1.0);
    return (std::max)(1, static_cast<int>(std::lround((std::min)(static_cast<double>(raw), (std::min)(rx, ry)))));
}

bool PcCompassRuntime::MouseSwayDirectionAllowsOuter(int dx, int dy, double dirX, double dirY) const {
    // 晃动模式专用：不按外环键时，也允许非上半区方向使用外圈作为“最大半径”。
    // W / W+A / W+D 属于上半区，仍锁在内圈，避免朝上方向误触外环。
    // A / D / S / S+A / S+D 属于非上半区，可在内圈到外圈之间做扇形游走。
    if (dx == 0 && dy == 0) return false;
    if (dy < 0) return false;
    if (dy > 0) return true;
    if (dx != 0) return true;
    return dirY >= -0.05;
}

bool PcCompassRuntime::MouseSwayUseWideRadius(const PcCompassBinding& c, int dx, int dy, double dirX, double dirY) const {
    // 这段“非上半区方向允许使用外圈”的规则只能给 MouseSway 晃动模式使用。
    // FixedCenter 固定模式必须只按中心键/反转状态决定内外圈，不能因为 A/D/S/SA/SD 自动切到外圈。
    if (c.motionMode != PcCompassMotionMode::MouseSway) return UseOuterBand(c);

    const int inner = (std::max)(1, RadiusNormToAxisNorm(c.innerRadiusNorm, true));
    const int outer = (std::max)(1, RadiusNormToAxisNorm(c.outerRadiusNorm, true));
    if (outer <= inner + 1) return false;
    return UseOuterBand(c) || MouseSwayDirectionAllowsOuter(dx, dy, dirX, dirY);
}

int PcCompassRuntime::MouseSwayActiveRadiusNorm(const PcCompassBinding& c, int dx, int dy, double dirX, double dirY) const {
    // 防御式保护：即使以后误调用，固定模式也不会走晃动模式的方向外圈逻辑。
    if (c.motionMode != PcCompassMotionMode::MouseSway) {
        return ActiveRadiusNorm(c);
    }
    if (MouseSwayUseWideRadius(c, dx, dy, dirX, dirY)) {
        return (std::max)(1, RadiusNormToAxisNorm(c.outerRadiusNorm, true));
    }
    return (std::max)(1, RadiusNormToAxisNorm(c.innerRadiusNorm, true));
}

double PcCompassRuntime::TurnScaleX(const PcCompassBinding& c) const {
    return ClampDouble(static_cast<double>(c.speedXStep) / 10.0, 0.25, 3.2);
}

double PcCompassRuntime::TurnScaleY(const PcCompassBinding& c) const {
    return ClampDouble(static_cast<double>(c.speedYStep) / 10.0, 0.25, 3.2);
}

double PcCompassRuntime::RandRange(double lo, double hi) {
    std::uniform_real_distribution<double> dist(lo, hi);
    return dist(rng_);
}

void PcCompassRuntime::ComputeDirection(const PcCompassBinding&, double& outX, double& outY, int& keyCount, int& dx, int& dy) {
    dx = 0;
    dy = 0;
    if (leftPressed_) dx -= 1;
    if (rightPressed_) dx += 1;
    if (upPressed_) dy -= 1;
    if (downPressed_) dy += 1;
    keyCount = (upPressed_ ? 1 : 0) + (leftPressed_ ? 1 : 0) + (downPressed_ ? 1 : 0) + (rightPressed_ ? 1 : 0);
    if (dx == 0 && dy == 0) {
        outX = 0.0;
        outY = 0.0;
        return;
    }
    const double len = std::sqrt(double(dx * dx + dy * dy));
    outX = double(dx) / len;
    outY = double(dy) / len;
}

bool PcCompassRuntime::TryGetCustomDirection(const PcCompassBinding& c, int dx, int dy, double& outX, double& outY) const {
    int ax = -1;
    int ay = -1;
    if (dx < 0 && dy < 0) {
        ax = c.diagLeftUpXNorm; ay = c.diagLeftUpYNorm;
    }
    else if (dx > 0 && dy < 0) {
        ax = c.diagRightUpXNorm; ay = c.diagRightUpYNorm;
    }
    else if (dx < 0 && dy > 0) {
        ax = c.diagLeftDownXNorm; ay = c.diagLeftDownYNorm;
    }
    else if (dx > 0 && dy > 0) {
        ax = c.diagRightDownXNorm; ay = c.diagRightDownYNorm;
    }
    else {
        return false;
    }
    if (ax < 0 || ay < 0) return false;
    const double vx = static_cast<double>(PcMappingProfile::ClampNorm(ax) - c.centerXNorm);
    const double vy = static_cast<double>(PcMappingProfile::ClampNorm(ay) - c.centerYNorm);
    const double len = std::sqrt(vx * vx + vy * vy);
    if (len <= 0.0001) return false;
    outX = vx / len;
    outY = vy / len;
    return true;
}

void PcCompassRuntime::ApplyMouseSway(const PcCompassBinding& c, double baseX, double baseY, int keyCount, double& outX, double& outY) {
    const double baseLen = std::sqrt(baseX * baseX + baseY * baseY);
    if (baseLen <= 0.0001) {
        swayBiasRad_ *= 0.86;
        outX = baseX;
        outY = baseY;
        return;
    }
    baseX /= baseLen;
    baseY /= baseLen;

    const long long age = nowMs() - lastMouseMs_;
    double tangentInput = 0.0;
    if (age <= 120) {
        const double tangentX = -baseY;
        const double tangentY = baseX;
        tangentInput = mouseFilteredDx_ * tangentX + mouseFilteredDy_ * tangentY;
    }

    // 鼠标只提供“当前方向扇区内”的偏航趋势，不直接控制触点位置。
    // 100% = 每个方向完整 1/16 扇区，即方向中心 ±22.5°。
    const int sectorRaw = compassSectorPercentForDirection(c, baseX, baseY, keyCount);
    const double sectorPercent = ClampDouble(static_cast<double>(sectorRaw) / 100.0, 0.25, 1.40);
    const double fullSectorHalf = 22.5 * kPi / 180.0;
    const double halfWidth = fullSectorHalf * sectorPercent;
    const double fullDeflect = (keyCount >= 2) ? 36.0 : 31.0;
    const double target = ClampDouble(tangentInput / fullDeflect, -1.0, 1.0) * halfWidth;
    const bool moving = std::fabs(tangentInput) > 0.18;
    const double follow = moving ? 0.11 : 0.038;
    swayBiasRad_ = swayBiasRad_ * (1.0 - follow) + target * follow;
    if (!moving && age > 90) swayBiasRad_ *= 0.965;
    swayBiasRad_ = ClampDouble(swayBiasRad_, -halfWidth, halfWidth);

    const double a = std::atan2(baseY, baseX) + swayBiasRad_;
    outX = std::cos(a);
    outY = std::sin(a);
}

void PcCompassRuntime::ResetParticleState() {
    currentXNorm_ = 500000.0;
    currentYNorm_ = 500000.0;
    targetXNorm_ = 500000.0;
    targetYNorm_ = 500000.0;
    dynamicTargetXNorm_ = 500000.0;
    dynamicTargetYNorm_ = 500000.0;
    velXNorm_ = 0.0;
    velYNorm_ = 0.0;
    keyboardDirX_ = 0.0;
    keyboardDirY_ = 0.0;
    lastTargetDirX_ = 0.0;
    lastTargetDirY_ = 0.0;
    lastPerturbX_ = 0.0;
    lastPerturbY_ = 0.0;
    justSwitchedDirection_ = false;
    switchBoostFrames_ = 0;
    isSingleDirection_ = false;
    targetUpdateCounter_ = 0;
    swayBiasRad_ = 0.0;
    swayRadiusNorm_ = 0.0;
    swayTargetRadiusNorm_ = 0.0;
    swayAngleOffsetRad_ = 0.0;
    swayTargetAngleOffsetRad_ = 0.0;
    swayAxialNorm_ = 0.0;
    swayLateralNorm_ = 0.0;
    swayTargetAxialNorm_ = 0.0;
    swayTargetLateralNorm_ = 0.0;
    swayGoalXNorm_ = 0.0;
    swayGoalYNorm_ = 0.0;
    swayMoveSpeedScale_ = 1.0;
    swayNextTargetMs_ = 0;
    lastSwayNoiseMs_ = 0;
    swayLastDx_ = 0;
    swayLastDy_ = 0;
    swayLastOuter_ = false;
    swayLastMouseSmall_ = false;
    swayLastWideRadius_ = false;
    swayDirectionalOuterAllowed_ = false;
    swayDirectionSwitchUntilMs_ = 0;
    mouseSmallUntilMs_ = 0;
    lastMouseButtonDownMs_ = 0;
    mouseFilteredDx_ *= 0.35;
    mouseFilteredDy_ *= 0.35;
}

bool PcCompassRuntime::ShouldRunTouchFrame(const PcCompassBinding&, bool hasDirectionInput) const {
    const long long now = nowUs();
    if (lastTouchFrameUs_ == 0) return true;
    // 固定内部防洪间隔，不再暴露“触摸 Hz”参数。
    // 实际手感由扇区目标点距离和每段随机速度决定。
    const long long wanted = hasDirectionInput ? 5000LL : 2500LL;
    return (now - lastTouchFrameUs_) >= wanted;
}

void PcCompassRuntime::MarkTouchFrameEmitted() {
    lastTouchFrameUs_ = nowUs();
}

void PcCompassRuntime::PressInitial(const PcCompassBinding& c, int activeRadius, double dirX, double dirY, int keyCount, bool mouseSwayMode) {
    const int safeActive = (std::max)(1, activeRadius);
    const int innerPx = VisualCircleRadiusPx(c.innerRadiusNorm);
    const int activePx = VisualCircleRadiusPx(safeActive);
    const int basePx = (std::max)(1, (std::min)(innerPx, activePx));

    double angle = RandRange(0.0, 2.0 * kPi);
    if (keyCount > 0 && (std::fabs(dirX) > 0.0001 || std::fabs(dirY) > 0.0001)) {
        angle = std::atan2(dirY, dirX) + RandRange(mouseSwayMode ? -38.0 : -12.0, mouseSwayMode ? 38.0 : 12.0) * kPi / 180.0;
    }

    // 首次 down 必须在“屏幕像素内圈”里随机，再换算回 norm。
    // 不能先用同一个 norm 半径随机，再靠 clamp 修正；1920x1080 下同样的 norm
    // 在 X/Y 上对应的像素距离不同，横向会偏大，偶发看起来像跑到外径。
    const double rPx = static_cast<double>(basePx) * RandRange(mouseSwayMode ? 0.22 : 0.08, mouseSwayMode ? 0.62 : 0.50);
    int w = 1920;
    int h = 1080;
    CurrentClientSize(w, h);
    double downX = static_cast<double>(PcMappingProfile::ClampNorm(c.centerXNorm)) + std::cos(angle) * rPx * 1000000.0 / static_cast<double>((std::max)(1, w));
    double downY = static_cast<double>(PcMappingProfile::ClampNorm(c.centerYNorm)) + std::sin(angle) * rPx * 1000000.0 / static_cast<double>((std::max)(1, h));

    // 防御性再限制一次，确保首帧绝不超过屏幕 UI 内圈。
    ClampPointToVisualCircleRadiusNorm(c.innerRadiusNorm, static_cast<double>(c.centerXNorm), static_cast<double>(c.centerYNorm), downX, downY);
    currentXNorm_ = PcMappingProfile::ClampNorm(static_cast<int>(std::lround(downX)));
    currentYNorm_ = PcMappingProfile::ClampNorm(static_cast<int>(std::lround(downY)));
    targetXNorm_ = currentXNorm_;
    targetYNorm_ = currentYNorm_;
    dynamicTargetXNorm_ = currentXNorm_;
    dynamicTargetYNorm_ = currentYNorm_;
    velXNorm_ = velYNorm_ = 0.0;
    lastPerturbX_ = lastPerturbY_ = 0.0;
    active_ = callbacks_.touchDown ? callbacks_.touchDown(slot_, static_cast<int>(std::lround(currentXNorm_)), static_cast<int>(std::lround(currentYNorm_))) : false;
    if (active_) MarkTouchFrameEmitted();
}

void PcCompassRuntime::UpdateDirectionTargetFixed(
    const PcCompassBinding& c,
    double centerX,
    double centerY,
    int dx,
    int dy,
    int keyCount,
    int activeRadius,
    bool hasCustomDirection,
    double customDirX,
    double customDirY
) {
    double dirX = 0.0;
    double dirY = 0.0;
    bool hasDir = false;

    if (hasCustomDirection) {
        dirX = customDirX;
        dirY = customDirY;
        const double len = std::sqrt(dirX * dirX + dirY * dirY);
        if (len > 0.0001) {
            dirX /= len;
            dirY /= len;
            hasDir = true;
        }
    }
    else if (dx != 0 || dy != 0) {
        const double len = std::sqrt(double(dx * dx + dy * dy));
        if (len > 0.0001) {
            dirX = double(dx) / len;
            dirY = double(dy) / len;
            hasDir = true;
        }
    }

    if (!hasDir) {
        targetXNorm_ = centerX;
        targetYNorm_ = centerY;
        dynamicTargetXNorm_ = centerX;
        dynamicTargetYNorm_ = centerY;
        swayGoalXNorm_ = centerX;
        swayGoalYNorm_ = centerY;
        swayNextTargetMs_ = 0;
        keyboardDirX_ = 0.0;
        keyboardDirY_ = 0.0;
        lastTargetDirX_ = 0.0;
        lastTargetDirY_ = 0.0;
        lastPerturbX_ = 0.0;
        lastPerturbY_ = 0.0;
        targetUpdateCounter_ = 0;
        justSwitchedDirection_ = false;
        switchBoostFrames_ = 0;
        isSingleDirection_ = false;
        return;
    }

    // 固定模式不再是“死点”，但也不使用鼠标稳定/RawInput 偏航。
    // 它只在当前按键方向的小扇区内做低速平滑游走：
    //   正向/斜向最大扇区来自 pc_mapping_profile.h 的 PC_COMPASS_FIXED_SOFT_* 参数；
    //   默认速度很低，避免边界处快速抖动。
    keyboardDirX_ = dirX;
    keyboardDirY_ = dirY;
    lastTargetDirX_ = dirX;
    lastTargetDirY_ = dirY;
    isSingleDirection_ = keyCount == 1;

    const bool outer = UseOuterBand(c);
    const long long now = nowMs();
    const bool directionChanged =
        (dx != swayLastDx_) ||
        (dy != swayLastDy_) ||
        (outer != swayLastOuter_);

    if (directionChanged) {
        swayLastDx_ = dx;
        swayLastDy_ = dy;
        swayLastOuter_ = outer;
        swayLastMouseSmall_ = false;
        swayNextTargetMs_ = 0;
        justSwitchedDirection_ = true;
        switchBoostFrames_ = 3;
        swayDirectionSwitchUntilMs_ = now + 160;
        // 切换方向时先消掉大部分旧方向速度，避免到达新方向边界后横向甩动。
        velXNorm_ *= 0.35;
        velYNorm_ *= 0.35;
        lastPerturbX_ = 0.0;
        lastPerturbY_ = 0.0;
    }

    const auto pickNextFixedTarget = [&]() {
        const bool diagonal = keyCount >= 2;
        const int rawSectorPct = diagonal
            ? PC_COMPASS_FIXED_SOFT_SWAY_DIAG_SECTOR_PERCENT
            : PC_COMPASS_FIXED_SOFT_SWAY_SECTOR_PERCENT;
        // 固定模式最大只允许 30% 当前方向扇区，避免跨到相邻方向。
        const double sectorPercent = ClampDouble(static_cast<double>(rawSectorPct) / 100.0, 0.0, 0.30);
        const double fullSectorHalf = 22.5 * kPi / 180.0;
        const double sectorHalf = fullSectorHalf * sectorPercent;
        const double baseAngle = std::atan2(dirY, dirX);

        const double fallbackMinR = activeRadius * (diagonal ? 0.86 : 0.82);
        const double fallbackMaxR = (std::max)(fallbackMinR + 1.0, activeRadius * (diagonal ? 1.00 : 0.98));
        const double minTravel = (std::max)(900.0, activeRadius * 0.035);
        const double maxTravel = (std::max)(minTravel + 1.0, activeRadius * 0.24);

        double bestX = centerX + dirX * ((fallbackMinR + fallbackMaxR) * 0.5);
        double bestY = centerY + dirY * ((fallbackMinR + fallbackMaxR) * 0.5);
        double bestScore = 1e100;

        for (int i = 0; i < 48; ++i) {
            const double offset = (sectorHalf <= 0.000001) ? 0.0 : RandRange(-sectorHalf, sectorHalf);
            const double angle = baseAngle + offset;
            const double ax = std::cos(angle);
            const double ay = std::sin(angle);
            const double visualLimit = (std::max)(1.0, FixedVisualMaxNormRadiusForDirection(c, ax, ay));
            const double minR = visualLimit * (diagonal ? 0.86 : 0.82);
            const double maxR = (std::max)(minR + 1.0, visualLimit * (diagonal ? 1.00 : 0.98));
            const double radius = RandRange(minR, maxR);
            double x = centerX + ax * radius;
            double y = centerY + ay * radius;
            ClampFixedPointToVisualCircle(c, centerX, centerY, x, y);
            const double dist = std::hypot(x - currentXNorm_, y - currentYNorm_);
            double score = 0.0;
            if (dist < minTravel) score += (minTravel - dist) * 2.4;
            else if (dist > maxTravel) score += (dist - maxTravel) * 1.2;

            // 避免连续选到几乎同一个点。固定模式范围很小，不需要很强的随机性。
            const double repeatDist = std::hypot(x - swayGoalXNorm_, y - swayGoalYNorm_);
            if (repeatDist < minTravel * 0.40) score += (minTravel * 0.40 - repeatDist) * 0.8;
            // 稍微偏向方向中心线，让“固定模式”仍然稳定。
            score += std::fabs(offset) * activeRadius * 0.06;

            if (score < bestScore) {
                bestScore = score;
                bestX = x;
                bestY = y;
                if (dist >= minTravel && dist <= maxTravel && i >= 10) break;
            }
        }

        swayGoalXNorm_ = ClampDouble(bestX, 0.0, 1000000.0);
        swayGoalYNorm_ = ClampDouble(bestY, 0.0, 1000000.0);
        targetXNorm_ = swayGoalXNorm_;
        targetYNorm_ = swayGoalYNorm_;
        dynamicTargetXNorm_ = swayGoalXNorm_;
        dynamicTargetYNorm_ = swayGoalYNorm_;

        const double travelDist = std::hypot(targetXNorm_ - currentXNorm_, targetYNorm_ - currentYNorm_);
        const int speedPct = (std::max)(1, (std::min)(5, PC_COMPASS_FIXED_SOFT_SWAY_SPEED_PERCENT));
        // speed=1 时很慢，speed=5 时接近可见但仍比晃动模式轻。
        const double speedScale = 5.0 / static_cast<double>(speedPct);
        const long long travelMs = static_cast<long long>(ClampDouble(
            (760.0 + (travelDist / (std::max)(1.0, static_cast<double>(activeRadius))) * RandRange(620.0, 1160.0)) * speedScale,
            700.0 * speedScale,
            2200.0 * speedScale));
        swayNextTargetMs_ = nowMs() + travelMs;
        };

    const double goalDist = std::hypot(targetXNorm_ - currentXNorm_, targetYNorm_ - currentYNorm_);
    const bool reachedGoal = goalDist <= (std::max)(650.0, activeRadius * 0.018);
    const bool invalidGoal = std::hypot(swayGoalXNorm_ - centerX, swayGoalYNorm_ - centerY) <= 1.0;

    if (directionChanged || invalidGoal || swayNextTargetMs_ == 0 || now >= swayNextTargetMs_ || reachedGoal) {
        pickNextFixedTarget();
    }

    if (switchBoostFrames_ > 0 && --switchBoostFrames_ <= 0) justSwitchedDirection_ = false;
}

void PcCompassRuntime::PickNextSwayTarget(const PcCompassBinding& c, double centerX, double centerY, double baseDirX, double baseDirY, int keyCount, int activeRadius, bool directionSwitch, bool mouseStabilityJustActivated) {
    const double len = std::sqrt(baseDirX * baseDirX + baseDirY * baseDirY);
    if (len <= 0.0001 || activeRadius <= 1) {
        swayGoalXNorm_ = centerX;
        swayGoalYNorm_ = centerY;
        targetXNorm_ = centerX;
        targetYNorm_ = centerY;
        return;
    }
    baseDirX /= len;
    baseDirY /= len;

    const bool single = (keyCount == 1);
    const bool manualOuter = UseOuterBand(c);
    const bool outer = manualOuter || swayDirectionalOuterAllowed_;
    const int innerRadius = (std::max)(1, RadiusNormToAxisNorm(c.innerRadiusNorm, true));
    double minR = activeRadius * (outer ? (single ? 0.66 : 0.76) : (single ? 0.50 : 0.62));
    double maxR = activeRadius * (outer ? (single ? 1.00 : 1.04) : (single ? 0.86 : 0.94));
    if (swayDirectionalOuterAllowed_ && !manualOuter) {
        // 自动外圈不是“固定外圈”：半径从内圈附近到外圈之间随机，形成扇形面积游走。
        minR = innerRadius * (single ? 0.56 : 0.64);
        maxR = activeRadius * (single ? 1.00 : 1.04);
    }
    const double maxRFinal = (std::max)(minR + 1.0, maxR);
    // 轮盘按 8 个方向划分，每个方向占自己的 1/16*2 扇区：中心 ±22.5°。
    // 扇区范围参数只收窄/放宽“当前方向自己的扇区”，不再跨到相邻方向的大范围。
    const int sectorRaw = compassSectorPercentForDirection(c, baseDirX, baseDirY, keyCount);
    const double sectorPercent = ClampDouble(static_cast<double>(sectorRaw) / 100.0, 0.25, 1.40);
    const double fullSectorHalf = 22.5 * kPi / 180.0;
    const double sectorHalf = fullSectorHalf * sectorPercent;
    const double baseAngle = std::atan2(baseDirY, baseDirX);

    const long long nowForSmall = nowMs();
    const bool mouseSmall = c.swayMouseButtonSmallStep && nowForSmall < mouseSmallUntilMs_;

    // 普通晃动：最小/最大扇区定义为当前方向扇区半宽的百分比。
    // 鼠标稳定模式：只使用“鼠标最大扇区”，让触点尽快回到当前方向中心线附近。
    const int minPct = (std::max)(PC_COMPASS_SWAY_STEP_MIN_MIN, (std::min)(PC_COMPASS_SWAY_STEP_MIN_MAX, c.swayStepMinPercent));
    const int maxPctRaw = (std::max)(PC_COMPASS_SWAY_STEP_MAX_MIN, (std::min)(PC_COMPASS_SWAY_STEP_MAX_MAX, c.swayStepMaxPercent));
    const int maxPct = (std::max)(minPct + PC_COMPASS_SWAY_STEP_MIN_GAP, maxPctRaw);
    double angleMin = sectorHalf * (static_cast<double>(minPct) / 100.0);
    double angleMax = sectorHalf * (static_cast<double>(maxPct) / 100.0);

    double minTravel = (std::max)(activeRadius * 0.18, 1800.0);
    double maxTravel = (std::max)(minTravel + 1.0, activeRadius * 0.70);
    if (mouseSmall) {
        const int smallSectorPct = (std::max)(PC_COMPASS_MOUSE_STABLE_SECTOR_MIN, (std::min)(PC_COMPASS_MOUSE_STABLE_SECTOR_MAX, c.swayMouseButtonStepPercent));
        angleMin = 0.0;
        angleMax = sectorHalf * (static_cast<double>(smallSectorPct) / 100.0);
        if (mouseStabilityJustActivated || directionSwitch) {
            angleMax = (std::min)(angleMax, sectorHalf * 0.12);
        }
        minTravel = (std::max)(activeRadius * 0.04, 900.0);
        maxTravel = (std::max)(minTravel + 1.0, activeRadius * 0.26);
    }

    const double mouseBiasScale = mouseSmall ? 0.16 : 0.55;
    const double mouseBias = ClampDouble(swayBiasRad_ * mouseBiasScale, -sectorHalf * 0.45, sectorHalf * 0.45);
    const double hardHalf = mouseSmall ? (std::max)(angleMax * 1.10, sectorHalf * 0.05) : sectorHalf * 1.08;

    double bestX = centerX + baseDirX * ((minR + maxRFinal) * 0.5);
    double bestY = centerY + baseDirY * ((minR + maxRFinal) * 0.5);
    double bestScore = 1e100;

    // 目标点选择：优先让“当前触点 -> 下一个点”的距离落在半径 1/3~2/3。
    // 这样每段移动有足够幅度，不像机械地抖几个像素。
    for (int i = 0; i < 64; ++i) {
        const bool overshoot = !mouseSmall && RandRange(0.0, 1.0) < 0.08;
        const double sign = (RandRange(0.0, 1.0) < 0.5) ? -1.0 : 1.0;
        const double mag = overshoot
            ? RandRange(angleMax, angleMax * 1.08)
            : RandRange(angleMin, angleMax);
        const double offset = ClampDouble(mouseBias + sign * mag, -hardHalf, hardHalf);
        const double angle = baseAngle + offset;
        const double radius = mouseSmall
            ? RandRange((minR + maxRFinal) * 0.46, (minR + maxRFinal) * 0.54)
            : RandRange(minR, maxRFinal);
        double x = centerX + std::cos(angle) * radius;
        double y = centerY + std::sin(angle) * radius;
        // 晃动模式的外圈也必须以屏幕上实际绘制的 UI 圆为边界。
        // activeRadius 是当前内/外圈半径；横屏时不能直接把它当 X/Y 归一化圆半径用。
        ClampPointToVisualCircleRadiusNorm(activeRadius, centerX, centerY, x, y);
        const double dist = std::hypot(x - currentXNorm_, y - currentYNorm_);
        double score = 0.0;
        if (dist < minTravel) score = (minTravel - dist) * 2.2;
        else if (dist > maxTravel) score = (dist - maxTravel) * 1.1;
        if (mouseSmall) {
            // 鼠标稳定模式优先靠近当前方向中心线，减少扇区左右乱跳。
            score += std::fabs(offset) * activeRadius * 0.18;
        }
        // 避免重复选到上一次目标附近。
        const double repeatDist = std::hypot(x - swayGoalXNorm_, y - swayGoalYNorm_);
        if (repeatDist < minTravel * 0.45) score += (minTravel * 0.45 - repeatDist) * 0.9;
        if (score < bestScore) {
            bestScore = score;
            bestX = x;
            bestY = y;
            if (dist >= minTravel && dist <= maxTravel && i >= 8) break;
        }
    }

    ClampPointToVisualCircleRadiusNorm(activeRadius, centerX, centerY, bestX, bestY);
    swayGoalXNorm_ = ClampDouble(bestX, 0.0, 1000000.0);
    swayGoalYNorm_ = ClampDouble(bestY, 0.0, 1000000.0);
    targetXNorm_ = swayGoalXNorm_;
    targetYNorm_ = swayGoalYNorm_;

    // 每段速度随机。
    // 关键分离：
    //   - 普通扇区内游走：使用“移动速%”。
    //   - 切换 W/A/S/D/LU/RU/LD/RD 方向：不使用“移动速%”，改由 X速度/Y速度控制。
    swayMoveSpeedScale_ = directionSwitch ? 1.0 : RandRange(0.72, 1.34);
    const double travelDist = std::hypot(targetXNorm_ - currentXNorm_, targetYNorm_ - currentYNorm_);
    const int normalSpeedPct = (std::max)(PC_COMPASS_SWAY_SPEED_MIN, (std::min)(PC_COMPASS_SWAY_SPEED_MAX, c.swaySpeedPercent));
    const int mouseSpeedPct = (std::max)(PC_COMPASS_MOUSE_STABLE_SPEED_MIN, (std::min)(PC_COMPASS_MOUSE_STABLE_SPEED_MAX, c.swayMouseButtonUpdatePercent));
    const int speedPct = mouseSmall ? mouseSpeedPct : normalSpeedPct;
    const double sectorTimeScale = mouseSmall
        ? ClampDouble(25.0 / static_cast<double>(speedPct), 0.50, 5.0)
        : ClampDouble(25.0 / static_cast<double>(speedPct), 1.25, 12.5);
    const double turnScale = ClampDouble((TurnScaleX(c) + TurnScaleY(c)) * 0.5, 0.25, 3.2);
    const double directionTimeScale = ClampDouble(1.0 / turnScale, 0.30, 4.0);
    const double timeScale = directionSwitch ? directionTimeScale : sectorTimeScale;
    const double baseMs = mouseSmall ? 220.0 : 320.0;
    const double randLo = mouseSmall ? 220.0 : 360.0;
    const double randHi = mouseSmall ? 480.0 : 760.0;
    const double minMs = mouseSmall ? 190.0 : 380.0;
    const double maxMs = mouseSmall ? 820.0 : 1040.0;
    const long long travelMs = static_cast<long long>(
        ClampDouble((baseMs + (travelDist / (std::max)(1.0, static_cast<double>(activeRadius))) * RandRange(randLo, randHi)) * timeScale,
            minMs * timeScale,
            maxMs * timeScale));
    const long long now = nowMs();
    swayNextTargetMs_ = now + travelMs;
    if (directionSwitch) {
        swayDirectionSwitchUntilMs_ = now + (std::max)(90LL, (std::min)(420LL, travelMs));
    }
}

void PcCompassRuntime::UpdateDirectionTargetMouseSway(const PcCompassBinding& c, double centerX, double centerY, int dx, int dy, int keyCount, int activeRadius, bool hasCustomDirection, double customDirX, double customDirY) {
    double baseDirX = 0.0;
    double baseDirY = 0.0;
    bool hasDir = false;
    if (hasCustomDirection) {
        baseDirX = customDirX;
        baseDirY = customDirY;
        const double len = std::sqrt(baseDirX * baseDirX + baseDirY * baseDirY);
        hasDir = len > 0.0001;
        if (hasDir) { baseDirX /= len; baseDirY /= len; }
    }
    else if (dx != 0 || dy != 0) {
        const double len = std::sqrt(double(dx * dx + dy * dy));
        baseDirX = double(dx) / len;
        baseDirY = double(dy) / len;
        hasDir = true;
    }

    if (!hasDir) {
        targetXNorm_ = centerX;
        targetYNorm_ = centerY;
        swayGoalXNorm_ = centerX;
        swayGoalYNorm_ = centerY;
        swayNextTargetMs_ = 0;
        swayTargetRadiusNorm_ = 0.0;
        swayBiasRad_ *= 0.86;
        return;
    }

    // 鼠标只影响当前方向扇区的偏航；真正的方向切换曲线在下面用“方向低通 + 粒子惯性”完成。
    double ignoredX = baseDirX;
    double ignoredY = baseDirY;
    ApplyMouseSway(c, baseDirX, baseDirY, keyCount, ignoredX, ignoredY);
    const double baseLen = std::hypot(baseDirX, baseDirY);
    if (baseLen > 0.0001) {
        baseDirX /= baseLen;
        baseDirY /= baseLen;
    }

    const bool manualOuter = UseOuterBand(c);
    const bool directionAllowsOuter = MouseSwayDirectionAllowsOuter(dx, dy, baseDirX, baseDirY);
    const bool wideRadius = MouseSwayUseWideRadius(c, dx, dy, baseDirX, baseDirY);
    const bool autoWideRadius = wideRadius && !manualOuter && directionAllowsOuter;
    swayDirectionalOuterAllowed_ = autoWideRadius;
    const bool outer = wideRadius;
    const long long now = nowMs();

    bool mouseStabilityJustActivated = false;
    if (c.swayMouseButtonSmallStep && lastMouseButtonDownMs_ > 0) {
        // 鼠标左/右键只作为“触发稳定模式”的事件。
        // 触发后在 swayMouseButtonHoldMs 时间内使用小范围扇区，不要求鼠标键一直按住。
        const long long ageMs = now - lastMouseButtonDownMs_;
        if (ageMs >= 0 && ageMs <= 350) {
            const long long holdMs = static_cast<long long>((std::max)(PC_COMPASS_MOUSE_STABLE_HOLD_MS_MIN, (std::min)(PC_COMPASS_MOUSE_STABLE_HOLD_MS_MAX, c.swayMouseButtonHoldMs)));
            const bool wasInactive = now >= mouseSmallUntilMs_;
            mouseSmallUntilMs_ = now + holdMs;
            mouseStabilityJustActivated = wasInactive;
            lastMouseButtonDownMs_ = 0;
        }
    }

    const bool mouseSmall = c.swayMouseButtonSmallStep && now < mouseSmallUntilMs_;
    const bool directionChanged = (dx != swayLastDx_) || (dy != swayLastDy_) || (outer != swayLastOuter_) || (wideRadius != swayLastWideRadius_) || (mouseSmall != swayLastMouseSmall_);

    // 方向切换不能用过大的线性低通，否则 W->A 第一帧目标就几乎贴到 A，
    // 粒子会沿 W 点到 A 点的弦线直切，曲线感消失。
    // 这里改成“角速度限制”：目标方向沿圆弧逐步转过去，速度仍由 X/Y 速度控制。
    const bool wasDirectionSwitching = now < swayDirectionSwitchUntilMs_;
    double driveDirX = baseDirX;
    double driveDirY = baseDirY;
    bool largeAngle = false;
    const double lastLen = std::hypot(lastTargetDirX_, lastTargetDirY_);
    if (lastLen > 0.0001) {
        const double lx = lastTargetDirX_ / lastLen;
        const double ly = lastTargetDirY_ / lastLen;
        const double dot = ClampDouble(lx * baseDirX + ly * baseDirY, -1.0, 1.0);
        const double angle = std::acos(dot);
        largeAngle = angle > (30.0 * kPi / 180.0);
        const bool curvedSwitch = !mouseSmall && largeAngle && (directionChanged || wasDirectionSwitching);
        if (curvedSwitch) {
            const double fromAngle = std::atan2(ly, lx);
            const double toAngle = std::atan2(baseDirY, baseDirX);
            const double turnScale = ClampDouble((TurnScaleX(c) + TurnScaleY(c)) * 0.5, 0.25, 3.2);
            const double maxStep = ClampDouble((6.0 + 5.0 * turnScale) * kPi / 180.0, 8.0 * kPi / 180.0, 18.0 * kPi / 180.0);
            const double step = ClampDouble(compassAngleDiff(toAngle, fromAngle), -maxStep, maxStep);
            const double driveAngle = fromAngle + step;
            driveDirX = std::cos(driveAngle);
            driveDirY = std::sin(driveAngle);
        }
        else {
            const double smoothFactor = largeAngle ? 0.42 : 0.56;
            driveDirX = lx * (1.0 - smoothFactor) + baseDirX * smoothFactor;
            driveDirY = ly * (1.0 - smoothFactor) + baseDirY * smoothFactor;
            const double dl = std::hypot(driveDirX, driveDirY);
            if (dl > 0.0001) {
                driveDirX /= dl;
                driveDirY /= dl;
            }
            else {
                driveDirX = baseDirX;
                driveDirY = baseDirY;
            }
        }
    }

    if (directionChanged || mouseStabilityJustActivated) {
        swayLastDx_ = dx;
        swayLastDy_ = dy;
        swayLastOuter_ = outer;
        swayLastWideRadius_ = wideRadius;
        swayLastMouseSmall_ = mouseSmall;
        swayNextTargetMs_ = 0;
        justSwitchedDirection_ = true;
        switchBoostFrames_ = 3;
        swayDirectionSwitchUntilMs_ = now + (mouseSmall ? 240 : 300);
        if (directionChanged) {
            const bool single = keyCount == 1;
            const int innerRadius = (std::max)(1, RadiusNormToAxisNorm(c.innerRadiusNorm, true));
            double minR = activeRadius * (outer ? (single ? 0.66 : 0.76) : (single ? 0.50 : 0.62));
            double maxR = activeRadius * (outer ? (single ? 1.00 : 1.04) : (single ? 0.86 : 0.94));
            if (autoWideRadius && !mouseSmall) {
                // 非上半区自动外圈：最大半径放到外圈，但最小半径仍从内圈附近开始，
                // 让 A/D/S/SA/SD 在内圈~外圈之间扇形游走，而不是贴死外圈。
                minR = innerRadius * (single ? 0.56 : 0.64);
                maxR = activeRadius * (single ? 1.00 : 1.04);
            }
            maxR = (std::max)(minR + 1.0, maxR);
            swayTargetRadiusNorm_ = RandRange(minR, maxR);
            lastPerturbX_ = 0.0;
            lastPerturbY_ = 0.0;
            // 注意：这里不清 velXNorm_/velYNorm_。Python 版本靠 0.92 惯性保留旧速度，曲线才自然。
        }
        if (mouseStabilityJustActivated) {
            // 进入鼠标稳定模式时，先清掉横向速度，让触点尽快回到当前方向中心线附近。
            velXNorm_ *= 0.35;
            velYNorm_ *= 0.35;
            swayBiasRad_ *= 0.25;
        }
    }

    keyboardDirX_ = driveDirX;
    keyboardDirY_ = driveDirY;
    lastTargetDirX_ = driveDirX;
    lastTargetDirY_ = driveDirY;

    const bool directionSwitching = now < swayDirectionSwitchUntilMs_;
    if (directionSwitching && !mouseSmall && swayTargetRadiusNorm_ > 1.0) {
        // 切方向阶段只让“角度”快速转过去，半径不要立刻跳到新方向的外圈。
        // 否则 W->A 会从上方向外圈左侧直接拉一条弦线，看起来像直线。
        const int innerRadius = (std::max)(1, RadiusNormToAxisNorm(c.innerRadiusNorm, true));
        const double curRadius = std::hypot(currentXNorm_ - centerX, currentYNorm_ - centerY);
        const bool autoOuterFan = autoWideRadius && !manualOuter;
        const double minSwitchRadius = autoOuterFan ? innerRadius * 0.52 : activeRadius * 0.48;
        const double maxSwitchRadius = autoOuterFan ? activeRadius * 1.02 : activeRadius * 0.96;
        const double radiusFollow = autoOuterFan ? 0.18 : 0.26;
        const double guideRadius = ClampDouble(
            curRadius * (1.0 - radiusFollow) + swayTargetRadiusNorm_ * radiusFollow,
            (std::min)(minSwitchRadius, maxSwitchRadius),
            (std::max)(minSwitchRadius + 1.0, maxSwitchRadius));
        double guideX = centerX + driveDirX * guideRadius;
        double guideY = centerY + driveDirY * guideRadius;
        ClampPointToVisualCircleRadiusNorm(activeRadius, centerX, centerY, guideX, guideY);
        targetXNorm_ = ClampDouble(guideX, 0.0, 1000000.0);
        targetYNorm_ = ClampDouble(guideY, 0.0, 1000000.0);
        swayGoalXNorm_ = targetXNorm_;
        swayGoalYNorm_ = targetYNorm_;
        // 保持下一帧继续沿圆弧更新目标；结束切换窗口后再进入内外圈扇形游走。
        swayNextTargetMs_ = now + 16;
    }
    else {
        const double goalDist = std::hypot(targetXNorm_ - currentXNorm_, targetYNorm_ - currentYNorm_);
        const bool reachedGoal = goalDist <= (std::max)(2200.0, activeRadius * 0.045);
        if (swayNextTargetMs_ == 0 || now >= swayNextTargetMs_ || reachedGoal) {
            PickNextSwayTarget(c, centerX, centerY, driveDirX, driveDirY, keyCount, activeRadius, directionChanged, mouseStabilityJustActivated);
        }
    }

    if (switchBoostFrames_ > 0 && --switchBoostFrames_ <= 0) justSwitchedDirection_ = false;
}

bool PcCompassRuntime::EmitParticleStep(const PcCompassBinding& c, double centerX, double centerY, int activeRadius, bool relaxToCenter, bool mouseSwayMode) {
    if (!active_) return false;

    double dxTarget = targetXNorm_ - currentXNorm_;
    double dyTarget = targetYNorm_ - currentYNorm_;

    const double curOffsetX = currentXNorm_ - centerX;
    const double curOffsetY = currentYNorm_ - centerY;
    const double curOffsetLen = std::sqrt(curOffsetX * curOffsetX + curOffsetY * curOffsetY);
    double edgeT = 0.0;
    if (!relaxToCenter && activeRadius > 0) {
        const double edgeStart = activeRadius * (mouseSwayMode ? 0.90 : 0.88);
        if (curOffsetLen > edgeStart) edgeT = ClampDouble((curOffsetLen - edgeStart) / (std::max)(0.0001, activeRadius - edgeStart), 0.0, 1.0);
    }

    if (mouseSwayMode && edgeT > 0.0 && curOffsetLen > 0.0001) {
        const double rx = curOffsetX / curOffsetLen;
        const double ry = curOffsetY / curOffsetLen;
        const double tx = -ry;
        const double ty = rx;
        const double radial = dxTarget * rx + dyTarget * ry;
        const double tangential = dxTarget * tx + dyTarget * ty;
        const double keepT = 1.0 - edgeT * 0.42;
        dxTarget = rx * radial + tx * (tangential * keepT);
        dyTarget = ry * radial + ty * (tangential * keepT);
    }

    const double distance = std::sqrt(dxTarget * dxTarget + dyTarget * dyTarget);
    // 晃动模式速度分离：
    //   - 扇区内部 A->B 游走使用“移动速%”；
    //   - 方向切换使用 X速度/Y速度，避免“移动速%”影响切方向速度。
    const bool directionSwitching = mouseSwayMode && !relaxToCenter && nowMs() < swayDirectionSwitchUntilMs_;
    const bool mouseSmallActive = mouseSwayMode && !relaxToCenter && c.swayMouseButtonSmallStep && nowMs() < mouseSmallUntilMs_;
    const int normalSpeedPct = (std::max)(PC_COMPASS_SWAY_SPEED_MIN, (std::min)(PC_COMPASS_SWAY_SPEED_MAX, c.swaySpeedPercent));
    const int mouseSpeedPct = (std::max)(PC_COMPASS_MOUSE_STABLE_SPEED_MIN, (std::min)(PC_COMPASS_MOUSE_STABLE_SPEED_MAX, c.swayMouseButtonUpdatePercent));
    const double sectorSpeed = mouseSwayMode
        ? (mouseSmallActive
            ? ClampDouble(static_cast<double>(mouseSpeedPct) / 25.0, 0.20, 2.0)
            : ClampDouble(static_cast<double>(normalSpeedPct) / 25.0, 0.08, 0.80))
        : 1.0;
    const double directionSpeed = ClampDouble((TurnScaleX(c) + TurnScaleY(c)) * 0.5, 0.25, 3.2);
    const double configuredSpeed = directionSwitching ? directionSpeed : sectorSpeed;
    const double fixedSoftSpeed = ClampDouble(static_cast<double>((std::max)(1, (std::min)(5, PC_COMPASS_FIXED_SOFT_SWAY_SPEED_PERCENT))) / 5.0, 0.20, 1.0);
    const double speedScale = mouseSwayMode
        ? (relaxToCenter ? 0.82 : (directionSwitching ? 0.76 : 0.52) * configuredSpeed * swayMoveSpeedScale_)
        : (relaxToCenter ? 1.0 : (0.32 + 0.16 * fixedSoftSpeed));
    const double baseAccel = mouseSwayMode ? 720.0 : 760.0;
    // 只给方向切换一个温和提速；主要转向曲线交给上层的方向低通目标处理，避免粒子被硬推到相邻方向。
    double accel = baseAccel * speedScale * (directionSwitching ? 1.28 : (justSwitchedDirection_ ? 1.2 : 1.0));
    const double accelScaleX = ClampDouble((mouseSwayMode ? 0.84 : 0.85) + 0.36 * TurnScaleX(c), mouseSwayMode ? 1.10 : 1.15, mouseSwayMode ? 2.45 : 2.80);
    const double accelScaleY = ClampDouble((mouseSwayMode ? 0.84 : 0.85) + 0.36 * TurnScaleY(c), mouseSwayMode ? 1.10 : 1.15, mouseSwayMode ? 2.45 : 2.80);

    // 粒子随机扰动只给“晃动模式”使用。
    // 固定模式的轻微滑动已经由小扇区目标点完成，这里继续加随机扰动会变成高频抖动。
    double perturbX = 0.0;
    double perturbY = 0.0;
    if (mouseSwayMode && !relaxToCenter && distance > 3.0) {
        const double dirX = keyboardDirX_;
        const double dirY = keyboardDirY_;
        const double px = -dirY;
        const double py = dirX;
        const double carry = 0.94;
        const double expand = 0.16;
        const double axial = lastPerturbX_ * carry + RandRange(-1.0, 1.0) * 0.20 * expand;
        const double lateral = lastPerturbY_ * carry + RandRange(-1.0, 1.0) * 0.70 * expand;
        perturbX = dirX * axial + px * lateral;
        perturbY = dirY * axial + py * lateral;
        lastPerturbX_ = perturbX;
        lastPerturbY_ = perturbY;
    }
    else {
        lastPerturbX_ = 0.0;
        lastPerturbY_ = 0.0;
    }
    // 粒子随机扰动
    if (mouseSwayMode && edgeT > 0.0) {
        const double perturbScale = 1.0 - edgeT * 0.97;
        perturbX *= perturbScale;
        perturbY *= perturbScale;
        const double storedDecay = 1.0 - edgeT * 0.70;
        lastPerturbX_ *= storedDecay;
        lastPerturbY_ *= storedDecay;
    }

    if (distance < 3.0) {
        velXNorm_ *= mouseSwayMode ? 0.86 : 0.82;
        velYNorm_ *= mouseSwayMode ? 0.86 : 0.82;
    }
    else {
        double ax = 0.0;
        double ay = 0.0;
        if (distance > 0.0001) {
            // 晃动模式的“手指漂移”已经在目标点里做了，粒子层只保留极弱扰动，
            // 防止高频随机扰动看起来像抖动。
            const double pFactor = mouseSwayMode ? 0.08 : 1.0;
            ax = (dxTarget / distance) * accel * accelScaleX + perturbX * pFactor * accelScaleX;
            ay = (dyTarget / distance) * accel * accelScaleY + perturbY * pFactor * accelScaleY;
        }
        double nvx = velXNorm_ * 0.92 + ax;
        double nvy = velYNorm_ * 0.92 + ay;
        const double maxBase = mouseSwayMode ? 6200.0 : 4200.0;
        const double maxSpeedBoost = mouseSwayMode ? ((directionSwitching ? 0.70 : 0.45) * configuredSpeed * swayMoveSpeedScale_) : (relaxToCenter ? 1.0 : (0.58 + 0.22 * fixedSoftSpeed));
        const double radiusFactor = mouseSwayMode ? (directionSwitching ? 0.30 : 0.20) : (relaxToCenter ? 0.22 : 0.18);
        const double maxX = (std::max)(maxBase * maxSpeedBoost * accelScaleX, activeRadius * radiusFactor * accelScaleX);
        const double maxY = (std::max)(maxBase * maxSpeedBoost * accelScaleY, activeRadius * radiusFactor * accelScaleY);
        const double normSpeed = (maxX > 0.0001 ? (nvx * nvx) / (maxX * maxX) : 0.0) + (maxY > 0.0001 ? (nvy * nvy) / (maxY * maxY) : 0.0);
        if (normSpeed > 1.0) {
            const double s = 1.0 / std::sqrt(normSpeed);
            nvx *= s;
            nvy *= s;
        }
        velXNorm_ = nvx;
        velYNorm_ = nvy;
    }

    if (!relaxToCenter && edgeT > 0.0) {
        const double edgeSlow = mouseSwayMode ? (1.0 - edgeT * 0.24) : (1.0 - edgeT * 0.08);
        velXNorm_ *= edgeSlow;
        velYNorm_ *= edgeSlow;
    }

    double nextX = currentXNorm_ + velXNorm_;
    double nextY = currentYNorm_ + velYNorm_;
    const double offX = nextX - centerX;
    const double offY = nextY - centerY;
    const double offLen = std::sqrt(offX * offX + offY * offY);
    // 固定模式不能沿用晃动模式的外扩余量。MouseSway 可以保留 1.04 的扇区游走余量，
    // FixedCenter 必须严格限制在当前 activeRadius 内，避免反转后跑出外圈。
    const double maxRadius = relaxToCenter
        ? (std::max)(2.0, static_cast<double>(activeRadius))
        : (mouseSwayMode
            ? (std::max)(10.0, activeRadius * 1.04)
            : (std::max)(8.0, static_cast<double>(activeRadius)));
    if (offLen > maxRadius && offLen > 0.0001) {
        const double rx = offX / offLen;
        const double ry = offY / offLen;
        const double tx = -ry;
        const double ty = rx;
        const double s = maxRadius / offLen;
        nextX = centerX + offX * s;
        nextY = centerY + offY * s;
        if (mouseSwayMode) {
            const double radialVel = velXNorm_ * rx + velYNorm_ * ry;
            const double tangentialVel = velXNorm_ * tx + velYNorm_ * ty;
            double keptRadial = radialVel;
            if (keptRadial > 0.0) keptRadial = 0.0;
            else keptRadial *= relaxToCenter ? 0.22 : 0.26;
            const double tangentDamping = relaxToCenter ? 0.06 : (justSwitchedDirection_ ? 0.11 : 0.14);
            const double keptTangential = tangentialVel * tangentDamping;
            velXNorm_ = rx * keptRadial + tx * keptTangential;
            velYNorm_ = ry * keptRadial + ty * keptTangential;
            lastPerturbX_ *= 0.05;
            lastPerturbY_ *= 0.05;
        }
        else {
            if (!relaxToCenter) {
                // 固定小扇区模式到达外圈后，消掉向外速度，只保留极少切向速度。
                // 这样不会在方向边界/外圈边界附近来回弹动。
                const double radialVel = velXNorm_ * rx + velYNorm_ * ry;
                const double tangentialVel = velXNorm_ * tx + velYNorm_ * ty;
                double keptRadial = radialVel;
                if (keptRadial > 0.0) keptRadial = 0.0;
                else keptRadial *= 0.18;
                const double keptTangential = tangentialVel * 0.08;
                velXNorm_ = rx * keptRadial + tx * keptTangential;
                velYNorm_ = ry * keptRadial + ty * keptTangential;
            }
            else {
                velXNorm_ *= 0.72;
                velYNorm_ *= 0.72;
            }
        }
    }

    // 最终触点统一按屏幕 UI 圆限制。
    // MouseSway 也不能超过屏幕上绘制的内/外圈，否则横屏 A/D/S 方向会明显跑出外圈。
    ClampPointToVisualCircleRadiusNorm(activeRadius, centerX, centerY, nextX, nextY);

    currentXNorm_ = ClampDouble(nextX, 0.0, 1000000.0);
    currentYNorm_ = ClampDouble(nextY, 0.0, 1000000.0);
    const int ix = PcMappingProfile::ClampNorm(static_cast<int>(std::lround(currentXNorm_)));
    const int iy = PcMappingProfile::ClampNorm(static_cast<int>(std::lround(currentYNorm_)));
    const bool ok = callbacks_.touchMove ? callbacks_.touchMove(slot_, ix, iy) : false;
    if (ok) MarkTouchFrameEmitted();
    return ok;
}

void PcCompassRuntime::ReleaseTouch() {
    if (active_ && callbacks_.touchUp) callbacks_.touchUp(slot_);
    active_ = false;
    zeroSinceMs_ = -1;
    ResetParticleState();
}

void PcCompassRuntime::Tick() {
    if (!profileProvider_ || profileProvider_().compasses.empty()) {
        ReleaseTouch();
        return;
    }
    const auto& c = profileProvider_().compasses.front();
    syncCompassKeyboardPhysicalState(c, vkDown_);
    UpdatePressedStates(c);

    double dirX = 0.0;
    double dirY = 0.0;
    int keyCount = 0;
    int dx = 0;
    int dy = 0;
    ComputeDirection(c, dirX, dirY, keyCount, dx, dy);

    slot_ = (std::max)(1, (std::min)(15, c.slot));
    const double centerX = static_cast<double>(PcMappingProfile::ClampNorm(c.centerXNorm));
    const double centerY = static_cast<double>(PcMappingProfile::ClampNorm(c.centerYNorm));
    const bool mouseSwayMode = (c.motionMode == PcCompassMotionMode::MouseSway);

    if (!mouseSwayMode) {
        // FixedCenter 固定模式：固定八方向，但必须遵循真实触摸顺序：
        // 1) 先在内圈范围内随机 touchDown；
        // 2) 下一帧再 touchMove 到当前方向的内/外圈固定点；
        // 3) 最终点按屏幕上实际绘制的 UI 圆限制，不能跑出外圈。
        if (keyCount <= 0) {
            ReleaseTouch();
            return;
        }

        const int activeRadius = FixedRawActiveRadiusNorm(c);
        double fixedDirX = dirX;
        double fixedDirY = dirY;
        double tx = 0.0;
        double ty = 0.0;

        // FixedCenter 的斜向不能再用数学 45 度固定方向。
        // WA / WD / SA / SD 只从轮盘 UI 上可拖动的四个斜角点读取“方向”。
        // 斜角点不是最终触摸坐标；最终坐标仍然按当前内圈/外圈 activeRadius 计算。
        // 如果旧配置没有斜角点，才退回固定八方向算法。
        if (keyCount >= 2) {
            double customDirX = fixedDirX;
            double customDirY = fixedDirY;
            if (fixedDiagonalDirection(c, dx, dy, customDirX, customDirY)) {
                fixedDirX = customDirX;
                fixedDirY = customDirY;
            }
        }

        tx = centerX + fixedDirX * static_cast<double>(activeRadius);
        ty = centerY + fixedDirY * static_cast<double>(activeRadius);
        ClampPointToVisualCircleRadiusNorm(activeRadius, centerX, centerY, tx, ty);
        tx = ClampDouble(tx, 0.0, 1000000.0);
        ty = ClampDouble(ty, 0.0, 1000000.0);

        // 清掉晃动/粒子状态，避免从 MouseSway 切回 FixedCenter 时残留速度或目标。
        targetXNorm_ = dynamicTargetXNorm_ = swayGoalXNorm_ = tx;
        targetYNorm_ = dynamicTargetYNorm_ = swayGoalYNorm_ = ty;
        keyboardDirX_ = lastTargetDirX_ = fixedDirX;
        keyboardDirY_ = lastTargetDirY_ = fixedDirY;
        velXNorm_ = 0.0;
        velYNorm_ = 0.0;
        lastPerturbX_ = 0.0;
        lastPerturbY_ = 0.0;
        swayBiasRad_ = 0.0;
        swayNextTargetMs_ = 0;
        zeroSinceMs_ = -1;

        if (!active_) {
            // 不能直接 down 到外圈/方向点；先在内圈随机按下，下一次 Tick 再滑动到目标点。
            PressInitial(c, activeRadius, fixedDirX, fixedDirY, keyCount, false);
            return;
        }

        if (std::fabs(currentXNorm_ - tx) > 0.5 || std::fabs(currentYNorm_ - ty) > 0.5) {
            currentXNorm_ = tx;
            currentYNorm_ = ty;
            const int ix = PcMappingProfile::ClampNorm(static_cast<int>(std::lround(currentXNorm_)));
            const int iy = PcMappingProfile::ClampNorm(static_cast<int>(std::lround(currentYNorm_)));
            const bool ok = callbacks_.touchMove ? callbacks_.touchMove(slot_, ix, iy) : false;
            if (ok) MarkTouchFrameEmitted();
        }
        return;
    }

    int activeRadius = ActiveRadiusNorm(c);
    const bool hasDirectionInput = keyCount > 0;
    if (!ShouldRunTouchFrame(c, hasDirectionInput) && active_) {
        // 仍然衰减鼠标滤波，避免高 DPI 鼠标输入在下一帧一次性释放。
        mouseFilteredDx_ *= 0.86;
        mouseFilteredDy_ *= 0.86;
        return;
    }

    if (keyCount <= 0) {
        if (zeroSinceMs_ < 0) {
            zeroSinceMs_ = nowMs();
            targetXNorm_ = centerX;
            targetYNorm_ = centerY;
            keyboardDirX_ = keyboardDirY_ = 0.0;
            lastTargetDirX_ = lastTargetDirY_ = 0.0;
            justSwitchedDirection_ = false;
        }
        if (active_) {
            (void)EmitParticleStep(c, centerX, centerY, activeRadius, true, true);
            if (nowMs() - zeroSinceMs_ >= 45) {
                ReleaseTouch();
            }
        }
        return;
    }
    zeroSinceMs_ = -1;

    double customX = 0.0;
    double customY = 0.0;
    const bool hasCustom = TryGetCustomDirection(c, dx, dy, customX, customY);
    double initialDirX = hasCustom ? customX : dirX;
    double initialDirY = hasCustom ? customY : dirY;

    // 晃动模式下，A/D/S/SA/SD 即使没有触发外环键，也把外圈作为最大活动半径；
    // W/WA/WD 仍使用内圈半径。
    activeRadius = MouseSwayActiveRadiusNorm(c, dx, dy, initialDirX, initialDirY);

    if (!active_) {
        // 新一次 WASD 触发的首帧不能吃上一次鼠标 RawInput 的残留偏航。
        // 否则最典型的表现就是：刚按 W，但因为之前鼠标横向 dx 残留，方向被拉成 WA/WD；
        // A 看起来正常，是因为 A 的切线主要吃 dy，横向 dx 对 A 不敏感。
        swayBiasRad_ = 0.0;
        mouseFilteredDx_ = 0.0;
        mouseFilteredDy_ = 0.0;
        lastMouseDx_ = 0;
        lastMouseDy_ = 0;
        lastMouseMs_ = 0;
        PressInitial(c, activeRadius, initialDirX, initialDirY, keyCount, true);
        return;
    }

    ApplyMouseSway(c, initialDirX, initialDirY, keyCount, initialDirX, initialDirY);
    UpdateDirectionTargetMouseSway(c, centerX, centerY, dx, dy, keyCount, activeRadius, hasCustom, customX, customY);
    (void)EmitParticleStep(c, centerX, centerY, activeRadius, false, true);
}

void PcCompassRuntime::AddRawMouseDelta(LPARAM lp) {
    UINT size = 0;
    GetRawInputData(reinterpret_cast<HRAWINPUT>(lp), RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER));
    if (size == 0 || size > 4096) return;
    BYTE buffer[4096];
    if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lp), RID_INPUT, buffer, &size, sizeof(RAWINPUTHEADER)) != size) return;
    RAWINPUT* raw = reinterpret_cast<RAWINPUT*>(buffer);
    if (raw->header.dwType != RIM_TYPEMOUSE) return;
    const USHORT flags = raw->data.mouse.usButtonFlags;
    if (flags & RI_MOUSE_LEFT_BUTTON_DOWN) { mouseLeftDown_ = true; lastMouseButtonDownMs_ = nowMs(); }
    if (flags & RI_MOUSE_LEFT_BUTTON_UP) mouseLeftDown_ = false;
    if (flags & RI_MOUSE_RIGHT_BUTTON_DOWN) { mouseRightDown_ = true; lastMouseButtonDownMs_ = nowMs(); }
    if (flags & RI_MOUSE_RIGHT_BUTTON_UP) mouseRightDown_ = false;
    lastMouseDx_ = raw->data.mouse.lLastX;
    lastMouseDy_ = raw->data.mouse.lLastY;
    // 高频鼠标不要直接把单帧 delta 打满偏航；用更强的低通和限幅。
    // 目标是“人手在同一方向附近连续滑动”，不是随 RawInput 每帧抖。
    const double dx = ClampDouble(static_cast<double>(lastMouseDx_), -30.0, 30.0);
    const double dy = ClampDouble(static_cast<double>(lastMouseDy_), -30.0, 30.0);
    mouseFilteredDx_ = mouseFilteredDx_ * 0.88 + dx * 0.12;
    mouseFilteredDy_ = mouseFilteredDy_ * 0.88 + dy * 0.12;
    lastMouseMs_ = nowMs();
}

bool PcCompassRuntime::HandleWindowMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LRESULT* outResult) {
    if (hwnd && IsWindow(hwnd)) lastHwnd_ = hwnd;
    if (outResult) *outResult = 0;
    switch (msg) {
    case WM_INPUT:
        AddRawMouseDelta(lp);
        return false;
    case WM_LBUTTONDOWN:
        mouseLeftDown_ = true;
        lastMouseButtonDownMs_ = nowMs();
        return false;
    case WM_LBUTTONUP:
        mouseLeftDown_ = false;
        return false;
    case WM_RBUTTONDOWN:
        mouseRightDown_ = true;
        lastMouseButtonDownMs_ = nowMs();
        return false;
    case WM_RBUTTONUP:
        mouseRightDown_ = false;
        return false;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        const UINT vk = NormalizeVk(static_cast<UINT>(wp), lp);
        if (vk < vkDown_.size()) vkDown_[vk] = true;
        if (!AnyCompassKeyRelated(static_cast<int>(vk))) return false;
        if (profileProvider_ && !profileProvider_().compasses.empty()) UpdatePressedStates(profileProvider_().compasses.front());
        return true;
    }
    case WM_KEYUP:
    case WM_SYSKEYUP: {
        const UINT vk = NormalizeVk(static_cast<UINT>(wp), lp);
        if (vk < vkDown_.size()) vkDown_[vk] = false;
        if (!AnyCompassKeyRelated(static_cast<int>(vk))) return false;
        if (profileProvider_ && !profileProvider_().compasses.empty()) UpdatePressedStates(profileProvider_().compasses.front());
        return true;
    }
    case WM_KILLFOCUS:
    case WM_CANCELMODE:
        Reset();
        return false;
    default:
        return false;
    }
}
