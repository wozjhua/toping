#include "d3d11_renderer.h"

// configureMappingRuntimeCallbacks
void D3D11Renderer::configureMappingRuntimeCallbacks() {
        PcMappingRuntime::Callbacks cb;
        cb.touchDown = [this](int slot, int xNorm, int yNorm) {
            return runtimeTouchDown(slot, xNorm, yNorm);
            };
        cb.touchMove = [this](int slot, int xNorm, int yNorm) {
            return runtimeTouchMove(slot, xNorm, yNorm);
            };
        cb.touchUp = [this](int slot) {
            return runtimeTouchUp(slot);
            };
        cb.key = [this](int linuxCode, bool down) {
            const bool ok = pcUinput_.SendLinuxKey(linuxCode, down);
            if (!ok) markPcUinputNeedsRetry();
            return ok;
            };
        cb.wheel = [this](int steps) {
            const bool ok = pcUinput_.SendWheelSteps(steps);
            if (!ok) markPcUinputNeedsRetry();
            return ok;
            };
        cb.specialAction = [this](PcKeySpecialAction action) {
            if (action == PcKeySpecialAction::ReclickLockOnKeyDown) {
                lockRuntime_.ReclickLockTouch(false);
            }
            else if (action == PcKeySpecialAction::DismountReclickLockOnKeyDown) {
                lockRuntime_.ReclickLockTouch(true);
            }
            };
        mappingRuntime_.SetCallbacks(std::move(cb));
        mappingRuntime_.SetLockActiveProvider([this] { return lockRuntime_.IsLocked(); });

        PcCompassRuntime::Callbacks ccb;
        ccb.touchDown = [this](int slot, int xNorm, int yNorm) { return runtimeTouchDown(slot, xNorm, yNorm); };
        ccb.touchMove = [this](int slot, int xNorm, int yNorm) { return runtimeTouchMove(slot, xNorm, yNorm); };
        ccb.touchUp = [this](int slot) { return runtimeTouchUp(slot); };
        compassRuntime_.SetCallbacks(std::move(ccb));
        compassRuntime_.SetProfileProvider([this]() -> const PcMappingProfile& { return mappingRuntime_.GetProfile(); });

        PcMenuRuntime::Callbacks mcb;
        mcb.touchDown = [this](int slot, int xNorm, int yNorm) { return runtimeTouchDown(slot, xNorm, yNorm); };
        mcb.touchMove = [this](int slot, int xNorm, int yNorm) { return runtimeTouchMove(slot, xNorm, yNorm); };
        mcb.touchUp = [this](int slot) { return runtimeTouchUp(slot); };
        mcb.tapTransient = [this](int xNorm, int yNorm, int avoidSlot) {
            return runtimeTapWithUnusedSlot(xNorm, yNorm, avoidSlot);
            };
        mcb.menuBegin = [this](PcMenuCategory category) {
            resetMenuDebugAllCounters();
            if (isMenuFreeCursorCategoryLocal(category)) {
                // 道具/背包：触发圆点已经随机点击完成。这里必须进入全局自由模式，
                // 即 locked_ = false，并抬起当前视角锁定 slot，后续鼠标移动只走道具/背包自由鼠标。
                lockRuntime_.EnterMenuFreeCursorMode();
                if (category == PcMenuCategory::MenuBagOperation) {
                    mappingEditorStatus_ = HLW(L"背包操作：按住 50ms 后触发，松开隐藏鼠标");
                }
                else {
                    mappingEditorStatus_ = HLW(L"道具操作：按下后触发，右键隐藏鼠标");
                }
            }
            else {
                // 轮盘仍保持旧语义：只暂停视角滑动输入，不强制释放锁定触点。
                lockRuntime_.SetInputSuspendedByMenu(true);
                mappingEditorStatus_ = HLW(L"菜单：已接管输入，视角滑动暂停");
            }
            updateStatusText(mappingEditorStatus_);
            };
        mcb.menuEnd = [this](PcMenuCategory category) {
            menuRuntime_.FlushPendingRawDelta(true);
            maybeRefreshMenuDebugOverlay(true);
            if (isMenuFreeCursorCategoryLocal(category)) {
                // 关闭 UI 已先完成临时 slot 点击；随后明确回到全局视角锁定模式。
                lockRuntime_.LeaveMenuFreeCursorMode();
                mappingEditorStatus_ = (category == PcMenuCategory::MenuBagOperation)
                    ? HLW(L"背包操作：松开隐藏鼠标，已进入视角锁定")
                    : HLW(L"道具操作：右键隐藏鼠标，已进入视角锁定");
            }
            else {
                lockRuntime_.SetInputSuspendedByMenu(false);
                mappingEditorStatus_ = lockRuntime_.IsLocked() ? HLW(L"菜单结束：视角滑动恢复") : HLW(L"菜单结束");
            }
            updateStatusText(mappingEditorStatus_);
            };
        mcb.freeCursorMove = [this](int xNorm, int yNorm) {
            warpSystemCursorToNorm(xNorm, yNorm);
            };
        menuRuntime_.SetCallbacks(std::move(mcb));
        menuRuntime_.SetProfileProvider([this]() -> const PcMappingProfile& { return mappingRuntime_.GetProfile(); });

        PcMacroRuntime::Callbacks macb;
        macb.touchDown = [this](int slot, int xNorm, int yNorm) { return runtimeTouchDown(slot, xNorm, yNorm); };
        macb.touchMove = [this](int slot, int xNorm, int yNorm) { return runtimeTouchMove(slot, xNorm, yNorm); };
        macb.touchUp = [this](int slot) { return runtimeTouchUp(slot); };
        macb.key = [this](int linuxCode, bool down) {
            const bool ok = pcUinput_.SendLinuxKey(linuxCode, down);
            if (!ok) markPcUinputNeedsRetry();
            return ok;
            };
        macb.wheel = [this](int steps) {
            const bool ok = pcUinput_.SendWheelSteps(steps);
            if (!ok) markPcUinputNeedsRetry();
            return ok;
            };
        macroRuntime_.SetCallbacks(std::move(macb));
        macroRuntime_.SetBindings(pcMacros_);
        SetPcMappingOverlayMacroSource(&pcMacros_, selectedMacroIndex_, selectedMacroStepId_, macroStepEditorVisible_);

        PcLockRuntime::Callbacks lcb;
        lcb.touchDown = [this](int slot, int xNorm, int yNorm) { return runtimeTouchDown(slot, xNorm, yNorm); };
        lcb.touchMove = [this](int slot, int xNorm, int yNorm) { return runtimeTouchMove(slot, xNorm, yNorm); };
        lcb.touchUp = [this](int slot) { return runtimeTouchUp(slot); };
        lcb.lockChanged = [this](bool locked) {
            resetLockDebugAllCounters();
            lockRuntime_.SnapshotAndResetDebugStats();
            lastLockDebugOverlayUpdate_ = std::chrono::steady_clock::now() - std::chrono::milliseconds(kLockDebugOverlayRefreshMs);
            mappingEditorStatus_ = locked
                ? HLW(L"视角：已锁定（RawInput 2ms 合并已开启，日志低频刷新）")
                : HLW(L"视角：自由模式");
            updateStatusText(mappingEditorStatus_);
            refreshMappingToolbarStatus();
            };
        lockRuntime_.SetCallbacks(std::move(lcb));
        lockRuntime_.SetProfileProvider([this]() -> const PcMappingProfile& { return mappingRuntime_.GetProfile(); });
        if (hwnd_) lockRuntime_.Start(hwnd_);

        // 只配置回调，不再清空现有映射。pc_uinput 自动重连会多次调用这里，
        // 如果此处重置 profile，会导致已保存/已创建的映射在重连时丢失。
        mappingRuntime_.SetEnabled(!mappingRuntime_.GetProfile().Empty() || !pcMacros_.empty());
    }

