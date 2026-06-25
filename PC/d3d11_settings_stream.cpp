#include "d3d11_renderer.h"

// updateHudIntervalLabel
void D3D11Renderer::updateHudIntervalLabel() {
    if (!settingsHwnd_) return;
    HWND slider = GetDlgItem(settingsHwnd_, IDC_SLIDER_HUD_INTERVAL);
    HWND label = GetDlgItem(settingsHwnd_, IDC_STATIC_HUD_INTERVAL_VALUE);
    if (!slider || !label) return;
    int value = static_cast<int>(SendMessageW(slider, TBM_GETPOS, 0, 0));
    value = clampInt(value, 100, 1000);
    wchar_t buf[64]{};
    std::swprintf(buf, 64, L"%d ms", value);
    SetWindowTextW(label, buf);
}

// updateAudioVolumeLabel
void D3D11Renderer::updateAudioVolumeLabel() {
    if (!settingsHwnd_) return;
    HWND slider = GetDlgItem(settingsHwnd_, IDC_SLIDER_AUDIO_VOLUME);
    HWND label = GetDlgItem(settingsHwnd_, IDC_STATIC_AUDIO_VOLUME_VALUE);
    if (!slider || !label) return;
    int value = static_cast<int>(SendMessageW(slider, TBM_GETPOS, 0, 0));
    value = clampInt(value, 0, 100);
    wchar_t buf[64]{};
    std::swprintf(buf, 64, L"%d%%", value);
    SetWindowTextW(label, buf);
}

// fpsFromSliderPos
int D3D11Renderer::fpsFromSliderPos(int pos) {
    static const int values[] = { 30, 60, 90, 120, 144, 165 };
    return values[clampInt(pos, 0, 5)];
}

// sliderPosFromFps
int D3D11Renderer::sliderPosFromFps(int fps) {
    static const int values[] = { 30, 60, 90, 120, 144, 165 };
    int best = 0;
    int bestDelta = 100000;
    for (int i = 0; i < 6; ++i) {
        const int d = std::abs(values[i] - fps);
        if (d < bestDelta) { bestDelta = d; best = i; }
    }
    return best;
}

// selectedFullscreenPresetFromControls
int D3D11Renderer::selectedFullscreenPresetFromControls() const {
    if (!settingsHwnd_) return clampInt(settings_.fullscreenPreset, 1, 5);
    if (SendMessageW(GetDlgItem(settingsHwnd_, IDC_RADIO_PRESET_900P), BM_GETCHECK, 0, 0) == BST_CHECKED) return 1;
    if (SendMessageW(GetDlgItem(settingsHwnd_, IDC_RADIO_PRESET_2K), BM_GETCHECK, 0, 0) == BST_CHECKED) return 3;
    if (SendMessageW(GetDlgItem(settingsHwnd_, IDC_RADIO_PRESET_3K), BM_GETCHECK, 0, 0) == BST_CHECKED) return 4;
    if (SendMessageW(GetDlgItem(settingsHwnd_, IDC_RADIO_PRESET_NATIVE), BM_GETCHECK, 0, 0) == BST_CHECKED) return 5;
    return 2; // default 1080p
}

// selectedCaptureModeFromControls
int D3D11Renderer::selectedCaptureModeFromControls() const {
    if (!settingsHwnd_) return settings_.captureMode;
    return SendMessageW(GetDlgItem(settingsHwnd_, IDC_RADIO_CAPTURE_CROP), BM_GETCHECK, 0, 0) == BST_CHECKED ? 0 : 1;
}

// fullscreenPresetText
const wchar_t* D3D11Renderer::fullscreenPresetText(int preset) const {
    switch (preset) {
    case 1: return L"1600x900";
    case 2: return L"1920x1080";
    case 3: return L"长边2560";
    case 4: return L"长边3200";
    case 5: return L"设备最大";
    default: return L"1920x1080";
    }
}

// updateStreamControlLabels
void D3D11Renderer::updateStreamControlLabels() {
    if (!settingsHwnd_) return;
    HWND q = GetDlgItem(settingsHwnd_, IDC_SLIDER_JPEG_QUALITY);
    HWND qv = GetDlgItem(settingsHwnd_, IDC_STATIC_JPEG_QUALITY_VALUE);
    if (q && qv) {
        int value = clampInt(static_cast<int>(SendMessageW(q, TBM_GETPOS, 0, 0)), 60, 100);
        wchar_t buf[64]{}; std::swprintf(buf, 64, L"Q%d", value);
        SetWindowTextW(qv, buf);
    }
    HWND fps = GetDlgItem(settingsHwnd_, IDC_SLIDER_FPS);
    HWND fpsv = GetDlgItem(settingsHwnd_, IDC_STATIC_FPS_VALUE);
    if (fps && fpsv) {
        const int value = fpsFromSliderPos(static_cast<int>(SendMessageW(fps, TBM_GETPOS, 0, 0)));
        wchar_t buf[64]{}; std::swprintf(buf, 64, L"%d FPS", value);
        SetWindowTextW(fpsv, buf);
    }
    HWND fs = GetDlgItem(settingsHwnd_, IDC_SLIDER_FULLSCREEN_SPLIT);
    HWND fsv = GetDlgItem(settingsHwnd_, IDC_STATIC_FULLSCREEN_SPLIT_VALUE);
    if (fs && fsv) {
        int value = clampInt(static_cast<int>(SendMessageW(fs, TBM_GETPOS, 0, 0)), 4, maxFullscreenSplitPartsForSettings());
        wchar_t buf[64]{}; std::swprintf(buf, 64, L"%d 核心", value);
        SetWindowTextW(fsv, buf);
    }
    HWND cs = GetDlgItem(settingsHwnd_, IDC_SLIDER_CROP_SPLIT);
    HWND csv = GetDlgItem(settingsHwnd_, IDC_STATIC_CROP_SPLIT_VALUE);
    if (cs && csv) {
        int value = clampInt(static_cast<int>(SendMessageW(cs, TBM_GETPOS, 0, 0)), 1, 4);
        wchar_t buf[64]{}; std::swprintf(buf, 64, L"%d 核心", value);
        SetWindowTextW(csv, buf);
    }
    HWND crop = GetDlgItem(settingsHwnd_, IDC_SLIDER_CROP_SIZE);
    HWND cropv = GetDlgItem(settingsHwnd_, IDC_STATIC_CROP_SIZE_VALUE);
    if (crop && cropv) {
        int value = clampInt(static_cast<int>(SendMessageW(crop, TBM_GETPOS, 0, 0)), 320, 1080);
        value = (value / 20) * 20;
        wchar_t buf[64]{}; std::swprintf(buf, 64, L"%d x %d", value, value);
        SetWindowTextW(cropv, buf);
    }
    updateJpegSubsamplingControls();
    updateRoiControlLabels();
}

// readStreamSettingsFromControlsIfOpen
void D3D11Renderer::readStreamSettingsFromControlsIfOpen() {
    if (!settingsHwnd_) return;
    settings_.captureMode = selectedCaptureModeFromControls();
    settings_.fullscreenPreset = selectedFullscreenPresetFromControls();
    HWND q = GetDlgItem(settingsHwnd_, IDC_SLIDER_JPEG_QUALITY);
    if (q) settings_.jpegQuality = clampInt(static_cast<int>(SendMessageW(q, TBM_GETPOS, 0, 0)), 60, 100);
    HWND fps = GetDlgItem(settingsHwnd_, IDC_SLIDER_FPS);
    if (fps) settings_.targetFps = fpsFromSliderPos(static_cast<int>(SendMessageW(fps, TBM_GETPOS, 0, 0)));
    HWND fs = GetDlgItem(settingsHwnd_, IDC_SLIDER_FULLSCREEN_SPLIT);
    if (fs) settings_.fullscreenSplitParts = clampInt(static_cast<int>(SendMessageW(fs, TBM_GETPOS, 0, 0)), 4, maxFullscreenSplitPartsForSettings());
    HWND cs = GetDlgItem(settingsHwnd_, IDC_SLIDER_CROP_SPLIT);
    if (cs) settings_.cropSplitParts = clampInt(static_cast<int>(SendMessageW(cs, TBM_GETPOS, 0, 0)), 1, 4);
    HWND crop = GetDlgItem(settingsHwnd_, IDC_SLIDER_CROP_SIZE);
    if (crop) {
        settings_.cropSize = clampInt(static_cast<int>(SendMessageW(crop, TBM_GETPOS, 0, 0)), 320, 1080);
        settings_.cropSize = (settings_.cropSize / 20) * 20;
    }
    settings_.keepFrameRate = SendMessageW(GetDlgItem(settingsHwnd_, IDC_CHECK_KEEP_FRAME), BM_GETCHECK, 0, 0) == BST_CHECKED;
    settings_.jpegSubsamplingMode = selectedJpegSubsamplingModeFromControls();
}

// sendRuntimeSettingsToAndroid
bool D3D11Renderer::sendRuntimeSettingsToAndroid() {
    settings_.captureMode = clampInt(settings_.captureMode, 0, 1);
    settings_.fullscreenPreset = clampInt(settings_.fullscreenPreset, 1, 5);
    settings_.fullscreenSplitParts = clampInt(settings_.fullscreenSplitParts, 4, maxFullscreenSplitPartsForSettings());
    settings_.cropSplitParts = clampInt(settings_.cropSplitParts, 1, 4);
    settings_.jpegQuality = clampInt(settings_.jpegQuality, 60, 100);
    settings_.targetFps = fpsFromSliderPos(sliderPosFromFps(settings_.targetFps));
    settings_.cropSize = clampInt(settings_.cropSize, 320, 1080);
    settings_.cropSize = (settings_.cropSize / 20) * 20;
    settings_.jpegSubsamplingMode = settings_.jpegSubsamplingMode == 444 ? 444 : 420;

    char cmd[256]{};
    std::snprintf(cmd, sizeof(cmd), "HLAPPLY %d %d %d %d %d %d %d %d %d\n",
        settings_.captureMode,
        settings_.fullscreenPreset,
        settings_.cropSize,
        settings_.jpegQuality,
        settings_.keepFrameRate ? 1 : 0,
        settings_.targetFps,
        settings_.fullscreenSplitParts,
        settings_.cropSplitParts,
        settings_.jpegSubsamplingMode);
    const bool ok = sendControlLineToAndroid(cmd);
    const bool roiOk = sendRoiSettingsToAndroid();
    pendingRuntimeSettingsSync_ = !(ok && roiOk);
    return ok && roiOk;
}

// applyStreamSettingsFromControls
bool D3D11Renderer::applyStreamSettingsFromControls(bool showMessage) {
    readStreamSettingsFromControlsIfOpen();
    readRoiSettingsFromControlsIfOpen();
    const bool ok = sendRuntimeSettingsToAndroid();
    updateStreamControlLabels();
    HWND status = GetDlgItem(settingsHwnd_, IDC_STATIC_JPEG_STATUS);
    if (status) {
        wchar_t buf[240]{};
        std::swprintf(buf, 240, L"画面参数：%ls | Q%d | %dFPS | 全屏%d核心/裁剪%d核心 | %ls",
            fullscreenPresetText(settings_.fullscreenPreset), settings_.jpegQuality, settings_.targetFps,
            settings_.fullscreenSplitParts, settings_.cropSplitParts, ok ? L"已发送" : L"未连接，连接后自动下发");
        SetWindowTextW(status, buf);
    }
    if (showMessage && !ok) {
        MessageBoxW(settingsHwnd_, L"当前没有可用的画面连接，参数已保存在PC窗口；连接成功后会自动下发一次。", L"灰狼投屏设置", MB_ICONINFORMATION);
    }
    return ok;
}

// isControlSocketConnected
bool D3D11Renderer::isControlSocketConnected() const {
    std::lock_guard<std::mutex> lk(state_.controlSocketMutex);
    return state_.controlSocket != INVALID_SOCKET;
}

// syncRuntimeSettingsOnConnection
void D3D11Renderer::syncRuntimeSettingsOnConnection() {
    const bool connected = isControlSocketConnected();
    if (!connected) {
        if (lastControlSocketConnected_) pendingRuntimeSettingsSync_ = true;
        lastControlSocketConnected_ = false;
        return;
    }
    const bool justConnected = !lastControlSocketConnected_;
    lastControlSocketConnected_ = true;
    if (justConnected) pendingRuntimeSettingsSync_ = true;
    if (!pendingRuntimeSettingsSync_) return;

    auto now = std::chrono::steady_clock::now();
    if (!justConnected && std::chrono::duration<double>(now - lastRuntimeSyncAttempt_).count() < 0.5) return;
    lastRuntimeSyncAttempt_ = now;

    // PC 成为唯一参数入口：连接建立后立即把 PC 当前画面参数同步到 Android/native，
    // 避免 Android 默认值和 PC F2 面板默认值不一致。
    readStreamSettingsFromControlsIfOpen();
    readRoiSettingsFromControlsIfOpen();
    const bool ok = sendRuntimeSettingsToAndroid();
    if (ok && settingsHwnd_) {
        updateStreamControlLabels();
        HWND status = GetDlgItem(settingsHwnd_, IDC_STATIC_JPEG_STATUS);
        if (status) SetWindowTextW(status, L"画面参数：已在连接建立后自动同步到安卓");
    }
}

// sendControlLineToSocket
bool D3D11Renderer::sendControlLineToSocket(SOCKET s, const std::string& line) {
    if (line.empty() || s == INVALID_SOCKET) return false;
    const char* data = line.c_str();
    int left = static_cast<int>(line.size());
    while (left > 0) {
        const int n = send(s, data, left, 0);
        if (n <= 0) return false;
        data += n;
        left -= n;
    }
    return true;
}

// sendControlLineToAndroid
bool D3D11Renderer::sendControlLineToAndroid(const std::string& line) {
    std::lock_guard<std::mutex> lk(state_.controlSocketMutex);
    return sendControlLineToSocket(state_.controlSocket, line);
}

// sendVideoControlLineToAndroid
bool D3D11Renderer::sendVideoControlLineToAndroid(const std::string& line) {
    // 视频参数要同时广播到两条可能的控制路径：
    // 1) controlSocket：旧设置/JPEG控制通道，PcScreenMirrorCoordinator 会直接调用 CenterRoiVideoStreamSender.updateRuntimeConfig()。
    // 2) videoControlSocket：H.264 视频 LocalSocket 的反向通道，CenterRoiVideoStreamSender 自己读取。
    // 上一版只要 videoControlSocket 发送成功就不再发 controlSocket，导致 UI 显示“已发送到控制+视频通道”，
    // 但如果 Android 端视频反向读取线程没有启动/被系统中断，参数就不会真正应用。
    std::lock_guard<std::mutex> lk(state_.controlSocketMutex);
    bool ok = false;
    if (state_.controlSocket != INVALID_SOCKET) {
        ok = sendControlLineToSocket(state_.controlSocket, line) || ok;
    }
    if (state_.videoControlSocket != INVALID_SOCKET && state_.videoControlSocket != state_.controlSocket) {
        ok = sendControlLineToSocket(state_.videoControlSocket, line) || ok;
    }
    return ok;
}

