#include "pc_uinput_mirror_controller.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windowsx.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {
static bool parseSizePairLocal(const char* text, int& outW, int& outH) {
    if (!text) return false;
    const char* p = text;
    while (*p) {
        while (*p && (*p < '0' || *p > '9')) ++p;
        if (!*p) break;
        int w = 0;
        int h = 0;
        int consumed = 0;
        if (std::sscanf(p, "%dx%d%n", &w, &h, &consumed) == 2 && w > 0 && h > 0) {
            outW = w;
            outH = h;
            return true;
        }
        ++p;
    }
    return false;
}
}

int PcUinputMirrorController::clampInt(int v, int lo, int hi) {
    return (std::max)(lo, (std::min)(hi, v));
}

bool PcUinputMirrorController::Start(HWND mirrorHwnd, const Config& cfg) {
    Stop();
    cfg_ = cfg;
    hwnd_ = mirrorHwnd;
    if (!IsWindow(hwnd_)) return false;
    touchDown_ = false;
    lastXNorm_ = -1;
    lastYNorm_ = -1;
    lastFrameW_ = 0;
    lastFrameH_ = 0;
    activeRotation_ = -1;
    mirrorViewportValid_ = false;
    mirrorVpX_ = 0.0f;
    mirrorVpY_ = 0.0f;
    mirrorVpW_ = 1.0f;
    mirrorVpH_ = 1.0f;
    mirrorViewportFrameW_ = 1920;
    mirrorViewportFrameH_ = 1080;
    androidScreenSizeLoaded_ = false;
    androidScreenW_ = 0;
    androidScreenH_ = 0;

    // 当前 Android 设备尺寸必须动态获取，不能写死 Y700。
    // 优先环境变量，方便手动校准；否则从 adb shell wm size 读取当前有效显示尺寸。
    int sw = 0;
    int sh = 0;
    if (const char* env = std::getenv("HUILANG_ANDROID_SCREEN")) {
        parseSizePairLocal(env, sw, sh);
    }
    if (sw <= 0 || sh <= 0) {
        PcUinputClient::QueryAdbDisplaySize(cfg_.client, sw, sh);
    }
    if (sw > 0 && sh > 0) {
        SetAndroidScreenSize(sw, sh);
    }

    return client_.Start(cfg_.client);
}

void PcUinputMirrorController::Stop() {
    if (touchDown_) {
        client_.TouchUp(cfg_.primarySlot);
        touchDown_ = false;
    }
    client_.Stop();
}

void PcUinputMirrorController::UpdateFrameGeometry(int frameW, int frameH) {
    if (frameW <= 0 || frameH <= 0) return;
    const bool landscape = frameW > frameH;
    int rot = landscape ? cfg_.landscapeRotation : cfg_.portraitRotation;
    if (rot < 0 || rot > 3) rot = -1;

    if (frameW == lastFrameW_ && frameH == lastFrameH_ && rot == activeRotation_) return;
    lastFrameW_ = frameW;
    lastFrameH_ = frameH;
    activeRotation_ = rot;

    if (client_.IsConnected()) {
        // 这里按旧 Android DISPLAYCFG 语义：uiW/uiH=当前投屏坐标系；
        // physW/physH=0 让 daemon 使用 Android 端真实物理尺寸；rotation=-1 让 daemon 自己检测。
        client_.SendConfig(rot, frameW, frameH, 0, 0);
    }
}

void PcUinputMirrorController::SetMirrorFrameViewport(
    float x,
    float y,
    float w,
    float h,
    int frameW,
    int frameH
) {
    if (w <= 1.0f || h <= 1.0f || frameW <= 0 || frameH <= 0) return;
    mirrorViewportValid_ = true;
    mirrorVpX_ = x;
    mirrorVpY_ = y;
    mirrorVpW_ = (std::max)(1.0f, w);
    mirrorVpH_ = (std::max)(1.0f, h);
    mirrorViewportFrameW_ = (std::max)(1, frameW);
    mirrorViewportFrameH_ = (std::max)(1, frameH);
}

void PcUinputMirrorController::SetAndroidScreenSize(int screenW, int screenH) {
    if (screenW <= 0 || screenH <= 0) return;
    androidScreenW_ = screenW;
    androidScreenH_ = screenH;
    androidScreenSizeLoaded_ = true;
}

void PcUinputMirrorController::CycleLandscapeRotation() {
    // 只在横屏帧下切换 90/270；用于快速确认不同设备横屏方向。
    cfg_.landscapeRotation = (cfg_.landscapeRotation == 1) ? 3 : 1;
    if (cfg_.landscapeRotation < 0) cfg_.landscapeRotation = 1;
    if (lastFrameW_ > lastFrameH_) {
        activeRotation_ = -1;
        UpdateFrameGeometry(lastFrameW_, lastFrameH_);
    }
}


bool PcUinputMirrorController::framePointToAndroidNorm(
    double frameX,
    double frameY,
    double frameW,
    double frameH,
    bool rejectOutsideContent,
    int& nx,
    int& ny
) const {
    if (frameW <= 1.0 || frameH <= 1.0) return false;

    // 和 clientPointToNorm() 使用同一套 Android 有效画面换算：
    // 标准 16:9 投屏帧内可能包含左右/上下黑边，runtime 坐标必须先从“帧坐标”映射到真实 Android 屏幕坐标。
    if (!androidScreenSizeLoaded_) {
        int sw = 0;
        int sh = 0;
        if (const char* env = std::getenv("HUILANG_ANDROID_SCREEN")) {
            parseSizePairLocal(env, sw, sh);
        }
        if (sw <= 0 || sh <= 0) {
            PcUinputClient::QueryAdbDisplaySize(cfg_.client, sw, sh);
        }
        if (sw <= 0 || sh <= 0) {
            // 保底：没有拿到设备尺寸时，不再套用旧设备比例，直接按当前投屏帧比例处理。
            sw = static_cast<int>(std::lround(frameW));
            sh = static_cast<int>(std::lround(frameH));
        }
        const bool frameLandscape = frameW >= frameH;
        const bool screenLandscape = sw >= sh;
        if (frameLandscape != screenLandscape) std::swap(sw, sh);
        androidScreenW_ = sw;
        androidScreenH_ = sh;
        androidScreenSizeLoaded_ = true;
    }

    // androidScreenW_/H_ 可能来自 adb shell wm size。很多设备在横屏投屏时，wm size 仍返回自然竖屏方向，
    // 例如 1080x2400；而当前投屏帧是 1920x1080。这里必须按“当前投屏帧方向”重排 W/H，
    // 否则会把左右黑边误判成上下黑边，坐标会比 v8 偏得更大。
    int orientedScreenW = (std::max)(1, androidScreenW_);
    int orientedScreenH = (std::max)(1, androidScreenH_);
    const bool frameLandscape = frameW >= frameH;
    const bool screenLandscape = orientedScreenW >= orientedScreenH;
    if (frameLandscape != screenLandscape) std::swap(orientedScreenW, orientedScreenH);

    const double screenW = static_cast<double>(orientedScreenW);
    const double screenH = static_cast<double>(orientedScreenH);
    const double screenAspect = screenW / screenH;
    const double frameAspect = frameW / frameH;

    double contentX = 0.0;
    double contentY = 0.0;
    double contentW = frameW;
    double contentH = frameH;

    if (screenAspect < frameAspect) {
        contentH = frameH;
        contentW = frameH * screenAspect;
        contentX = (frameW - contentW) * 0.5;
        contentY = 0.0;
    } else if (screenAspect > frameAspect) {
        contentW = frameW;
        contentH = frameW / screenAspect;
        contentX = 0.0;
        contentY = (frameH - contentH) * 0.5;
    }

    if (rejectOutsideContent) {
        if (frameX < contentX || frameY < contentY ||
            frameX >= contentX + contentW || frameY >= contentY + contentH) {
            return false;
        }
    } else {
        // 映射 runtime 不能因为目标点轻微进入黑边就丢包，否则摇杆会断触；这里夹到有效画面边缘。
        frameX = (std::max)(contentX, (std::min)(contentX + contentW - 1.0, frameX));
        frameY = (std::max)(contentY, (std::min)(contentY + contentH - 1.0, frameY));
    }

    double ax = (frameX - contentX) / (std::max)(1.0, contentW);
    double ay = (frameY - contentY) / (std::max)(1.0, contentH);

    ax = (std::max)(0.0, (std::min)(1.0, ax));
    ay = (std::max)(0.0, (std::min)(1.0, ay));

    nx = clampInt(static_cast<int>(std::lround(ax * 1000000.0)), 0, 1000000);
    ny = clampInt(static_cast<int>(std::lround(ay * 1000000.0)), 0, 1000000);
    return true;
}

bool PcUinputMirrorController::FrameNormToAndroidNorm(int frameXNorm1000000, int frameYNorm1000000, int& outXNorm1000000, int& outYNorm1000000) const {
    const double frameW = static_cast<double>((std::max)(1, mirrorViewportFrameW_ > 0 ? mirrorViewportFrameW_ : lastFrameW_));
    const double frameH = static_cast<double>((std::max)(1, mirrorViewportFrameH_ > 0 ? mirrorViewportFrameH_ : lastFrameH_));
    if (frameW <= 1.0 || frameH <= 1.0) {
        outXNorm1000000 = clampInt(frameXNorm1000000, 0, 1000000);
        outYNorm1000000 = clampInt(frameYNorm1000000, 0, 1000000);
        return true;
    }

    const double fx = (static_cast<double>(clampInt(frameXNorm1000000, 0, 1000000)) / 1000000.0) * (frameW - 1.0);
    const double fy = (static_cast<double>(clampInt(frameYNorm1000000, 0, 1000000)) / 1000000.0) * (frameH - 1.0);
    return framePointToAndroidNorm(fx, fy, frameW, frameH, false, outXNorm1000000, outYNorm1000000);
}

bool PcUinputMirrorController::clientPointToNorm(HWND hwnd, int x, int y, int& nx, int& ny) const {
    RECT rc{};
    if (!IsWindow(hwnd) || !GetClientRect(hwnd, &rc)) return false;

    const int winW = (std::max)(1L, rc.right - rc.left);
    const int winH = (std::max)(1L, rc.bottom - rc.top);

    // 1) 窗口坐标 -> D3D 实际绘制出来的投屏帧区域。
    //    这里不能自己再猜 16:9 黑边，必须使用 d3d11_render_present.cpp 每帧同步过来的 frameVp。
    double drawX = 0.0;
    double drawY = 0.0;
    double drawW = static_cast<double>(winW);
    double drawH = static_cast<double>(winH);
    double frameW = static_cast<double>((std::max)(1, lastFrameW_));
    double frameH = static_cast<double>((std::max)(1, lastFrameH_));

    if (mirrorViewportValid_) {
        drawX = static_cast<double>(mirrorVpX_);
        drawY = static_cast<double>(mirrorVpY_);
        drawW = static_cast<double>((std::max)(1.0f, mirrorVpW_));
        drawH = static_cast<double>((std::max)(1.0f, mirrorVpH_));
        frameW = static_cast<double>((std::max)(1, mirrorViewportFrameW_));
        frameH = static_cast<double>((std::max)(1, mirrorViewportFrameH_));
    }

    const double localX = static_cast<double>(x) - drawX;
    const double localY = static_cast<double>(y) - drawY;

    // 点到 PC/D3D 外层黑边，不发送，避免落到旧坐标。
    if (localX < 0.0 || localY < 0.0 || localX >= drawW || localY >= drawH) {
        return false;
    }

    const double frameX = localX * frameW / drawW;
    const double frameY = localY * frameH / drawH;

    // 2) 投屏帧坐标 -> Android 真实有效画面坐标。
    //    和 FrameNormToAndroidNorm() 共享同一套有效画面/内部黑边换算。
    return framePointToAndroidNorm(frameX, frameY, frameW, frameH, true, nx, ny);
}

void PcUinputMirrorController::sendTouchMoveFromPoint(HWND hwnd, int x, int y, bool force) {
    if (!client_.IsConnected() || !cfg_.enableMouse) return;
    int nx = 0, ny = 0;
    if (!clientPointToNorm(hwnd, x, y, nx, ny)) return;
    if (!force && nx == lastXNorm_ && ny == lastYNorm_) return;
    lastXNorm_ = nx;
    lastYNorm_ = ny;
    if (touchDown_) client_.TouchMove(cfg_.primarySlot, nx, ny);
}

static UINT mapLeftRightModifierLocal(UINT vk, LPARAM lp) {
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

int PcUinputMirrorController::vkToLinuxCode(UINT vk, LPARAM lp) {
    vk = mapLeftRightModifierLocal(vk, lp);
    switch (vk) {
        case '0': return 11; case '1': return 2; case '2': return 3; case '3': return 4; case '4': return 5;
        case '5': return 6; case '6': return 7; case '7': return 8; case '8': return 9; case '9': return 10;
        case 'A': return 30; case 'B': return 48; case 'C': return 46; case 'D': return 32; case 'E': return 18;
        case 'F': return 33; case 'G': return 34; case 'H': return 35; case 'I': return 23; case 'J': return 36;
        case 'K': return 37; case 'L': return 38; case 'M': return 50; case 'N': return 49; case 'O': return 24;
        case 'P': return 25; case 'Q': return 16; case 'R': return 19; case 'S': return 31; case 'T': return 20;
        case 'U': return 22; case 'V': return 47; case 'W': return 17; case 'X': return 45; case 'Y': return 21;
        case 'Z': return 44;
        case VK_ESCAPE: return 1;
        case VK_TAB: return 15;
        case VK_BACK: return 14;
        case VK_RETURN: return 28;
        case VK_SPACE: return 57;
        case VK_LSHIFT: return 42;
        case VK_RSHIFT: return 54;
        case VK_LCONTROL: return 29;
        case VK_RCONTROL: return 97;
        case VK_LMENU: return 56;
        case VK_RMENU: return 100;
        case VK_UP: return 103;
        case VK_DOWN: return 108;
        case VK_LEFT: return 105;
        case VK_RIGHT: return 106;
        case VK_HOME: return 102;
        case VK_END: return 107;
        case VK_PRIOR: return 104;
        case VK_NEXT: return 109;
        default: return 0;
    }
}


bool PcUinputMirrorController::SendTouchDownNorm(int slot, int xNorm1000000, int yNorm1000000) {
    if (!client_.IsConnected()) return false;
    return client_.TouchDown(slot, clampInt(xNorm1000000, 0, 1000000), clampInt(yNorm1000000, 0, 1000000));
}

bool PcUinputMirrorController::SendTouchMoveNorm(int slot, int xNorm1000000, int yNorm1000000) {
    if (!client_.IsConnected()) return false;
    return client_.TouchMove(slot, clampInt(xNorm1000000, 0, 1000000), clampInt(yNorm1000000, 0, 1000000));
}

bool PcUinputMirrorController::SendTouchUp(int slot) {
    if (!client_.IsConnected()) return false;
    return client_.TouchUp(slot);
}

bool PcUinputMirrorController::SendLinuxKey(int linuxCode, bool down) {
    if (!client_.IsConnected()) return false;
    return client_.Key(linuxCode, down);
}

bool PcUinputMirrorController::SendWheelSteps(int steps) {
    if (!client_.IsConnected()) return false;
    return client_.Wheel(steps);
}

bool PcUinputMirrorController::HandleWindowMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LRESULT* outResult) {
    if (outResult) *outResult = 0;
    if (!client_.IsConnected()) return false;

    switch (msg) {
        case WM_LBUTTONDOWN: {
            if (!cfg_.enableMouse) return false;
            SetFocus(hwnd);
            int nx = 0, ny = 0;
            if (!clientPointToNorm(hwnd, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), nx, ny)) {
                return true;
            }
            lastXNorm_ = nx;
            lastYNorm_ = ny;
            touchDown_ = true;
            SetCapture(hwnd);
            client_.TouchDown(cfg_.primarySlot, nx, ny);
            return true;
        }
        case WM_MOUSEMOVE: {
            if (touchDown_) {
                sendTouchMoveFromPoint(hwnd, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), true);
            }
            return false;
        }
        case WM_LBUTTONUP: {
            if (touchDown_) {
                sendTouchMoveFromPoint(hwnd, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), true);
                client_.TouchUp(cfg_.primarySlot);
                touchDown_ = false;
                if (GetCapture() == hwnd) ReleaseCapture();
                return true;
            }
            return false;
        }
        case WM_MOUSEWHEEL: {
            if (!cfg_.enableWheel) return false;
            int delta = GET_WHEEL_DELTA_WPARAM(wp);
            int steps = delta / WHEEL_DELTA;
            if (steps == 0 && delta != 0) steps = delta > 0 ? 1 : -1;
            client_.Wheel(steps);
            return true;
        }
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN: {
            if ((lp & 0x40000000L) != 0) return false;
            // 临时校准热键：F8 在横屏 rotation=1/3 之间切换，不发送给 Android。
            // 如果横屏左右/上下错位，按一次 F8 后再做四角测试。
            if (wp == VK_F8) {
                CycleLandscapeRotation();
                return true;
            }
            if (!cfg_.enableKeyboard) return false;
            int code = vkToLinuxCode(static_cast<UINT>(wp), lp);
            if (code > 0) client_.Key(code, true);
            return false;
        }
        case WM_KEYUP:
        case WM_SYSKEYUP: {
            if (!cfg_.enableKeyboard) return false;
            int code = vkToLinuxCode(static_cast<UINT>(wp), lp);
            if (code > 0) client_.Key(code, false);
            return false;
        }
        case WM_KILLFOCUS:
        case WM_CANCELMODE: {
            if (touchDown_) {
                client_.TouchUp(cfg_.primarySlot);
                touchDown_ = false;
            }
            if (GetCapture() == hwnd) ReleaseCapture();
            client_.Reset();
            return false;
        }
        default:
            return false;
    }
}
