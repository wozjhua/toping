#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "pc_uinput_client.h"

// Minimal mirror-window input controller for pc_uinputd v1.
// It sends only final touch actions to Android: DOWN/MOVE/UP/WHEEL/KEY.
class PcUinputMirrorController final {
public:
    struct Config {
        PcUinputClient::Config client;
        bool enableMouse = true;
        bool enableKeyboard = true;
        bool enableWheel = true;
        int primarySlot = 0;

        // Android 的 settings user_rotation 在部分设备横屏时仍会返回 0。
        // PC 已经知道当前投屏帧宽高，所以在帧尺寸变化时主动下发坐标基准。
        // portraitRotation 一般为 0；landscapeRotation 若左右颠倒，可在这里改成 1 或 3。
        int portraitRotation = -1;      // -1 = 使用 Android daemon 端完整检测/旧 DISPLAYCFG 语义
        int landscapeRotation = -1;     // 可用 F8 临时在 1/3 间校准
    };

    bool Start(HWND mirrorHwnd, const Config& cfg = Config{});
    void Stop();
    bool IsConnected() const { return client_.IsConnected(); }
    std::wstring StatusText() const { return client_.StatusText(); }

    // frameW/frameH 是当前 PC 实际收到/显示的投屏帧尺寸。
    // 横屏帧会强制把 pc_uinputd 切到 landscapeRotation，并把 UI 坐标基准设为 frameW x frameH。
    void UpdateFrameGeometry(int frameW, int frameH);

    // D3D 每帧把主画面真正绘制到窗口里的 viewport 同步过来。
    // 鼠标坐标必须使用这个区域，而不是自己按窗口大小猜黑边。
    void SetMirrorFrameViewport(float x, float y, float w, float h, int frameW, int frameH);

    void CycleLandscapeRotation();
    void SetAndroidScreenSize(int screenW, int screenH);

    bool HandleWindowMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LRESULT* outResult = nullptr);

    // 给 PC 映射运行时使用的低层动作接口。
    // PcUinputMirrorController 仍然负责普通投屏鼠标点击；映射模块只通过这些接口发送最终动作。
    // PC 映射 runtime 的坐标通常是“投屏帧/Overlay 坐标系”的 norm。
    // 当 Android 真实画面被塞进 16:9 标准帧并产生内部黑边时，
    // runtime 发送前必须和普通鼠标自由模式走同一套 frame -> Android 有效画面换算。
    bool FrameNormToAndroidNorm(int frameXNorm1000000, int frameYNorm1000000, int& outXNorm1000000, int& outYNorm1000000) const;

    bool SendTouchDownNorm(int slot, int xNorm1000000, int yNorm1000000);
    bool SendTouchMoveNorm(int slot, int xNorm1000000, int yNorm1000000);
    bool SendTouchUp(int slot);
    bool SendLinuxKey(int linuxCode, bool down);
    bool SendWheelSteps(int steps);

private:
    static int clampInt(int v, int lo, int hi);
    bool framePointToAndroidNorm(double frameX, double frameY, double frameW, double frameH, bool rejectOutsideContent, int& nx, int& ny) const;
    bool clientPointToNorm(HWND hwnd, int x, int y, int& nx, int& ny) const;
    static int vkToLinuxCode(UINT vk, LPARAM lp);
    void sendTouchMoveFromPoint(HWND hwnd, int x, int y, bool force);

private:
    Config cfg_;
    PcUinputClient client_;
    HWND hwnd_ = nullptr;
    bool touchDown_ = false;
    int lastXNorm_ = -1;
    int lastYNorm_ = -1;
    int lastFrameW_ = 0;
    int lastFrameH_ = 0;
    int activeRotation_ = -1;

    // D3D 实际显示区域：窗口坐标 -> 投屏帧坐标必须走这里。
    bool mirrorViewportValid_ = false;
    float mirrorVpX_ = 0.0f;
    float mirrorVpY_ = 0.0f;
    float mirrorVpW_ = 1.0f;
    float mirrorVpH_ = 1.0f;
    int mirrorViewportFrameW_ = 1920;
    int mirrorViewportFrameH_ = 1080;

    // Android 真实/有效显示尺寸，用来计算标准投屏帧里的内部黑边。
    // 不能写死某一台设备；启动时优先 HUILANG_ANDROID_SCREEN，其次 adb shell wm size。
    // 如果两者都失败，按当前投屏帧比例兜底，避免换设备后继续套用旧设备比例。
    mutable bool androidScreenSizeLoaded_ = false;
    mutable int androidScreenW_ = 0;
    mutable int androidScreenH_ = 0;
};
