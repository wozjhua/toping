#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "pc_mapping_profile.h"

// 菜单按钮设置弹窗。非阻塞打开后 owner 收到：
//   WM_PC_MENU_OPTIONS_DONE，wParam=cookie，lParam=PcMenuOptions*；取消 lParam=0。调用方负责 delete。
constexpr UINT WM_PC_MENU_OPTIONS_DONE = WM_APP + 436;

struct PcMenuOptions {
    PcMenuCategory category = PcMenuCategory::MenuRadial;

    // 轮盘菜单仍使用相对移动倍率；道具/背包是自由鼠标模式，不使用这两个值。
    double relativeSpeedX = 0.5;
    double relativeSpeedY = 0.5;

    // 轮盘/道具/背包共用的圆形按钮 UI 半径。显示范围 20~60，默认 30。
    int radiusNorm = PC_MENU_BUTTON_RADIUS_DEFAULT_NORM;

    PcMenuTriggerPlacement triggerPlacement = PcMenuTriggerPlacement::Bottom;
    PcItemLandingZone itemLandingZone = PcItemLandingZone::Bottom;
    bool itemWheelScrollEnabled = false;
    bool itemWheelInvert = false;
    int itemWheelScrollSpeed = PC_MENU_ITEM_WHEEL_SPEED_DEFAULT;
    int itemWheelMaxDistancePx = PC_MENU_ITEM_WHEEL_DISTANCE_DEFAULT;
    int itemWheelStepPx = PC_MENU_ITEM_WHEEL_STEP_DEFAULT;
};

bool OpenMenuOptionsDialog(HINSTANCE inst, HWND ownerHwnd, const PcMenuOptions& options, WPARAM cookie);
