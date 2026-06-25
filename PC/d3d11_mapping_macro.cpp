#include "d3d11_renderer.h"

// nextMacroStepId
int D3D11Renderer::nextMacroStepId(const PcMacroRuntime::Binding& macro) const {
        int nextId = 1;
        for (const auto& step : macro.steps) nextId = (std::max)(nextId, step.id + 1);
        return nextId;
    }

// normalizeMacroSteps
void D3D11Renderer::normalizeMacroSteps(PcMacroRuntime::Binding& macro) {
        std::sort(macro.steps.begin(), macro.steps.end(), [](const auto& a, const auto& b) {
            if (a.order != b.order) return a.order < b.order;
            return a.id < b.id;
            });
        for (size_t i = 0; i < macro.steps.size(); ++i) macro.steps[i].order = static_cast<int>(i + 1);
        if (macro.startStepId <= 0 && !macro.steps.empty()) macro.startStepId = macro.steps.front().id;
    }

// syncMacroRuntimeAndOverlay
void D3D11Renderer::syncMacroRuntimeAndOverlay() {
        macroRuntime_.SetBindings(pcMacros_);
        SetPcMappingOverlayMacroSource(&pcMacros_, selectedMacroIndex_, selectedMacroStepId_, macroStepEditorVisible_);
        mappingRuntime_.SetEnabled(true);
        mappingOverlayDirty_ = true;
        dirtyWindow_ = true;
        refreshMappingToolbarStatus();
    }

// makeDefaultMacroStep
PcMacroRuntime::Step D3D11Renderer::makeDefaultMacroStep(const PcMacroRuntime::Binding& macro, int xNorm, int yNorm) const {
        PcMacroRuntime::Step step;
        step.id = nextMacroStepId(macro);
        step.order = static_cast<int>(macro.steps.size() + 1);
        step.actionType = PcMacroRuntime::StepActionType::Tap;
        step.xNorm = PcMappingProfile::ClampNorm(xNorm);
        step.yNorm = PcMappingProfile::ClampNorm(yNorm);
        step.endXNorm = PcMappingProfile::ClampNorm(xNorm + 84000);
        step.endYNorm = PcMappingProfile::ClampNorm(yNorm);
        step.durationMs = 5;
        step.delayAfterMs = 5;
        step.repeatCount = 1;
        step.randomRadiusNorm = 0;
        step.nextStepId = 0;
        return step;
    }

// addMacroStep
bool D3D11Renderer::addMacroStep(size_t macroIndex, int afterStepId, bool selectNew) {
        if (macroIndex >= pcMacros_.size()) return false;
        auto& macro = pcMacros_[macroIndex];
        int baseX = macro.xNorm + 84000;
        int baseY = macro.yNorm;
        PcMacroRuntime::Step* base = nullptr;
        if (afterStepId > 0) {
            for (auto& step : macro.steps) if (step.id == afterStepId) { base = &step; break; }
        }
        else if (!macro.steps.empty()) {
            base = &macro.steps.back();
        }
        int oldNext = 0;
        if (base) {
            baseX = base->xNorm + 84000;
            baseY = base->yNorm;
            oldNext = base->nextStepId;
        }
        PcMacroRuntime::Step step = makeDefaultMacroStep(macro, baseX, baseY);
        step.nextStepId = oldNext;
        if (base) base->nextStepId = step.id;
        if (macro.startStepId <= 0) macro.startStepId = step.id;
        macro.steps.push_back(step);
        normalizeMacroSteps(macro);
        selectedMacroIndex_ = static_cast<int>(macroIndex);
        selectedMacroStepId_ = selectNew ? step.id : selectedMacroStepId_;
        selectedMappingBindingIndex_ = -1;
        selectedMenuIndex_ = -1;
        compassSelected_ = false;
        macroStepEditorVisible_ = false;
        syncMacroRuntimeAndOverlay();
        mappingEditorStatus_ = HLW(L"普通宏：已新增一步");
        updateStatusText(mappingEditorStatus_);
        return true;
    }

// rebindMacroBinding
bool D3D11Renderer::rebindMacroBinding(size_t macroIndex) {
        if (macroIndex >= pcMacros_.size()) return false;
        mappingEditorStatus_ = HLW(L"普通宏：请按触发键");
        updateStatusText(mappingEditorStatus_);
        PcCapturedKeyCombo combo;
        if (!CaptureKeyComboDialog(inst_, hwnd_, combo) || (combo.vkCodes.empty() && combo.mouseButtonCode <= 0)) {
            mappingEditorStatus_ = HLW(L"普通宏重绑已取消");
            updateStatusText(mappingEditorStatus_);
            refreshMappingToolbarStatus();
            return true;
        }
        auto& macro = pcMacros_[macroIndex];
        macro.comboTriggerCodes = combo.vkCodes;
        macro.mouseButtonCode = combo.mouseButtonCode;
        macro.triggerLabel = combo.label;
        selectedMacroIndex_ = static_cast<int>(macroIndex);
        selectedMacroStepId_ = macro.startStepId;
        selectedMappingBindingIndex_ = -1;
        selectedMenuIndex_ = -1;
        compassSelected_ = false;
        syncMacroRuntimeAndOverlay();
        mappingEditorStatus_ = HLW(L"普通宏已绑定：") + combo.label;
        updateStatusText(mappingEditorStatus_);
        return true;
    }
