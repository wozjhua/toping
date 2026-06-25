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
#include "pc_lock_options_dialog.h"

// PC 端视角锁定运行时。
// Android pc_uinputd 只执行最终 TOUCH_DOWN/MOVE/UP；这里负责锁鼠标、RawInput 相对移动、单/双 slot 触点重锚。
class PcLockRuntime final {
public:
    struct Callbacks {
        std::function<bool(int slot, int xNorm, int yNorm)> touchDown;
        std::function<bool(int slot, int xNorm, int yNorm)> touchMove;
        std::function<bool(int slot)> touchUp;
        std::function<void(bool locked)> lockChanged;
    };

    // 诊断统计：只用于低频 UI 可视化，不改变输入语义。
    // SnapshotAndResetDebugStats() 返回上一个刷新窗口内的计数，并保留当前锁定/slot 状态快照。
    struct DebugStats {
        uint64_t rawInputMessages = 0;
        uint64_t rawInputMouse = 0;
        uint64_t rawInputIgnoredNotReady = 0;
        uint64_t rawInputBytesRejected = 0;
        uint64_t coalesceFlushes = 0;
        uint64_t coalesceForcedFlushes = 0;
        uint64_t coalesceDeferredPackets = 0;
        uint64_t applyCalls = 0;
        uint64_t applySkippedState = 0;
        uint64_t applySkippedZero = 0;
        uint64_t applySkippedTiny = 0;
        uint64_t moveAttempts = 0;
        uint64_t touchMoves = 0;
        uint64_t reanchors = 0;
        uint64_t touchDowns = 0;
        uint64_t touchUps = 0;
        int64_t rawDx = 0;
        int64_t rawDy = 0;
        bool locked = false;
        bool inputSuspended = false;
        int mode = 0;
        int primarySlot = -1;
        bool primaryDown = false;
        int primaryXNorm = 500000;
        int primaryYNorm = 500000;
        int auxSlot = -1;
        bool auxDown = false;
        int auxXNorm = 500000;
        int auxYNorm = 500000;
    };

    void SetCallbacks(Callbacks cb);
    void SetProfileProvider(std::function<const PcMappingProfile& ()> provider);
    void Start(HWND hwnd);
    void Stop();
    void Reset();
    // 菜单/其它高优先级模式接管 RawInput 时，只暂停视角移动逻辑，不释放当前视角触点。
    // 这更接近旧版 input_mode_state：菜单 active 时鼠标相对移动走菜单，不再走 lock slide。
    void SetInputSuspendedByMenu(bool suspended);
    void PauseTouchForMenu();
    void ResumeTouchAfterMenu();
    // 道具/背包二阶段进入“自由模式”时必须切全局视角锁定状态：locked_ = false。
    // 这会释放当前视角三种模式占用的 slot，并让后续 RawInput 不再走视角滑动。
    // 关闭道具/背包时再切回 locked_ = true，重新进入视角锁定模式。
    void EnterMenuFreeCursorMode();
    void LeaveMenuFreeCursorMode();
    void SetLockedFromMenu(bool locked);

    // 道具/背包强制进入自由模式后，旧的视角触发键物理状态不能继续保留，
    // 否则在二阶段自由鼠标里再次按视角触发键时 comboWasDown_ 会卡住，无法回锁。
    void ClearKeyStateForMenuFreeMode();

    bool IsLocked() const { return locked_; }
    bool IsInputSuspendedByMenu() const { return menuInputSuspended_; }
    bool HasLockConfig() const;
    void ReclickLockTouch(bool dismountStyle = false);
    std::wstring StatusText() const;
    DebugStats SnapshotDebugStats() const;
    DebugStats SnapshotAndResetDebugStats();
    bool FlushPendingRawDelta(bool force = false);
    // 锁定态 WM_INPUT 快速入口：只消费 RawInput 鼠标包、累计 dx/dy、按 2ms 合并刷新。
    // 用于 d3d11_native.cpp 的锁定态 fast path，避免每个鼠标包进入通用 HandleWindowMessage 分支。
    bool ConsumeLockedRawMouse(HWND hwnd, LPARAM lp);

    bool HandleWindowMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LRESULT* outResult = nullptr);

private:
    struct SlotState {
        int slot = -1;
        bool down = false;
        int xNorm = 500000;
        int yNorm = 500000;
        double xNormF = 500000.0;
        double yNormF = 500000.0;
    };

    struct DeltaMoveResult {
        bool moved = false;
        bool blocked = false;
        double remainingXNorm = 0.0;
        double remainingYNorm = 0.0;
    };

    static UINT NormalizeVk(UINT vk, LPARAM lp);
    static int ClampInt(int v, int lo, int hi);
    static bool ComboSatisfied(const PcLockBinding& b, const std::array<bool, 256>& vkDown);
    static std::wstring ModeLabel(PcLockSlideTouchMode mode);

    void NotifyLockedChanged();
    void RegisterRawMouse(bool enable);
    bool NormToScreenPoint(int xNorm, int yNorm, POINT& out) const;
    void LockCursorToNormPoint(int xNorm, int yNorm);
    void RefreshLockedCursorAnchor();
    void ClipMouse(bool enable);
    bool LoadActiveLockConfig();
    void ClearActiveLockConfig();
    void RebuildRuntimeCache(const PcLockBinding& cfg);
    void SetLocked(bool locked);
    void BeginTouchLocked(const PcLockBinding& cfg);
    void EndTouchLocked();
    void ClearPendingRawDelta();
    void ClearPendingRebuild();
    void ClearPendingAuxDown();
    void SchedulePendingAuxDown();
    bool FinishPendingAuxDownIfReady(const PcLockBinding& cfg, bool force = false);
    int ClampRebuildDelayMs(const PcLockBinding& cfg) const;
    bool FinishPendingRebuildIfReady(const PcLockBinding& cfg);
    void ScheduleSingleRebuild(const PcLockBinding& cfg, double preferredXNorm, double preferredYNorm, const wchar_t* tag);
    void ScheduleDualRebuild(const PcLockBinding& cfg, double preferredXNorm, double preferredYNorm, int mode, const wchar_t* tag);
    void AccumulateRawDelta(int dx, int dy);
    void ApplyRawDelta(const PcLockBinding& cfg, int dx, int dy);

    void SingleMove(const PcLockBinding& cfg, double dxNorm, double dyNorm);
    void DualSequentialMove(const PcLockBinding& cfg, double dxNorm, double dyNorm);
    void RebuildSequentialPair(const PcLockBinding& cfg, double preferredXNorm, double preferredYNorm, const wchar_t* tag);
    void DualMove(const PcLockBinding& cfg, double dxNorm, double dyNorm, bool sequential);
    bool MoveSlotClamped(const PcLockBinding& cfg, SlotState& st, double dxNorm, double dyNorm);
    DeltaMoveResult MoveSlotConsumeDelta(const PcLockBinding& cfg, SlotState& st, double dxNorm, double dyNorm);
    bool LiftSlot(SlotState& st);
    void ReanchorSlot(const PcLockBinding& cfg, SlotState& st, double preferredXNorm, double preferredYNorm, const wchar_t* tag);
    SlotState* ChooseSlotForDelta(const PcLockBinding& cfg, double dxNorm, double dyNorm);
    double DistanceToEdgeSqForDelta(const PcLockBinding& cfg, const SlotState& st, double dxNorm, double dyNorm) const;

private:
    HWND hwnd_{};
    Callbacks callbacks_;
    std::function<const PcMappingProfile& ()> profileProvider_;
    std::array<bool, 256> vkDown_{};
    bool comboWasDown_ = false;
    bool locked_ = false;
    HCURSOR prevCursor_{};
    bool cursorHidden_ = false;
    POINT lockedCursorScreenPoint_{};
    bool lockedCursorPointValid_ = false;
    bool rawRegistered_ = false;
    bool menuInputSuspended_ = false;
    bool menuReleasedTouch_ = false; // 兼容旧字段；道具/背包新流程不再用“临时释放但保持 locked_”。

    SlotState primary_{ 2, false, 500000, 500000, 500000.0, 500000.0 };
    SlotState aux_{ 9, false, 500000, 500000, 500000.0, 500000.0 };
    int activeSlotIndex_ = 0; // 0=primary, 1=aux

    PcLockBinding activeCfg_{};
    bool activeCfgValid_ = false;
    int cachedClientW_ = 1000;
    int cachedClientH_ = 1000;
    double rawScaleX_ = PcLockOptionLimits::kSpeedNormDefault * 1000000.0 / 999.0;
    double rawScaleY_ = PcLockOptionLimits::kSpeedNormDefault * 1000000.0 / 999.0;

    bool pendingRawDeltaActive_ = false;
    int64_t pendingRawDx_ = 0;
    int64_t pendingRawDy_ = 0;
    uint64_t pendingRawPackets_ = 0;
    std::chrono::steady_clock::time_point pendingRawDeltaStart_{};

    // 边界重按延迟状态。0=无，1=单slot，2=双slot同时，3=双slot顺序。
    int pendingRebuildMode_ = 0;
    int pendingRebuildXNorm_ = 500000;
    int pendingRebuildYNorm_ = 500000;
    std::chrono::steady_clock::time_point pendingRebuildDue_{};

    static constexpr UINT_PTR kPendingAuxDownTimerId = 0x4C41; // 'LA'，仅在顺序双 slot 延迟期间启用。
    bool pendingAuxDownActive_ = false;
    std::chrono::steady_clock::time_point pendingAuxDownDue_{};

    DebugStats debugStats_{};
};
