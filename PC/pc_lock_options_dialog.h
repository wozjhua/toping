#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "pc_mapping_profile.h"

// 视角设置弹窗。非阻塞打开后 owner 收到：
//   WM_PC_LOCK_OPTIONS_DONE，wParam=cookie，lParam=PcLockOptions*；取消 lParam=0。调用方负责 delete。
constexpr UINT WM_PC_LOCK_OPTIONS_DONE = WM_APP + 432;

namespace PcLockOptionLimits {

    //鼠标移动速度
    inline constexpr double kSpeedNormMin = 0.1;
    inline constexpr double kSpeedNormMax = 1.2;
    inline constexpr double kSpeedNormDefault = 0.5;
    inline constexpr double kSpeedNormStep = 0.1;


    //边界重置时间
    inline constexpr int kRebuildDownDelayMinMs = 2;
    inline constexpr int kRebuildDownDelayMaxMs = 40;
    inline constexpr int kRebuildDownDelayDefaultMs = 5;
    inline constexpr int kRebuildDownDelayStepMs = 1;

    // 当前模式 3 里 primary -> aux 的固定间隔。
    // 先集中到这里，后续再把 Sleep 改成非阻塞 pending。
    inline constexpr int kSequentialAuxDownDelayMs = 5;

    // RawInput 合并时间
    inline constexpr int kRawDeltaCoalesceMs = 2;

    inline double ClampSpeedNorm(double v) {
        return v < kSpeedNormMin ? kSpeedNormMin :
            v > kSpeedNormMax ? kSpeedNormMax : v;
    }

    inline int ClampRebuildDownDelayMs(int v) {
        return v < kRebuildDownDelayMinMs ? kRebuildDownDelayMinMs :
            v > kRebuildDownDelayMaxMs ? kRebuildDownDelayMaxMs : v;
    }

} // namespace PcLockOptionLimits

struct PcLockOptions {
    PcLockSlideTouchMode mode = PcLockSlideTouchMode::DualSequential;
    double speedXNorm = PcLockOptionLimits::kSpeedNormDefault;
    double speedYNorm = PcLockOptionLimits::kSpeedNormDefault;
    int rebuildDownDelayMs = PcLockOptionLimits::kRebuildDownDelayDefaultMs;
};
bool OpenLockOptionsDialog(HINSTANCE inst, HWND ownerHwnd, const PcLockOptions& options, WPARAM cookie);
