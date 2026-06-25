#include "d3d11_renderer.h"

// compassButtonByIndex
PcCompassButtonBinding* D3D11Renderer::compassButtonByIndex(PcCompassBinding& c, int index) {
        switch (index) {
        case 0: return &c.up;
        case 1: return &c.left;
        case 2: return &c.down;
        case 3: return &c.right;
        case 4: return &c.center;
        default: return nullptr;
        }
    }

// deleteCompassBinding
bool D3D11Renderer::deleteCompassBinding() {
        PcMappingProfile profile = mappingRuntime_.GetProfile();
        if (profile.compasses.empty()) return false;
        profile.compasses.clear();
        compassRuntime_.Reset();
        mappingRuntime_.SetProfile(profile);
        mappingRuntime_.SetEnabled(true);
        compassSelected_ = false;
        selectedMappingBindingIndex_ = -1;
        selectedMenuIndex_ = -1;
        compassDrag_ = false;
        compassDiagDrag_ = false;
        compassSectorDrag_ = false;
        mappingOverlayDirty_ = true;
        dirtyWindow_ = true;
        mappingEditorStatus_ = HLW(L"轮盘已删除");
        updateStatusText(mappingEditorStatus_);
        refreshMappingToolbarStatus();
        return true;
    }

// rebindCompassButton
bool D3D11Renderer::rebindCompassButton(int buttonIndex) {
        PcMappingProfile profile = mappingRuntime_.GetProfile();
        if (profile.compasses.empty()) return false;
        PcCompassButtonBinding* btn = compassButtonByIndex(profile.compasses.front(), buttonIndex);
        if (!btn) return false;
        mappingEditorStatus_ = HLW(L"重绑轮盘：请按新的键盘组合");
        updateStatusText(mappingEditorStatus_);
        PcCapturedKeyCombo combo;
        if (!CaptureKeyComboDialog(inst_, hwnd_, combo) || combo.vkCodes.empty() || combo.mouseButtonCode > 0) {
            mappingEditorStatus_ = HLW(L"重绑轮盘已取消：暂只支持键盘组合");
            updateStatusText(mappingEditorStatus_);
            refreshMappingToolbarStatus();
            return true;
        }
        btn->comboTriggerCodes = combo.vkCodes;
        btn->triggerLabel = combo.label;
        compassRuntime_.Reset();
        mappingRuntime_.SetProfile(profile);
        mappingRuntime_.SetEnabled(true);
        mappingOverlayDirty_ = true;
        dirtyWindow_ = true;
        mappingEditorStatus_ = HLW(L"轮盘已重绑：") + combo.label;
        updateStatusText(mappingEditorStatus_);
        refreshMappingToolbarStatus();
        return true;
    }

// setCompassMotionMode
bool D3D11Renderer::setCompassMotionMode(int mode) {
        PcMappingProfile profile = mappingRuntime_.GetProfile();
        if (profile.compasses.empty()) return false;
        if (mode < 1 || mode > 2) return true;
        profile.compasses.front().motionMode = static_cast<PcCompassMotionMode>(mode);
        compassRuntime_.Reset();
        mappingRuntime_.SetProfile(profile);
        mappingRuntime_.SetEnabled(true);
        mappingOverlayDirty_ = true;
        dirtyWindow_ = true;
        mappingEditorStatus_ = (mode == 1) ? HLW(L"轮盘模式：固定") : HLW(L"轮盘模式：晃动");
        updateStatusText(mappingEditorStatus_);
        refreshMappingToolbarStatus();
        return true;
    }

// toggleCompassRadiusReverse
bool D3D11Renderer::toggleCompassRadiusReverse() {
        PcMappingProfile profile = mappingRuntime_.GetProfile();
        if (profile.compasses.empty()) return false;
        profile.compasses.front().centerOuterReversed = !profile.compasses.front().centerOuterReversed;
        compassRuntime_.Reset();
        mappingRuntime_.SetProfile(profile);
        mappingRuntime_.SetEnabled(true);
        mappingOverlayDirty_ = true;
        dirtyWindow_ = true;
        mappingEditorStatus_ = profile.compasses.front().centerOuterReversed ? HLW(L"轮盘半径：反转开") : HLW(L"轮盘半径：反转关");
        updateStatusText(mappingEditorStatus_);
        refreshMappingToolbarStatus();
        return true;
    }

// editCompassOptions
bool D3D11Renderer::editCompassOptions() {
        PcMappingProfile profile = mappingRuntime_.GetProfile();
        if (profile.compasses.empty()) {
            mappingEditorStatus_ = HLW(L"轮盘设置：请先创建轮盘");
            updateStatusText(mappingEditorStatus_);
            refreshMappingToolbarStatus();
            return true;
        }
        const auto& c = profile.compasses.front();
        PcCompassOptions opt;
        opt.motionMode = c.motionMode;
        opt.centerOuterReversed = c.centerOuterReversed;
        opt.innerRadiusNorm = c.innerRadiusNorm;
        opt.outerRadiusNorm = c.outerRadiusNorm;
        opt.speedXStep = c.speedXStep;
        opt.speedYStep = c.speedYStep;
        opt.swaySectorPercent = c.swaySectorPercent;
        opt.swayDiagonalSectorPercent = c.swayDiagonalSectorPercent;
        opt.swayStepMinPercent = c.swayStepMinPercent;
        opt.swayStepMaxPercent = c.swayStepMaxPercent;
        opt.swaySpeedPercent = c.swaySpeedPercent;
        opt.swayMouseButtonSmallStep = c.swayMouseButtonSmallStep;
        opt.swayMouseButtonStepPercent = c.swayMouseButtonStepPercent;
        opt.swayMouseButtonUpdatePercent = c.swayMouseButtonUpdatePercent;
        opt.swayMouseButtonHoldMs = c.swayMouseButtonHoldMs;
        mappingEditorStatus_ = HLW(L"轮盘设置：独立窗口已打开");
        updateStatusText(mappingEditorStatus_);
        refreshMappingToolbarStatus();
        if (!OpenCompassOptionsDialog(inst_, hwnd_, opt, 0)) {
            mappingEditorStatus_ = HLW(L"轮盘设置打开失败");
            updateStatusText(mappingEditorStatus_);
            refreshMappingToolbarStatus();
        }
        return true;
    }

// clampCompassInnerRadiusNorm
int D3D11Renderer::clampCompassInnerRadiusNorm(int v) {
        return (std::max)(PC_COMPASS_INNER_RADIUS_MIN_NORM, (std::min)(PC_COMPASS_INNER_RADIUS_MAX_NORM, v));
    }

// clampCompassOuterRadiusNorm
int D3D11Renderer::clampCompassOuterRadiusNorm(int v) {
        return (std::max)(PC_COMPASS_OUTER_RADIUS_MIN_NORM, (std::min)(PC_COMPASS_OUTER_RADIUS_MAX_NORM, v));
    }

// normalizeCompassRadii
void D3D11Renderer::normalizeCompassRadii(PcCompassBinding& c) {
        c.innerRadiusNorm = clampCompassInnerRadiusNorm(c.innerRadiusNorm);
        c.outerRadiusNorm = clampCompassOuterRadiusNorm(c.outerRadiusNorm);
        static constexpr int kMinGap = PC_COMPASS_RADIUS_MIN_GAP_NORM;
        if (c.outerRadiusNorm < c.innerRadiusNorm + kMinGap) {
            c.outerRadiusNorm = (std::min)(PC_COMPASS_OUTER_RADIUS_MAX_NORM, c.innerRadiusNorm + kMinGap);
            if (c.outerRadiusNorm < c.innerRadiusNorm + kMinGap) {
                c.innerRadiusNorm = (std::max)(PC_COMPASS_INNER_RADIUS_MIN_NORM, c.outerRadiusNorm - kMinGap);
            }
        }
    }

// compassVisualRadiusPxForClamp
//摇杆轮盘半径限制
    int D3D11Renderer::compassVisualRadiusPxForClamp(const PcCompassBinding& c, int width, int height) {
        (void)width;
        (void)height;
        // 必须和 pc_mapping_overlay.cpp::compassRadiusPx()、PcCompassRuntime::VisualCircleRadiusPx() 保持一致：
        // 外半径滑条值直接按 PC 像素/DP 使用，240 -> 240px。
        const int r = PcMappingProfile::ClampRadiusNorm(c.outerRadiusNorm) / PC_COMPASS_RADIUS_UNIT;
        return (std::max)(20, r);
    }

// clampCompassToBounds
void D3D11Renderer::clampCompassToBounds(PcCompassBinding& c) {
        normalizeCompassRadii(c);

        // 基础归一化：只保证字段合法。
        // 注意：这里不能用 outerRadiusNorm 直接限制 centerX/Y。
        // outerRadiusNorm 是相对 min(width,height) 的半径；在横屏下如果直接当作 X 轴归一化边距，
        // 会导致轮盘离左右边界很远就被卡住。
        c.centerXNorm = PcMappingProfile::ClampNorm(c.centerXNorm);
        c.centerYNorm = PcMappingProfile::ClampNorm(c.centerYNorm);

        c.speedXStep = (std::max)(PC_COMPASS_SPEED_STEP_MIN, (std::min)(PC_COMPASS_SPEED_STEP_MAX, c.speedXStep));
        c.speedYStep = (std::max)(PC_COMPASS_SPEED_STEP_MIN, (std::min)(PC_COMPASS_SPEED_STEP_MAX, c.speedYStep));
    }

// clampCompassUiPositionToWindow
void D3D11Renderer::clampCompassUiPositionToWindow(PcCompassBinding& c) const {
        clampCompassToBounds(c);

        RECT rc{};
        if (!IsWindow(hwnd_) || !GetClientRect(hwnd_, &rc)) return;

        const int width = (std::max)(1L, rc.right - rc.left);
        const int height = (std::max)(1L, rc.bottom - rc.top);
        const int outerPx = compassVisualRadiusPxForClamp(c, width, height);

        // 限制的是“轮盘 UI 外圈”在当前窗口内，而不是用 outerRadiusNorm 直接限制中心点。
        // 横屏下 X/Y 的归一化边距不同：同样 205px，X 轴约 10.7%，Y 轴约 19%。
        const int marginXNorm = pxMarginToNorm(outerPx, width);
        const int marginYNorm = pxMarginToNorm(outerPx, height);

        const int minX = (std::max)(0, (std::min)(500000, marginXNorm));
        const int maxX = 1000000 - minX;
        const int minY = (std::max)(0, (std::min)(500000, marginYNorm));
        const int maxY = 1000000 - minY;

        c.centerXNorm = (std::max)(minX, (std::min)(maxX, PcMappingProfile::ClampNorm(c.centerXNorm)));
        c.centerYNorm = (std::max)(minY, (std::min)(maxY, PcMappingProfile::ClampNorm(c.centerYNorm)));
    }

// resetCompassDiagonalAnchors
void D3D11Renderer::resetCompassDiagonalAnchors(PcCompassBinding& c) {
        const int diagR = (c.innerRadiusNorm + c.outerRadiusNorm) / 2;
        const int diag = static_cast<int>(diagR * 0.70710678);
        c.diagLeftUpXNorm = PcMappingProfile::ClampNorm(c.centerXNorm - diag);
        c.diagLeftUpYNorm = PcMappingProfile::ClampNorm(c.centerYNorm - diag);
        c.diagRightUpXNorm = PcMappingProfile::ClampNorm(c.centerXNorm + diag);
        c.diagRightUpYNorm = PcMappingProfile::ClampNorm(c.centerYNorm - diag);
        c.diagLeftDownXNorm = PcMappingProfile::ClampNorm(c.centerXNorm - diag);
        c.diagLeftDownYNorm = PcMappingProfile::ClampNorm(c.centerYNorm + diag);
        c.diagRightDownXNorm = PcMappingProfile::ClampNorm(c.centerXNorm + diag);
        c.diagRightDownYNorm = PcMappingProfile::ClampNorm(c.centerYNorm + diag);
    }

// shiftCompassDiagonalAnchors
void D3D11Renderer::shiftCompassDiagonalAnchors(PcCompassBinding& c, int dx, int dy) {
        auto shiftOne = [](int& x, int& y, int dx, int dy) {
            if (x < 0 || y < 0) return;
            x = PcMappingProfile::ClampNorm(x + dx);
            y = PcMappingProfile::ClampNorm(y + dy);
            };
        shiftOne(c.diagLeftUpXNorm, c.diagLeftUpYNorm, dx, dy);
        shiftOne(c.diagRightUpXNorm, c.diagRightUpYNorm, dx, dy);
        shiftOne(c.diagLeftDownXNorm, c.diagLeftDownYNorm, dx, dy);
        shiftOne(c.diagRightDownXNorm, c.diagRightDownYNorm, dx, dy);
    }

// clampCompassDiagonalAnchor
void D3D11Renderer::clampCompassDiagonalAnchor(PcCompassBinding& c, int slot) {
        int* px = nullptr;
        int* py = nullptr;
        switch (slot) {
        case 0: px = &c.diagLeftUpXNorm; py = &c.diagLeftUpYNorm; break;
        case 1: px = &c.diagRightUpXNorm; py = &c.diagRightUpYNorm; break;
        case 2: px = &c.diagLeftDownXNorm; py = &c.diagLeftDownYNorm; break;
        case 3: px = &c.diagRightDownXNorm; py = &c.diagRightDownYNorm; break;
        default: return;
        }
        if (!px || !py) return;
        *px = PcMappingProfile::ClampNorm(*px);
        *py = PcMappingProfile::ClampNorm(*py);
        int dx = *px - c.centerXNorm;
        int dy = *py - c.centerYNorm;
        if (dx == 0 && dy == 0) {
            resetCompassDiagonalAnchors(c);
            return;
        }
        // 锚点只负责方向，不允许拖出外圈太多，也不允许贴到中心点。
        const double len = std::sqrt(static_cast<double>(dx) * dx + static_cast<double>(dy) * dy);
        const double minR = (std::max)(8000, c.innerRadiusNorm / 3);
        const double maxR = (std::max)(minR + 1.0, static_cast<double>(c.outerRadiusNorm));
        double targetLen = len;
        if (targetLen < minR) targetLen = minR;
        if (targetLen > maxR) targetLen = maxR;
        const double scale = targetLen / (std::max)(1.0, len);
        *px = PcMappingProfile::ClampNorm(c.centerXNorm + static_cast<int>(std::lround(dx * scale)));
        *py = PcMappingProfile::ClampNorm(c.centerYNorm + static_cast<int>(std::lround(dy * scale)));
    }

// updateCompassDiagonalAnchorFromPoint
bool D3D11Renderer::updateCompassDiagonalAnchorFromPoint(int x, int y) {
        if (compassDiagDragSlot_ < 0 || compassDiagDragSlot_ > 3) return false;
        int nx = 0, ny = 0;
        if (!clientPointToNormLocal(hwnd_, x, y, nx, ny)) return false;
        PcMappingProfile profile = mappingRuntime_.GetProfile();
        if (profile.compasses.empty()) return false;
        auto& c = profile.compasses.front();
        switch (compassDiagDragSlot_) {
        case 0: c.diagLeftUpXNorm = nx; c.diagLeftUpYNorm = ny; break;
        case 1: c.diagRightUpXNorm = nx; c.diagRightUpYNorm = ny; break;
        case 2: c.diagLeftDownXNorm = nx; c.diagLeftDownYNorm = ny; break;
        case 3: c.diagRightDownXNorm = nx; c.diagRightDownYNorm = ny; break;
        default: break;
        }
        clampCompassToBounds(c);
        clampCompassDiagonalAnchor(c, compassDiagDragSlot_);
        compassRuntime_.Reset();
        mappingRuntime_.SetProfile(profile);
        mappingRuntime_.SetEnabled(true);
        mappingOverlayDirty_ = true;
        dirtyWindow_ = true;
        mappingEditorStatus_ = HLW(L"轮盘斜向锚点：拖动调整中");
        refreshMappingToolbarStatus();
        return true;
    }

// compassNormAngleDiff
double D3D11Renderer::compassNormAngleDiff(double a, double b) {
        const double pi = 3.14159265358979323846;
        double d = a - b;
        while (d > pi) d -= 2.0 * pi;
        while (d < -pi) d += 2.0 * pi;
        return d;
    }

// compassFixedSectorCenterAngle
double D3D11Renderer::compassFixedSectorCenterAngle(int index) {
        const double pi = 3.14159265358979323846;
        switch (index) {
        case 0: return -pi / 2.0; // W
        case 1: return pi;        // A
        case 2: return pi / 2.0;  // S
        case 3: return 0.0;       // D
        default: return 0.0;
        }
    }

// compassDiagonalSectorCenterAngle
double D3D11Renderer::compassDiagonalSectorCenterAngle(const PcCompassBinding& c, int index) {
        int ax = -1;
        int ay = -1;
        switch (index) {
        case 0: ax = c.diagLeftUpXNorm; ay = c.diagLeftUpYNorm; break;
        case 1: ax = c.diagRightUpXNorm; ay = c.diagRightUpYNorm; break;
        case 2: ax = c.diagLeftDownXNorm; ay = c.diagLeftDownYNorm; break;
        case 3: ax = c.diagRightDownXNorm; ay = c.diagRightDownYNorm; break;
        default: break;
        }
        if (ax >= 0 && ay >= 0) {
            const int dx = PcMappingProfile::ClampNorm(ax) - c.centerXNorm;
            const int dy = PcMappingProfile::ClampNorm(ay) - c.centerYNorm;
            if (dx != 0 || dy != 0) return std::atan2(static_cast<double>(dy), static_cast<double>(dx));
        }
        const double pi = 3.14159265358979323846;
        switch (index) {
        case 0: return -3.0 * pi / 4.0;
        case 1: return -pi / 4.0;
        case 2: return 3.0 * pi / 4.0;
        case 3: return pi / 4.0;
        default: return -pi / 4.0;
        }
    }

// compassDiagonalSectorPercentPtr
int* D3D11Renderer::compassDiagonalSectorPercentPtr(PcCompassBinding& c, int index) {
        switch (index) {
        case 0: return &c.swayDiagLeftUpSectorPercent;
        case 1: return &c.swayDiagRightUpSectorPercent;
        case 2: return &c.swayDiagLeftDownSectorPercent;
        case 3: return &c.swayDiagRightDownSectorPercent;
        default: return &c.swayDiagonalSectorPercent;
        }
    }

// averageCompassDiagonalSectorPercent
int D3D11Renderer::averageCompassDiagonalSectorPercent(const PcCompassBinding& c) {
        return (c.swayDiagLeftUpSectorPercent + c.swayDiagRightUpSectorPercent + c.swayDiagLeftDownSectorPercent + c.swayDiagRightDownSectorPercent) / 4;
    }

// updateCompassSectorFromPoint
bool D3D11Renderer::updateCompassSectorFromPoint(int x, int y) {
        if (compassSectorDragGroup_ < 0) return false;
        int nx = 0, ny = 0;
        if (!clientPointToNormLocal(hwnd_, x, y, nx, ny)) return false;
        PcMappingProfile profile = mappingRuntime_.GetProfile();
        if (profile.compasses.empty()) return false;
        auto& c = profile.compasses.front();
        const int dx = nx - c.centerXNorm;
        const int dy = ny - c.centerYNorm;
        if (dx == 0 && dy == 0) return true;
        const double pi = 3.14159265358979323846;
        const double fullHalf = 22.5 * pi / 180.0;
        const double angle = std::atan2(static_cast<double>(dy), static_cast<double>(dx));
        const bool diagonal = compassSectorDragGroup_ == 1;
        const int index = (std::max)(0, (std::min)(3, compassSectorDragIndex_));
        const double center = diagonal ? compassDiagonalSectorCenterAngle(c, index) : compassFixedSectorCenterAngle(index);
        const double diff = std::fabs(compassNormAngleDiff(angle, center));
        int percent = static_cast<int>(std::lround((diff / fullHalf) * 100.0));
        if (diagonal) {
            percent = (std::max)(PC_COMPASS_SWAY_DIAG_SECTOR_MIN, (std::min)(PC_COMPASS_SWAY_DIAG_SECTOR_MAX, percent));
            int* ptr = compassDiagonalSectorPercentPtr(c, index);
            if (ptr) *ptr = percent;
            c.swayDiagonalSectorPercent = averageCompassDiagonalSectorPercent(c);
        }
        else {
            percent = (std::max)(PC_COMPASS_SWAY_SECTOR_MIN, (std::min)(PC_COMPASS_SWAY_SECTOR_MAX, percent));
            // WASD 正向扇区目前仍使用一套统一参数；拖任意紫色手柄会同步调整四个正向扇区。
            c.swaySectorPercent = percent;
        }
        compassRuntime_.Reset();
        mappingRuntime_.SetProfile(profile);
        mappingRuntime_.SetEnabled(true);
        mappingOverlayDirty_ = true;
        dirtyWindow_ = true;
        mappingEditorStatus_ = diagonal ? HLW(L"轮盘斜向扇区：单独拖动调整中") : HLW(L"轮盘正向扇区：拖动调整中");
        refreshMappingToolbarStatus();
        return true;
    }

// applyCompassOptionsResult
void D3D11Renderer::applyCompassOptionsResult(const PcCompassOptions& opt) {
        PcMappingProfile profile = mappingRuntime_.GetProfile();
        if (profile.compasses.empty()) return;
        auto& c = profile.compasses.front();
        c.motionMode = opt.motionMode;
        c.centerOuterReversed = opt.centerOuterReversed;
        c.innerRadiusNorm = clampCompassInnerRadiusNorm(opt.innerRadiusNorm);
        c.outerRadiusNorm = clampCompassOuterRadiusNorm(opt.outerRadiusNorm);
        normalizeCompassRadii(c);
        c.speedXStep = (std::max)(PC_COMPASS_SPEED_STEP_MIN, (std::min)(PC_COMPASS_SPEED_STEP_MAX, opt.speedXStep));
        c.speedYStep = (std::max)(PC_COMPASS_SPEED_STEP_MIN, (std::min)(PC_COMPASS_SPEED_STEP_MAX, opt.speedYStep));
        c.swaySectorPercent = (std::max)(PC_COMPASS_SWAY_SECTOR_MIN, (std::min)(PC_COMPASS_SWAY_SECTOR_MAX, opt.swaySectorPercent));
        c.swayDiagonalSectorPercent = (std::max)(PC_COMPASS_SWAY_DIAG_SECTOR_MIN, (std::min)(PC_COMPASS_SWAY_DIAG_SECTOR_MAX, opt.swayDiagonalSectorPercent));
        c.swayDiagLeftUpSectorPercent = c.swayDiagonalSectorPercent;
        c.swayDiagRightUpSectorPercent = c.swayDiagonalSectorPercent;
        c.swayDiagLeftDownSectorPercent = c.swayDiagonalSectorPercent;
        c.swayDiagRightDownSectorPercent = c.swayDiagonalSectorPercent;
        c.swayStepMinPercent = (std::max)(PC_COMPASS_SWAY_STEP_MIN_MIN, (std::min)(PC_COMPASS_SWAY_STEP_MIN_MAX, opt.swayStepMinPercent));
        c.swayStepMaxPercent = (std::max)(PC_COMPASS_SWAY_STEP_MAX_MIN, (std::min)(PC_COMPASS_SWAY_STEP_MAX_MAX, opt.swayStepMaxPercent));
        if (c.swayStepMaxPercent < c.swayStepMinPercent + PC_COMPASS_SWAY_STEP_MIN_GAP) c.swayStepMaxPercent = c.swayStepMinPercent + PC_COMPASS_SWAY_STEP_MIN_GAP;
        c.swaySpeedPercent = (std::max)(PC_COMPASS_SWAY_SPEED_MIN, (std::min)(PC_COMPASS_SWAY_SPEED_MAX, opt.swaySpeedPercent));
        c.swayMouseButtonSmallStep = opt.swayMouseButtonSmallStep;
        c.swayMouseButtonStepPercent = (std::max)(PC_COMPASS_MOUSE_STABLE_SECTOR_MIN, (std::min)(PC_COMPASS_MOUSE_STABLE_SECTOR_MAX, opt.swayMouseButtonStepPercent));
        c.swayMouseButtonUpdatePercent = (std::max)(PC_COMPASS_MOUSE_STABLE_SPEED_MIN, (std::min)(PC_COMPASS_MOUSE_STABLE_SPEED_MAX, opt.swayMouseButtonUpdatePercent));
        c.swayMouseButtonHoldMs = (std::max)(PC_COMPASS_MOUSE_STABLE_HOLD_MS_MIN, (std::min)(PC_COMPASS_MOUSE_STABLE_HOLD_MS_MAX, opt.swayMouseButtonHoldMs));
        clampCompassUiPositionToWindow(c);
        for (int i = 0; i < 4; ++i) clampCompassDiagonalAnchor(c, i);
        compassRuntime_.Reset();
        mappingRuntime_.SetProfile(profile);
        mappingRuntime_.SetEnabled(true);
        mappingOverlayDirty_ = true;
        dirtyWindow_ = true;
        mappingEditorStatus_ = HLW(L"轮盘设置已更新");
        updateStatusText(mappingEditorStatus_);
        refreshMappingToolbarStatus();
    }

// updateCompassPositionFromPoint
bool D3D11Renderer::updateCompassPositionFromPoint(int x, int y) {
        int nx = 0, ny = 0;
        if (!clientPointToNormLocal(hwnd_, x, y, nx, ny)) return false;
        PcMappingProfile profile = mappingRuntime_.GetProfile();
        if (profile.compasses.empty()) return false;
        auto c = compassDrag_ ? compassDragStart_ : profile.compasses.front();
        if (compassDrag_) {
            const int dx = nx - compassDragStartXNorm_;
            const int dy = ny - compassDragStartYNorm_;
            c.centerXNorm = PcMappingProfile::ClampNorm(compassDragStart_.centerXNorm + dx);
            c.centerYNorm = PcMappingProfile::ClampNorm(compassDragStart_.centerYNorm + dy);
            const int beforeX = c.centerXNorm;
            const int beforeY = c.centerYNorm;
            clampCompassUiPositionToWindow(c);
            shiftCompassDiagonalAnchors(c, c.centerXNorm - compassDragStart_.centerXNorm, c.centerYNorm - compassDragStart_.centerYNorm);
            (void)beforeX; (void)beforeY;
        }
        else {
            c.centerXNorm = nx;
            c.centerYNorm = ny;
            clampCompassUiPositionToWindow(c);
            resetCompassDiagonalAnchors(c);
        }
        profile.compasses.front() = c;
        compassRuntime_.Reset();
        mappingRuntime_.SetProfile(profile);
        mappingRuntime_.SetEnabled(true);
        mappingOverlayDirty_ = true;
        dirtyWindow_ = true;
        mappingEditorStatus_ = HLW(L"轮盘坐标：拖动更新中");
        refreshMappingToolbarStatus();
        return true;
    }
