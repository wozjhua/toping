#include "pc_macro_runtime.h"

#include <algorithm>
#include <cmath>
#include <unordered_set>
#include <utility>
#include <random>
#include <windowsx.h>

void PcMacroRuntime::SetCallbacks(Callbacks cb) {
    callbacks_ = std::move(cb);
}

void PcMacroRuntime::SetBindings(std::vector<Binding> bindings) {
    bindings_ = std::move(bindings);
    comboWasDown_.assign(bindings_.size(), false);
    HandleTriggerTransitions();
}

void PcMacroRuntime::Reset() {
    StopAllJobs();
    vkDown_.fill(false);
    mouseDown_.fill(false);
    comboWasDown_.assign(bindings_.size(), false);
}

std::wstring PcMacroRuntime::StatusText() const {
    if (bindings_.empty()) return L"普通宏=未创建";
    int enabled = 0;
    for (const auto& b : bindings_) if (b.enabled) ++enabled;
    std::wstring s = L"普通宏=" + std::to_wstring(enabled) + L"/" + std::to_wstring(bindings_.size());
    if (!jobs_.empty()) s += L" 运行中=" + std::to_wstring(jobs_.size());
    return s;
}

UINT PcMacroRuntime::NormalizeVk(UINT vk, LPARAM lp) {
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

int PcMacroRuntime::ClampNorm(int v) {
    return (std::max)(0, (std::min)(1000000, v));
}

bool PcMacroRuntime::IsDownMessage(UINT msg) {
    return msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN ||
           msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN || msg == WM_XBUTTONDOWN;
}

bool PcMacroRuntime::IsUpMessage(UINT msg) {
    return msg == WM_KEYUP || msg == WM_SYSKEYUP ||
           msg == WM_LBUTTONUP || msg == WM_RBUTTONUP || msg == WM_MBUTTONUP || msg == WM_XBUTTONUP ||
           msg == WM_KILLFOCUS || msg == WM_CANCELMODE;
}

int PcMacroRuntime::MouseButtonCodeFromMessage(UINT msg, WPARAM wp) {
    switch (msg) {
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
            return 1;
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
            return 2;
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:
            return 3;
        case WM_XBUTTONDOWN:
        case WM_XBUTTONUP: {
            const WORD xb = GET_XBUTTON_WPARAM(wp);
            return xb == XBUTTON2 ? 5 : 4;
        }
        default:
            return 0;
    }
}

bool PcMacroRuntime::ComboSatisfied(const Binding& b, const std::array<bool, 256>& vkDown, const std::array<bool, 8>& mouseDown) {
    if (!b.enabled) return false;
    if (b.comboTriggerCodes.empty() && b.mouseButtonCode <= 0) return false;

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

    if (b.mouseButtonCode > 0) {
        if (b.mouseButtonCode >= static_cast<int>(mouseDown.size())) return false;
        if (!mouseDown[static_cast<size_t>(b.mouseButtonCode)]) return false;
    }
    return true;
}

std::vector<PcMacroRuntime::Step> PcMacroRuntime::BuildStepSequence(const Binding& b) {
    std::vector<Step> sorted = b.steps;
    std::sort(sorted.begin(), sorted.end(), [](const Step& a, const Step& b) {
        if (a.order != b.order) return a.order < b.order;
        return a.id < b.id;
    });
    if (sorted.empty()) return sorted;

    if (b.startStepId <= 0) return sorted;

    std::vector<Step> out;
    std::unordered_set<int> seen;
    int cur = b.startStepId;
    while (cur > 0 && seen.insert(cur).second) {
        auto it = std::find_if(sorted.begin(), sorted.end(), [cur](const Step& s) { return s.id == cur; });
        if (it == sorted.end()) break;
        out.push_back(*it);
        if (it->nextStepId > 0) {
            cur = it->nextStepId;
        } else {
            auto next = it;
            ++next;
            if (next == sorted.end()) break;
            cur = next->id;
        }
    }
    return out.empty() ? sorted : out;
}

void PcMacroRuntime::BuildRandomizedStepCoords(const Step& step, int& xNorm, int& yNorm, int& endXNorm, int& endYNorm) {
    xNorm = ClampNorm(step.xNorm);
    yNorm = ClampNorm(step.yNorm);
    endXNorm = ClampNorm(step.endXNorm);
    endYNorm = ClampNorm(step.endYNorm);

    const int radius = (std::max)(0, (std::min)(120000, step.randomRadiusNorm));
    if (radius <= 0) return;

    static thread_local std::mt19937 rng([] {
        LARGE_INTEGER qpc{};
        QueryPerformanceCounter(&qpc);
        const auto seed = static_cast<unsigned int>(GetTickCount64() ^ static_cast<ULONGLONG>(qpc.QuadPart));
        return seed;
    }());

    std::uniform_real_distribution<double> angleDist(0.0, 6.28318530717958647692);
    std::uniform_real_distribution<double> unitDist(0.0, 1.0);
    const double angle = angleDist(rng);
    const double dist = std::sqrt(unitDist(rng)) * static_cast<double>(radius);
    const int dx = static_cast<int>(std::lround(std::cos(angle) * dist));
    const int dy = static_cast<int>(std::lround(std::sin(angle) * dist));

    // Tap：随机按下点。Swipe：起点和终点使用同一随机偏移，保持滑动方向和长度。
    xNorm = ClampNorm(step.xNorm + dx);
    yNorm = ClampNorm(step.yNorm + dy);
    endXNorm = ClampNorm(step.endXNorm + dx);
    endYNorm = ClampNorm(step.endYNorm + dy);
}

void PcMacroRuntime::HandleTriggerTransitions() {
    if (comboWasDown_.size() != bindings_.size()) comboWasDown_.assign(bindings_.size(), false);

    for (size_t i = 0; i < bindings_.size(); ++i) {
        const auto& b = bindings_[i];
        const bool down = ComboSatisfied(b, vkDown_, mouseDown_);
        const bool was = comboWasDown_[i];
        if (down && !was) {
            if (b.triggerCondition == TriggerCondition::Hold) {
                if (!IsHoldJobRunning(b.id)) StartBinding(b, true);
            } else {
                StartBinding(b, false);
            }
        } else if (!down && was) {
            if (b.triggerCondition == TriggerCondition::Hold) StopHoldBinding(b.id);
        }
        comboWasDown_[i] = down;
    }
}

bool PcMacroRuntime::HandleWindowMessage(HWND, UINT msg, WPARAM wp, LPARAM lp, LRESULT* outResult) {
    if (outResult) *outResult = 0;
    const bool downMsg = IsDownMessage(msg);
    const bool upMsg = IsUpMessage(msg);
    if (!downMsg && !upMsg) return false;

    if (msg == WM_KILLFOCUS || msg == WM_CANCELMODE) {
        Reset();
        return false;
    }

    const int mouseButton = MouseButtonCodeFromMessage(msg, wp);
    if (mouseButton > 0) {
        mouseDown_[static_cast<size_t>(mouseButton)] = downMsg;
    } else if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN || msg == WM_KEYUP || msg == WM_SYSKEYUP) {
        const UINT vk = NormalizeVk(static_cast<UINT>(wp), lp);
        if (vk < vkDown_.size()) {
            vkDown_[vk] = downMsg;
            if (vk == VK_LSHIFT || vk == VK_RSHIFT) vkDown_[VK_SHIFT] = vkDown_[VK_LSHIFT] || vkDown_[VK_RSHIFT];
            if (vk == VK_LCONTROL || vk == VK_RCONTROL) vkDown_[VK_CONTROL] = vkDown_[VK_LCONTROL] || vkDown_[VK_RCONTROL];
            if (vk == VK_LMENU || vk == VK_RMENU) vkDown_[VK_MENU] = vkDown_[VK_LMENU] || vkDown_[VK_RMENU];
        }
    }

    const auto beforeJobs = jobs_.size();
    const auto beforeCombos = comboWasDown_;
    HandleTriggerTransitions();

    bool related = jobs_.size() != beforeJobs;
    if (!related && beforeCombos.size() == comboWasDown_.size()) {
        for (size_t i = 0; i < comboWasDown_.size(); ++i) {
            if (comboWasDown_[i] != beforeCombos[i]) { related = true; break; }
        }
    }

    if (!related) return false;
    // 只要命中宏触发，默认吞掉触发键，避免同时变成普通键/普通点击。
    return true;
}

void PcMacroRuntime::StartBinding(const Binding& b, bool holdLoop) {
    if (!b.enabled) return;
    if (b.steps.empty()) return;
    if (holdLoop && IsHoldJobRunning(b.id)) return;
    if (!holdLoop && IsJobRunning(b.id)) return;

    RunningJob job;
    job.bindingId = b.id;
    job.holdLoop = holdLoop;
    job.activeSlot = b.slot;
    job.steps = BuildStepSequence(b);
    if (job.steps.empty()) return;
    job.stepIndex = 0;
    job.phase = Phase::StartStep;
    jobs_.push_back(std::move(job));
}

void PcMacroRuntime::StopHoldBinding(const std::string& id) {
    for (auto& job : jobs_) {
        if (job.bindingId == id && job.holdLoop) StopJob(job);
    }
    jobs_.erase(std::remove_if(jobs_.begin(), jobs_.end(), [](const RunningJob& j) { return j.phase == Phase::Done; }), jobs_.end());
}

void PcMacroRuntime::StopAllJobs() {
    for (auto& job : jobs_) StopJob(job);
    jobs_.clear();
}

void PcMacroRuntime::StopJob(RunningJob& job) {
    if (job.touchDown && callbacks_.touchUp && job.activeSlot >= 0) callbacks_.touchUp(job.activeSlot);
    if (job.keyDown && callbacks_.key && job.currentStep.linuxKeyCode > 0) callbacks_.key(job.currentStep.linuxKeyCode, false);
    job.touchDown = false;
    job.keyDown = false;
    job.phase = Phase::Done;
}

bool PcMacroRuntime::IsJobRunning(const std::string& id) const {
    for (const auto& j : jobs_) if (j.bindingId == id && j.phase != Phase::Done) return true;
    return false;
}

bool PcMacroRuntime::IsHoldJobRunning(const std::string& id) const {
    for (const auto& j : jobs_) if (j.bindingId == id && j.holdLoop && j.phase != Phase::Done) return true;
    return false;
}

void PcMacroRuntime::Tick() {
    const ULONGLONG now = GetTickCount64();
    HandleTriggerTransitions();
    for (auto& job : jobs_) AdvanceJob(job, now);
    jobs_.erase(std::remove_if(jobs_.begin(), jobs_.end(), [](const RunningJob& j) { return j.phase == Phase::Done; }), jobs_.end());
}

void PcMacroRuntime::AdvanceJob(RunningJob& job, ULONGLONG nowMs) {
    if (job.phase == Phase::Done) return;
    if (job.steps.empty()) { job.phase = Phase::Done; return; }
    if (job.phase == Phase::StartStep) {
        BeginCurrentStep(job, nowMs);
        return;
    }

    if (job.phase == Phase::SwipeMove && job.touchDown) {
        const int duration = (std::max)(20, (std::min)(200, job.currentStep.durationMs));
        const double t = (std::min)(1.0, (std::max)(0.0, static_cast<double>(nowMs - job.phaseStartMs) / static_cast<double>(duration)));
        const int x = ClampNorm(static_cast<int>(std::lround(job.runXNorm + (job.runEndXNorm - job.runXNorm) * t)));
        const int y = ClampNorm(static_cast<int>(std::lround(job.runYNorm + (job.runEndYNorm - job.runYNorm) * t)));
        if (callbacks_.touchMove) callbacks_.touchMove(job.activeSlot, x, y);
    }

    if (nowMs < job.phaseEndMs) return;

    // DelayAfter 是“动作结束后的等待阶段”。等待结束后必须进入下一步，
    // 不能再次根据 currentStep.delayAfterMs 重新设置 DelayAfter，否则会卡在第一步，
    // 表现为：后续步骤不执行，并且 PRESS 宏一直处于运行中，无法第二次触发。
    if (job.phase == Phase::DelayAfter) {
        FinishCurrentStep(job, nowMs);
        return;
    }

    if (job.phase == Phase::TapHold || job.phase == Phase::SwipeMove) {
        if (job.touchDown && callbacks_.touchUp) callbacks_.touchUp(job.activeSlot);
        job.touchDown = false;
    } else if (job.phase == Phase::KeyHold) {
        if (job.keyDown && callbacks_.key && job.currentStep.linuxKeyCode > 0) callbacks_.key(job.currentStep.linuxKeyCode, false);
        job.keyDown = false;
    }

    if (job.currentStep.delayAfterMs > 0) {
        job.phase = Phase::DelayAfter;
        job.phaseStartMs = nowMs;
        job.phaseEndMs = nowMs + static_cast<ULONGLONG>((std::max)(0, job.currentStep.delayAfterMs));
        return;
    }

    FinishCurrentStep(job, nowMs);
}

void PcMacroRuntime::BeginCurrentStep(RunningJob& job, ULONGLONG nowMs) {
    if (job.stepIndex >= job.steps.size()) {
        MoveToNextStepOrLoop(job, nowMs);
        return;
    }
    // repeatRemaining > 0 表示同一个步骤的重复执行尚未结束，不能重新初始化，
    // 否则 repeatCount > 1 会被每次重置成无限循环。
    if (job.repeatRemaining <= 0) {
        job.currentStep = job.steps[job.stepIndex];
        job.currentStep.xNorm = ClampNorm(job.currentStep.xNorm);
        job.currentStep.yNorm = ClampNorm(job.currentStep.yNorm);
        job.currentStep.endXNorm = ClampNorm(job.currentStep.endXNorm);
        job.currentStep.endYNorm = ClampNorm(job.currentStep.endYNorm);
        // activeSlot 来自 Binding.slot，整条宏步骤链固定使用同一个 slot。
        // 不能按步骤切 slot，否则 HOLD 宏/多步骤宏会和普通按键池语义不一致。
        job.repeatRemaining = (std::max)(1, job.currentStep.repeatCount);
    }

    BuildRandomizedStepCoords(job.currentStep, job.runXNorm, job.runYNorm, job.runEndXNorm, job.runEndYNorm);

    switch (job.currentStep.actionType) {
        case StepActionType::Tap: {
            const int holdMs = (std::max)(1, job.currentStep.durationMs);
            job.touchDown = callbacks_.touchDown ? callbacks_.touchDown(job.activeSlot, job.runXNorm, job.runYNorm) : false;
            job.phase = Phase::TapHold;
            job.phaseStartMs = nowMs;
            job.phaseEndMs = nowMs + static_cast<ULONGLONG>(holdMs);
            break;
        }
        case StepActionType::Swipe: {
            const int durMs = (std::max)(20, (std::min)(200, job.currentStep.durationMs));
            job.touchDown = callbacks_.touchDown ? callbacks_.touchDown(job.activeSlot, job.runXNorm, job.runYNorm) : false;
            job.phase = Phase::SwipeMove;
            job.phaseStartMs = nowMs;
            job.phaseEndMs = nowMs + static_cast<ULONGLONG>(durMs);
            break;
        }
        case StepActionType::Wait: {
            const int waitMs = (std::max)(1, job.currentStep.durationMs);
            job.phase = Phase::DelayAfter;
            job.phaseStartMs = nowMs;
            job.phaseEndMs = nowMs + static_cast<ULONGLONG>(waitMs + (std::max)(0, job.currentStep.delayAfterMs));
            break;
        }
        case StepActionType::Key: {
            const int holdMs = (std::max)(1, job.currentStep.durationMs);
            job.keyDown = callbacks_.key && job.currentStep.linuxKeyCode > 0 ? callbacks_.key(job.currentStep.linuxKeyCode, true) : false;
            job.phase = Phase::KeyHold;
            job.phaseStartMs = nowMs;
            job.phaseEndMs = nowMs + static_cast<ULONGLONG>(holdMs);
            break;
        }
        case StepActionType::Wheel: {
            if (callbacks_.wheel && job.currentStep.wheelSteps != 0) callbacks_.wheel(job.currentStep.wheelSteps);
            if (job.currentStep.delayAfterMs > 0) {
                job.phase = Phase::DelayAfter;
                job.phaseStartMs = nowMs;
                job.phaseEndMs = nowMs + static_cast<ULONGLONG>(job.currentStep.delayAfterMs);
            } else {
                FinishCurrentStep(job, nowMs);
            }
            break;
        }
        default:
            FinishCurrentStep(job, nowMs);
            break;
    }
}

void PcMacroRuntime::FinishCurrentStep(RunningJob& job, ULONGLONG nowMs) {
    if (job.repeatRemaining > 1) {
        --job.repeatRemaining;
        job.phase = Phase::StartStep;
        return;
    }
    job.repeatRemaining = 0;
    ++job.stepIndex;
    MoveToNextStepOrLoop(job, nowMs);
}

void PcMacroRuntime::MoveToNextStepOrLoop(RunningJob& job, ULONGLONG) {
    if (job.stepIndex < job.steps.size()) {
        job.phase = Phase::StartStep;
        return;
    }
    if (job.holdLoop) {
        // HOLD 宏只有在触发组合仍然满足时才从头循环；松开时 StopHoldBinding 会结束它。
        job.stepIndex = 0;
        job.phase = Phase::StartStep;
        return;
    }
    job.phase = Phase::Done;
}
