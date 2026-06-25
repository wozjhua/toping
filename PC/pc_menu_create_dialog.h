#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "pc_mapping_profile.h"

// 返回 true 表示用户选择了菜单类型。使用系统弹出菜单，避免新增复杂 UI 阻塞投屏太久。
bool ShowPcMenuCreatePopup(HWND ownerHwnd, PcMenuCategory& outCategory);
const wchar_t* PcMenuCategoryLabel(PcMenuCategory cat);
