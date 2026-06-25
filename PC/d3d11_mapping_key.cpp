#include "d3d11_renderer.h"

// allocateMappingSlot
int D3D11Renderer::allocateMappingSlot() {
        // slot 0 = 投屏直接点击；slot 1 = 轮盘；slot 2/9 = 视角锁定专用，slot 10/11 = 菜单。
        // 普通按键和普通宏共用同一套分配规则：创建时拿一个未被已创建映射使用的 slot。
        const auto& profile = mappingRuntime_.GetProfile();
        auto used = [&](int slot) {
            for (const auto& b : profile.bindings) if (b.slot == slot) return true;
            for (const auto& m : pcMacros_) if (m.slot == slot) return true;
            return false;
            };
        for (int attempt = 0; attempt < 64; ++attempt) {
            int slot = mappingNextSlot_++;
            if (mappingNextSlot_ > 15) mappingNextSlot_ = 1;
            if (slot == 0 || slot == 1 || slot == 2 || slot == 9 || slot == 10 || slot == 11) continue;
            if (used(slot)) continue;
            return slot;
        }
        for (int slot = 3; slot <= 15; ++slot) {
            if (slot == 9 || slot == 10 || slot == 11) continue;
            if (!used(slot)) return slot;
        }
        return 3;
    }

// editMappingBindingOptions
bool D3D11Renderer::editMappingBindingOptions(size_t index) {
        PcMappingProfile profile = mappingRuntime_.GetProfile();
        if (index >= profile.bindings.size()) return false;
        const auto& b = profile.bindings[index];
        PcNormalKeyOptions opt;
        opt.touchMode = b.touchMode;
        opt.specialAction = b.specialAction;
        opt.radiusNorm = b.radiusNorm;
        opt.randomRadiusNorm = b.randomRadiusNorm;
        opt.randomMoveRadiusNorm = b.randomMoveRadiusNorm;
        opt.randomMoveIntervalMs = b.randomMoveIntervalMs;

        mappingEditorStatus_ = HLW(L"普通按键选项：独立窗口已打开");
        updateStatusText(mappingEditorStatus_);
        refreshMappingToolbarStatus();

        if (!OpenNormalKeyOptionsDialog(inst_, hwnd_, opt, static_cast<WPARAM>(index))) {
            mappingEditorStatus_ = HLW(L"普通按键选项打开失败");
            updateStatusText(mappingEditorStatus_);
            refreshMappingToolbarStatus();
            return true;
        }
        return true;
    }

// rebindMappingBinding
bool D3D11Renderer::rebindMappingBinding(size_t index) {
        PcMappingProfile profile = mappingRuntime_.GetProfile();
        if (index >= profile.bindings.size()) return false;
        mappingEditorStatus_ = HLW(L"重绑按键：请按新的键鼠组合");
        updateStatusText(mappingEditorStatus_);

        PcCapturedKeyCombo combo;
        if (!CaptureKeyComboDialog(inst_, hwnd_, combo) || (combo.vkCodes.empty() && combo.mouseButtonCode <= 0)) {
            mappingEditorStatus_ = HLW(L"重绑已取消");
            updateStatusText(mappingEditorStatus_);
            refreshMappingToolbarStatus();
            return true;
        }

        auto& b = profile.bindings[index];
        if (combo.mouseButtonCode > 0) {
            b.triggerType = PcMappingTriggerType::MouseButton;
            b.triggerCode = combo.mouseButtonCode;
            b.comboTriggerCodes = combo.vkCodes;
        }
        else {
            b.triggerType = PcMappingTriggerType::KeyboardVk;
            b.triggerCode = combo.vkCodes.empty() ? 0 : combo.vkCodes.front();
            b.comboTriggerCodes = combo.vkCodes;
        }
        b.triggerLabel = combo.label;
        mappingRuntime_.SetProfile(profile);
        mappingRuntime_.SetEnabled(true);
        selectedMappingBindingIndex_ = static_cast<int>(index);
        mappingOverlayDirty_ = true;
        dirtyWindow_ = true;
        mappingEditorStatus_ = HLW(L"已重绑：") + combo.label;
        updateStatusText(mappingEditorStatus_);
        refreshMappingToolbarStatus();
        return true;
    }

// deleteMappingBinding
bool D3D11Renderer::deleteMappingBinding(size_t index) {
        PcMappingProfile profile = mappingRuntime_.GetProfile();
        if (index >= profile.bindings.size()) return false;
        profile.bindings.erase(profile.bindings.begin() + static_cast<std::ptrdiff_t>(index));
        mappingRuntime_.SetProfile(profile);
        mappingRuntime_.SetEnabled(true);
        selectedMappingBindingIndex_ = -1;
        selectedMenuIndex_ = -1;
        compassSelected_ = false;
        mappingBindingDrag_ = false;
        mappingOverlayDirty_ = true;
        dirtyWindow_ = true;
        mappingEditorStatus_ = HLW(L"普通按键已删除");
        updateStatusText(mappingEditorStatus_);
        refreshMappingToolbarStatus();
        return true;
    }

// clampBindingToBounds
void D3D11Renderer::clampBindingToBounds(PcMappingBinding& b) {
        const int r = PcMappingProfile::ClampButtonRadiusNorm(b.radiusNorm);
        b.xNorm = (std::max)(r, (std::min)(1000000 - r, PcMappingProfile::ClampNorm(b.xNorm)));
        b.yNorm = (std::max)(r, (std::min)(1000000 - r, PcMappingProfile::ClampNorm(b.yNorm)));
    }
