#include "d3d11_renderer.h"

// handleLockToggleWhileMenuFree
bool D3D11Renderer::handleLockToggleWhileMenuFree(UINT msg, WPARAM wp, LPARAM lp, LRESULT* outResult) {
        if (!menuRuntime_.IsSecondaryFreeActive()) return false;
        const bool keyMsg =
            msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN ||
            msg == WM_KEYUP || msg == WM_SYSKEYUP;
        if (!keyMsg) return false;

        LRESULT lockResult = 0;
        const bool handled = lockRuntime_.HandleWindowMessage(hwnd_, msg, wp, lp, &lockResult);
        if (handled && (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN)) {
            // 老版 LOCK_TOGGLE 语义：道具/背包二阶段自由鼠标中，视角触发键可以直接退出菜单并回到锁定。
            // 这里不点击关闭 UI；右键关闭才执行关闭圆点点击。
            menuRuntime_.Reset();
            if (outResult) *outResult = lockResult;
            return true;
        }
        return false;
    }

// currentLockModeInt
int D3D11Renderer::currentLockModeInt() const {
        const auto& p = mappingRuntime_.GetProfile();
        if (p.locks.empty()) return 0;
        return static_cast<int>(p.locks.front().mode);
    }

// setCurrentLockModeInt
void D3D11Renderer::setCurrentLockModeInt(int mode) {
        PcMappingProfile profile = mappingRuntime_.GetProfile();
        if (profile.locks.empty()) {
            mappingEditorStatus_ = HLW(L"视角模式：请先创建视角");
            updateStatusText(mappingEditorStatus_);
            refreshMappingToolbarStatus();
            return;
        }
        if (mode < 1 || mode > 3) return;
        profile.locks.front().mode = static_cast<PcLockSlideTouchMode>(mode);
        lockRuntime_.Reset();
        mappingRuntime_.SetProfile(profile);
        mappingRuntime_.SetEnabled(true);
        SetPcMappingOverlayMacroSource(&pcMacros_, selectedMacroIndex_, selectedMacroStepId_, macroStepEditorVisible_);
        mappingOverlayDirty_ = true;
        dirtyWindow_ = true;
        mappingEditorStatus_ = HLW(L"视角模式已切换：");
        if (mode == 1) mappingEditorStatus_ += HLW(L"1 单slot");
        else if (mode == 2) mappingEditorStatus_ += HLW(L"2 双slot同时");
        else mappingEditorStatus_ += HLW(L"3 双slot顺序");
        updateStatusText(mappingEditorStatus_);
        refreshMappingToolbarStatus();
    }

// hitTestLockEditPoint
D3D11Renderer::LockEditDrag D3D11Renderer::hitTestLockEditPoint(int x, int y) const {
        if (!mappingEditMode_ || lockRuntime_.IsLocked() || mappingCreateMode_ != MappingCreateMode::None) return LockEditDrag::None;
        const auto& p = mappingRuntime_.GetProfile();
        if (p.locks.empty() || !IsWindow(hwnd_)) return LockEditDrag::None;
        RECT rc{};
        if (!GetClientRect(hwnd_, &rc)) return LockEditDrag::None;
        const int w = (std::max)(1L, rc.right - rc.left);
        const int h = (std::max)(1L, rc.bottom - rc.top);
        const auto& l = p.locks.front();
        int left = normToClientCoord(l.leftNorm, w);
        int right = normToClientCoord(l.rightNorm, w);
        int top = normToClientCoord(l.topNorm, h);
        int bottom = normToClientCoord(l.bottomNorm, h);
        if (left > right) std::swap(left, right);
        if (top > bottom) std::swap(top, bottom);
        const int tol = 14;
        const bool nearL = std::abs(x - left) <= tol;
        const bool nearR = std::abs(x - right) <= tol;
        const bool nearT = std::abs(y - top) <= tol;
        const bool nearB = std::abs(y - bottom) <= tol;
        const bool inX = x >= left - tol && x <= right + tol;
        const bool inY = y >= top - tol && y <= bottom + tol;
        if (nearL && nearT) return LockEditDrag::TopLeft;
        if (nearR && nearT) return LockEditDrag::TopRight;
        if (nearL && nearB) return LockEditDrag::BottomLeft;
        if (nearR && nearB) return LockEditDrag::BottomRight;
        if (nearL && inY) return LockEditDrag::Left;
        if (nearR && inY) return LockEditDrag::Right;
        if (nearT && inX) return LockEditDrag::Top;
        if (nearB && inX) return LockEditDrag::Bottom;

        const int cx = normToClientCoord(l.centerXNorm, w);
        const int cy = normToClientCoord(l.centerYNorm, h);
        const int dx = x - cx;
        const int dy = y - cy;
        if (dx * dx + dy * dy <= 34 * 34) return LockEditDrag::Move;
        return LockEditDrag::None;
    }

// setLockRectSymmetric
void D3D11Renderer::setLockRectSymmetric(PcLockBinding& l, int halfW, int halfH) {
        static constexpr int kMinHalfW = 45000 / 2;
        static constexpr int kMinHalfH = 45000 / 2;

        l.centerXNorm = PcMappingProfile::ClampNorm(l.centerXNorm);
        l.centerYNorm = PcMappingProfile::ClampNorm(l.centerYNorm);

        const int maxHalfW = (std::max)(0, (std::min)(l.centerXNorm, 1000000 - l.centerXNorm));
        const int maxHalfH = (std::max)(0, (std::min)(l.centerYNorm, 1000000 - l.centerYNorm));

        halfW = std::abs(halfW);
        halfH = std::abs(halfH);
        halfW = (std::min)((std::max)(halfW, (std::min)(kMinHalfW, maxHalfW)), maxHalfW);
        halfH = (std::min)((std::max)(halfH, (std::min)(kMinHalfH, maxHalfH)), maxHalfH);

        l.leftNorm = PcMappingProfile::ClampNorm(l.centerXNorm - halfW);
        l.rightNorm = PcMappingProfile::ClampNorm(l.centerXNorm + halfW);
        l.topNorm = PcMappingProfile::ClampNorm(l.centerYNorm - halfH);
        l.bottomNorm = PcMappingProfile::ClampNorm(l.centerYNorm + halfH);
    }

// clampLockRectAndCenter
void D3D11Renderer::clampLockRectAndCenter(PcLockBinding& l) {
        // 视角点必须始终是范围中心。范围不再单边偏移，也不再把中心点 clamp 到某一侧。
        l.centerXNorm = PcMappingProfile::ClampNorm(l.centerXNorm);
        l.centerYNorm = PcMappingProfile::ClampNorm(l.centerYNorm);

        l.leftNorm = PcMappingProfile::ClampNorm(l.leftNorm);
        l.rightNorm = PcMappingProfile::ClampNorm(l.rightNorm);
        l.topNorm = PcMappingProfile::ClampNorm(l.topNorm);
        l.bottomNorm = PcMappingProfile::ClampNorm(l.bottomNorm);
        if (l.leftNorm > l.rightNorm) std::swap(l.leftNorm, l.rightNorm);
        if (l.topNorm > l.bottomNorm) std::swap(l.topNorm, l.bottomNorm);

        const int halfW = (std::max)(std::abs(l.centerXNorm - l.leftNorm), std::abs(l.rightNorm - l.centerXNorm));
        const int halfH = (std::max)(std::abs(l.centerYNorm - l.topNorm), std::abs(l.bottomNorm - l.centerYNorm));
        setLockRectSymmetric(l, halfW, halfH);
    }

// lockHalfW
int D3D11Renderer::lockHalfW(const PcLockBinding& l) {
        return (std::max)(std::abs(l.centerXNorm - l.leftNorm), std::abs(l.rightNorm - l.centerXNorm));
    }

// lockHalfH
int D3D11Renderer::lockHalfH(const PcLockBinding& l) {
        return (std::max)(std::abs(l.centerYNorm - l.topNorm), std::abs(l.bottomNorm - l.centerYNorm));
    }

// shiftLockBinding
void D3D11Renderer::shiftLockBinding(PcLockBinding& l, int dx, int dy) {
        const int width = l.rightNorm - l.leftNorm;
        const int height = l.bottomNorm - l.topNorm;
        int nl = l.leftNorm + dx;
        int nt = l.topNorm + dy;
        nl = (std::max)(0, (std::min)(1000000 - width, nl));
        nt = (std::max)(0, (std::min)(1000000 - height, nt));
        const int realDx = nl - l.leftNorm;
        const int realDy = nt - l.topNorm;
        l.leftNorm = nl;
        l.rightNorm = nl + width;
        l.topNorm = nt;
        l.bottomNorm = nt + height;
        l.centerXNorm = PcMappingProfile::ClampNorm(l.centerXNorm + realDx);
        l.centerYNorm = PcMappingProfile::ClampNorm(l.centerYNorm + realDy);
        clampLockRectAndCenter(l);
    }

// updateLockEditDrag
bool D3D11Renderer::updateLockEditDrag(int nx, int ny) {
        if (lockEditDrag_ == LockEditDrag::None) return false;
        PcMappingProfile profile = mappingRuntime_.GetProfile();
        if (profile.locks.empty()) return true;
        PcLockBinding l = lockEditStart_;
        const int dx = nx - lockEditStartXNorm_;
        const int dy = ny - lockEditStartYNorm_;
        int halfW = lockHalfW(lockEditStart_);
        int halfH = lockHalfH(lockEditStart_);
        switch (lockEditDrag_) {
        case LockEditDrag::Move:
            shiftLockBinding(l, dx, dy);
            break;
        case LockEditDrag::Left:
            halfW = lockEditStart_.centerXNorm - (lockEditStart_.leftNorm + dx);
            setLockRectSymmetric(l, halfW, halfH);
            break;
        case LockEditDrag::Right:
            halfW = (lockEditStart_.rightNorm + dx) - lockEditStart_.centerXNorm;
            setLockRectSymmetric(l, halfW, halfH);
            break;
        case LockEditDrag::Top:
            halfH = lockEditStart_.centerYNorm - (lockEditStart_.topNorm + dy);
            setLockRectSymmetric(l, halfW, halfH);
            break;
        case LockEditDrag::Bottom:
            halfH = (lockEditStart_.bottomNorm + dy) - lockEditStart_.centerYNorm;
            setLockRectSymmetric(l, halfW, halfH);
            break;
        case LockEditDrag::TopLeft:
            halfW = lockEditStart_.centerXNorm - (lockEditStart_.leftNorm + dx);
            halfH = lockEditStart_.centerYNorm - (lockEditStart_.topNorm + dy);
            setLockRectSymmetric(l, halfW, halfH);
            break;
        case LockEditDrag::TopRight:
            halfW = (lockEditStart_.rightNorm + dx) - lockEditStart_.centerXNorm;
            halfH = lockEditStart_.centerYNorm - (lockEditStart_.topNorm + dy);
            setLockRectSymmetric(l, halfW, halfH);
            break;
        case LockEditDrag::BottomLeft:
            halfW = lockEditStart_.centerXNorm - (lockEditStart_.leftNorm + dx);
            halfH = (lockEditStart_.bottomNorm + dy) - lockEditStart_.centerYNorm;
            setLockRectSymmetric(l, halfW, halfH);
            break;
        case LockEditDrag::BottomRight:
            halfW = (lockEditStart_.rightNorm + dx) - lockEditStart_.centerXNorm;
            halfH = (lockEditStart_.bottomNorm + dy) - lockEditStart_.centerYNorm;
            setLockRectSymmetric(l, halfW, halfH);
            break;
        default:
            break;
        }
        if (lockEditDrag_ != LockEditDrag::Move) clampLockRectAndCenter(l);
        profile.locks.front() = l;
        mappingRuntime_.SetProfile(profile);
        mappingRuntime_.SetEnabled(true);
        mappingOverlayDirty_ = true;
        dirtyWindow_ = true;
        mappingEditorStatus_ = HLW(L"视角边界：拖动调整中");
        refreshMappingToolbarStatus();
        return true;
    }

// handleLockEditMessage
bool D3D11Renderer::handleLockEditMessage(UINT msg, WPARAM, LPARAM lp) {
        if (!mappingEditMode_) return false;
        if (mappingCreateMode_ != MappingCreateMode::None) return false;
        if (lockRuntime_.IsLocked()) return false;
        if (!mappingRuntime_.IsEnabled() || mappingRuntime_.GetProfile().locks.empty()) return false;

        switch (msg) {
        case WM_LBUTTONDOWN: {
            const int x = GET_X_LPARAM(lp);
            const int y = GET_Y_LPARAM(lp);
            LockEditDrag hit = hitTestLockEditPoint(x, y);
            if (hit == LockEditDrag::None) return false;
            int nx = 0, ny = 0;
            if (!clientPointToNormLocal(hwnd_, x, y, nx, ny)) return false;
            lockEditDrag_ = hit;
            lockEditStartXNorm_ = nx;
            lockEditStartYNorm_ = ny;
            lockEditStart_ = mappingRuntime_.GetProfile().locks.front();
            SetCapture(hwnd_);
            mappingEditorStatus_ = (hit == LockEditDrag::Move) ? HLW(L"视角边界：拖动中心移动范围") : HLW(L"视角边界：拖动边框调整大小");
            updateStatusText(mappingEditorStatus_);
            return true;
        }
        case WM_MOUSEMOVE:
            if (lockEditDrag_ != LockEditDrag::None) {
                int nx = 0, ny = 0;
                clientPointToNormLocal(hwnd_, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), nx, ny);
                return updateLockEditDrag(nx, ny);
            }
            return false;
        case WM_LBUTTONUP:
            if (lockEditDrag_ != LockEditDrag::None) {
                int nx = 0, ny = 0;
                clientPointToNormLocal(hwnd_, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), nx, ny);
                updateLockEditDrag(nx, ny);
                lockEditDrag_ = LockEditDrag::None;
                if (GetCapture() == hwnd_) ReleaseCapture();
                mappingEditorStatus_ = HLW(L"视角边界已更新");
                updateStatusText(mappingEditorStatus_);
                return true;
            }
            return false;
        case WM_CANCELMODE:
        case WM_KILLFOCUS:
            if (lockEditDrag_ != LockEditDrag::None) {
                lockEditDrag_ = LockEditDrag::None;
                if (GetCapture() == hwnd_) ReleaseCapture();
            }
            return false;
        default:
            return false;
        }
    }

// lockDebugModeText
const wchar_t* D3D11Renderer::lockDebugModeText(int mode) {
        switch (mode) {
        case 1: return HLW(L"1单");
        case 2: return HLW(L"2双同");
        case 3: return HLW(L"3双顺");
        default: return HLW(L"未建");
        }
    }

// resetLockDebugDispatchCounters
void D3D11Renderer::resetLockDebugDispatchCounters() {
        lockDebugRawMessages_ = 0;
        lockDebugMenuChecks_ = 0;
        lockDebugMenuHandled_ = 0;
        lockDebugMenuActiveDrops_ = 0;
        lockDebugLockChecks_ = 0;
        lockDebugLockHandled_ = 0;
        lockDebugCompassChecks_ = 0;
        lockDebugPcUinputChecks_ = 0;
        lockDebugWindowActive_ = false;
    }

// markLockDebugRawMessage
void D3D11Renderer::markLockDebugRawMessage() {
        const auto now = std::chrono::steady_clock::now();
        if (!lockDebugWindowActive_) {
            lockDebugWindowActive_ = true;
            lockDebugWindowStart_ = now;
        }
        lockDebugLastInput_ = now;
        ++lockDebugRawMessages_;
    }

// resetLockDebugTotalCounters
void D3D11Renderer::resetLockDebugTotalCounters() {
        lockDebugTotalsStart_ = std::chrono::steady_clock::now();
        lockDebugTotalActiveMs_ = 0;
        lockDebugTotalRawMessages_ = 0;
        lockDebugTotalRawMouse_ = 0;
        lockDebugTotalApplyCalls_ = 0;
        lockDebugTotalCoalesceFlushes_ = 0;
        lockDebugTotalCoalesceForcedFlushes_ = 0;
        lockDebugTotalMoveAttempts_ = 0;
        lockDebugTotalTouchMoves_ = 0;
        lockDebugTotalReanchors_ = 0;
        lockDebugTotalTouchDowns_ = 0;
        lockDebugTotalTouchUps_ = 0;
        lockDebugTotalRawDx_ = 0;
        lockDebugTotalRawDy_ = 0;
    }

// resetLockDebugAllCounters
void D3D11Renderer::resetLockDebugAllCounters() {
        resetLockDebugDispatchCounters();
        resetLockDebugTotalCounters();
    }

// maybeRefreshLockDebugOverlay
void D3D11Renderer::maybeRefreshLockDebugOverlay(bool force) {
        (void)force;

        // 不再输出“视角诊断”长文本，也不再用 RawInput 诊断刷新覆盖映射状态栏。
        // 保留轻量尾包刷新和统计清零，避免 2ms 合并的最后一小段位移滞留。
        if (lockDebugWindowActive_ || lockDebugRawMessages_ != 0) {
            lockRuntime_.FlushPendingRawDelta(true);
            lockRuntime_.SnapshotAndResetDebugStats();
            resetLockDebugDispatchCounters();
        }
        lastLockDebugOverlayUpdate_ = std::chrono::steady_clock::now();
    }

// chooseLockModeDialog
PcLockSlideTouchMode D3D11Renderer::chooseLockModeDialog(HWND hwnd) {
        int rc = MessageBoxW(
            hwnd,
            HLW(L"选择视角模式：\n\n是：3 双slot顺序（默认，和平类）\n否：2 双slot同时（州类）\n取消：1 单slot重锚（其他）"),
            HLW(L"视角模式"),
            MB_YESNOCANCEL | MB_ICONQUESTION
        );
        if (rc == IDNO) return PcLockSlideTouchMode::DualSimultaneous;
        if (rc == IDCANCEL) return PcLockSlideTouchMode::SingleReanchor;
        return PcLockSlideTouchMode::DualSequential;
    }

// lockMouseButtonCodeToVk
int D3D11Renderer::lockMouseButtonCodeToVk(int mouseButtonCode) {
        // PcCapturedKeyCombo: 1=左键，2=右键，3=中键，4=侧键1，5=侧键2。
        // 视角锁定禁止绑定左键，其他鼠标键转换为 Windows VK 存入 comboTriggerCodes，
        // 这样可以继续复用现有 HLMAP 保存格式和 ComboSatisfied 逻辑。
        switch (mouseButtonCode) {
        case 2: return VK_RBUTTON;
        case 3: return VK_MBUTTON;
        case 4: return VK_XBUTTON1;
        case 5: return VK_XBUTTON2;
        default: return 0;
        }
    }

// makeLockTriggerCodesFromCapture
bool D3D11Renderer::makeLockTriggerCodesFromCapture(const PcCapturedKeyCombo& combo, std::vector<int>& outCodes, std::wstring& outError) {
        outCodes = combo.vkCodes;
        if (combo.mouseButtonCode == 1) {
            outError = HLW(L"视角热键不能绑定鼠标左键");
            return false;
        }
        if (combo.mouseButtonCode > 0) {
            const int mouseVk = lockMouseButtonCodeToVk(combo.mouseButtonCode);
            if (mouseVk <= 0) {
                outError = HLW(L"视角热键不支持这个鼠标键");
                return false;
            }
            if (std::find(outCodes.begin(), outCodes.end(), mouseVk) == outCodes.end()) {
                outCodes.push_back(mouseVk);
            }
        }
        if (outCodes.empty()) {
            outError = HLW(L"视角热键不能为空");
            return false;
        }
        return true;
    }

// editLockOptions
bool D3D11Renderer::editLockOptions() {
        PcMappingProfile profile = mappingRuntime_.GetProfile();
        if (profile.locks.empty()) {
            mappingEditorStatus_ = HLW(L"视角设置：请先创建视角");
            updateStatusText(mappingEditorStatus_);
            refreshMappingToolbarStatus();
            return true;
        }
        const auto& l = profile.locks.front();
        PcLockOptions opt;
        opt.mode = l.mode;
        opt.speedXNorm = l.speedXNorm;
        opt.speedYNorm = l.speedYNorm;
        opt.rebuildDownDelayMs = l.rebuildDownDelayMs;
        mappingEditorStatus_ = HLW(L"视角设置：独立窗口已打开");
        updateStatusText(mappingEditorStatus_);
        refreshMappingToolbarStatus();
        if (!OpenLockOptionsDialog(inst_, hwnd_, opt, 0)) {
            mappingEditorStatus_ = HLW(L"视角设置打开失败");
            updateStatusText(mappingEditorStatus_);
            refreshMappingToolbarStatus();
        }
        return true;
    }

// applyLockOptionsResult
void D3D11Renderer::applyLockOptionsResult(const PcLockOptions& opt) {
        PcMappingProfile profile = mappingRuntime_.GetProfile();
        if (profile.locks.empty()) return;
        auto& l = profile.locks.front();
        l.mode = opt.mode;
        l.speedXNorm = (std::max)(0.1, (std::min)(1.2, opt.speedXNorm));
        l.speedYNorm = (std::max)(0.1, (std::min)(1.2, opt.speedYNorm));
        l.rebuildDownDelayMs = (std::max)(0, (std::min)(300, opt.rebuildDownDelayMs));
        lockRuntime_.Reset();
        mappingRuntime_.SetProfile(profile);
        mappingRuntime_.SetEnabled(true);
        mappingOverlayDirty_ = true;
        dirtyWindow_ = true;
        mappingEditorStatus_ = HLW(L"视角设置已更新");
        updateStatusText(mappingEditorStatus_);
        refreshMappingToolbarStatus();
    }

// rebindLockBinding
bool D3D11Renderer::rebindLockBinding() {
        PcMappingProfile profile = mappingRuntime_.GetProfile();
        if (profile.locks.empty()) return false;
        mappingEditorStatus_ = HLW(L"重绑视角：请按新的锁定热键");
        updateStatusText(mappingEditorStatus_);
        PcCapturedKeyCombo combo;
        std::vector<int> lockCodes;
        std::wstring lockError;
        if (!CaptureKeyComboDialog(inst_, hwnd_, combo) || !makeLockTriggerCodesFromCapture(combo, lockCodes, lockError)) {
            mappingEditorStatus_ = lockError.empty() ? HLW(L"重绑视角已取消") : (HLW(L"重绑视角已取消：") + lockError);
            updateStatusText(mappingEditorStatus_);
            refreshMappingToolbarStatus();
            return true;
        }
        profile.locks.front().comboTriggerCodes = lockCodes;
        profile.locks.front().triggerLabel = combo.label;
        lockRuntime_.Reset();
        mappingRuntime_.SetProfile(profile);
        mappingRuntime_.SetEnabled(true);
        mappingOverlayDirty_ = true;
        dirtyWindow_ = true;
        mappingEditorStatus_ = HLW(L"视角已重绑：") + combo.label;
        updateStatusText(mappingEditorStatus_);
        refreshMappingToolbarStatus();
        return true;
    }
