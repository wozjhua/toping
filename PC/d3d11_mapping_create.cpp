#include "d3d11_renderer.h"

// beginCreateMappingKey
void D3D11Renderer::beginCreateMappingKey() {
        if (!mappingEditMode_) setMappingEditMode(true);
        mappingCreateMode_ = MappingCreateMode::Key;
        mappingEditorStatus_ = HLW(L"创建按键：请点击投屏位置");
        updateStatusText(mappingEditorStatus_);
        refreshMappingToolbarStatus();
    }

// beginCreateMappingCompass
void D3D11Renderer::beginCreateMappingCompass() {
        if (!mappingEditMode_) setMappingEditMode(true);
        mappingCreateMode_ = MappingCreateMode::Compass;
        mappingEditorStatus_ = HLW(L"创建轮盘：请点击轮盘中心位置");
        updateStatusText(mappingEditorStatus_);
        refreshMappingToolbarStatus();
    }

// beginCreateMappingLock
void D3D11Renderer::beginCreateMappingLock() {
        if (!mappingEditMode_) setMappingEditMode(true);
        mappingCreateMode_ = MappingCreateMode::Lock;
        mappingEditorStatus_ = HLW(L"创建视角：请点击视角中心点");
        updateStatusText(mappingEditorStatus_);
        refreshMappingToolbarStatus();
    }

// beginCreateMappingMenu
void D3D11Renderer::beginCreateMappingMenu(PcMenuCategory cat) {
        if (!mappingEditMode_) setMappingEditMode(true);
        switch (cat) {
        case PcMenuCategory::MenuRadial:
            mappingCreateMode_ = MappingCreateMode::MenuRadial;
            mappingEditorStatus_ = HLW(L"创建轮盘菜单：请点击中心位置");
            break;
        case PcMenuCategory::MenuBagOperation:
            mappingCreateMode_ = MappingCreateMode::MenuBag;
            mappingEditorStatus_ = HLW(L"创建背包操作：按住 50ms 后触发，松开隐藏鼠标");
            break;
        case PcMenuCategory::MenuItemOperation:
        case PcMenuCategory::MenuHorizontal:
        case PcMenuCategory::MenuVertical:
            mappingCreateMode_ = MappingCreateMode::MenuItem;
            mappingEditorStatus_ = HLW(L"创建道具操作：按下后触发，右键隐藏鼠标");
            break;
        default:
            mappingCreateMode_ = MappingCreateMode::MenuRadial;
            mappingEditorStatus_ = HLW(L"创建轮盘菜单：请点击中心位置");
            break;
        }
        updateStatusText(mappingEditorStatus_);
        refreshMappingToolbarStatus();
    }

// beginCreateMappingMacro
void D3D11Renderer::beginCreateMappingMacro() {
        if (!mappingEditMode_) setMappingEditMode(true);
        mappingCreateMode_ = MappingCreateMode::None;
        RECT rc{};
        GetClientRect(hwnd_, &rc);
        const int cxPx = (std::max)(1L, rc.right - rc.left) / 2;
        const int cyPx = (std::max)(1L, rc.bottom - rc.top) / 2;
        int nx = 500000, ny = 500000;
        clientPointToNormLocal(hwnd_, cxPx, cyPx, nx, ny);

        PcMacroRuntime::Binding macro;
        macro.id = "macro_" + std::to_string(static_cast<unsigned long long>(pcMacros_.size() + 1));
        macro.enabled = true;
        macro.triggerLabel = L"";
        macro.triggerCondition = PcMacroRuntime::TriggerCondition::Press;
        macro.slot = allocateMappingSlot();
        macro.xNorm = nx;
        macro.yNorm = ny;
        macro.radiusNorm = 70000;
        macro.startStepId = 1;
        macro.consumeEvent = true;

        PcMacroRuntime::Step first = makeDefaultMacroStep(macro, PcMappingProfile::ClampNorm(nx + 84000), ny);
        first.id = 1;
        first.order = 1;
        macro.steps.push_back(first);

        pcMacros_.push_back(macro);
        selectedMacroIndex_ = static_cast<int>(pcMacros_.size() - 1);
        selectedMacroStepId_ = first.id;
        selectedMappingBindingIndex_ = -1;
        selectedMenuIndex_ = -1;
        compassSelected_ = false;
        macroStepEditorVisible_ = false;
        syncMacroRuntimeAndOverlay();
        mappingEditorStatus_ = HLW(L"普通宏：已创建。点“绑”设置触发键，点“按下/按住”切换，点 + 新增步骤");
        updateStatusText(mappingEditorStatus_);
    }

// handleMappingCreateClick
bool D3D11Renderer::handleMappingCreateClick(LPARAM lp) {
        if (mappingCreateMode_ == MappingCreateMode::None) return false;

        const int x = GET_X_LPARAM(lp);
        const int y = GET_Y_LPARAM(lp);
        int nx = 0, ny = 0;
        if (!clientPointToNormLocal(hwnd_, x, y, nx, ny)) {
            mappingCreateMode_ = MappingCreateMode::None;
            mappingEditorStatus_ = HLW(L"创建失败：坐标无效");
            updateStatusText(mappingEditorStatus_);
            return true;
        }


        if (mappingCreateMode_ == MappingCreateMode::Macro) {
            // 普通宏现在和其它映射一样：点击工具条按钮时立即创建 Overlay 节点。
            // 这里保留兜底，避免旧状态触发弹窗式创建。
            mappingCreateMode_ = MappingCreateMode::None;
            beginCreateMappingMacro();
            return true;
        }

        if (mappingCreateMode_ == MappingCreateMode::MenuRadial || mappingCreateMode_ == MappingCreateMode::MenuItem || mappingCreateMode_ == MappingCreateMode::MenuBag) {
            PcMenuCategory cat = PcMenuCategory::MenuRadial;
            if (mappingCreateMode_ == MappingCreateMode::MenuItem) cat = PcMenuCategory::MenuItemOperation;
            else if (mappingCreateMode_ == MappingCreateMode::MenuBag) cat = PcMenuCategory::MenuBagOperation;

            PcMappingProfile profile = mappingRuntime_.GetProfile();
            PcMenuBinding m;
            m.id = "menu" + std::to_string(static_cast<unsigned long long>(profile.menus.size() + 1));
            m.category = cat;
            m.triggerMode = (cat == PcMenuCategory::MenuRadial) ? PcMenuTriggerMode::HoldRelative : PcMenuTriggerMode::HoldFreeCursor;
            m.shapeType = (cat == PcMenuCategory::MenuRadial)
                ? PcMenuShapeType::Radial
                : (cat == PcMenuCategory::MenuBagOperation ? PcMenuShapeType::BagOperation : PcMenuShapeType::ItemOperation);
            m.triggerPlacement = PcMenuTriggerPlacement::Bottom;
            m.itemLandingZone = PcItemLandingZone::Bottom;
            m.itemWheelScrollEnabled = false;
            m.itemWheelInvert = false;
            m.itemWheelScrollSpeed = PC_MENU_ITEM_WHEEL_SPEED_DEFAULT;
            m.itemWheelMaxDistancePx = PC_MENU_ITEM_WHEEL_DISTANCE_DEFAULT;
            m.itemWheelStepPx = PC_MENU_ITEM_WHEEL_STEP_DEFAULT;
            m.centerXNorm = nx;
            m.centerYNorm = ny;
            m.triggerXNorm = nx;
            m.triggerYNorm = ny;
            m.freeCursorXNorm = nx;
            m.freeCursorYNorm = ny;
            m.radiusNorm = PC_MENU_BUTTON_RADIUS_DEFAULT_NORM;
            if (cat != PcMenuCategory::MenuRadial) {
                const int offset = 125000;
                const int dir = (nx + offset * 2 <= 1000000) ? 1 : -1;
                m.freeCursorXNorm = PcMappingProfile::ClampNorm(nx + dir * offset);
                m.freeCursorYNorm = ny;
                m.closeXNorm = PcMappingProfile::ClampNorm(nx + dir * offset * 2);
                m.closeYNorm = ny;
            }
            else {
                m.closeXNorm = nx;
                m.closeYNorm = ny;
            }
            m.slot = menuSlotForCategory(cat);
            m.segmentCount = 6;
            m.triggerLabel = HLW(L"未绑定");
            const int r = clampMenuButtonRadiusNormLocal(m.radiusNorm);
            m.centerXNorm = (std::max)(r, (std::min)(1000000 - r, m.centerXNorm));
            m.centerYNorm = (std::max)(r, (std::min)(1000000 - r, m.centerYNorm));
            resetMenuRangeToDefault(m);
            profile.menus.push_back(m);
            const size_t pendingIndex = profile.menus.size() - 1;
            selectedMenuIndex_ = static_cast<int>(pendingIndex);
            selectedMappingBindingIndex_ = -1;
            compassSelected_ = false;
            mappingRuntime_.SetProfile(profile);
            mappingRuntime_.SetEnabled(true);
            mappingOverlayDirty_ = true;
            dirtyWindow_ = true;
            mappingEditorStatus_ = std::wstring(PcMenuCategoryLabel(cat)) + HLW(L"：已放置，请绑定触发键");
            updateStatusText(mappingEditorStatus_);
            refreshMappingToolbarStatus();
            render(false);

            PcCapturedKeyCombo combo;
            if (!CaptureKeyComboDialog(inst_, hwnd_, combo) || combo.vkCodes.empty() || combo.mouseButtonCode > 0) {
                PcMappingProfile cancelProfile = mappingRuntime_.GetProfile();
                if (pendingIndex < cancelProfile.menus.size() && cancelProfile.menus[pendingIndex].id == m.id) {
                    cancelProfile.menus.erase(cancelProfile.menus.begin() + static_cast<std::ptrdiff_t>(pendingIndex));
                }
                selectedMenuIndex_ = -1;
                mappingRuntime_.SetProfile(cancelProfile);
                mappingRuntime_.SetEnabled(!cancelProfile.Empty());
                mappingOverlayDirty_ = true;
                dirtyWindow_ = true;
                mappingCreateMode_ = MappingCreateMode::None;
                mappingEditorStatus_ = HLW(L"创建菜单已取消");
                updateStatusText(mappingEditorStatus_);
                refreshMappingToolbarStatus();
                return true;
            }

            PcMappingProfile finalProfile = mappingRuntime_.GetProfile();
            if (pendingIndex < finalProfile.menus.size()) {
                finalProfile.menus[pendingIndex].comboTriggerCodes = combo.vkCodes;
                finalProfile.menus[pendingIndex].triggerLabel = combo.label;
            }
            menuRuntime_.Reset();
            mappingRuntime_.SetProfile(finalProfile);
            mappingRuntime_.SetEnabled(true);
            mappingOverlayDirty_ = true;
            dirtyWindow_ = true;
            mappingCreateMode_ = MappingCreateMode::None;
            mappingEditorStatus_ = std::wstring(PcMenuCategoryLabel(cat)) + HLW(L"已创建：") + combo.label;
            updateStatusText(mappingEditorStatus_);
            refreshMappingToolbarStatus();
            return true;
        }

        if (mappingCreateMode_ == MappingCreateMode::Compass) {
            PcMappingProfile profile = mappingRuntime_.GetProfile();
            PcCompassBinding c;
            c.id = "compass_" + std::to_string(static_cast<unsigned long long>(profile.compasses.size() + 1));
            c.centerXNorm = nx;
            c.centerYNorm = ny;
 
            c.up.comboTriggerCodes = { 'W' };
            c.left.comboTriggerCodes = { 'A' };
            c.down.comboTriggerCodes = { 'S' };
            c.right.comboTriggerCodes = { 'D' };
            c.center.comboTriggerCodes = { VK_LSHIFT };
            c.up.triggerLabel = HLW(L"W");
            c.left.triggerLabel = HLW(L"A");
            c.down.triggerLabel = HLW(L"S");
            c.right.triggerLabel = HLW(L"D");
            c.center.triggerLabel = HLW(L"Shift");
            clampCompassUiPositionToWindow(c);
            resetCompassDiagonalAnchors(c);
            profile.compasses.clear(); // 第一版只保留一个轮盘，避免多轮盘抢 slot=1。
            profile.compasses.push_back(c);
            mappingRuntime_.SetProfile(profile);
            mappingRuntime_.SetEnabled(true);
            selectedMappingBindingIndex_ = -1;
            selectedMenuIndex_ = -1;
            compassSelected_ = true;
            mappingOverlayDirty_ = true;
            dirtyWindow_ = true;
            mappingCreateMode_ = MappingCreateMode::None;
            mappingEditorStatus_ = HLW(L"已创建轮盘：W/A/S/D + Shift");
            updateStatusText(mappingEditorStatus_);
            refreshMappingToolbarStatus();
            return true;
        }

        if (mappingCreateMode_ == MappingCreateMode::Lock) {
            mappingEditorStatus_ = HLW(L"创建视角：请绑定锁定热键");
            updateStatusText(mappingEditorStatus_);

            PcCapturedKeyCombo combo;
            std::vector<int> lockCodes;
            std::wstring lockError;
            if (!CaptureKeyComboDialog(inst_, hwnd_, combo) || !makeLockTriggerCodesFromCapture(combo, lockCodes, lockError)) {
                mappingCreateMode_ = MappingCreateMode::None;
                mappingEditorStatus_ = lockError.empty() ? HLW(L"创建视角已取消") : (HLW(L"创建视角已取消：") + lockError);
                updateStatusText(mappingEditorStatus_);
                return true;
            }

            PcMappingProfile profile = mappingRuntime_.GetProfile();
            PcLockBinding l;
            l.id = "lock_" + std::to_string(static_cast<unsigned long long>(profile.locks.size() + 1));
            l.comboTriggerCodes = lockCodes;
            l.triggerLabel = combo.label;
            l.centerXNorm = nx;
            l.centerYNorm = ny;
            const int halfW = 190000;
            const int halfH = 300000;
            // 视角触发点就是范围中心：创建时直接生成以点击点为中心的对称矩形。
            setLockRectSymmetric(l, halfW, halfH);
            // 默认使用 3 双slot顺序；后续直接在 LOCK overlay 的 1/2/3 chip 上切换。
            l.mode = PcLockSlideTouchMode::DualSequential;
            l.primarySlot = 2;
            l.auxSlot = 9;
            l.speedXNorm = 0.5;
            l.speedYNorm = 0.5;
            profile.locks.clear(); // 第一版只保留一个视角锁，避免多个锁同时抢 RawInput。
            profile.locks.push_back(l);

            mappingRuntime_.SetProfile(profile);
            mappingRuntime_.SetEnabled(true);
            selectedMappingBindingIndex_ = -1;
            selectedMenuIndex_ = -1;
            compassSelected_ = false;
            mappingOverlayDirty_ = true;
            dirtyWindow_ = true;

            mappingCreateMode_ = MappingCreateMode::None;
            mappingEditorStatus_ = HLW(L"已创建视角：") + combo.label;
            updateStatusText(mappingEditorStatus_);
            refreshMappingToolbarStatus();
            return true;
        }

        if (mappingCreateMode_ != MappingCreateMode::Key) {
            mappingCreateMode_ = MappingCreateMode::None;
            return true;
        }

        PcMappingProfile profile = mappingRuntime_.GetProfile();
        PcMappingBinding b;
        b.id = "key_" + std::to_string(static_cast<unsigned long long>(profile.bindings.size() + 1));
        b.triggerType = PcMappingTriggerType::KeyboardVk;
        b.triggerCode = 0;
        b.triggerLabel = HLW(L"未绑定");
        b.actionType = PcMappingActionType::None;
        b.slot = allocateMappingSlot();
        b.xNorm = nx;
        b.yNorm = ny;
        b.radiusNorm = PC_BUTTON_RADIUS_DEFAULT_NORM;
        b.consumeEvent = true;
        clampBindingToBounds(b);
        profile.bindings.push_back(b);
        const size_t pendingIndex = profile.bindings.size() - 1;
        selectedMappingBindingIndex_ = static_cast<int>(pendingIndex);
        selectedMenuIndex_ = -1;
        compassSelected_ = false;
        mappingRuntime_.SetProfile(profile);
        mappingRuntime_.SetEnabled(true);
        mappingOverlayDirty_ = true;
        dirtyWindow_ = true;
        mappingEditorStatus_ = HLW(L"创建按键：已放置未绑定按钮，请按要绑定的组合键");
        updateStatusText(mappingEditorStatus_);
        refreshMappingToolbarStatus();
        render(false);

        PcCapturedKeyCombo combo;
        if (!CaptureKeyComboDialog(inst_, hwnd_, combo) || (combo.vkCodes.empty() && combo.mouseButtonCode <= 0)) {
            PcMappingProfile cancelProfile = mappingRuntime_.GetProfile();
            if (pendingIndex < cancelProfile.bindings.size() && cancelProfile.bindings[pendingIndex].id == b.id) {
                cancelProfile.bindings.erase(cancelProfile.bindings.begin() + static_cast<std::ptrdiff_t>(pendingIndex));
            }
            selectedMappingBindingIndex_ = -1;
            mappingRuntime_.SetProfile(cancelProfile);
            mappingRuntime_.SetEnabled(!cancelProfile.Empty());
            mappingOverlayDirty_ = true;
            dirtyWindow_ = true;
            mappingCreateMode_ = MappingCreateMode::None;
            mappingEditorStatus_ = HLW(L"创建按键已取消");
            updateStatusText(mappingEditorStatus_);
            refreshMappingToolbarStatus();
            return true;
        }

        PcMappingProfile finalProfile = mappingRuntime_.GetProfile();
        if (pendingIndex >= finalProfile.bindings.size()) return true;
        auto& finalBinding = finalProfile.bindings[pendingIndex];
        if (combo.mouseButtonCode > 0) {
            finalBinding.triggerType = PcMappingTriggerType::MouseButton;
            finalBinding.triggerCode = combo.mouseButtonCode;
            finalBinding.comboTriggerCodes = combo.vkCodes;
        }
        else {
            finalBinding.triggerType = PcMappingTriggerType::KeyboardVk;
            finalBinding.triggerCode = combo.vkCodes.empty() ? 0 : combo.vkCodes.front();
            finalBinding.comboTriggerCodes = combo.vkCodes;
        }
        finalBinding.triggerLabel = combo.label;
        finalBinding.actionType = PcMappingActionType::TouchHold;
        finalBinding.consumeEvent = true;
        clampBindingToBounds(finalBinding);
        selectedMappingBindingIndex_ = static_cast<int>(pendingIndex);
        selectedMenuIndex_ = -1;
        compassSelected_ = false;
        mappingRuntime_.SetProfile(finalProfile);
        mappingRuntime_.SetEnabled(true);
        mappingOverlayDirty_ = true;
        dirtyWindow_ = true;

        mappingCreateMode_ = MappingCreateMode::None;
        mappingEditorStatus_ = HLW(L"已创建按键：") + combo.label + HLW(L" -> 触点") + std::to_wstring(finalBinding.slot);
        updateStatusText(mappingEditorStatus_);
        refreshMappingToolbarStatus();
        return true;
    }
