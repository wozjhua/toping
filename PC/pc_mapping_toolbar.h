#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <functional>
#include <string>
#include <vector>

// 右侧独立映射工具窗口：固定 20FPS 刷新，只负责创建映射 UI 的入口。
class PcMappingToolbarPanel final {
public:
    struct Callbacks {
        std::function<void()> beginCreateKey;
        std::function<void()> beginCreateCompass;
        std::function<void()> beginCreateLock;
        std::function<void()> beginCreateMenuRadial;
        std::function<void()> beginCreateMenuItem;
        std::function<void()> beginCreateMenuBag;
        std::function<void()> beginCreateMacro;
        std::function<void()> showProfiles;
        std::function<void()> toggleMappingUi;
        std::function<void(int slot)> saveMappings;
        std::function<void(int slot)> loadMappings;
        std::function<void(bool editMode)> setEditMode;
        std::function<bool()> isEditMode;
        std::function<bool()> isMappingUiHidden;
        std::function<std::wstring()> statusText;
        std::function<int()> mappingCount;
    };

    bool Start(HINSTANCE inst, HWND ownerHwnd, Callbacks callbacks);
    void Stop();
    void UpdateOwnerPosition();
    void SetVisible(bool visible);
    void SetExpanded(bool expanded);
    bool IsVisible() const { return hwnd_ != nullptr && IsWindowVisible(hwnd_) != FALSE; }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT HandleMessage(UINT msg, WPARAM wp, LPARAM lp);
    void ApplyEditMode(bool editMode);
    void ShowExpandedControls(bool show);
    void PaintBackground(HDC dc);
    void CreateControls();
    void LayoutControls();
    void RefreshText();
    HWND CreateButton(const wchar_t* text, int id);
    HWND CreateStatic(const wchar_t* text, int id);

private:
    HINSTANCE inst_{};
    HWND ownerHwnd_{};
    HWND hwnd_{};
    HFONT font_{};
    HFONT titleFont_{};
    HBRUSH bgBrush_{};
    HBRUSH cardBrush_{};
    Callbacks callbacks_{};
    bool expanded_ = false;

    HWND toggle_{};
    HWND btnKey_{};
    HWND btnCompass_{};
    HWND btnLock_{};
    HWND btnMenuRadial_{};
    HWND btnMenuItem_{};
    HWND btnMenuBag_{};
    HWND btnMacro_{};
    HWND btnProfile_{};
    HWND btnHideUi_{};
    HWND btnLoad1_{};
    HWND btnSave1_{};
    HWND btnLoad2_{};
    HWND btnSave2_{};
    HWND btnLoad3_{};
    HWND btnSave3_{};
    HWND status_{};
    HWND count_{};
};

struct PcCapturedKeyCombo {
    // 支持键盘组合、鼠标单键、键盘+鼠标组合。
    // mouseButtonCode: 0=无；1=左键 2=右键 3=中键 4=侧键1 5=侧键2。
    std::vector<int> vkCodes;
    int mouseButtonCode = 0;
    std::wstring label;
};

// 简单组合键捕获弹窗：按下组合键并松开任意一个键后完成，ESC 取消。
bool CaptureKeyComboDialog(HINSTANCE inst, HWND ownerHwnd, PcCapturedKeyCombo& outCombo);

std::wstring PcVkToDisplayName(int vk);
