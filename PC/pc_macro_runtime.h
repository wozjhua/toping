#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <array>
#include <functional>
#include <string>
#include <vector>

// PC 投屏普通宏运行时。
// 只负责运行宏：监听触发键、执行步骤、处理 PRESS/HOLD 触发语义。
// 宏的创建 UI、步骤编辑、保存/读取配置不要放在这里，后续单独接入即可。
class PcMacroRuntime final {
public:
    enum class TriggerCondition {
        Press = 0, // 按下：执行一次步骤链，松开不打断
        Hold = 1,  // 按住：循环执行步骤链，松开停止
    };

    enum class StepActionType {
        Tap = 0,
        Swipe = 1,
        Wait = 2,
        Key = 3,
        Wheel = 4,
    };

    struct Step {
        int id = 0;
        int order = 0;
        StepActionType actionType = StepActionType::Tap;
        int slot = 12;
        int xNorm = 500000;
        int yNorm = 500000;
        int endXNorm = 500000;
        int endYNorm = 500000;
        int durationMs = 35;
        int delayAfterMs = 35;
        int repeatCount = 1;
        int randomRadiusNorm = 0;
        int nextStepId = 0; // 0 表示按 order 顺序
        int linuxKeyCode = 0;
        int wheelSteps = 0;
    };

    struct Binding {
        std::string id;
        bool enabled = true;
        std::vector<int> comboTriggerCodes;
        int mouseButtonCode = 0; // 0=无；1=左键 2=右键 3=中键 4=侧键1 5=侧键2
        std::wstring triggerLabel;
        TriggerCondition triggerCondition = TriggerCondition::Press;
        int startStepId = 0;
        // 普通宏 UI 主节点位置。宏运行时也使用 slot 字段作为整条步骤链的固定触点。
        int slot = 12;
        int xNorm = 500000;
        int yNorm = 500000;
        int radiusNorm = 70000;
        std::vector<Step> steps;
        bool consumeEvent = true;
    };

    struct Callbacks {
        std::function<bool(int slot, int xNorm, int yNorm)> touchDown;
        std::function<bool(int slot, int xNorm, int yNorm)> touchMove;
        std::function<bool(int slot)> touchUp;
        std::function<bool(int linuxCode, bool down)> key;
        std::function<bool(int steps)> wheel;
    };

    void SetCallbacks(Callbacks cb);
    void SetBindings(std::vector<Binding> bindings);
    const std::vector<Binding>& GetBindings() const { return bindings_; }

    void Reset();
    void Tick();
    bool HandleWindowMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LRESULT* outResult = nullptr);
    std::wstring StatusText() const;

private:
    enum class Phase {
        StartStep,
        TapHold,
        SwipeMove,
        KeyHold,
        DelayAfter,
        Done,
    };

    struct RunningJob {
        std::string bindingId;
        bool holdLoop = false;
        std::vector<Step> steps;
        size_t stepIndex = 0;
        int repeatRemaining = 0;
        Phase phase = Phase::StartStep;
        ULONGLONG phaseStartMs = 0;
        ULONGLONG phaseEndMs = 0;
        Step currentStep;
        bool touchDown = false;
        bool keyDown = false;
        int activeSlot = -1;
        int runXNorm = 500000;
        int runYNorm = 500000;
        int runEndXNorm = 500000;
        int runEndYNorm = 500000;
    };

    static UINT NormalizeVk(UINT vk, LPARAM lp);
    static int ClampNorm(int v);
    static int MouseButtonCodeFromMessage(UINT msg, WPARAM wp);
    static bool IsDownMessage(UINT msg);
    static bool IsUpMessage(UINT msg);
    static bool ComboSatisfied(const Binding& b, const std::array<bool, 256>& vkDown, const std::array<bool, 8>& mouseDown);
    static std::vector<Step> BuildStepSequence(const Binding& b);
    static void BuildRandomizedStepCoords(const Step& step, int& xNorm, int& yNorm, int& endXNorm, int& endYNorm);

    void HandleTriggerTransitions();
    void StartBinding(const Binding& b, bool holdLoop);
    void StopHoldBinding(const std::string& id);
    void StopAllJobs();
    void StopJob(RunningJob& job);
    bool IsJobRunning(const std::string& id) const;
    bool IsHoldJobRunning(const std::string& id) const;
    void AdvanceJob(RunningJob& job, ULONGLONG nowMs);
    void BeginCurrentStep(RunningJob& job, ULONGLONG nowMs);
    void FinishCurrentStep(RunningJob& job, ULONGLONG nowMs);
    void MoveToNextStepOrLoop(RunningJob& job, ULONGLONG nowMs);

private:
    Callbacks callbacks_{};
    std::vector<Binding> bindings_{};
    std::array<bool, 256> vkDown_{};
    std::array<bool, 8> mouseDown_{};
    std::vector<bool> comboWasDown_{};
    std::vector<RunningJob> jobs_{};
};
