#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <array>
#include <chrono>
#include <functional>
#include <random>

#include "pc_mapping_profile.h"

// PC 端轮盘运行时。
// PC 负责 WASD/中心键状态、固定/晃动、反转、内外圈、对角锚点和粒子移动算法；
// Android 只执行最终 TOUCH_DOWN / TOUCH_MOVE / TOUCH_UP。
class PcCompassRuntime final {
public:
    struct Callbacks {
        std::function<bool(int slot, int xNorm, int yNorm)> touchDown;
        std::function<bool(int slot, int xNorm, int yNorm)> touchMove;
        std::function<bool(int slot)> touchUp;
    };

    void SetCallbacks(Callbacks cb);
    void SetProfileProvider(std::function<const PcMappingProfile&()> provider);
    void Reset();
    void Tick();
    bool IsActive() const { return active_; }
    std::wstring StatusText() const;
    bool HandleWindowMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LRESULT* outResult = nullptr);

private:
    static UINT NormalizeVk(UINT vk, LPARAM lp);
    static int ClampInt(int v, int lo, int hi);
    static double ClampDouble(double v, double lo, double hi);
    static bool ComboSatisfied(const PcCompassButtonBinding& b, const std::array<bool, 256>& vkDown);
    static bool IsRelatedKey(const PcCompassButtonBinding& b, int vk);
    static int RadiusNormToAxisNorm(int radiusNorm, bool xAxis);

    void ReleaseTouch();
    bool AnyCompassKeyRelated(int vk) const;
    bool UpdatePressedStates(const PcCompassBinding& c);
    void ComputeDirection(const PcCompassBinding& c, double& outX, double& outY, int& keyCount, int& dx, int& dy);
    bool TryGetCustomDirection(const PcCompassBinding& c, int dx, int dy, double& outX, double& outY) const;
    void ApplyMouseSway(const PcCompassBinding& c, double baseX, double baseY, int keyCount, double& outX, double& outY);

    bool UseOuterBand(const PcCompassBinding& c) const;
    int ActiveRadiusNorm(const PcCompassBinding& c) const;
    int FixedRawActiveRadiusNorm(const PcCompassBinding& c) const;
    bool CurrentClientSize(int& outW, int& outH) const;
    int VisualCircleRadiusPx(int radiusNorm) const;
    double FixedVisualMaxNormRadiusForDirection(const PcCompassBinding& c, double dirX, double dirY) const;
    void ClampPointToVisualCircleRadiusNorm(int radiusNorm, double centerX, double centerY, double& x, double& y) const;
    void ClampFixedPointToVisualCircle(const PcCompassBinding& c, double centerX, double centerY, double& x, double& y) const;
    int FixedActiveRadiusNorm(const PcCompassBinding& c) const;
    bool MouseSwayDirectionAllowsOuter(int dx, int dy, double dirX, double dirY) const;
    bool MouseSwayUseWideRadius(const PcCompassBinding& c, int dx, int dy, double dirX, double dirY) const;
    int MouseSwayActiveRadiusNorm(const PcCompassBinding& c, int dx, int dy, double dirX, double dirY) const;
    double TurnScaleX(const PcCompassBinding& c) const;
    double TurnScaleY(const PcCompassBinding& c) const;
    double RandRange(double lo, double hi);

    void ResetParticleState();
    void PressInitial(const PcCompassBinding& c, int activeRadius, double dirX, double dirY, int keyCount, bool mouseSwayMode);
    void UpdateDirectionTargetFixed(const PcCompassBinding& c, double centerX, double centerY, int dx, int dy, int keyCount, int activeRadius, bool hasCustomDirection, double customDirX, double customDirY);
    void UpdateDirectionTargetMouseSway(const PcCompassBinding& c, double centerX, double centerY, int dx, int dy, int keyCount, int activeRadius, bool hasCustomDirection, double customDirX, double customDirY);
    void PickNextSwayTarget(const PcCompassBinding& c, double centerX, double centerY, double baseDirX, double baseDirY, int keyCount, int activeRadius, bool directionSwitch, bool mouseStabilityJustActivated);
    bool EmitParticleStep(const PcCompassBinding& c, double centerX, double centerY, int activeRadius, bool relaxToCenter, bool mouseSwayMode);
    void AddRawMouseDelta(LPARAM lp);
    bool ShouldRunTouchFrame(const PcCompassBinding& c, bool hasDirectionInput) const;
    void MarkTouchFrameEmitted();

private:
    Callbacks callbacks_;
    std::function<const PcMappingProfile&()> profileProvider_;
    HWND lastHwnd_{};
    std::array<bool, 256> vkDown_{};

    bool upPressed_ = false;
    bool leftPressed_ = false;
    bool downPressed_ = false;
    bool rightPressed_ = false;
    bool centerPressed_ = false;

    bool active_ = false;
    int slot_ = 1;

    double currentXNorm_ = 500000.0;
    double currentYNorm_ = 500000.0;
    double targetXNorm_ = 500000.0;
    double targetYNorm_ = 500000.0;
    double dynamicTargetXNorm_ = 500000.0;
    double dynamicTargetYNorm_ = 500000.0;
    double velXNorm_ = 0.0;
    double velYNorm_ = 0.0;
    double keyboardDirX_ = 0.0;
    double keyboardDirY_ = 0.0;
    double lastTargetDirX_ = 0.0;
    double lastTargetDirY_ = 0.0;
    double lastPerturbX_ = 0.0;
    double lastPerturbY_ = 0.0;

    // 晃动模式使用“扇区大步长平滑游走”：
    // 每次选择下一个目标点时，目标与当前触点之间的距离尽量控制在当前半径的 1/3~2/3，
    // 速度也为每段随机。这样不像每帧小像素抖动，更接近人手在轮盘扇区里移动。
    double swayRadiusNorm_ = 0.0;
    double swayTargetRadiusNorm_ = 0.0;
    double swayAngleOffsetRad_ = 0.0;
    double swayTargetAngleOffsetRad_ = 0.0;
    double swayAxialNorm_ = 0.0;
    double swayLateralNorm_ = 0.0;
    double swayTargetAxialNorm_ = 0.0;
    double swayTargetLateralNorm_ = 0.0;
    double swayGoalXNorm_ = 0.0;
    double swayGoalYNorm_ = 0.0;
    double swayMoveSpeedScale_ = 1.0;
    long long swayNextTargetMs_ = 0;
    long long lastSwayNoiseMs_ = 0;
    int swayLastDx_ = 0;
    int swayLastDy_ = 0;
    bool swayLastOuter_ = false;
    bool swayLastMouseSmall_ = false;
    bool swayLastWideRadius_ = false;
    bool swayDirectionalOuterAllowed_ = false;
    long long swayDirectionSwitchUntilMs_ = 0;
    long long mouseSmallUntilMs_ = 0;
    long long lastMouseButtonDownMs_ = 0;

    bool justSwitchedDirection_ = false;
    int switchBoostFrames_ = 0;
    bool isSingleDirection_ = false;
    int targetUpdateCounter_ = 0;
    long long zeroSinceMs_ = -1;

    long long lastMouseMs_ = 0;
    int lastMouseDx_ = 0;
    int lastMouseDy_ = 0;
    bool mouseLeftDown_ = false;
    bool mouseRightDown_ = false;
    double mouseFilteredDx_ = 0.0;
    double mouseFilteredDy_ = 0.0;
    double swayBiasRad_ = 0.0;
    mutable long long lastTouchFrameUs_ = 0;
    std::mt19937 rng_{std::random_device{}()};
};
