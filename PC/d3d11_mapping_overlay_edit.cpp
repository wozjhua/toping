#include "d3d11_renderer.h"

// handleMappingOverlayEditMessage
bool D3D11Renderer::handleMappingOverlayEditMessage(UINT msg, WPARAM, LPARAM lp) {
        if (mappingOverlayHidden_) return false;
        if (!mappingEditMode_ || mappingCreateMode_ != MappingCreateMode::None || lockRuntime_.IsLocked()) return false;
        if (!mappingRuntime_.IsEnabled()) return false;

        switch (msg) {
        case WM_LBUTTONDOWN: {
            RECT rc{};
            if (!GetClientRect(hwnd_, &rc)) return false;
            const int w = (std::max)(1L, rc.right - rc.left);
            const int h = (std::max)(1L, rc.bottom - rc.top);
            PcMappingOverlayOptions opt;
            opt.editMode = mappingEditMode_;
            opt.selectedBindingIndex = selectedMappingBindingIndex_;
            opt.selectedMenuIndex = selectedMenuIndex_;
            opt.selectedCompass = compassSelected_;
            opt.macros = &pcMacros_;
            opt.selectedMacroIndex = selectedMacroIndex_;
            opt.selectedMacroStepId = selectedMacroStepId_;
            opt.macroStepEditorVisible = macroStepEditorVisible_;
            PcMappingOverlayHit hit = HitTestPcMappingOverlay(w, h, mappingRuntime_.GetProfile(), opt, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            if (hit.kind == PcMappingOverlayHit::Kind::MacroBind) {
                selectedMacroIndex_ = static_cast<int>(hit.macroIndex);
                selectedMappingBindingIndex_ = -1;
                selectedMenuIndex_ = -1;
                compassSelected_ = false;
                syncMacroRuntimeAndOverlay();
                return rebindMacroBinding(hit.macroIndex);
            }
            if (hit.kind == PcMappingOverlayHit::Kind::MacroDelete) {
                if (hit.macroIndex < pcMacros_.size()) {
                    macroRuntime_.Reset();
                    pcMacros_.erase(pcMacros_.begin() + static_cast<std::ptrdiff_t>(hit.macroIndex));
                    selectedMacroIndex_ = -1;
                    selectedMacroStepId_ = 0;
                    macroStepEditorVisible_ = false;
                    syncMacroRuntimeAndOverlay();
                    mappingEditorStatus_ = HLW(L"普通宏已删除");
                    updateStatusText(mappingEditorStatus_);
                    return true;
                }
            }
            if (hit.kind == PcMappingOverlayHit::Kind::MacroTriggerCondition) {
                if (hit.macroIndex < pcMacros_.size()) {
                    auto& macro = pcMacros_[hit.macroIndex];
                    macro.triggerCondition = macro.triggerCondition == PcMacroRuntime::TriggerCondition::Press
                        ? PcMacroRuntime::TriggerCondition::Hold
                        : PcMacroRuntime::TriggerCondition::Press;
                    selectedMacroIndex_ = static_cast<int>(hit.macroIndex);
                    selectedMappingBindingIndex_ = -1;
                    selectedMenuIndex_ = -1;
                    compassSelected_ = false;
                    syncMacroRuntimeAndOverlay();
                    mappingEditorStatus_ = std::wstring(HLW(L"普通宏触发方式：")) + (macro.triggerCondition == PcMacroRuntime::TriggerCondition::Hold ? HLW(L"按住") : HLW(L"按下"));
                    updateStatusText(mappingEditorStatus_);
                    return true;
                }
            }
            if (hit.kind == PcMappingOverlayHit::Kind::MacroAddStep) {
                return addMacroStep(hit.macroIndex, selectedMacroStepId_, true);
            }
            if (hit.kind == PcMappingOverlayHit::Kind::MacroStepEditorAddAfter) {
                return addMacroStep(hit.macroIndex, hit.macroStepId, true);
            }
            if (hit.kind == PcMappingOverlayHit::Kind::MacroStepEditorClose) {
                macroStepEditorVisible_ = false;
                syncMacroRuntimeAndOverlay();
                return true;
            }
            if (hit.kind == PcMappingOverlayHit::Kind::MacroStepEditorDelete) {
                if (hit.macroIndex < pcMacros_.size()) {
                    auto& macro = pcMacros_[hit.macroIndex];
                    if (macro.steps.size() > 1) {
                        const int deleteId = hit.macroStepId;
                        for (auto& step : macro.steps) {
                            if (step.nextStepId == deleteId) step.nextStepId = 0;
                        }
                        if (macro.startStepId == deleteId) macro.startStepId = 0;
                        macro.steps.erase(std::remove_if(macro.steps.begin(), macro.steps.end(), [deleteId](const auto& step) { return step.id == deleteId; }), macro.steps.end());
                        normalizeMacroSteps(macro);
                        selectedMacroIndex_ = static_cast<int>(hit.macroIndex);
                        selectedMacroStepId_ = macro.steps.empty() ? 0 : macro.steps.front().id;
                        macroStepEditorVisible_ = false;
                        syncMacroRuntimeAndOverlay();
                        mappingEditorStatus_ = HLW(L"普通宏：步骤已删除");
                        updateStatusText(mappingEditorStatus_);
                    }
                    return true;
                }
            }
            auto adjustMacroStep = [&](size_t macroIndex, int stepId, auto&& fn) -> bool {
                if (macroIndex >= pcMacros_.size()) return false;
                auto& macro = pcMacros_[macroIndex];
                for (auto& step : macro.steps) {
                    if (step.id != stepId) continue;
                    fn(step);
                    selectedMacroIndex_ = static_cast<int>(macroIndex);
                    selectedMacroStepId_ = stepId;
                    selectedMappingBindingIndex_ = -1;
                    selectedMenuIndex_ = -1;
                    compassSelected_ = false;
                    syncMacroRuntimeAndOverlay();
                    return true;
                }
                return false;
                };
            if (hit.kind == PcMappingOverlayHit::Kind::MacroStepEditorAction) {
                return adjustMacroStep(hit.macroIndex, hit.macroStepId, [](auto& step) {
                    if (step.actionType == PcMacroRuntime::StepActionType::Tap) {
                        step.actionType = PcMacroRuntime::StepActionType::Swipe;
                        step.durationMs = 40;
                        if (step.endXNorm == step.xNorm && step.endYNorm == step.yNorm) {
                            step.endXNorm = PcMappingProfile::ClampNorm(step.xNorm + 84000);
                        }
                    }
                    else if (step.actionType == PcMacroRuntime::StepActionType::Swipe) {
                        step.actionType = PcMacroRuntime::StepActionType::Wait;
                    }
                    else {
                        step.actionType = PcMacroRuntime::StepActionType::Tap;
                        step.durationMs = (std::max)(1, (std::min)(5000, step.durationMs));
                    }
                    });
            }
            if (hit.kind == PcMappingOverlayHit::Kind::MacroStepEditorDurationMinus) return adjustMacroStep(hit.macroIndex, hit.macroStepId, [](auto& step) {
                if (step.actionType == PcMacroRuntime::StepActionType::Swipe) {
                    const int cur = (std::max)(20, (std::min)(200, step.durationMs));
                    step.durationMs = (std::max)(20, cur - 10);
                }
                else {
                    step.durationMs = (std::max)(1, step.durationMs - 2);
                }
                });
            if (hit.kind == PcMappingOverlayHit::Kind::MacroStepEditorDurationPlus) return adjustMacroStep(hit.macroIndex, hit.macroStepId, [](auto& step) {
                if (step.actionType == PcMacroRuntime::StepActionType::Swipe) {
                    const int cur = (std::max)(20, (std::min)(200, step.durationMs));
                    step.durationMs = (std::min)(200, cur + 10);
                }
                else {
                    step.durationMs = (std::min)(5000, step.durationMs + 2);
                }
                });
            if (hit.kind == PcMappingOverlayHit::Kind::MacroStepEditorDelayMinus) return adjustMacroStep(hit.macroIndex, hit.macroStepId, [](auto& step) { step.delayAfterMs = (std::max)(0, step.delayAfterMs - 2); });
            if (hit.kind == PcMappingOverlayHit::Kind::MacroStepEditorDelayPlus) return adjustMacroStep(hit.macroIndex, hit.macroStepId, [](auto& step) { step.delayAfterMs = (std::min)(5000, step.delayAfterMs + 2); });
            if (hit.kind == PcMappingOverlayHit::Kind::MacroStepEditorRepeatMinus) return adjustMacroStep(hit.macroIndex, hit.macroStepId, [](auto& step) { step.repeatCount = (std::max)(1, step.repeatCount - 1); });
            if (hit.kind == PcMappingOverlayHit::Kind::MacroStepEditorRepeatPlus) return adjustMacroStep(hit.macroIndex, hit.macroStepId, [](auto& step) { step.repeatCount = (std::min)(32, step.repeatCount + 1); });
            if (hit.kind == PcMappingOverlayHit::Kind::MacroStepEditorRandomMinus) return adjustMacroStep(hit.macroIndex, hit.macroStepId, [](auto& step) { step.randomRadiusNorm = (std::max)(0, step.randomRadiusNorm - 1000); });
            if (hit.kind == PcMappingOverlayHit::Kind::MacroStepEditorRandomPlus) return adjustMacroStep(hit.macroIndex, hit.macroStepId, [](auto& step) { step.randomRadiusNorm = (std::min)(120000, step.randomRadiusNorm + 1000); });
            if (hit.kind == PcMappingOverlayHit::Kind::MacroStepSwipeEndHandle) {
                if (hit.macroIndex < pcMacros_.size()) {
                    selectedMacroIndex_ = static_cast<int>(hit.macroIndex);
                    selectedMacroStepId_ = hit.macroStepId;
                    selectedMappingBindingIndex_ = -1;
                    selectedMenuIndex_ = -1;
                    compassSelected_ = false;
                    macroSwipeEndDrag_ = true;
                    SetCapture(hwnd_);
                    syncMacroRuntimeAndOverlay();
                    mappingEditorStatus_ = HLW(L"普通宏：拖动滑动尾点设置终点");
                    updateStatusText(mappingEditorStatus_);
                    return true;
                }
            }
            if (hit.kind == PcMappingOverlayHit::Kind::MacroStepRangeHandle) {
                if (hit.macroIndex < pcMacros_.size()) {
                    selectedMacroIndex_ = static_cast<int>(hit.macroIndex);
                    selectedMacroStepId_ = hit.macroStepId;
                    selectedMappingBindingIndex_ = -1;
                    selectedMenuIndex_ = -1;
                    compassSelected_ = false;
                    macroStepRangeDrag_ = true;
                    SetCapture(hwnd_);
                    syncMacroRuntimeAndOverlay();
                    mappingEditorStatus_ = HLW(L"普通宏：拖动调整当前步骤随机范围");
                    updateStatusText(mappingEditorStatus_);
                    return true;
                }
            }
            if (hit.kind == PcMappingOverlayHit::Kind::MacroStepConfig) {
                selectedMacroIndex_ = static_cast<int>(hit.macroIndex);
                selectedMacroStepId_ = hit.macroStepId;
                macroStepEditorVisible_ = true;
                selectedMappingBindingIndex_ = -1;
                selectedMenuIndex_ = -1;
                compassSelected_ = false;
                syncMacroRuntimeAndOverlay();
                mappingEditorStatus_ = HLW(L"普通宏：步骤编辑面板已打开");
                updateStatusText(mappingEditorStatus_);
                return true;
            }
            if (hit.kind == PcMappingOverlayHit::Kind::MacroStepBody) {
                if (hit.macroIndex < pcMacros_.size()) {
                    selectedMacroIndex_ = static_cast<int>(hit.macroIndex);
                    selectedMacroStepId_ = hit.macroStepId;
                    selectedMappingBindingIndex_ = -1;
                    selectedMenuIndex_ = -1;
                    compassSelected_ = false;
                    int nx = 0, ny = 0;
                    clientPointToNormLocal(hwnd_, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), nx, ny);
                    macroDragStartXNorm_ = nx;
                    macroDragStartYNorm_ = ny;
                    for (const auto& step : pcMacros_[hit.macroIndex].steps) if (step.id == hit.macroStepId) { macroStepDragStart_ = step; break; }
                    macroStepDrag_ = true;
                    SetCapture(hwnd_);
                    syncMacroRuntimeAndOverlay();
                    mappingEditorStatus_ = HLW(L"普通宏：拖动步骤位置，点“编”修改参数");
                    updateStatusText(mappingEditorStatus_);
                    return true;
                }
            }
            if (hit.kind == PcMappingOverlayHit::Kind::MacroBody) {
                if (hit.macroIndex < pcMacros_.size()) {
                    selectedMacroIndex_ = static_cast<int>(hit.macroIndex);
                    selectedMacroStepId_ = pcMacros_[hit.macroIndex].startStepId;
                    selectedMappingBindingIndex_ = -1;
                    selectedMenuIndex_ = -1;
                    compassSelected_ = false;
                    int nx = 0, ny = 0;
                    clientPointToNormLocal(hwnd_, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), nx, ny);
                    macroDragStartXNorm_ = nx;
                    macroDragStartYNorm_ = ny;
                    macroDragStart_ = pcMacros_[hit.macroIndex];
                    macroDrag_ = true;
                    SetCapture(hwnd_);
                    syncMacroRuntimeAndOverlay();
                    mappingEditorStatus_ = HLW(L"普通宏：已选中，可拖动；点 绑/删/按下/+/步骤编 修改");
                    updateStatusText(mappingEditorStatus_);
                    return true;
                }
            }
            if (hit.kind == PcMappingOverlayHit::Kind::MenuOptions) {
                selectedMenuIndex_ = static_cast<int>(hit.menuIndex);
                selectedMappingBindingIndex_ = -1;
                compassSelected_ = false;
                return editMenuOptions(hit.menuIndex);
            }
            if (hit.kind == PcMappingOverlayHit::Kind::MenuRebind) {
                selectedMenuIndex_ = static_cast<int>(hit.menuIndex);
                selectedMappingBindingIndex_ = -1;
                compassSelected_ = false;
                return rebindMenuBinding(hit.menuIndex);
            }
            if (hit.kind == PcMappingOverlayHit::Kind::MenuDelete) {
                return deleteMenuBinding(hit.menuIndex);
            }
            if (hit.kind == PcMappingOverlayHit::Kind::MenuBody ||
                hit.kind == PcMappingOverlayHit::Kind::MenuTriggerAnchor ||
                hit.kind == PcMappingOverlayHit::Kind::MenuLandingAnchor ||
                hit.kind == PcMappingOverlayHit::Kind::MenuCloseAnchor) {
                selectedMenuIndex_ = static_cast<int>(hit.menuIndex);
                PcMappingProfile profile = mappingRuntime_.GetProfile();
                if (hit.menuIndex < profile.menus.size()) {
                    int nx = 0, ny = 0;
                    clientPointToNormLocal(hwnd_, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), nx, ny);
                    menuDragStartXNorm_ = nx;
                    menuDragStartYNorm_ = ny;
                    menuDragStart_ = profile.menus[hit.menuIndex];
                    selectedMappingBindingIndex_ = -1;
                    compassSelected_ = false;
                    menuEditDrag_ = (hit.kind == PcMappingOverlayHit::Kind::MenuTriggerAnchor)
                        ? MenuEditDrag::TriggerAnchor
                        : (hit.kind == PcMappingOverlayHit::Kind::MenuLandingAnchor
                            ? MenuEditDrag::LandingAnchor
                            : (hit.kind == PcMappingOverlayHit::Kind::MenuCloseAnchor ? MenuEditDrag::CloseAnchor : MenuEditDrag::Body));
                    menuEditDragMoved_ = false;
                    SetCapture(hwnd_);
                    mappingOverlayDirty_ = true;
                    dirtyWindow_ = true;
                    mappingEditorStatus_ = (menuEditDrag_ == MenuEditDrag::TriggerAnchor)
                        ? HLW(L"菜单：已选中触发键圆点，可拖动；点设置/重绑/删除修改")
                        : (menuEditDrag_ == MenuEditDrag::LandingAnchor
                            ? HLW(L"菜单：拖动鼠标落点圆点")
                            : (menuEditDrag_ == MenuEditDrag::CloseAnchor ? HLW(L"菜单：拖动关闭圆点") : HLW(L"菜单：相对拖动移动，点击设置修改")));
                    updateStatusText(mappingEditorStatus_);
                    refreshMappingToolbarStatus();
                    return true;
                }
            }
            if (hit.kind == PcMappingOverlayHit::Kind::CompassSectorHandle) {
                compassSectorDrag_ = true;
                compassSectorDragGroup_ = hit.compassSectorGroup;
                compassSectorDragIndex_ = hit.compassSectorIndex;
                SetCapture(hwnd_);
                mappingOverlayDirty_ = true;
                dirtyWindow_ = true;
                mappingEditorStatus_ = (compassSectorDragGroup_ == 1) ? HLW(L"轮盘：拖动蓝色手柄调整斜向扇区") : HLW(L"轮盘：拖动紫色手柄调整正向扇区");
                updateStatusText(mappingEditorStatus_);
                refreshMappingToolbarStatus();
                return true;
            }
            if (hit.kind == PcMappingOverlayHit::Kind::CompassDiagAnchor) {
                compassDiagDrag_ = true;
                compassDiagDragSlot_ = hit.compassDiagSlot;
                SetCapture(hwnd_);
                mappingOverlayDirty_ = true;
                dirtyWindow_ = true;
                mappingEditorStatus_ = HLW(L"轮盘：拖动 LU/RU/LD/RD 调整斜向方向");
                updateStatusText(mappingEditorStatus_);
                refreshMappingToolbarStatus();
                return true;
            }
            if (hit.kind == PcMappingOverlayHit::Kind::CompassOptions) {
                return editCompassOptions();
            }
            if (hit.kind == PcMappingOverlayHit::Kind::CompassDelete) {
                return deleteCompassBinding();
            }
            if (hit.kind == PcMappingOverlayHit::Kind::CompassMotionMode) {
                return setCompassMotionMode(hit.compassMotionMode);
            }
            if (hit.kind == PcMappingOverlayHit::Kind::CompassRadiusReverse) {
                return toggleCompassRadiusReverse();
            }
            if (hit.kind == PcMappingOverlayHit::Kind::CompassButtonRebind) {
                compassSelected_ = true;
                selectedMappingBindingIndex_ = -1;
                selectedMenuIndex_ = -1;
                mappingOverlayDirty_ = true;
                dirtyWindow_ = true;
                return rebindCompassButton(hit.compassButton);
            }
            if (hit.kind == PcMappingOverlayHit::Kind::CompassBody) {
                compassSelected_ = true;
                selectedMappingBindingIndex_ = -1;
                selectedMenuIndex_ = -1;
                int nx = 0, ny = 0;
                clientPointToNormLocal(hwnd_, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), nx, ny);
                compassDragStartXNorm_ = nx;
                compassDragStartYNorm_ = ny;
                if (!mappingRuntime_.GetProfile().compasses.empty()) {
                    compassDragStart_ = mappingRuntime_.GetProfile().compasses.front();
                }
                compassDrag_ = true;
                SetCapture(hwnd_);
                mappingOverlayDirty_ = true;
                dirtyWindow_ = true;
                mappingEditorStatus_ = HLW(L"轮盘：已选中，可拖动；点设置/删除，点击 W/A/S/D/Shift 重绑");
                updateStatusText(mappingEditorStatus_);
                refreshMappingToolbarStatus();
                return true;
            }
            if (hit.kind == PcMappingOverlayHit::Kind::LockModeChip) {
                setCurrentLockModeInt(hit.lockMode);
                return true;
            }
            if (hit.kind == PcMappingOverlayHit::Kind::LockOptions) {
                return editLockOptions();
            }
            if (hit.kind == PcMappingOverlayHit::Kind::LockRebind) {
                return rebindLockBinding();
            }
            if (hit.kind == PcMappingOverlayHit::Kind::BindingOptions) {
                selectedMappingBindingIndex_ = static_cast<int>(hit.bindingIndex);
                selectedMenuIndex_ = -1;
                compassSelected_ = false;
                mappingOverlayDirty_ = true;
                dirtyWindow_ = true;
                return editMappingBindingOptions(hit.bindingIndex);
            }
            if (hit.kind == PcMappingOverlayHit::Kind::BindingRebind) {
                selectedMappingBindingIndex_ = static_cast<int>(hit.bindingIndex);
                selectedMenuIndex_ = -1;
                compassSelected_ = false;
                mappingOverlayDirty_ = true;
                dirtyWindow_ = true;
                return rebindMappingBinding(hit.bindingIndex);
            }
            if (hit.kind == PcMappingOverlayHit::Kind::BindingDelete) {
                return deleteMappingBinding(hit.bindingIndex);
            }
            if (hit.kind == PcMappingOverlayHit::Kind::BindingChip) {
                selectedMappingBindingIndex_ = static_cast<int>(hit.bindingIndex);
                selectedMenuIndex_ = -1;
                compassSelected_ = false;
                int nx = 0, ny = 0;
                clientPointToNormLocal(hwnd_, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), nx, ny);
                mappingBindingDragStartXNorm_ = nx;
                mappingBindingDragStartYNorm_ = ny;
                const auto& prof = mappingRuntime_.GetProfile();
                if (hit.bindingIndex < prof.bindings.size()) {
                    mappingBindingDragStart_ = prof.bindings[hit.bindingIndex];
                }
                mappingBindingDrag_ = true;
                mappingBindingDragMoved_ = false;
                SetCapture(hwnd_);
                mappingOverlayDirty_ = true;
                dirtyWindow_ = true;
                mappingEditorStatus_ = HLW(L"已选中按键：相对拖动更新坐标，点“设置/重绑/删除”修改");
                updateStatusText(mappingEditorStatus_);
                refreshMappingToolbarStatus();
                return true;
            }
            if (selectedMappingBindingIndex_ >= 0 || selectedMenuIndex_ >= 0 || compassSelected_ || selectedMacroIndex_ >= 0) {
                selectedMappingBindingIndex_ = -1;
                selectedMenuIndex_ = -1;
                selectedMacroIndex_ = -1;
                selectedMacroStepId_ = 0;
                macroStepEditorVisible_ = false;
                compassSelected_ = false;
                syncMacroRuntimeAndOverlay();
            }
            return false;
        }
        case WM_MOUSEMOVE:
            if (macroDrag_ && selectedMacroIndex_ >= 0 && selectedMacroIndex_ < static_cast<int>(pcMacros_.size())) {
                int nx = 0, ny = 0;
                clientPointToNormLocal(hwnd_, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), nx, ny);
                const int dx = nx - macroDragStartXNorm_;
                const int dy = ny - macroDragStartYNorm_;
                auto macro = macroDragStart_;
                macro.xNorm = PcMappingProfile::ClampNorm(macroDragStart_.xNorm + dx);
                macro.yNorm = PcMappingProfile::ClampNorm(macroDragStart_.yNorm + dy);
                for (auto& step : macro.steps) {
                    step.xNorm = PcMappingProfile::ClampNorm(step.xNorm + dx);
                    step.yNorm = PcMappingProfile::ClampNorm(step.yNorm + dy);
                    step.endXNorm = PcMappingProfile::ClampNorm(step.endXNorm + dx);
                    step.endYNorm = PcMappingProfile::ClampNorm(step.endYNorm + dy);
                }
                pcMacros_[static_cast<size_t>(selectedMacroIndex_)] = macro;
                syncMacroRuntimeAndOverlay();
                return true;
            }
            if (macroSwipeEndDrag_ && selectedMacroIndex_ >= 0 && selectedMacroIndex_ < static_cast<int>(pcMacros_.size())) {
                int nx = 0, ny = 0;
                clientPointToNormLocal(hwnd_, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), nx, ny);
                auto& macro = pcMacros_[static_cast<size_t>(selectedMacroIndex_)];
                for (auto& step : macro.steps) {
                    if (step.id != selectedMacroStepId_) continue;
                    step.endXNorm = PcMappingProfile::ClampNorm(nx);
                    step.endYNorm = PcMappingProfile::ClampNorm(ny);
                    break;
                }
                syncMacroRuntimeAndOverlay();
                return true;
            }
            if (macroStepRangeDrag_ && selectedMacroIndex_ >= 0 && selectedMacroIndex_ < static_cast<int>(pcMacros_.size())) {
                RECT rc{};
                if (!GetClientRect(hwnd_, &rc)) return false;
                const int w = (std::max)(1L, rc.right - rc.left);
                const int h = (std::max)(1L, rc.bottom - rc.top);
                auto& macro = pcMacros_[static_cast<size_t>(selectedMacroIndex_)];
                for (auto& step : macro.steps) {
                    if (step.id != selectedMacroStepId_) continue;
                    const int cx = normToClientCoord(step.xNorm, static_cast<int>(w));
                    const int cy = normToClientCoord(step.yNorm, static_cast<int>(h));
                    const int mx = GET_X_LPARAM(lp);
                    const int my = GET_Y_LPARAM(lp);
                    const double dist = std::hypot(static_cast<double>(mx - cx), static_cast<double>(my - cy));
                    const int base = (std::max)(1, (std::min)(static_cast<int>(w), static_cast<int>(h)));
                    step.randomRadiusNorm = PcMappingProfile::ClampNorm(static_cast<int>(std::lround(dist * 1000000.0 / static_cast<double>(base))));
                    step.randomRadiusNorm = (std::max)(0, (std::min)(120000, step.randomRadiusNorm));
                    break;
                }
                syncMacroRuntimeAndOverlay();
                return true;
            }
            if (macroStepDrag_ && selectedMacroIndex_ >= 0 && selectedMacroIndex_ < static_cast<int>(pcMacros_.size())) {
                int nx = 0, ny = 0;
                clientPointToNormLocal(hwnd_, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), nx, ny);
                const int dx = nx - macroDragStartXNorm_;
                const int dy = ny - macroDragStartYNorm_;
                auto& macro = pcMacros_[static_cast<size_t>(selectedMacroIndex_)];
                for (auto& step : macro.steps) {
                    if (step.id != selectedMacroStepId_) continue;
                    step.xNorm = PcMappingProfile::ClampNorm(macroStepDragStart_.xNorm + dx);
                    step.yNorm = PcMappingProfile::ClampNorm(macroStepDragStart_.yNorm + dy);
                    step.endXNorm = PcMappingProfile::ClampNorm(macroStepDragStart_.endXNorm + dx);
                    step.endYNorm = PcMappingProfile::ClampNorm(macroStepDragStart_.endYNorm + dy);
                    break;
                }
                syncMacroRuntimeAndOverlay();
                return true;
            }
            if (menuEditDrag_ != MenuEditDrag::None) {
                return updateMenuPositionFromPoint(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            }
            if (compassSectorDrag_) {
                updateCompassSectorFromPoint(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
                return true;
            }
            if (compassDiagDrag_) {
                updateCompassDiagonalAnchorFromPoint(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
                return true;
            }
            if (compassDrag_) {
                updateCompassPositionFromPoint(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
                return true;
            }
            if (mappingBindingDrag_) {
                mappingBindingDragMoved_ = true;
                updateSelectedBindingPositionFromPoint(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
                return true;
            }
            return false;
        case WM_LBUTTONUP:
            if (macroDrag_ || macroStepDrag_ || macroStepRangeDrag_ || macroSwipeEndDrag_) {
                if (GetCapture() == hwnd_) ReleaseCapture();
                const bool wasRange = macroStepRangeDrag_;
                const bool wasSwipeEnd = macroSwipeEndDrag_;
                macroDrag_ = false;
                macroStepDrag_ = false;
                macroStepRangeDrag_ = false;
                macroSwipeEndDrag_ = false;
                syncMacroRuntimeAndOverlay();
                mappingEditorStatus_ = wasRange ? HLW(L"普通宏随机范围已更新") : (wasSwipeEnd ? HLW(L"普通宏滑动终点已更新") : HLW(L"普通宏位置已更新"));
                updateStatusText(mappingEditorStatus_);
                return true;
            }
            if (menuEditDrag_ != MenuEditDrag::None) {
                const MenuEditDrag dragKind = menuEditDrag_;
                bool moved = menuEditDragMoved_;
                int nx = 0, ny = 0;
                if (!moved && clientPointToNormLocal(hwnd_, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), nx, ny)) {
                    moved = std::abs(nx - menuDragStartXNorm_) > 4000 || std::abs(ny - menuDragStartYNorm_) > 4000;
                }
                if (moved) {
                    updateMenuPositionFromPoint(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
                }
                menuEditDrag_ = MenuEditDrag::None;
                menuEditDragMoved_ = false;
                if (GetCapture() == hwnd_) ReleaseCapture();
                mappingEditorStatus_ = !moved
                    ? HLW(L"菜单已选中：点设置/重绑/删除修改")
                    : ((dragKind == MenuEditDrag::Body)
                        ? HLW(L"菜单位置已更新")
                        : (dragKind == MenuEditDrag::TriggerAnchor
                            ? HLW(L"菜单触发键圆点已更新")
                            : (dragKind == MenuEditDrag::LandingAnchor ? HLW(L"菜单鼠标落点圆点已更新") : HLW(L"菜单关闭圆点已更新"))));
                updateStatusText(mappingEditorStatus_);
                refreshMappingToolbarStatus();
                return true;
            }
            if (compassSectorDrag_) {
                updateCompassSectorFromPoint(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
                compassSectorDrag_ = false;
                compassSectorDragGroup_ = -1;
                compassSectorDragIndex_ = -1;
                if (GetCapture() == hwnd_) ReleaseCapture();
                mappingEditorStatus_ = HLW(L"轮盘扇区范围已更新");
                updateStatusText(mappingEditorStatus_);
                refreshMappingToolbarStatus();
                return true;
            }
            if (compassDiagDrag_) {
                updateCompassDiagonalAnchorFromPoint(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
                compassDiagDrag_ = false;
                compassDiagDragSlot_ = -1;
                if (GetCapture() == hwnd_) ReleaseCapture();
                mappingEditorStatus_ = HLW(L"轮盘斜向锚点已更新");
                updateStatusText(mappingEditorStatus_);
                refreshMappingToolbarStatus();
                return true;
            }
            if (compassDrag_) {
                updateCompassPositionFromPoint(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
                compassDrag_ = false;
                if (GetCapture() == hwnd_) ReleaseCapture();
                mappingEditorStatus_ = HLW(L"轮盘坐标已更新");
                updateStatusText(mappingEditorStatus_);
                refreshMappingToolbarStatus();
                return true;
            }
            if (mappingBindingDrag_) {
                if (mappingBindingDragMoved_) {
                    updateSelectedBindingPositionFromPoint(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
                    mappingEditorStatus_ = HLW(L"按键坐标已更新");
                    updateStatusText(mappingEditorStatus_);
                }
                mappingBindingDrag_ = false;
                mappingBindingDragMoved_ = false;
                if (GetCapture() == hwnd_) ReleaseCapture();
                refreshMappingToolbarStatus();
                return true;
            }
            return false;
        case WM_CANCELMODE:
        case WM_KILLFOCUS:
            if (macroDrag_ || macroStepDrag_ || macroStepRangeDrag_ || macroSwipeEndDrag_) {
                macroDrag_ = false;
                macroStepDrag_ = false;
                macroStepRangeDrag_ = false;
                macroSwipeEndDrag_ = false;
                if (GetCapture() == hwnd_) ReleaseCapture();
            }
            if (menuEditDrag_ != MenuEditDrag::None) {
                menuEditDrag_ = MenuEditDrag::None;
                menuEditDragMoved_ = false;
                if (GetCapture() == hwnd_) ReleaseCapture();
            }
            if (compassSectorDrag_) {
                compassSectorDrag_ = false;
                compassSectorDragGroup_ = -1;
                compassSectorDragIndex_ = -1;
                if (GetCapture() == hwnd_) ReleaseCapture();
            }
            if (compassDiagDrag_) {
                compassDiagDrag_ = false;
                compassDiagDragSlot_ = -1;
                if (GetCapture() == hwnd_) ReleaseCapture();
            }
            if (compassDrag_) {
                compassDrag_ = false;
                if (GetCapture() == hwnd_) ReleaseCapture();
            }
            if (mappingBindingDrag_) {
                mappingBindingDrag_ = false;
                mappingBindingDragMoved_ = false;
                if (GetCapture() == hwnd_) ReleaseCapture();
            }
            return false;
        default:
            return false;
        }
    }
