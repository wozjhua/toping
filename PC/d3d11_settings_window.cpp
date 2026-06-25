#include "d3d11_renderer.h"

// applySettingsFromControls
bool D3D11Renderer::applySettingsFromControls() {
    const UINT hudKey = parseVirtualKey(getControlText(IDC_EDIT_HUD_KEY));
    const UINT fullKey1 = parseVirtualKey(getControlText(IDC_EDIT_FULLSCREEN_KEY1));
    const UINT fullKey2 = parseVirtualKey(getControlText(IDC_EDIT_FULLSCREEN_KEY2));
    const UINT settingsKey = parseVirtualKey(getControlText(IDC_EDIT_SETTINGS_KEY));
    const UINT exitKey = parseVirtualKey(getControlText(IDC_EDIT_EXIT_KEY));
    if (!hudKey || !fullKey1 || !fullKey2 || !settingsKey || !exitKey) {
        MessageBoxW(settingsHwnd_, L"热键格式无效。支持 F1~F24、Enter、Esc、Space、Tab、A~Z、0~9。", L"灰狼投屏设置", MB_ICONWARNING);
        return false;
    }

    int interval = settings_.hudRefreshMs;
    HWND slider = GetDlgItem(settingsHwnd_, IDC_SLIDER_HUD_INTERVAL);
    if (slider) {
        interval = static_cast<int>(SendMessageW(slider, TBM_GETPOS, 0, 0));
    }
    interval = clampInt(interval, 100, 1000);

    int audioVolume = settings_.audioVolumePercent;
    HWND audioSlider = GetDlgItem(settingsHwnd_, IDC_SLIDER_AUDIO_VOLUME);
    if (audioSlider) {
        audioVolume = static_cast<int>(SendMessageW(audioSlider, TBM_GETPOS, 0, 0));
    }
    audioVolume = clampInt(audioVolume, 0, 100);

    HWND compactCheck = GetDlgItem(settingsHwnd_, IDC_CHECK_COMPACT_HUD_MODE);
    HWND debugCheck = GetDlgItem(settingsHwnd_, IDC_CHECK_DEBUG_HUD_MODE);
    settings_.compactHudMode = compactCheck && SendMessageW(compactCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
    settings_.debugHudMode = !debugCheck || SendMessageW(debugCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;

    settings_.hudKey = hudKey;
    settings_.fullscreenKey1 = fullKey1;
    settings_.fullscreenKey2 = fullKey2;
    settings_.settingsKey = settingsKey;
    settings_.exitKey = exitKey;
    settings_.hudRefreshMs = interval;
    settings_.audioVolumePercent = audioVolume;
    g_audioVolumePercent.store(audioVolume, std::memory_order_release);
    applyStreamSettingsFromControls(false);
    applyVideoSettingsFromControls(false);
    applyRoiSettingsFromControls(false);
    fillSettingsControls();
    updateSettingsCompactLayout();
    hudDirty_ = true;
    dirtyWindow_ = true;
    return true;
}

// resetSettingsControlsToDefault
void D3D11Renderer::resetSettingsControlsToDefault() {
    settings_.hudKey = VK_F3;
    settings_.fullscreenKey1 = VK_RETURN;
    settings_.fullscreenKey2 = VK_F11;
    settings_.settingsKey = VK_F2;
    settings_.splitDebugKey = VK_F4;
    settings_.exitKey = VK_ESCAPE;
    settings_.hudRefreshMs = 200;
    settings_.audioVolumePercent = DEFAULT_AUDIO_VOLUME_PERCENT;
    settings_.jpegSubsamplingMode = 420;
    settings_.jpegQuality = 100;
    settings_.keepFrameRate = true;
    settings_.targetFps = VideoStreamTuning::kVideoFpsDefault;
    settings_.fullscreenPreset = 2;
    settings_.fullscreenSplitParts = g_initialFullscreenSplitParts;
    settings_.cropSize = 640;
    settings_.cropSplitParts = 2;
    settings_.captureMode = 1;
    settings_.videoPreset = VideoStreamTuning::kVideoPresetDefault;
    settings_.videoWidth = VideoStreamTuning::kVideoWidthDefault;
    settings_.videoHeight = VideoStreamTuning::kVideoHeightDefault;
    settings_.videoBitrateMbps = VideoStreamTuning::kVideoBitrateDefaultMbps;
    settings_.videoFps = VideoStreamTuning::kVideoFpsDefault;
    settings_.videoQualityMode = VideoStreamTuning::kVideoQualityDefault;
    settings_.centerRoiUseVideo = VideoStreamTuning::kCenterRoiUseVideoDefault;
    settings_.roiRegion = VideoStreamTuning::kRoiRegionDefault;
    settings_.roiEdgeQualityReduction = VideoStreamTuning::kRoiEdgeQualityReductionDefault;
    settings_.roiEdgeScalePercent = VideoStreamTuning::kRoiEdgeScalePercentDefault;
    settings_.roiWidthPercent = VideoStreamTuning::kRoiWidthPercentDefault;
    settings_.roiHeightPercent = VideoStreamTuning::kRoiHeightPercentDefault;
    settings_.roiTopLowPercent = VideoStreamTuning::kRoiTopLowPercentDefault;
    settings_.roiJpegCenterWidthPercent = VideoStreamTuning::kRoiJpegCenterWidthPercentDefault;
    settings_.roiCenterCpuWeightPercent = VideoStreamTuning::kRoiCenterCpuWeightPercentDefault;
    settings_.roiBigCoreWeightPercent = VideoStreamTuning::kRoiBigCoreWeightPercentDefault;
    settings_.jpegCenterOnly = VideoStreamTuning::kJpegCenterOnlyDefault;
    settings_.strictHybridSync = false;
    settings_.compactHudMode = false;
    settings_.debugHudMode = true;
    g_audioVolumePercent.store(DEFAULT_AUDIO_VOLUME_PERCENT, std::memory_order_release);
    fillSettingsControls();
    applyStreamSettingsFromControls(false);
    applyVideoSettingsFromControls(false);
    applyRoiSettingsFromControls(false);
    fillSettingsControls();
    hudDirty_ = true;
    dirtyWindow_ = true;
}

// SettingsWndProc
LRESULT CALLBACK D3D11Renderer::SettingsWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<D3D11Renderer*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCT*>(lp);
        self = reinterpret_cast<D3D11Renderer*>(cs->lpCreateParams);
        SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        return TRUE;
    }
    if (!self) return DefWindowProc(hwnd, msg, wp, lp);
    switch (msg) {
    case WM_CREATE:
        self->createSettingsControls(hwnd);
        return 0;
    case WM_COMMAND:
        if (HIWORD(wp) == EN_SETFOCUS) {
            const int id = LOWORD(wp);
            if (id == IDC_EDIT_HUD_KEY || id == IDC_EDIT_FULLSCREEN_KEY1 ||
                id == IDC_EDIT_FULLSCREEN_KEY2 || id == IDC_EDIT_SETTINGS_KEY ||
                id == IDC_EDIT_EXIT_KEY) {
                self->activeHotkeyEditId_ = id;
                self->settingsHotkeyCaptureArmed_ = false;
                SetTimer(hwnd, IDC_TIMER_HOTKEY_CAPTURE, 50, nullptr);
                return 0;
            }
        }
        switch (LOWORD(wp)) {
        case IDC_BUTTON_APPLY:
            self->applySettingsFromControls();
            return 0;
        case IDC_BUTTON_STREAM_APPLY:
            self->applyStreamSettingsFromControls(true);
            return 0;
        case IDC_BUTTON_VIDEO_APPLY:
            self->applyVideoSettingsFromControls(true);
            return 0;
        case IDC_BUTTON_ROI_APPLY:
            self->applyRoiSettingsFromControls(true);
            return 0;
        case IDC_BUTTON_WIFI_CONNECT:
            self->runWifiAdbCommand(false);
            return 0;
        case IDC_BUTTON_WIFI_TCPIP:
            self->runWifiAdbCommand(true);
            return 0;
        case IDC_BUTTON_ADB_TOGGLE:
            self->switchAdbModeByButton();
            return 0;
        case IDC_RADIO_VIDEO_PRESET_640P:
        case IDC_RADIO_VIDEO_PRESET_1080P:
        case IDC_RADIO_VIDEO_PRESET_2K:
        case IDC_RADIO_VIDEO_PRESET_3K:
        case IDC_RADIO_VIDEO_PRESET_NATIVE:
            self->settings_.videoPreset = self->selectedVideoPresetFromControls();
            self->updateVideoControlLabels();
            return 0;
        case IDC_RADIO_VIDEO_Q_LOW:
        case IDC_RADIO_VIDEO_Q_HIGH:
        case IDC_RADIO_VIDEO_Q_GOP:
            self->settings_.videoQualityMode = VideoStreamTuning::NormalizeVideoQualityMode(self->selectedVideoQualityModeFromControls());
            self->updateVideoControlLabels();
            return 0;
        case IDC_RADIO_CENTER_ROI_VIDEO:
        case IDC_RADIO_CENTER_ROI_JPEG:
            self->settings_.centerRoiUseVideo = (LOWORD(wp) == IDC_RADIO_CENTER_ROI_VIDEO);
            self->state_.centerRoiStrictSync.store(
                self->settings_.centerRoiUseVideo && self->settings_.strictHybridSync &&
                !(self->settings_.jpegCenterOnly && self->settings_.roiRegion == ROI_REGION_CENTER),
                std::memory_order_release);
            if (!self->settings_.centerRoiUseVideo) {
                self->clearCenterRoiOverlay();
            }
            self->updateRoiControlLabels();
            return 0;
        case IDC_RADIO_ROI_REGION_CENTER:
        case IDC_RADIO_ROI_REGION_BOTTOM:
            self->settings_.roiRegion = (LOWORD(wp) == IDC_RADIO_ROI_REGION_BOTTOM) ? ROI_REGION_BOTTOM : ROI_REGION_CENTER;
            self->state_.centerRoiStrictSync.store(
                self->settings_.centerRoiUseVideo && self->settings_.strictHybridSync &&
                !(self->settings_.jpegCenterOnly && self->settings_.roiRegion == ROI_REGION_CENTER),
                std::memory_order_release);
            self->updateRoiControlLabels();
            return 0;
        case IDC_CHECK_JPEG_CENTER_ONLY:
            self->settings_.jpegCenterOnly = SendMessageW(GetDlgItem(hwnd, IDC_CHECK_JPEG_CENTER_ONLY), BM_GETCHECK, 0, 0) == BST_CHECKED;
            self->state_.centerRoiStrictSync.store(
                self->settings_.centerRoiUseVideo && self->settings_.strictHybridSync &&
                !(self->settings_.jpegCenterOnly && self->settings_.roiRegion == ROI_REGION_CENTER),
                std::memory_order_release);
            if (self->settings_.jpegCenterOnly && self->settings_.roiRegion == ROI_REGION_CENTER) {
                self->pendingSyncJpegQueue_.clear();
                self->pendingSyncCenterQueue_.clear();
            }
            self->updateRoiControlLabels();
            return 0;
        case IDC_CHECK_STRICT_HYBRID_SYNC:
            self->settings_.strictHybridSync = SendMessageW(GetDlgItem(hwnd, IDC_CHECK_STRICT_HYBRID_SYNC), BM_GETCHECK, 0, 0) == BST_CHECKED;
            self->state_.centerRoiStrictSync.store(
                self->settings_.centerRoiUseVideo && self->settings_.strictHybridSync &&
                !(self->settings_.jpegCenterOnly && self->settings_.roiRegion == ROI_REGION_CENTER),
                std::memory_order_release);
            if (!self->settings_.strictHybridSync) {
                self->pendingSyncJpegQueue_.clear();
                self->pendingSyncCenterQueue_.clear();
            }
            self->updateRoiControlLabels();
            return 0;
        case IDC_CHECK_COMPACT_HUD_MODE:
            self->settings_.compactHudMode = SendMessageW(GetDlgItem(hwnd, IDC_CHECK_COMPACT_HUD_MODE), BM_GETCHECK, 0, 0) == BST_CHECKED;
            self->updateSettingsCompactLayout();
            self->hudDirty_ = true;
            self->dirtyWindow_ = true;
            return 0;
        case IDC_CHECK_DEBUG_HUD_MODE:
            self->settings_.debugHudMode = SendMessageW(GetDlgItem(hwnd, IDC_CHECK_DEBUG_HUD_MODE), BM_GETCHECK, 0, 0) == BST_CHECKED;
            self->hudDirty_ = true;
            self->dirtyWindow_ = true;
            return 0;
        case IDC_RADIO_JPEG_420:
        case IDC_RADIO_JPEG_444:
            self->settings_.jpegSubsamplingMode = (LOWORD(wp) == IDC_RADIO_JPEG_444) ? 444 : 420;
            self->updateStreamControlLabels();
            return 0;
        case IDC_RADIO_CAPTURE_FULLSCREEN:
        case IDC_RADIO_CAPTURE_CROP:
        case IDC_RADIO_PRESET_720P:
        case IDC_RADIO_PRESET_900P:
        case IDC_RADIO_PRESET_1080P:
        case IDC_RADIO_PRESET_2K:
        case IDC_RADIO_PRESET_3K:
        case IDC_RADIO_PRESET_NATIVE:
        case IDC_CHECK_KEEP_FRAME:
            self->updateStreamControlLabels();
            return 0;
        case IDC_BUTTON_RESET:
            self->resetSettingsControlsToDefault();
            return 0;
        case IDC_BUTTON_CLOSE:
            self->hideSettingsWindow(hwnd);
            return 0;
        default:
            break;
        }
        break;
    case WM_HSCROLL:
        self->updateHudIntervalLabel();
        self->updateAudioVolumeLabel();
        self->updateStreamControlLabels();
        self->updateRoiControlLabels();
        return 0;
    case WM_CTLCOLORDLG:
        return reinterpret_cast<LRESULT>(self->settingsBgBrush_ ? self->settingsBgBrush_ : reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN: {
        HDC hdc = reinterpret_cast<HDC>(wp);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(232, 241, 255));
        return reinterpret_cast<LRESULT>(self->settingsBgBrush_ ? self->settingsBgBrush_ : reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
    }
    case WM_TIMER:
        if (wp == IDC_TIMER_HOTKEY_CAPTURE) {
            self->captureHotkeyByTimer();
            return 0;
        }
        break;
    case WM_CLOSE:
        self->hideSettingsWindow(hwnd);
        return 0;
    case WM_DESTROY:
        self->stopSettingsHotkeyCapture(hwnd);
        if (self->settingsHwnd_ == hwnd) self->settingsHwnd_ = nullptr;
        return 0;
    default:
        break;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

// showSettingsWindow
void D3D11Renderer::showSettingsWindow() {
    if (settingsHwnd_) {
        fillSettingsControls();
        updateSettingsCompactLayout();
        tryAutoFillWifiIpFromUsb(false, false);
        ShowWindow(settingsHwnd_, SW_SHOWNORMAL);
        SetForegroundWindow(settingsHwnd_);
        SetFocus(GetDlgItem(settingsHwnd_, IDC_EDIT_HUD_KEY));
        armSettingsHotkeyCapture(settingsHwnd_);
        return;
    }

    static bool classRegistered = false;
    if (!classRegistered) {
        INITCOMMONCONTROLSEX icc{};
        icc.dwSize = sizeof(icc);
        icc.dwICC = ICC_BAR_CLASSES;
        InitCommonControlsEx(&icc);

        WNDCLASSW wc{};
        wc.lpfnWndProc = SettingsWndProc;
        wc.hInstance = inst_;
        wc.lpszClassName = L"HuiLangMirrorSettingsWindow";
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = CreateSolidBrush(RGB(12, 18, 28));
        RegisterClassW(&wc);
        classRegistered = true;
    }

    settingsHwnd_ = CreateWindowExW(
        WS_EX_TOOLWINDOW,
        L"HuiLangMirrorSettingsWindow",
        L"灰狼投屏设置",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        120, 50, 760, 850,
        hwnd_,
        nullptr,
        inst_,
        this
    );
}

