#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <string>
#include <vector>

struct PcCapturedKeyCombo {
    // 支持键盘组合、鼠标单键、键盘+鼠标组合。
    // mouseButtonCode: 0=无；1=左键 2=右键 3=中键 4=侧键1 5=侧键2。
    std::vector<int> vkCodes;
    int mouseButtonCode = 0;
    std::wstring label;
};

std::wstring PcVkToDisplayName(int vk);
bool CaptureKeyComboDialog(HINSTANCE inst, HWND ownerHwnd, PcCapturedKeyCombo& outCombo);
