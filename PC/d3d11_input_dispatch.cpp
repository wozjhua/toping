#include "d3d11_renderer.h"

// WndProc
LRESULT CALLBACK D3D11Renderer::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        auto* self = reinterpret_cast<D3D11Renderer*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
        if (msg == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCT*>(lp);
            auto* p = reinterpret_cast<D3D11Renderer*>(cs->lpCreateParams);
            SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)p);
            return DefWindowProc(hwnd, msg, wp, lp);
        }
        if (!self) return DefWindowProc(hwnd, msg, wp, lp);
        return self->handleMessage(msg, wp, lp);
    }

// handleMessage
LRESULT D3D11Renderer::handleMessage(UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
        case WM_CREATE:
            disableImeForMirrorWindow();
            return 0;

        case WM_SETFOCUS:
        case WM_INPUTLANGCHANGE:
            disableImeForMirrorWindow();
            break;

        case WM_IME_SETCONTEXT:
        case WM_IME_STARTCOMPOSITION:
        case WM_IME_COMPOSITION:
        case WM_IME_ENDCOMPOSITION:
        case WM_IME_CHAR:
            // 投屏窗口禁用 IME 后，残留的 IME 消息也直接吞掉，避免组合窗口/候选窗抢焦点。
            return 0;

        case WM_PC_NORMAL_KEY_OPTIONS_DONE: {
            const size_t index = static_cast<size_t>(wp);
            auto* opt = reinterpret_cast<PcNormalKeyOptions*>(lp);
            if (opt) {
                applyNormalKeyOptionsResult(index, *opt);
                delete opt;
            }
            else {
                mappingEditorStatus_ = HLW(L"普通按键选项已取消");
                updateStatusText(mappingEditorStatus_);
                refreshMappingToolbarStatus();
            }
            return 0;
        }
        case WM_PC_LOCK_OPTIONS_DONE: {
            auto* opt = reinterpret_cast<PcLockOptions*>(lp);
            if (opt) {
                applyLockOptionsResult(*opt);
                delete opt;
            }
            else {
                mappingEditorStatus_ = HLW(L"视角设置已取消");
                updateStatusText(mappingEditorStatus_);
                refreshMappingToolbarStatus();
            }
            return 0;
        }
        case WM_PC_MENU_OPTIONS_DONE: {
            const size_t index = static_cast<size_t>(wp);
            auto* opt = reinterpret_cast<PcMenuOptions*>(lp);
            if (opt) {
                applyMenuOptionsResult(index, *opt);
                delete opt;
            }
            else {
                mappingEditorStatus_ = HLW(L"菜单设置已取消");
                updateStatusText(mappingEditorStatus_);
                refreshMappingToolbarStatus();
            }
            return 0;
        }
        case WM_PC_COMPASS_OPTIONS_DONE: {
            auto* opt = reinterpret_cast<PcCompassOptions*>(lp);
            if (opt) {
                applyCompassOptionsResult(*opt);
                delete opt;
            }
            else {
                mappingEditorStatus_ = HLW(L"轮盘设置已取消");
                updateStatusText(mappingEditorStatus_);
                refreshMappingToolbarStatus();
            }
            return 0;
        }
        case WM_INPUT: {
            // 菜单已经激活时，RawInput 直接走菜单轻量入口：只读包、累计 dx/dy、按 2ms 合并。
            // 菜单的按键/右键关闭仍走普通消息分支，不延迟状态切换。
            if (menuRuntime_.IsActive()) {
                markMenuDebugRawMessage();
                ++menuDebugMenuEntry_;
                if (menuRuntime_.ConsumeActiveRawMouse(hwnd_, lp)) {
                    ++menuDebugMenuAccepted_;
                }
                maybeRefreshMenuDebugOverlay();
                return 0;
            }

            markLockDebugRawMessage();

            // 视角锁定且菜单未激活时，RawInput 直接走轻量读包入口。
            // 这个入口只读取鼠标包、累计 dx/dy、按 2ms 合并，不再进入通用 HandleWindowMessage 分支。
            if (lockRuntime_.IsLocked()) {
                ++lockDebugLockChecks_;
                if (lockRuntime_.ConsumeLockedRawMouse(hwnd_, lp)) {
                    ++lockDebugLockHandled_;
                    maybeRefreshLockDebugOverlay();
                    return 0;
                }
            }

            // 非锁定状态或非鼠标 RawInput，保留原来的完整分发链路。
            LRESULT menuResult = 0;
            ++lockDebugMenuChecks_;
            if (menuRuntime_.HandleWindowMessage(hwnd_, msg, wp, lp, &menuResult)) {
                ++lockDebugMenuHandled_;
                maybeRefreshLockDebugOverlay();
                return menuResult;
            }

            LRESULT lockResult = 0;
            ++lockDebugLockChecks_;
            if (lockRuntime_.HandleWindowMessage(hwnd_, msg, wp, lp, &lockResult)) {
                ++lockDebugLockHandled_;
                maybeRefreshLockDebugOverlay();
                return lockResult;
            }

            LRESULT compassResult = 0;
            ++lockDebugCompassChecks_;
            compassRuntime_.HandleWindowMessage(hwnd_, msg, wp, lp, &compassResult);
            LRESULT inputResult = 0;
            ++lockDebugPcUinputChecks_;
            pcUinput_.HandleWindowMessage(hwnd_, msg, wp, lp, &inputResult);
            maybeRefreshLockDebugOverlay();
            return 0;
        }
        case WM_SIZE:
            recreateRTV();
            dirtyWindow_ = true;
            mappingOverlayDirty_ = true;
            pcUinput_.HandleWindowMessage(hwnd_, msg, wp, lp, nullptr);
            mappingToolbar_.UpdateOwnerPosition();
            return 0;

        case WM_MOVE:
            pcUinput_.HandleWindowMessage(hwnd_, msg, wp, lp, nullptr);
            mappingToolbar_.UpdateOwnerPosition();
            return 0;

        case WM_SYSKEYDOWN:
        case WM_SYSKEYUP: {
            LRESULT lockMenuResult = 0;
            if (handleLockToggleWhileMenuFree(msg, wp, lp, &lockMenuResult)) {
                return lockMenuResult;
            }

            LRESULT menuResult = 0;
            if (menuRuntime_.HandleWindowMessage(hwnd_, msg, wp, lp, &menuResult)) {
                return menuResult;
            }

            LRESULT compassResult = 0;
            if (compassRuntime_.HandleWindowMessage(hwnd_, msg, wp, lp, &compassResult)) {
                return compassResult;
            }

            LRESULT macroResult = 0;
            if (macroRuntime_.HandleWindowMessage(hwnd_, msg, wp, lp, &macroResult)) {
                return macroResult;
            }

            LRESULT mappingResult = 0;
            if (mappingRuntime_.HandleWindowMessage(hwnd_, msg, wp, lp, &mappingResult)) {
                return mappingResult;
            }

            LRESULT lockResult = 0;
            if (lockRuntime_.HandleWindowMessage(hwnd_, msg, wp, lp, &lockResult)) {
                return lockResult;
            }

            LRESULT inputResult = 0;
            if (pcUinput_.HandleWindowMessage(hwnd_, msg, wp, lp, &inputResult)) {
                return inputResult;
            }
            // Windows 默认会把 Alt/F10 当成系统菜单键。
            // 进入系统菜单/菜单循环后，D3D Present 和消息泵会看起来像被暂停。
            // 这里吞掉 Alt 自身，只保留我们自己的快捷键处理。
            if (wp == VK_MENU || wp == VK_LMENU || wp == VK_RMENU) {
                return 0;
            }
            break;
        }

        case WM_SYSCHAR:
            // 防止 Alt+按键生成系统字符后继续触发系统菜单蜂鸣/菜单逻辑。
            return 0;

        case WM_SYSCOMMAND:
            // SC_KEYMENU 是 Alt/F10 触发系统菜单的入口，必须拦掉。
            if ((wp & 0xFFF0) == SC_KEYMENU) {
                return 0;
            }
            break;

        case WM_KEYDOWN: {
            const UINT key = static_cast<UINT>(wp);
            const bool repeat = (lp & (1LL << 30)) != 0;

            if (!repeat && (key == L'0' || key == VK_NUMPAD0)) {
                togglePerfLogRecording();
                return 0;
            }
            if (key == settings_.settingsKey) {
                showSettingsWindow();
                return 0;
            }
            if (key == settings_.exitKey) {
                PostQuitMessage(0);
                return 0;
            }
            if (key == settings_.fullscreenKey1 || key == settings_.fullscreenKey2) {
                toggleFullscreen();
                return 0;
            }
            if (key == settings_.hudKey) {
                hudVisible_ = !hudVisible_;
                dirtyWindow_ = true;
                return 0;
            }
            if (key == settings_.splitDebugKey) {
                splitDebugOverlayVisible_ = !splitDebugOverlayVisible_;
                hudDirty_ = true;
                dirtyWindow_ = true;
                return 0;
            }

            LRESULT lockMenuResult = 0;
            if (handleLockToggleWhileMenuFree(msg, wp, lp, &lockMenuResult)) {
                return lockMenuResult;
            }

            LRESULT menuResult = 0;
            if (menuRuntime_.HandleWindowMessage(hwnd_, msg, wp, lp, &menuResult)) {
                return menuResult;
            }

            LRESULT compassResult = 0;
            if (compassRuntime_.HandleWindowMessage(hwnd_, msg, wp, lp, &compassResult)) {
                return compassResult;
            }

            LRESULT macroResult = 0;
            if (macroRuntime_.HandleWindowMessage(hwnd_, msg, wp, lp, &macroResult)) {
                return macroResult;
            }

            LRESULT mappingResult = 0;
            if (mappingRuntime_.HandleWindowMessage(hwnd_, msg, wp, lp, &mappingResult)) {
                return mappingResult;
            }

            LRESULT lockResult = 0;
            if (lockRuntime_.HandleWindowMessage(hwnd_, msg, wp, lp, &lockResult)) {
                return lockResult;
            }

            LRESULT inputResult = 0;
            if (pcUinput_.HandleWindowMessage(hwnd_, msg, wp, lp, &inputResult)) {
                return inputResult;
            }

            break;
        }

        case WM_MOUSEMOVE:
        case WM_LBUTTONDOWN:
            if (handleMappingOverlayEditMessage(msg, wp, lp)) {
                return 0;
            }
            if (handleLockEditMessage(msg, wp, lp)) {
                return 0;
            }
            if (msg == WM_LBUTTONDOWN && mappingEditMode_ && handleMappingCreateClick(lp)) {
                return 0;
            }
            // fall through to normal mouse handling
        case WM_LBUTTONUP:
            if (handleMappingOverlayEditMessage(msg, wp, lp)) {
                return 0;
            }
            if (handleLockEditMessage(msg, wp, lp)) {
                return 0;
            }
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:
        case WM_XBUTTONDOWN:
        case WM_XBUTTONUP:
        case WM_KEYUP:
        case WM_MOUSEWHEEL:
        case WM_KILLFOCUS: {
            handleMappingOverlayEditMessage(msg, wp, lp);
            handleLockEditMessage(msg, wp, lp);
            LRESULT lockMenuResult = 0;
            if (handleLockToggleWhileMenuFree(msg, wp, lp, &lockMenuResult)) {
                return lockMenuResult;
            }

            LRESULT menuResult = 0;
            if (menuRuntime_.HandleWindowMessage(hwnd_, msg, wp, lp, &menuResult)) {
                return menuResult;
            }

            LRESULT compassResult = 0;
            if (compassRuntime_.HandleWindowMessage(hwnd_, msg, wp, lp, &compassResult)) {
                return compassResult;
            }

            LRESULT macroResult = 0;
            if (macroRuntime_.HandleWindowMessage(hwnd_, msg, wp, lp, &macroResult)) {
                return macroResult;
            }

            LRESULT mappingResult = 0;
            if (mappingRuntime_.HandleWindowMessage(hwnd_, msg, wp, lp, &mappingResult)) {
                return mappingResult;
            }

            LRESULT lockResult = 0;
            if (lockRuntime_.HandleWindowMessage(hwnd_, msg, wp, lp, &lockResult)) {
                return lockResult;
            }

            LRESULT inputResult = 0;
            if (pcUinput_.HandleWindowMessage(hwnd_, msg, wp, lp, &inputResult)) {
                return inputResult;
            }
            break;
        }

        case WM_DESTROY:
            mappingToolbar_.Stop();
            lockRuntime_.Stop();
            compassRuntime_.Reset();
            menuRuntime_.Reset();
            macroRuntime_.Reset();
            mappingRuntime_.Reset();
            runtimeTouchSlots_.fill(false);
            pcUinput_.Stop();
            PostQuitMessage(0);
            return 0;
        default:
            break;
        }
        return DefWindowProc(hwnd_, msg, wp, lp);
    }

