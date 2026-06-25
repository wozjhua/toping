#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>

#include "pc_mapping_profile.h"

// PC 菜单运行时：
// - 轮盘菜单：长按触发，期间独占输入；旧版逻辑是停止视角滑动状态机，不强制释放已有 lock slot。
// - 道具操作：按下触发键后点击触发 UI，进入自由鼠标；右键点击关闭 UI 并恢复视角锁定。
// - 背包操作：按住触发键 50ms 后点击触发 UI，进入自由鼠标；松开触发键点击关闭 UI 并恢复视角锁定。
// Android 端只接收最终 touchDown/touchMove/touchUp。
class PcMenuRuntime final {
public:
    // 菜单 RawInput 诊断统计：只用于 UI 低频展示，不改变菜单状态机语义。
    // rawInputMouse 表示成功读取到的鼠标 RawInput 包数；applyCalls 表示 2ms 合并后真正调用 ApplyRelative 的次数。
    struct DebugStats {
        uint64_t rawInputMouse = 0;
        uint64_t rawInputBytesRejected = 0;
        uint64_t rawInputIgnoredNotReady = 0;
        uint64_t applyCalls = 0;
        uint64_t coalesceFlushes = 0;
        uint64_t coalesceForcedFlushes = 0;
        int64_t rawDx = 0;
        int64_t rawDy = 0;
        bool active = false;
        bool secondaryFree = false;
        int cursorXNorm = 500000;
        int cursorYNorm = 500000;
    };

    struct Callbacks {
        std::function<bool(int slot, int xNorm, int yNorm)> touchDown;
        std::function<bool(int slot)> touchUp;
        std::function<bool(int slot, int xNorm, int yNorm)> touchMove;

        // 一次性点击：宿主从当前未占用 slot 池里取一个 slot，执行 down/up 后立即回收。
        // 道具/背包的“触发 UI”和“关闭 UI”必须走这个路径，不能复用视角锁定 slot，
        // 也不能把菜单自由鼠标 slot 长时间占住。
        std::function<bool(int xNorm, int yNorm, int avoidSlot)> tapTransient;

        // 菜单开始/结束时由宿主切换输入接管开关。
        // 注意：这里不是全局释放 slot；轮盘菜单只暂停视角滑动逻辑，保持旧版状态机语义。
        std::function<void(PcMenuCategory category)> menuBegin;
        std::function<void(PcMenuCategory category)> menuEnd;

        // 道具/背包二阶段自由鼠标的位置变化。宿主用它同步 Windows 可见鼠标，
        // 避免内部菜单光标和实际系统光标分离导致点击偏移。
        std::function<void(int xNorm, int yNorm)> freeCursorMove;
    };

    void SetCallbacks(Callbacks cb);
    void SetProfileProvider(std::function<const PcMappingProfile&()> provider);
    void Reset();
    void Tick();
    bool IsActive() const { return active_; }
    bool IsFreeCursorActive() const { return active_ && stage_ == Stage::SecondaryFree; }
    bool IsSecondaryFreeActive() const { return IsFreeCursorActive(); }
    std::wstring StatusText() const;

    // 菜单 active 时的轻量 RawInput 入口：只读鼠标包、累计 dx/dy、按 2ms 合并。
    // 按键释放、右键关闭、菜单结束仍然走 HandleWindowMessage，不在这里延迟。
    bool ConsumeActiveRawMouse(HWND hwnd, LPARAM lp);
    void FlushPendingRawDelta(bool force = false);
    DebugStats SnapshotDebugStats() const;
    DebugStats SnapshotAndResetDebugStats();

    bool HandleWindowMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LRESULT* outResult = nullptr);

private:
    enum class Stage {
        None,
        RadialActive,
        TriggerHeld,
        SecondaryFree,
    };

    static UINT NormalizeVk(UINT vk, LPARAM lp);
    static bool ComboSatisfied(const PcMenuBinding& m, const std::array<bool, 256>& vkDown);
    static bool IsRelatedKey(const PcMenuBinding& m, int vk);
    static int ClampNorm(int v);
    static int ClampRadiusNorm(int v);
    static long long NowMs();
    static int RandomAnchorRadiusNorm(const PcMenuBinding& m);

    const PcMenuBinding* FindActiveMenu() const;
    const PcMenuBinding* FindPressedMenu() const;
    bool IsFreeCursorMenu(const PcMenuBinding& m) const;
    bool IsRadialMenu(const PcMenuBinding& m) const;
    bool IsBagMenu(const PcMenuBinding& m) const;

    void BeginMenu(const PcMenuBinding& m);
    void TapTriggerPoint(const PcMenuBinding& m);
    void EnterSecondaryFree(const PcMenuBinding& m);
    void TapClosePoint(const PcMenuBinding& m);
    void EndMenu(bool notifyEnd = true);
    void ApplyRelative(int dx, int dy);
    bool ReadRawMouseDelta(HWND hwnd, LPARAM lp, int& dx, int& dy);
    void QueueRawDelta(int dx, int dy);
    void ApplyPendingRawDelta(bool forced);
    void UpdateClientSize(HWND hwnd);
    double RawDeltaToNormX(const PcMenuBinding& m, int dx) const;
    double RawDeltaToNormY(const PcMenuBinding& m, int dy) const;
    void ClampToMenuRange(double& x, double& y) const;
    void ClampToRadialVisible(double& x, double& y) const;
    void CommitCursor(double x, double y);
    void PickRandomPointInAnchor(int centerXNorm, int centerYNorm, int radiusNorm, int& outXNorm, int& outYNorm) const;
    void AddRawMouseDelta(HWND hwnd, LPARAM lp);
    bool HandleMouseButton(UINT msg, WPARAM wp, LPARAM lp);
    bool TriggerStateChanged(const PcMenuBinding& m, bool& stillPressedOut) const;
    void ClampToRadial(int& x, int& y) const;

private:
    Callbacks callbacks_{};
    std::function<const PcMappingProfile&()> profileProvider_;
    std::array<bool, 256> vkDown_{};

    bool active_ = false;
    std::string activeId_;
    std::string suppressReopenId_;
    PcMenuCategory activeCategory_ = PcMenuCategory::MenuRadial;
    Stage stage_ = Stage::None;
    int activeSlot_ = 10;
    int centerXNorm_ = 500000;
    int centerYNorm_ = 500000;
    int triggerXNorm_ = 500000;
    int triggerYNorm_ = 500000;
    int closeXNorm_ = 500000;
    int closeYNorm_ = 500000;
    int cursorXNorm_ = 500000;
    int cursorYNorm_ = 500000;
    double cursorXNormF_ = 500000.0;
    double cursorYNormF_ = 500000.0;
    int radiusNorm_ = 90000;
    int rangeLeftNorm_ = 0;
    int rangeTopNorm_ = 0;
    int rangeRightNorm_ = 1000000;
    int rangeBottomNorm_ = 1000000;
    double speedX_ = 0.5;
    double speedY_ = 0.5;
    int clientWidth_ = 1000;
    int clientHeight_ = 1000;

    static constexpr int kRawDeltaCoalesceMs = 2;
    int pendingRawDx_ = 0;
    int pendingRawDy_ = 0;
    bool pendingRawActive_ = false;
    std::chrono::steady_clock::time_point pendingRawFirstAt_{};
    std::chrono::steady_clock::time_point pendingRawLastAt_{};
    DebugStats debugStats_{};

    bool touchActive_ = false;
    bool leftDown_ = false;
    bool triggerWasDown_ = false;
    bool menuBeginNotified_ = false;
    long long triggerDownMs_ = 0;
};
