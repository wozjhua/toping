#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <random>
#include <string>
#include <vector>

#include "pc_mapping_profile.h"

// PC 映射运行时。当前先实现“普通按键”：单键/组合键 -> 固定坐标 TouchHold/TouchTap。
class PcMappingRuntime final {
public:
    PcMappingRuntime();

    struct Callbacks {
        std::function<bool(int slot, int xNorm, int yNorm)> touchDown;
        std::function<bool(int slot, int xNorm, int yNorm)> touchMove;
        std::function<bool(int slot)> touchUp;
        std::function<bool(int linuxCode, bool down)> key;
        std::function<bool(int wheelSteps)> wheel;
        std::function<void(PcKeySpecialAction action)> specialAction;
    };

    void SetCallbacks(Callbacks cb);
    void SetProfile(const PcMappingProfile& profile);
    const PcMappingProfile& GetProfile() const;

    void SetEnabled(bool enabled);
    bool IsEnabled() const;
    void SetLockActiveProvider(std::function<bool()> provider);

    void Reset();
    void Tick();
    std::wstring StatusText() const;

    bool HandleWindowMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LRESULT* outResult = nullptr);

private:
    struct ActiveTouch {
        // active=true 表示 TOUCH_DOWN 已经真正发到设备。
        // pendingDown=true 表示该绑定正在等待安全延迟/空闲物理触点；此时尚未占用 slot。
        bool active = false;
        bool pendingDown = false;
        uint64_t generation = 0;
        std::chrono::steady_clock::time_point pendingDownDue{};
        int slot = -1;
        int baseXNorm = 0;
        int baseYNorm = 0;
        int curXNorm = 0;
        int curYNorm = 0;

        // RandomMove：在 UI 圆半径内持续随机游走。
        // 每段移动先选一个目标点，再用较长时间平滑插值，避免高速抖动。
        bool hasMoveTarget = false;
        int moveStartXNorm = 0;
        int moveStartYNorm = 0;
        int moveTargetXNorm = 0;
        int moveTargetYNorm = 0;
        int moveDurationMs = 0;
        std::chrono::steady_clock::time_point moveStart{};
        std::chrono::steady_clock::time_point lastMove{};
    };

    static UINT NormalizeVk(UINT vk, LPARAM lp);
    static int MouseButtonCodeFromMessage(UINT msg, WPARAM wp);
    bool HandleTriggerStateChanged(PcMappingTriggerType type, int code, bool down, bool repeat);
    bool IsBindingComboSatisfied(const PcMappingBinding& b) const;
    bool ExecuteBindingAtIndex(size_t bindingIndex, bool down, bool repeat);
    void ReleaseBindingTouch(size_t bindingIndex);
    static bool IsRuntimeTouchSlot(int slot);
    bool IsReservedRuntimeTouchSlot(int slot) const;
    bool RuntimeSlotOwnedBy(size_t bindingIndex, int slot) const;
    int AllocateRuntimeSlot(size_t bindingIndex);
    void ReleaseRuntimeSlot(size_t bindingIndex, int slot, bool startCooldown);
    void SchedulePendingDownTimer(HWND hwnd);
    void StopPendingDownTimer();
    bool HasPendingDownTouches() const;
    void FinishPendingDownTouches();
    void RandomPointInRadius(int radius, int& dx, int& dy);
    void ClampPair(int& x, int& y) const;
    void UpdateClientSize(HWND hwnd);
    bool IsLockActive() const;
    void RebuildBindingBuckets();
    bool BindingBelongsToLockedLeftBox(const PcMappingBinding& b) const;
    bool HandleTriggerStateChangedInList(const std::vector<size_t>& indices, PcMappingTriggerType type, int code, bool down, bool repeat);

private:
    bool enabled_ = false;
    PcMappingProfile profile_;
    Callbacks callbacks_;
    HWND hwnd_{};
    bool pendingDownTimerActive_ = false;
    static constexpr UINT_PTR kPendingDownTimerId = 0x504D4433u; // "PMD3"：普通映射 3ms pending down
    static constexpr int kRuntimeTouchSlotCount = 10;
    static constexpr int kNoRuntimeSlotOwner = -1;

    std::array<bool, 256> vkDown_{};
    std::array<bool, 8> mouseDown_{};
    std::vector<bool> bindingActive_;
    std::vector<ActiveTouch> activeTouches_;
    std::array<std::chrono::steady_clock::time_point, kRuntimeTouchSlotCount> slotCooldownUntil_{};
    std::array<int, kRuntimeTouchSlotCount> runtimeSlotOwner_{};
    std::vector<size_t> normalBindingIndices_;
    std::vector<size_t> lockedLeftBindingIndices_;
    std::function<bool()> lockActiveProvider_;
    int clientWidthPx_ = 0;
    int clientHeightPx_ = 0;
    std::mt19937 rng_{ std::random_device{}() };
};
