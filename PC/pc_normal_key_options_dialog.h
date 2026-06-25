#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "pc_mapping_profile.h"

// 普通按键选项弹窗：统一自绘风格；支持同步和非阻塞打开。
// 非阻塞打开后，owner 会收到 WM_PC_NORMAL_KEY_OPTIONS_DONE：
//   wParam = cookie
//   lParam = PcNormalKeyOptions*；取消时 lParam=0。调用方负责 delete。
constexpr UINT WM_PC_NORMAL_KEY_OPTIONS_DONE = WM_APP + 431;


namespace PcNormalKeyOptionLimits {

    // 普通按键安全延迟：按键触发后先占用 slot，等待该时间后才真正 TOUCH_DOWN。
    // 用于避免部分设备在前一个触点刚 UP 后立刻 DOWN 时，把两个触点串成滑动。
    inline constexpr int kPendingDownDelayMs = 3;

    // 同一 slot 抬起后的最短冷却时间。当前普通映射通常是固定独立 slot；
    // 这个值用于防御重复 slot/快速重入场景。
    inline constexpr int kSlotReuseCooldownMs = 3;

} // namespace PcNormalKeyOptionLimits

struct PcNormalKeyOptions {
    PcKeyTouchMode touchMode = PcKeyTouchMode::RandomMove;
    PcKeySpecialAction specialAction = PcKeySpecialAction::None;
    int radiusNorm = PC_BUTTON_RADIUS_DEFAULT_NORM;
    int randomRadiusNorm = 3200;
    int randomMoveRadiusNorm = 1800;
    int randomMoveIntervalMs = 16;
};

bool ShowNormalKeyOptionsDialog(HINSTANCE inst, HWND ownerHwnd, PcNormalKeyOptions& options);
bool OpenNormalKeyOptionsDialog(HINSTANCE inst, HWND ownerHwnd, const PcNormalKeyOptions& options, WPARAM cookie);
