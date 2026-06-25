#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "pc_mapping_profile.h"

// 轮盘设置弹窗：非阻塞独立窗口。owner 收到 WM_PC_COMPASS_OPTIONS_DONE：
//   wParam = cookie
//   lParam = PcCompassOptions*；取消时 lParam=0。调用方负责 delete。
constexpr UINT WM_PC_COMPASS_OPTIONS_DONE = WM_APP + 433;

struct PcCompassOptions {
    PcCompassMotionMode motionMode = PcCompassMotionMode::FixedCenter;
    bool centerOuterReversed = false;
    int innerRadiusNorm = PC_COMPASS_INNER_RADIUS_DEFAULT_NORM;
    int outerRadiusNorm = PC_COMPASS_OUTER_RADIUS_DEFAULT_NORM;
    int speedXStep = PC_COMPASS_SPEED_STEP_DEFAULT;
    int speedYStep = PC_COMPASS_SPEED_STEP_DEFAULT;
    int swaySectorPercent = PC_COMPASS_SWAY_SECTOR_DEFAULT;
    int swayDiagonalSectorPercent = PC_COMPASS_SWAY_DIAG_SECTOR_DEFAULT;

    int swayStepMinPercent = PC_COMPASS_SWAY_STEP_MIN_DEFAULT;
    int swayStepMaxPercent = PC_COMPASS_SWAY_STEP_MAX_DEFAULT;
    int swaySpeedPercent = PC_COMPASS_SWAY_SPEED_DEFAULT;
    bool swayMouseButtonSmallStep = false;
    int swayMouseButtonStepPercent = PC_COMPASS_MOUSE_STABLE_SECTOR_DEFAULT; // 鼠标稳定最大扇区
    int swayMouseButtonUpdatePercent = PC_COMPASS_MOUSE_STABLE_SPEED_DEFAULT; // 鼠标稳定扇区速度
    int swayMouseButtonHoldMs = PC_COMPASS_MOUSE_STABLE_HOLD_MS_DEFAULT; // 鼠标稳定持续时间
};

bool OpenCompassOptionsDialog(HINSTANCE inst, HWND ownerHwnd, const PcCompassOptions& options, WPARAM cookie);
