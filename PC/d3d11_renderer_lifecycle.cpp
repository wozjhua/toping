#include "d3d11_renderer.h"
#include "audio_receiver.h"
#include "video_stream_tuning.h"

D3D11Renderer::RuntimeSettings::RuntimeSettings()
    : hudKey(VK_F3),
      fullscreenKey1(VK_RETURN),
      fullscreenKey2(VK_F11),
      settingsKey(VK_F2),
      splitDebugKey(VK_F4),
      exitKey(VK_ESCAPE),
      hudRefreshMs(200),
      audioVolumePercent(DEFAULT_AUDIO_VOLUME_PERCENT),
      jpegSubsamplingMode(420),
      jpegQuality(100),
      keepFrameRate(true),
      targetFps(144),
      fullscreenPreset(2),
      fullscreenSplitParts(g_initialFullscreenSplitParts),
      cropSize(640),
      cropSplitParts(2),
      captureMode(1),
      videoPreset(VideoStreamTuning::kVideoPresetDefault),
      videoWidth(VideoStreamTuning::kVideoWidthDefault),
      videoHeight(VideoStreamTuning::kVideoHeightDefault),
      videoBitrateMbps(VideoStreamTuning::kVideoBitrateDefaultMbps),
      videoFps(VideoStreamTuning::kVideoFpsDefault),
      videoQualityMode(VideoStreamTuning::kVideoQualityDefault),
      centerRoiUseVideo(VideoStreamTuning::kCenterRoiUseVideoDefault),
      roiRegion(VideoStreamTuning::kRoiRegionDefault),
      roiEdgeQualityReduction(VideoStreamTuning::kRoiEdgeQualityReductionDefault),
      roiEdgeScalePercent(VideoStreamTuning::kRoiEdgeScalePercentDefault),
      roiWidthPercent(VideoStreamTuning::kRoiWidthPercentDefault),
      roiHeightPercent(VideoStreamTuning::kRoiHeightPercentDefault),
      roiTopLowPercent(VideoStreamTuning::kRoiTopLowPercentDefault),
      roiJpegCenterWidthPercent(VideoStreamTuning::kRoiJpegCenterWidthPercentDefault),
      roiCenterCpuWeightPercent(VideoStreamTuning::kRoiCenterCpuWeightPercentDefault),
      roiBigCoreWeightPercent(VideoStreamTuning::kRoiBigCoreWeightPercentDefault),
      jpegCenterOnly(VideoStreamTuning::kJpegCenterOnlyDefault),
      stretchFrame(true),
      strictHybridSync(false),
      compactHudMode(false),
      debugHudMode(true) {
    for (int& v : splitWeightPercent) v = 100;
}

// D3D11Renderer
D3D11Renderer::D3D11Renderer(HINSTANCE inst, SharedState& state) : inst_(inst), state_(state), adbSerialOverride_(g_initialAdbSerialOverride) {
        adbWifiMode_ = IsProbablyTcpAdbSerialLocal(adbSerialOverride_);
    }

// D3D11Renderer
D3D11Renderer::~D3D11Renderer() { cleanup(); }

// init
bool D3D11Renderer::init() {
        if (!createWindow()) return false;
        if (!createDevice()) return false;
        if (!createShaders()) return false;
        if (!createSampler()) return false;
        if (!createHudResources()) return false;
        // 明确以窗口模式启动，避免任何残留全屏状态。
        fullscreen_ = false;
        SetWindowLongW(hwnd_, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        ShowWindow(hwnd_, SW_SHOWNORMAL);
        UpdateWindow(hwnd_);
        startPcUinputAuto();
        startMappingToolbar();
        loadDefaultMappingProfileIfPresent();
        return true;
    }

// d3dDevice
ID3D11Device* D3D11Renderer::d3dDevice() const { return device_; }

// d3dContext
ID3D11DeviceContext* D3D11Renderer::d3dContext() const { return ctx_; }

// run
int D3D11Renderer::run() {
        // High-FPS display should not be fully event-driven.  The old loop waited on
        // frameReadyEvent and rendered only after the wait returned; at 115~120fps the
        // Windows wait/message path can coalesce or jitter and make disp fall behind recv.
        // Keep this thread actively polling the latest decoded frame with a very short wait.
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
        timeBeginPeriod(1);

        MSG msg{};
        auto displayWindowStart = std::chrono::steady_clock::now();
        int displayCount = 0;
        int idleSpin = 0;

        while (!state_.stop.load()) {
            while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) {
                    state_.stop.store(true);
                    break;
                }
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }
            if (state_.stop.load()) break;

            // 菜单和视角 2ms 合并后的最后一小段位移，不能只等下一次 WM_INPUT；主循环也尝试刷新。
            menuRuntime_.FlushPendingRawDelta(false);
            lockRuntime_.FlushPendingRawDelta(false);

            // 诊断刷新不能只依赖 WM_INPUT；短促滑动结束后，也要能把最后一小段样本刷出来。
            maybeRefreshMenuDebugOverlay();
            maybeRefreshLockDebugOverlay();

            checkPerfLogAutoStop();
            menuRuntime_.Tick();
            macroRuntime_.Tick();

            const bool menuActive = menuRuntime_.IsActive();
            if (!menuActive) {
                mappingRuntime_.Tick();
            }

            // WASD 摇杆不是菜单/道具/背包的一部分，不能因为菜单 active 就停掉。
            // 菜单自由鼠标只接管鼠标/视角锁定；W/A/S/D 的 down/up 仍必须由摇杆运行时持续处理。
            // 否则菜单触发后 compass Tick 被暂停，摇杆就会表现为被打断或等菜单结束才更新。
            compassRuntime_.Tick();

            // Poll latest frame every loop.  Do not rely on frameReadyEvent timing.
            const bool hadNewFrame = consumeLatestFrame();
            syncRuntimeSettingsOnConnection();
            maybeRetryPcUinputAuto(false);
            if (hadNewFrame || dirtyWindow_) {
                idleSpin = 0;
                render(hadNewFrame);
                dirtyWindow_ = false;

                // “实际显示”只统计真正换了新画面的 Present，不把 HUD/窗口重绘算进去。
                if (hadNewFrame) {
                    displayCount++;
                    auto now = std::chrono::steady_clock::now();
                    auto elapsed = std::chrono::duration<double>(now - displayWindowStart).count();
                    if (elapsed >= 1.0) {
                        std::lock_guard<std::mutex> lk(state_.mutex);
                        state_.displayFps = displayCount / elapsed;
                        cachedDisplayFps_ = state_.displayFps;
                        displayCount = 0;
                        displayWindowStart = now;
                        titleDirty_ = true;
                        hudDirty_ = true;
                    }
                }

                maybeUpdateWindowTitle(false);
                continue;
            }

            maybeUpdateWindowTitle(false);

            // No frame right now.  First yield very cheaply, then wait at most 1ms.
            // This keeps latency low without burning a full CPU core when Android pauses.
            if (idleSpin < 2) {
                ++idleSpin;
                Sleep(0);
            }
            else {
                idleSpin = 0;
                HANDLE waitHandles[1];
                DWORD handleCount = 0;
                if (state_.frameReadyEvent) {
                    waitHandles[handleCount++] = state_.frameReadyEvent;
                }
                MsgWaitForMultipleObjectsEx(
                    handleCount,
                    waitHandles,
                    1,
                    QS_ALLINPUT,
                    MWMO_INPUTAVAILABLE
                );
            }
        }

        timeEndPeriod(1);
        return 0;
    }

// markPcUinputNeedsRetry
void D3D11Renderer::markPcUinputNeedsRetry() {
        pcUinputAutoStarted_ = false;
        lastPcUinputAutoAttempt_ = std::chrono::steady_clock::now() - std::chrono::seconds(10);
    }

// runtimeTouchDown
bool D3D11Renderer::runtimeTouchDown(int slot, int xNorm, int yNorm) {
        int tx = xNorm;
        int ty = yNorm;
        pcUinput_.FrameNormToAndroidNorm(xNorm, yNorm, tx, ty);
        const bool ok = pcUinput_.SendTouchDownNorm(slot, tx, ty);
        if (ok && slot >= 0 && slot < static_cast<int>(runtimeTouchSlots_.size())) {
            runtimeTouchSlots_[static_cast<size_t>(slot)] = true;
        }
        if (!ok) markPcUinputNeedsRetry();
        return ok;
    }

// runtimeTouchMove
bool D3D11Renderer::runtimeTouchMove(int slot, int xNorm, int yNorm) {
        int tx = xNorm;
        int ty = yNorm;
        pcUinput_.FrameNormToAndroidNorm(xNorm, yNorm, tx, ty);
        const bool ok = pcUinput_.SendTouchMoveNorm(slot, tx, ty);
        if (!ok) markPcUinputNeedsRetry();
        return ok;
    }

// runtimeTouchUp
bool D3D11Renderer::runtimeTouchUp(int slot) {
        const bool ok = pcUinput_.SendTouchUp(slot);
        if (slot >= 0 && slot < static_cast<int>(runtimeTouchSlots_.size())) {
            runtimeTouchSlots_[static_cast<size_t>(slot)] = false;
        }
        if (!ok) markPcUinputNeedsRetry();
        return ok;
    }

// runtimeTapWithUnusedSlot
bool D3D11Renderer::runtimeTapWithUnusedSlot(int xNorm, int yNorm, int avoidSlot) {
        // 一次性点击必须像普通按键一样：申请未占用 slot -> down -> up -> 回收到池子。
        // 避免复用视角锁定 slot 或菜单自由鼠标 slot，防止背包/道具触发后状态错乱。
        static constexpr int kCandidates[] = { 15, 14, 13, 12, 11, 10, 8, 7, 6, 5, 4, 3, 2, 1 };
        for (int slot : kCandidates) {
            if (slot == avoidSlot) continue;
            if (slot < 0 || slot >= static_cast<int>(runtimeTouchSlots_.size())) continue;
            if (runtimeTouchSlots_[static_cast<size_t>(slot)]) continue;
            const bool down = runtimeTouchDown(slot, xNorm, yNorm);
            if (down) runtimeTouchUp(slot);
            return down;
        }
        return false;
    }

// updateStatusText
void D3D11Renderer::updateStatusText(const std::wstring& text) {
        {
            std::lock_guard<std::mutex> lk(state_.mutex);
            state_.status = text;
        }
        currentStatus_ = text;
        hudDirty_ = true;
        titleDirty_ = true;
        dirtyWindow_ = true;
    }

// clientPointToNormLocal
bool D3D11Renderer::clientPointToNormLocal(HWND hwnd, int x, int y, int& nx, int& ny) {
        RECT rc{};
        if (!IsWindow(hwnd) || !GetClientRect(hwnd, &rc)) return false;
        const int w = (std::max)(1L, rc.right - rc.left);
        const int h = (std::max)(1L, rc.bottom - rc.top);
        const int cx = (std::max)(0, (std::min)(w - 1, x));
        const int cy = (std::max)(0, (std::min)(h - 1, y));
        nx = (w <= 1) ? 0 : static_cast<int>((static_cast<long long>(cx) * 1000000LL + (w - 1) / 2) / (w - 1));
        ny = (h <= 1) ? 0 : static_cast<int>((static_cast<long long>(cy) * 1000000LL + (h - 1) / 2) / (h - 1));
        nx = (std::max)(0, (std::min)(1000000, nx));
        ny = (std::max)(0, (std::min)(1000000, ny));
        return true;
    }

// normToClientCoord
int D3D11Renderer::normToClientCoord(int norm, int size) {
        const int s = (std::max)(1, size);
        const int v = static_cast<int>((static_cast<long long>(PcMappingProfile::ClampNorm(norm)) * (s - 1) + 500000LL) / 1000000LL);
        return (std::max)(0, (std::min)(s - 1, v));
    }

// warpSystemCursorToNorm
void D3D11Renderer::warpSystemCursorToNorm(int xNorm, int yNorm) const {
        if (!IsWindow(hwnd_)) return;
        RECT rc{};
        if (!GetClientRect(hwnd_, &rc)) return;
        const int w = (std::max)(1L, rc.right - rc.left);
        const int h = (std::max)(1L, rc.bottom - rc.top);
        POINT pt{ normToClientCoord(xNorm, w), normToClientCoord(yNorm, h) };
        ClientToScreen(hwnd_, &pt);
        SetCursorPos(pt.x, pt.y);
    }

// startPcUinputAuto
void D3D11Renderer::startPcUinputAuto() {
        lastPcUinputAutoAttempt_ = std::chrono::steady_clock::now();

        PcUinputMirrorController::Config cfg;
        cfg.enableMouse = true;
        cfg.enableKeyboard = true;
        cfg.enableWheel = true;
        cfg.primarySlot = 0;
        cfg.client.daemonLocalPath = "pc_uinputd";
        cfg.client.daemonRemotePath = "/data/local/tmp/pc_uinputd";
        cfg.client.socketName = "huilang_pc_uinput";
        cfg.client.adbPort = 18889;
        cfg.client.adbSerialOverride = adbSerialOverride_;
        // 不再完全依赖 Android settings user_rotation；有些设备横屏时仍返回 0。
        // 后续每收到新帧会用 currentFrame_.width/height 主动下发坐标基准和 rotation。
        cfg.client.rotation = -1;
        cfg.portraitRotation = 0;
        cfg.landscapeRotation = 1; // 若横屏方向相反，运行时按 F8 可在 1/3 之间切换。
        runtimeTouchSlots_.fill(false);
        const bool ok = pcUinput_.Start(hwnd_, cfg);
        pcUinputAutoStarted_ = ok && pcUinput_.IsConnected();
        configureMappingRuntimeCallbacks();
        updateStatusText(pcUinput_.StatusText());
        if (!pcUinputAutoStarted_) {
            OutputDebugStringW((HLW(L"[pc_uinput] auto start failed: ") + pcUinput_.StatusText() + HLW(L"\n")).c_str());
        }
    }

// maybeRetryPcUinputAuto
void D3D11Renderer::maybeRetryPcUinputAuto(bool force) {
        if (!IsWindow(hwnd_)) return;
        if (pcUinput_.IsConnected()) {
            pcUinputAutoStarted_ = true;
            return;
        }
        pcUinputAutoStarted_ = false;

        // 画面控制通道还没连上时不要反复 adb push/forward，避免启动阶段卡顿。
        // 一旦主通道连上，自动执行一次和 F2 “连接/切换”相同的 pc_uinput 重启流程。
        if (!force && !isControlSocketConnected()) return;

        const auto now = std::chrono::steady_clock::now();
        if (!force && std::chrono::duration<double>(now - lastPcUinputAutoAttempt_).count() < 2.0) return;
        startPcUinputAuto();
    }

// disableImeForMirrorWindow
void D3D11Renderer::disableImeForMirrorWindow() const {
        if (!IsWindow(hwnd_)) return;

        // 投屏主窗口只接收映射/控制按键，不需要文字输入。
        // 禁用该窗口的 IME，避免中文输入法把 Shift 当成“中/英切换”，导致 Shift 映射键抖动或失效。
        ImmAssociateContext(hwnd_, nullptr);
        ImmAssociateContextEx(hwnd_, nullptr, IACE_CHILDREN);
    }

// createWindow
bool D3D11Renderer::createWindow() {
        WNDCLASS wc{};
        wc.lpfnWndProc = WndProc;
        wc.hInstance = inst_;
        wc.lpszClassName = WindowClassName();
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        RegisterClass(&wc);

        hwnd_ = CreateWindowEx(
            0,
            WindowClassName(),
            WindowTitleBase(),
            WS_OVERLAPPEDWINDOW,
            windowedRect_.left, windowedRect_.top,
            windowedRect_.right - windowedRect_.left,
            windowedRect_.bottom - windowedRect_.top,
            nullptr,
            nullptr,
            inst_,
            this
        );
        if (hwnd_) {
            disableImeForMirrorWindow();
        }
        return hwnd_ != nullptr;
    }

// maybeUpdateWindowTitle
void D3D11Renderer::maybeUpdateWindowTitle(bool /*force*/) {
        // Intentionally disabled: user wants borderless fullscreen + no title churn.
    }

// toggleFullscreen
void D3D11Renderer::toggleFullscreen() {
        fullscreen_ = !fullscreen_;
        if (fullscreen_) {
            GetWindowRect(hwnd_, &windowedRect_);
            MONITORINFO mi{ sizeof(mi) };
            GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &mi);
            SetWindowLongW(hwnd_, GWL_STYLE, WS_POPUP | WS_VISIBLE);
            SetWindowPos(
                hwnd_,
                HWND_TOP,
                mi.rcMonitor.left,
                mi.rcMonitor.top,
                mi.rcMonitor.right - mi.rcMonitor.left,
                mi.rcMonitor.bottom - mi.rcMonitor.top,
                SWP_FRAMECHANGED
            );
        }
        else {
            SetWindowLongW(hwnd_, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
            // 回到窗口模式后，立刻允许按当前投屏尺寸重新适配窗口。
            windowSizedToFrame_ = false;
            fitWindowToFrameIfNeeded();
            if (!windowSizedToFrame_) {
                SetWindowPos(
                    hwnd_,
                    nullptr,
                    windowedRect_.left,
                    windowedRect_.top,
                    windowedRect_.right - windowedRect_.left,
                    windowedRect_.bottom - windowedRect_.top,
                    SWP_FRAMECHANGED | SWP_NOZORDER
                );
            }
        }
        dirtyWindow_ = true;
        titleDirty_ = true;
        maybeUpdateWindowTitle(true);
    }

