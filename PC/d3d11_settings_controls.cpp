#include "d3d11_renderer.h"

// clampInt
int D3D11Renderer::clampInt(int v, int lo, int hi) {
    return (std::max)(lo, (std::min)(hi, v));
}

// createLabel
HWND D3D11Renderer::createLabel(HWND parent, const wchar_t* text, int x, int y, int w, int h) {
    HWND ctrl = CreateWindowW(L"STATIC", text, WS_CHILD | WS_VISIBLE, x, y, w, h, parent, nullptr, nullptr, nullptr);
    if (ctrl && settingsFont_) SendMessageW(ctrl, WM_SETFONT, reinterpret_cast<WPARAM>(settingsFont_), TRUE);
    return ctrl;
}

// createEdit
HWND D3D11Renderer::createEdit(HWND parent, int id, int x, int y, int w, int h) {
    HWND ctrl = CreateWindowExW(
        WS_EX_CLIENTEDGE,
        L"EDIT",
        L"",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_READONLY,
        x, y, w, h,
        parent,
        reinterpret_cast<HMENU>(static_cast<intptr_t>(id)),
        nullptr,
        nullptr
    );
    if (ctrl && settingsFont_) SendMessageW(ctrl, WM_SETFONT, reinterpret_cast<WPARAM>(settingsFont_), TRUE);
    if (ctrl) {
        SetPropW(ctrl, L"HuiLangHotkeySelf", reinterpret_cast<HANDLE>(this));
        LONG_PTR oldProc = SetWindowLongPtrW(ctrl, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&D3D11Renderer::HotkeyEditProc));
        SetPropW(ctrl, L"HuiLangHotkeyOldProc", reinterpret_cast<HANDLE>(oldProc));
    }
    return ctrl;
}

// createValueEdit
HWND D3D11Renderer::createValueEdit(HWND parent, int id, int x, int y, int w, int h) {
    HWND ctrl = CreateWindowExW(
        WS_EX_CLIENTEDGE,
        L"EDIT",
        L"",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_NUMBER,
        x, y, w, h,
        parent,
        reinterpret_cast<HMENU>(static_cast<intptr_t>(id)),
        nullptr,
        nullptr
    );
    if (ctrl && settingsFont_) SendMessageW(ctrl, WM_SETFONT, reinterpret_cast<WPARAM>(settingsFont_), TRUE);
    return ctrl;
}

// createTextEdit
HWND D3D11Renderer::createTextEdit(HWND parent, int id, int x, int y, int w, int h, const wchar_t* placeholder) {
    HWND ctrl = CreateWindowExW(
        WS_EX_CLIENTEDGE,
        L"EDIT",
        placeholder ? placeholder : L"",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        x, y, w, h,
        parent,
        reinterpret_cast<HMENU>(static_cast<intptr_t>(id)),
        nullptr,
        nullptr
    );
    if (ctrl && settingsFont_) SendMessageW(ctrl, WM_SETFONT, reinterpret_cast<WPARAM>(settingsFont_), TRUE);
    return ctrl;
}

// createButton
HWND D3D11Renderer::createButton(HWND parent, const wchar_t* text, int id, int x, int y, int w, int h, DWORD extraStyle) {
    HWND ctrl = CreateWindowW(L"BUTTON", text, WS_CHILD | WS_VISIBLE | extraStyle, x, y, w, h, parent, reinterpret_cast<HMENU>(static_cast<intptr_t>(id)), nullptr, nullptr);
    if (ctrl && settingsFont_) SendMessageW(ctrl, WM_SETFONT, reinterpret_cast<WPARAM>(settingsFont_), TRUE);
    return ctrl;
}

// createLabelWithId
HWND D3D11Renderer::createLabelWithId(HWND parent, const wchar_t* text, int id, int x, int y, int w, int h) {
    HWND ctrl = CreateWindowW(L"STATIC", text, WS_CHILD | WS_VISIBLE, x, y, w, h, parent,
        reinterpret_cast<HMENU>(static_cast<intptr_t>(id)), nullptr, nullptr);
    if (ctrl && settingsFont_) SendMessageW(ctrl, WM_SETFONT, reinterpret_cast<WPARAM>(settingsFont_), TRUE);
    return ctrl;
}

// setCtrlVisible
void D3D11Renderer::setCtrlVisible(int id, bool visible) {
    if (!settingsHwnd_) return;
    HWND h = GetDlgItem(settingsHwnd_, id);
    if (h) ShowWindow(h, visible ? SW_SHOW : SW_HIDE);
}

// setCtrlEnabled
void D3D11Renderer::setCtrlEnabled(int id, bool enabled) {
    if (!settingsHwnd_) return;
    HWND h = GetDlgItem(settingsHwnd_, id);
    if (h) EnableWindow(h, enabled ? TRUE : FALSE);
}

// setCtrlText
void D3D11Renderer::setCtrlText(int id, const wchar_t* text) {
    if (!settingsHwnd_) return;
    HWND h = GetDlgItem(settingsHwnd_, id);
    if (h) SetWindowTextW(h, text ? text : L"");
}

// hideSettingsWindow
void D3D11Renderer::hideSettingsWindow(HWND hwnd) {
    stopSettingsHotkeyCapture(hwnd);
    ShowWindow(hwnd, SW_HIDE);
}

// updateSettingsCompactLayout
void D3D11Renderer::updateSettingsCompactLayout() {
    if (!settingsHwnd_) return;

    const bool compact = settings_.compactHudMode;
    for (HWND child = GetWindow(settingsHwnd_, GW_CHILD); child != nullptr; child = GetWindow(child, GW_HWNDNEXT)) {
        RECT wr{};
        if (!GetWindowRect(child, &wr)) continue;
        POINT pt{ wr.left, wr.top };
        ScreenToClient(settingsHwnd_, &pt);

        // 右侧 H.264 / ROI 参数区从 x=452 开始。精简模式下隐藏这整块，
        // 非精简模式不要盲目 ShowWindow(SW_SHOW) 右侧控件，否则会把
        // updateRoiControlLabels() 根据 JPEG/H.264 模式隐藏的控件重新显示出来。
        const bool rightVideoPanel = pt.x >= 450;
        if (compact && rightVideoPanel) {
            ShowWindow(child, SW_HIDE);
        }
        else if (!compact) {
            // 从精简模式恢复时，先把右侧区域全部恢复显示，再交给
            // updateRoiControlLabels()/updateVideoControlLabels() 按当前
            // JPEG/H.264、中心/底部模式隐藏不相关控件。
            // 否则之前被精简模式隐藏、但不在 ROI 更新函数显式管理的
            // 控件（例如“区域内容”单选、外围降Q/缩放、groupbox）会一直隐藏。
            ShowWindow(child, SW_SHOW);
        }
        else if (!rightVideoPanel) {
            ShowWindow(child, SW_SHOW);
        }
    }

    const int targetW = compact ? 446 : 760;
    // 功能区压缩后，精简模式不再保留顶部标题和大段空白。
    // 普通模式保留右侧 ROI/H.264 参数区，因此高度略高。
    const int targetH = compact ? 690 : 720;
    SetWindowPos(settingsHwnd_, nullptr, 0, 0, targetW, targetH,
        SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    if (!compact) {
        updateRoiControlLabels();
    }
}

// isButtonChecked
bool D3D11Renderer::isButtonChecked(int id) const {
    return settingsHwnd_ && SendMessageW(GetDlgItem(settingsHwnd_, id), BM_GETCHECK, 0, 0) == BST_CHECKED;
}

// fillSettingsControls
void D3D11Renderer::fillSettingsControls() {
    if (!settingsHwnd_) return;
    SetWindowTextW(GetDlgItem(settingsHwnd_, IDC_EDIT_HUD_KEY), keyName(settings_.hudKey).c_str());
    SetWindowTextW(GetDlgItem(settingsHwnd_, IDC_EDIT_FULLSCREEN_KEY1), keyName(settings_.fullscreenKey1).c_str());
    SetWindowTextW(GetDlgItem(settingsHwnd_, IDC_EDIT_FULLSCREEN_KEY2), keyName(settings_.fullscreenKey2).c_str());
    SetWindowTextW(GetDlgItem(settingsHwnd_, IDC_EDIT_SETTINGS_KEY), keyName(settings_.settingsKey).c_str());
    SetWindowTextW(GetDlgItem(settingsHwnd_, IDC_EDIT_EXIT_KEY), keyName(settings_.exitKey).c_str());
    HWND slider = GetDlgItem(settingsHwnd_, IDC_SLIDER_HUD_INTERVAL);
    if (slider) {
        SendMessageW(slider, TBM_SETRANGE, TRUE, MAKELPARAM(100, 1000));
        SendMessageW(slider, TBM_SETTICFREQ, 100, 0);
        SendMessageW(slider, TBM_SETPAGESIZE, 0, 100);
        SendMessageW(slider, TBM_SETLINESIZE, 0, 50);
        SendMessageW(slider, TBM_SETPOS, TRUE, clampInt(settings_.hudRefreshMs, 100, 1000));
    }
    updateHudIntervalLabel();
    HWND audioSlider = GetDlgItem(settingsHwnd_, IDC_SLIDER_AUDIO_VOLUME);
    if (audioSlider) {
        SendMessageW(audioSlider, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
        SendMessageW(audioSlider, TBM_SETTICFREQ, 10, 0);
        SendMessageW(audioSlider, TBM_SETPAGESIZE, 0, 10);
        SendMessageW(audioSlider, TBM_SETLINESIZE, 0, 5);
        SendMessageW(audioSlider, TBM_SETPOS, TRUE, clampInt(settings_.audioVolumePercent, 0, 100));
    }
    updateAudioVolumeLabel();

    HWND qSlider = GetDlgItem(settingsHwnd_, IDC_SLIDER_JPEG_QUALITY);
    if (qSlider) {
        SendMessageW(qSlider, TBM_SETRANGE, TRUE, MAKELPARAM(60, 100));
        SendMessageW(qSlider, TBM_SETPOS, TRUE, clampInt(settings_.jpegQuality, 60, 100));
    }
    HWND fpsSlider = GetDlgItem(settingsHwnd_, IDC_SLIDER_FPS);
    if (fpsSlider) {
        SendMessageW(fpsSlider, TBM_SETRANGE, TRUE, MAKELPARAM(0, 5));
        SendMessageW(fpsSlider, TBM_SETPOS, TRUE, sliderPosFromFps(settings_.targetFps));
    }
    HWND fullSplit = GetDlgItem(settingsHwnd_, IDC_SLIDER_FULLSCREEN_SPLIT);
    if (fullSplit) {
        SendMessageW(fullSplit, TBM_SETRANGE, TRUE, MAKELPARAM(4, maxFullscreenSplitPartsForSettings()));
        SendMessageW(fullSplit, TBM_SETPOS, TRUE, clampInt(settings_.fullscreenSplitParts, 4, maxFullscreenSplitPartsForSettings()));
    }
    HWND cropSplit = GetDlgItem(settingsHwnd_, IDC_SLIDER_CROP_SPLIT);
    if (cropSplit) {
        SendMessageW(cropSplit, TBM_SETRANGE, TRUE, MAKELPARAM(1, 4));
        SendMessageW(cropSplit, TBM_SETPOS, TRUE, clampInt(settings_.cropSplitParts, 1, 4));
    }
    HWND cropSize = GetDlgItem(settingsHwnd_, IDC_SLIDER_CROP_SIZE);
    if (cropSize) {
        SendMessageW(cropSize, TBM_SETRANGE, TRUE, MAKELPARAM(320, 1080));
        SendMessageW(cropSize, TBM_SETPOS, TRUE, clampInt(settings_.cropSize, 320, 1080));
    }
    CheckRadioButton(settingsHwnd_, IDC_RADIO_PRESET_900P, IDC_RADIO_PRESET_NATIVE,
        IDC_RADIO_PRESET_720P + clampInt(settings_.fullscreenPreset, 1, 5));
    CheckRadioButton(settingsHwnd_, IDC_RADIO_CAPTURE_FULLSCREEN, IDC_RADIO_CAPTURE_CROP,
        settings_.captureMode == 0 ? IDC_RADIO_CAPTURE_CROP : IDC_RADIO_CAPTURE_FULLSCREEN);
    CheckRadioButton(settingsHwnd_, IDC_RADIO_JPEG_420, IDC_RADIO_JPEG_444,
        settings_.jpegSubsamplingMode == 444 ? IDC_RADIO_JPEG_444 : IDC_RADIO_JPEG_420);
    SendMessageW(GetDlgItem(settingsHwnd_, IDC_CHECK_KEEP_FRAME), BM_SETCHECK, settings_.keepFrameRate ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(GetDlgItem(settingsHwnd_, IDC_CHECK_STRETCH_FRAME), BM_SETCHECK, settings_.stretchFrame ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(GetDlgItem(settingsHwnd_, IDC_CHECK_COMPACT_HUD_MODE), BM_SETCHECK, settings_.compactHudMode ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(GetDlgItem(settingsHwnd_, IDC_CHECK_DEBUG_HUD_MODE), BM_SETCHECK, settings_.debugHudMode ? BST_CHECKED : BST_UNCHECKED, 0);
    setIntEdit(IDC_EDIT_VIDEO_BITRATE, settings_.videoBitrateMbps);
    setIntEdit(IDC_EDIT_VIDEO_FPS, settings_.videoFps);
    setWifiIpFieldsFromEndpoint(wifiAdbEndpoint_);
    updateAdbToggleButtonText();
    HWND roiW = GetDlgItem(settingsHwnd_, IDC_SLIDER_ROI_WIDTH_PERCENT);
    if (roiW) SendMessageW(roiW, TBM_SETPOS, TRUE, VideoStreamTuning::NormalizeRoiPercent(settings_.roiWidthPercent));
    HWND roiH = GetDlgItem(settingsHwnd_, IDC_SLIDER_ROI_HEIGHT_PERCENT);
    if (roiH) SendMessageW(roiH, TBM_SETPOS, TRUE, VideoStreamTuning::NormalizeRoiPercent(settings_.roiHeightPercent));
    HWND roiQ = GetDlgItem(settingsHwnd_, IDC_SLIDER_ROI_EDGE_Q);
    if (roiQ) SendMessageW(roiQ, TBM_SETPOS, TRUE, VideoStreamTuning::NormalizeRoiEdgeQualityReduction(settings_.roiEdgeQualityReduction));
    HWND roiScale = GetDlgItem(settingsHwnd_, IDC_SLIDER_ROI_EDGE_SCALE);
    if (roiScale) SendMessageW(roiScale, TBM_SETPOS, TRUE, VideoStreamTuning::NormalizeRoiEdgeScalePercent(settings_.roiEdgeScalePercent));
    HWND roiTop = GetDlgItem(settingsHwnd_, IDC_SLIDER_ROI_TOP_LOW_PERCENT);
    if (roiTop) SendMessageW(roiTop, TBM_SETPOS, TRUE, VideoStreamTuning::NormalizeRoiTopLowPercent(settings_.roiTopLowPercent));
    HWND roiCenterWidth = GetDlgItem(settingsHwnd_, IDC_SLIDER_ROI_JPEG_CENTER_WIDTH_PERCENT);
    if (roiCenterWidth) SendMessageW(roiCenterWidth, TBM_SETPOS, TRUE, VideoStreamTuning::NormalizeRoiJpegCenterWidthPercent(settings_.roiJpegCenterWidthPercent));
    HWND roiWeight = GetDlgItem(settingsHwnd_, IDC_SLIDER_ROI_CENTER_WEIGHT_PERCENT);
    if (roiWeight) SendMessageW(roiWeight, TBM_SETPOS, TRUE, VideoStreamTuning::NormalizeRoiCenterCpuWeightPercent(settings_.roiCenterCpuWeightPercent));
    HWND bigCoreWeight = GetDlgItem(settingsHwnd_, IDC_SLIDER_ROI_BIG_CORE_WEIGHT_PERCENT);
    if (bigCoreWeight) SendMessageW(bigCoreWeight, TBM_SETPOS, TRUE, VideoStreamTuning::NormalizeRoiBigCoreWeightPercent(settings_.roiBigCoreWeightPercent));
    HWND centerOnly = GetDlgItem(settingsHwnd_, IDC_CHECK_JPEG_CENTER_ONLY);
    if (centerOnly) SendMessageW(centerOnly, BM_SETCHECK, settings_.jpegCenterOnly ? BST_CHECKED : BST_UNCHECKED, 0);
    CheckRadioButton(settingsHwnd_, IDC_RADIO_CENTER_ROI_VIDEO, IDC_RADIO_CENTER_ROI_JPEG,
        settings_.centerRoiUseVideo ? IDC_RADIO_CENTER_ROI_VIDEO : IDC_RADIO_CENTER_ROI_JPEG);
    CheckRadioButton(settingsHwnd_, IDC_RADIO_ROI_REGION_CENTER, IDC_RADIO_ROI_REGION_BOTTOM,
        settings_.roiRegion == ROI_REGION_BOTTOM ? IDC_RADIO_ROI_REGION_BOTTOM : IDC_RADIO_ROI_REGION_CENTER);
    updateVideoControlLabels();
    updateRoiControlLabels();
    updateStreamControlLabels();
    updateSettingsCompactLayout();

}

// createSettingsControls
void D3D11Renderer::createSettingsControls(HWND hwnd) {
    settingsHwnd_ = hwnd;
    if (!settingsBgBrush_) settingsBgBrush_ = CreateSolidBrush(RGB(12, 18, 28));
    if (!settingsCardBrush_) settingsCardBrush_ = CreateSolidBrush(RGB(22, 31, 46));
    if (!settingsFieldBrush_) settingsFieldBrush_ = CreateSolidBrush(RGB(238, 242, 247));
    if (!settingsFont_) {
        settingsFont_ = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    }
    if (!settingsTitleFont_) {
        settingsTitleFont_ = CreateFontW(-22, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    }

    // 顶部标题已移除：设置窗口直接从功能区开始，减少无效占用。
    // 左上：画面控制卡片
    createButton(hwnd, L"画面参数", 0, 18, 12, 420, 350, BS_GROUPBOX);
    createLabel(hwnd, L"采集模式", 36, 40, 70, 20);
    HWND capFull = CreateWindowW(L"BUTTON", L"全屏缩放", WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | WS_GROUP,
        116, 38, 92, 22, hwnd, reinterpret_cast<HMENU>(static_cast<intptr_t>(IDC_RADIO_CAPTURE_FULLSCREEN)), nullptr, nullptr);
    HWND capCrop = CreateWindowW(L"BUTTON", L"中心裁剪", WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
        212, 38, 92, 22, hwnd, reinterpret_cast<HMENU>(static_cast<intptr_t>(IDC_RADIO_CAPTURE_CROP)), nullptr, nullptr);
    if (capFull && settingsFont_) SendMessageW(capFull, WM_SETFONT, reinterpret_cast<WPARAM>(settingsFont_), TRUE);
    if (capCrop && settingsFont_) SendMessageW(capCrop, WM_SETFONT, reinterpret_cast<WPARAM>(settingsFont_), TRUE);

    createLabel(hwnd, L"分辨率", 36, 70, 70, 20);
    HWND r900 = CreateWindowW(L"BUTTON", L"900p", WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | WS_GROUP,
        116, 68, 62, 22, hwnd, reinterpret_cast<HMENU>(static_cast<intptr_t>(IDC_RADIO_PRESET_900P)), nullptr, nullptr);
    HWND r1080 = CreateWindowW(L"BUTTON", L"1080p", WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
        180, 68, 76, 22, hwnd, reinterpret_cast<HMENU>(static_cast<intptr_t>(IDC_RADIO_PRESET_1080P)), nullptr, nullptr);
    HWND r2k = CreateWindowW(L"BUTTON", L"2K", WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
        260, 68, 48, 22, hwnd, reinterpret_cast<HMENU>(static_cast<intptr_t>(IDC_RADIO_PRESET_2K)), nullptr, nullptr);
    HWND r3k = CreateWindowW(L"BUTTON", L"3K", WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
        312, 68, 48, 22, hwnd, reinterpret_cast<HMENU>(static_cast<intptr_t>(IDC_RADIO_PRESET_3K)), nullptr, nullptr);
    HWND rNative = CreateWindowW(L"BUTTON", L"设备最大", WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
        116, 92, 92, 22, hwnd, reinterpret_cast<HMENU>(static_cast<intptr_t>(IDC_RADIO_PRESET_NATIVE)), nullptr, nullptr);
    HWND rr[] = { r900, r1080, r2k, r3k, rNative };
    for (HWND h : rr) if (h && settingsFont_) SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(settingsFont_), TRUE);

    createLabel(hwnd, L"采样", 36, 120, 78, 20);
    HWND jpg420 = CreateWindowW(L"BUTTON", L"  高质", WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | WS_GROUP,
        116, 118, 104, 22, hwnd, reinterpret_cast<HMENU>(static_cast<intptr_t>(IDC_RADIO_JPEG_420)), nullptr, nullptr);
    HWND jpg444 = CreateWindowW(L"BUTTON", L"  极限", WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
        224, 118, 110, 22, hwnd, reinterpret_cast<HMENU>(static_cast<intptr_t>(IDC_RADIO_JPEG_444)), nullptr, nullptr);
    if (jpg420 && settingsFont_) SendMessageW(jpg420, WM_SETFONT, reinterpret_cast<WPARAM>(settingsFont_), TRUE);
    if (jpg444 && settingsFont_) SendMessageW(jpg444, WM_SETFONT, reinterpret_cast<WPARAM>(settingsFont_), TRUE);

    createLabel(hwnd, L"质量", 36, 152, 70, 20);
    HWND qSlider = CreateWindowExW(0, TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS | TBS_TOOLTIPS,
        116, 144, 238, 30, hwnd, reinterpret_cast<HMENU>(static_cast<intptr_t>(IDC_SLIDER_JPEG_QUALITY)), nullptr, nullptr);
    if (qSlider) {
        SendMessageW(qSlider, TBM_SETRANGE, TRUE, MAKELPARAM(60, 100));
        SendMessageW(qSlider, TBM_SETTICFREQ, 10, 0);
        SendMessageW(qSlider, TBM_SETPAGESIZE, 0, 5);
    }
    HWND qValue = CreateWindowW(L"STATIC", L"Q100", WS_CHILD | WS_VISIBLE | SS_CENTER,
        360, 152, 54, 20, hwnd, reinterpret_cast<HMENU>(static_cast<intptr_t>(IDC_STATIC_JPEG_QUALITY_VALUE)), nullptr, nullptr);
    if (qValue && settingsFont_) SendMessageW(qValue, WM_SETFONT, reinterpret_cast<WPARAM>(settingsFont_), TRUE);

    createLabel(hwnd, L"FPS", 36, 184, 70, 20);
    HWND fpsSlider = CreateWindowExW(0, TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS | TBS_TOOLTIPS,
        116, 176, 238, 30, hwnd, reinterpret_cast<HMENU>(static_cast<intptr_t>(IDC_SLIDER_FPS)), nullptr, nullptr);
    if (fpsSlider) {
        SendMessageW(fpsSlider, TBM_SETRANGE, TRUE, MAKELPARAM(0, 5));
        SendMessageW(fpsSlider, TBM_SETTICFREQ, 1, 0);
        SendMessageW(fpsSlider, TBM_SETPAGESIZE, 0, 1);
    }
    HWND fpsValue = CreateWindowW(L"STATIC", L"144 FPS", WS_CHILD | WS_VISIBLE | SS_CENTER,
        360, 184, 70, 20, hwnd, reinterpret_cast<HMENU>(static_cast<intptr_t>(IDC_STATIC_FPS_VALUE)), nullptr, nullptr);
    if (fpsValue && settingsFont_) SendMessageW(fpsValue, WM_SETFONT, reinterpret_cast<WPARAM>(settingsFont_), TRUE);

    HWND keep = CreateWindowW(L"BUTTON", L"保帧率降质", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        116, 214, 128, 22, hwnd, reinterpret_cast<HMENU>(static_cast<intptr_t>(IDC_CHECK_KEEP_FRAME)), nullptr, nullptr);
    if (keep && settingsFont_) SendMessageW(keep, WM_SETFONT, reinterpret_cast<WPARAM>(settingsFont_), TRUE);

    HWND stretchFrame = CreateWindowW(L"BUTTON", L"拉伸填满", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        260, 214, 120, 22, hwnd, reinterpret_cast<HMENU>(static_cast<intptr_t>(IDC_CHECK_STRETCH_FRAME)), nullptr, nullptr);
    if (stretchFrame && settingsFont_) SendMessageW(stretchFrame, WM_SETFONT, reinterpret_cast<WPARAM>(settingsFont_), TRUE);

    createLabel(hwnd, L"全屏核心数", 36, 246, 76, 20);
    HWND fullSplit = CreateWindowExW(0, TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS | TBS_TOOLTIPS,
        116, 238, 170, 30, hwnd, reinterpret_cast<HMENU>(static_cast<intptr_t>(IDC_SLIDER_FULLSCREEN_SPLIT)), nullptr, nullptr);
    if (fullSplit) { SendMessageW(fullSplit, TBM_SETRANGE, TRUE, MAKELPARAM(4, maxFullscreenSplitPartsForSettings())); SendMessageW(fullSplit, TBM_SETTICFREQ, 1, 0); }
    HWND fullSplitValue = CreateWindowW(L"STATIC", L"6 核心", WS_CHILD | WS_VISIBLE | SS_CENTER,
        292, 246, 70, 20, hwnd, reinterpret_cast<HMENU>(static_cast<intptr_t>(IDC_STATIC_FULLSCREEN_SPLIT_VALUE)), nullptr, nullptr);
    if (fullSplitValue && settingsFont_) SendMessageW(fullSplitValue, WM_SETFONT, reinterpret_cast<WPARAM>(settingsFont_), TRUE);

    createLabel(hwnd, L"裁剪核心数", 36, 278, 76, 20);
    HWND cropSplit = CreateWindowExW(0, TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS | TBS_TOOLTIPS,
        116, 270, 170, 30, hwnd, reinterpret_cast<HMENU>(static_cast<intptr_t>(IDC_SLIDER_CROP_SPLIT)), nullptr, nullptr);
    if (cropSplit) { SendMessageW(cropSplit, TBM_SETRANGE, TRUE, MAKELPARAM(1, 4)); SendMessageW(cropSplit, TBM_SETTICFREQ, 1, 0); }
    HWND cropSplitValue = CreateWindowW(L"STATIC", L"2 核心", WS_CHILD | WS_VISIBLE | SS_CENTER,
        292, 278, 70, 20, hwnd, reinterpret_cast<HMENU>(static_cast<intptr_t>(IDC_STATIC_CROP_SPLIT_VALUE)), nullptr, nullptr);
    if (cropSplitValue && settingsFont_) SendMessageW(cropSplitValue, WM_SETFONT, reinterpret_cast<WPARAM>(settingsFont_), TRUE);
    createLabel(hwnd, L"裁剪尺寸", 36, 318, 76, 20);

    HWND cropSize = CreateWindowExW(
        0,
        TRACKBAR_CLASSW,
        L"",
        WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS | TBS_TOOLTIPS,
        112, 310, 132, 30,
        hwnd,
        reinterpret_cast<HMENU>(static_cast<intptr_t>(IDC_SLIDER_CROP_SIZE)),
        nullptr,
        nullptr
    );

    if (cropSize) {
        SendMessageW(cropSize, TBM_SETRANGE, TRUE, MAKELPARAM(320, 1080));
        SendMessageW(cropSize, TBM_SETTICFREQ, 100, 0);
        SendMessageW(cropSize, TBM_SETPAGESIZE, 0, 20);
    }

    HWND cropSizeValue = CreateWindowW(
        L"STATIC",
        L"640 x 640",
        WS_CHILD | WS_VISIBLE | SS_CENTER,
        252, 318, 82, 20,
        hwnd,
        reinterpret_cast<HMENU>(static_cast<intptr_t>(IDC_STATIC_CROP_SIZE_VALUE)),
        nullptr,
        nullptr
    );

    if (cropSizeValue && settingsFont_) {
        SendMessageW(cropSizeValue, WM_SETFONT, reinterpret_cast<WPARAM>(settingsFont_), TRUE);
    }

    createButton(
        hwnd,
        L"应用画面参数",
        IDC_BUTTON_STREAM_APPLY,
        300, 350, 114, 28,
        BS_DEFPUSHBUTTON
    );

    // ADB/WiFi 控制移到窗口底部左侧，避免挤占标题栏和右侧 ROI/H.264 参数区。
    // 右上：ROI 策略。H.264 是省带宽/省 CPU 的可选路径；JPEG 分块参数始终服务低延迟路径。
    createButton(hwnd, L"ROI策略 / H.264省带宽", 0, 452, 12, 270, 706, BS_GROUPBOX);

    createLabelWithId(hwnd, L"区域类型", IDC_STATIC_ROI_CONTENT_LABEL, 472, 82, 70, 20);
    HWND roiVideo = CreateWindowW(L"BUTTON", L"H.264视频", WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | WS_GROUP,
        552, 78, 92, 22, hwnd, reinterpret_cast<HMENU>(static_cast<intptr_t>(IDC_RADIO_CENTER_ROI_VIDEO)), nullptr, nullptr);
    HWND roiJpeg = CreateWindowW(L"BUTTON", L"JPEG", WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
        650, 78, 66, 22, hwnd, reinterpret_cast<HMENU>(static_cast<intptr_t>(IDC_RADIO_CENTER_ROI_JPEG)), nullptr, nullptr);
    if (roiVideo && settingsFont_) SendMessageW(roiVideo, WM_SETFONT, reinterpret_cast<WPARAM>(settingsFont_), TRUE);
    if (roiJpeg && settingsFont_) SendMessageW(roiJpeg, WM_SETFONT, reinterpret_cast<WPARAM>(settingsFont_), TRUE);

    createLabelWithId(hwnd, L"码率Mbps", IDC_STATIC_VIDEO_BITRATE_LABEL, 472, 118, 70, 20);
    createValueEdit(hwnd, IDC_EDIT_VIDEO_BITRATE, 552, 114, 72, 24);
    createLabelWithId(hwnd, L"FPS", IDC_STATIC_VIDEO_FPS_LABEL, 636, 118, 34, 20);
    createValueEdit(hwnd, IDC_EDIT_VIDEO_FPS, 672, 114, 38, 24);
    createLabelWithId(hwnd, L"画质", IDC_STATIC_VIDEO_QUALITY_LABEL, 472, 154, 48, 20);
    HWND vqLow = CreateWindowW(L"BUTTON", L"低延迟I帧", WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | WS_GROUP,
        524, 150, 110, 22, hwnd, reinterpret_cast<HMENU>(static_cast<intptr_t>(IDC_RADIO_VIDEO_Q_LOW)), nullptr, nullptr);
    HWND vqHigh = CreateWindowW(L"BUTTON", L"高画质I帧", WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
        524, 176, 110, 22, hwnd, reinterpret_cast<HMENU>(static_cast<intptr_t>(IDC_RADIO_VIDEO_Q_HIGH)), nullptr, nullptr);
   
    HWND vqCtrls[] = { vqLow, vqHigh };
    for (HWND h : vqCtrls) if (h && settingsFont_) SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(settingsFont_), TRUE);
    createButton(hwnd, L"应用视频参数", IDC_BUTTON_VIDEO_APPLY, 524, 236, 122, 30, BS_DEFPUSHBUTTON);
    HWND videoStatus = CreateWindowW(L"STATIC", L"视频参数未发送", WS_CHILD | WS_VISIBLE,
        462, 278, 250, 34, hwnd, reinterpret_cast<HMENU>(static_cast<intptr_t>(IDC_STATIC_VIDEO_STATUS)), nullptr, nullptr);
    if (videoStatus && settingsFont_) SendMessageW(videoStatus, WM_SETFONT, reinterpret_cast<WPARAM>(settingsFont_), TRUE);

    createLabelWithId(hwnd, L"视频位置", IDC_STATIC_VIDEO_REGION_LABEL, 472, 320, 70, 20);
    HWND roiCenterRegion = CreateWindowW(L"BUTTON", L"中心", WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | WS_GROUP,
        552, 316, 62, 22, hwnd, reinterpret_cast<HMENU>(static_cast<intptr_t>(IDC_RADIO_ROI_REGION_CENTER)), nullptr, nullptr);
    HWND roiBottomRegion = CreateWindowW(L"BUTTON", L"底部", WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
        616, 316, 62, 22, hwnd, reinterpret_cast<HMENU>(static_cast<intptr_t>(IDC_RADIO_ROI_REGION_BOTTOM)), nullptr, nullptr);
    if (roiCenterRegion && settingsFont_) SendMessageW(roiCenterRegion, WM_SETFONT, reinterpret_cast<WPARAM>(settingsFont_), TRUE);
    if (roiBottomRegion && settingsFont_) SendMessageW(roiBottomRegion, WM_SETFONT, reinterpret_cast<WPARAM>(settingsFont_), TRUE);
    HWND strictSync = CreateWindowW(L"BUTTON", L"画面必须同步", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        552, 344, 140, 22, hwnd, reinterpret_cast<HMENU>(static_cast<intptr_t>(IDC_CHECK_STRICT_HYBRID_SYNC)), nullptr, nullptr);
    if (strictSync && settingsFont_) SendMessageW(strictSync, WM_SETFONT, reinterpret_cast<WPARAM>(settingsFont_), TRUE);

    createLabelWithId(hwnd, L"视频宽度", IDC_STATIC_ROI_VIDEO_WIDTH_LABEL, 472, 376, 70, 20);
    HWND roiWidthSlider = CreateWindowExW(0, TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS | TBS_TOOLTIPS,
        552, 368, 114, 30, hwnd, reinterpret_cast<HMENU>(static_cast<intptr_t>(IDC_SLIDER_ROI_WIDTH_PERCENT)), nullptr, nullptr);
    if (roiWidthSlider) { SendMessageW(roiWidthSlider, TBM_SETRANGE, TRUE, MAKELPARAM(VideoStreamTuning::kRoiPercentMin, VideoStreamTuning::kRoiPercentMax)); SendMessageW(roiWidthSlider, TBM_SETTICFREQ, 5, 0); SendMessageW(roiWidthSlider, TBM_SETPAGESIZE, 0, 2); }
    HWND roiWidthValue = CreateWindowW(L"STATIC", L"63%", WS_CHILD | WS_VISIBLE | SS_CENTER,
        670, 376, 50, 20, hwnd, reinterpret_cast<HMENU>(static_cast<intptr_t>(IDC_STATIC_ROI_WIDTH_VALUE)), nullptr, nullptr);
    if (roiWidthValue && settingsFont_) SendMessageW(roiWidthValue, WM_SETFONT, reinterpret_cast<WPARAM>(settingsFont_), TRUE);

    createLabelWithId(hwnd, L"视频高度", IDC_STATIC_ROI_VIDEO_HEIGHT_LABEL, 472, 408, 70, 20);
    HWND roiHeightSlider = CreateWindowExW(0, TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS | TBS_TOOLTIPS,
        552, 400, 114, 30, hwnd, reinterpret_cast<HMENU>(static_cast<intptr_t>(IDC_SLIDER_ROI_HEIGHT_PERCENT)), nullptr, nullptr);
    if (roiHeightSlider) { SendMessageW(roiHeightSlider, TBM_SETRANGE, TRUE, MAKELPARAM(VideoStreamTuning::kRoiPercentMin, VideoStreamTuning::kRoiPercentMax)); SendMessageW(roiHeightSlider, TBM_SETTICFREQ, 5, 0); SendMessageW(roiHeightSlider, TBM_SETPAGESIZE, 0, 2); }
    HWND roiHeightValue = CreateWindowW(L"STATIC", L"20%", WS_CHILD | WS_VISIBLE | SS_CENTER,
        670, 408, 50, 20, hwnd, reinterpret_cast<HMENU>(static_cast<intptr_t>(IDC_STATIC_ROI_HEIGHT_VALUE)), nullptr, nullptr);
    if (roiHeightValue && settingsFont_) SendMessageW(roiHeightValue, WM_SETFONT, reinterpret_cast<WPARAM>(settingsFont_), TRUE);

    createLabelWithId(hwnd, L"JPEG中高", IDC_STATIC_ROI_TOP_LOW_LABEL, 472, 440, 70, 20);
    HWND roiTopSlider = CreateWindowExW(0, TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS | TBS_TOOLTIPS,
        552, 432, 114, 30, hwnd, reinterpret_cast<HMENU>(static_cast<intptr_t>(IDC_SLIDER_ROI_TOP_LOW_PERCENT)), nullptr, nullptr);
    if (roiTopSlider) { SendMessageW(roiTopSlider, TBM_SETRANGE, TRUE, MAKELPARAM(VideoStreamTuning::kRoiTopLowPercentMin, VideoStreamTuning::kRoiTopLowPercentMax)); SendMessageW(roiTopSlider, TBM_SETTICFREQ, 5, 0); SendMessageW(roiTopSlider, TBM_SETPAGESIZE, 0, 2); }
    HWND roiTopValue = CreateWindowW(L"STATIC", L"30%", WS_CHILD | WS_VISIBLE | SS_CENTER,
        670, 440, 50, 20, hwnd, reinterpret_cast<HMENU>(static_cast<intptr_t>(IDC_STATIC_ROI_TOP_LOW_VALUE)), nullptr, nullptr);
    if (roiTopValue && settingsFont_) SendMessageW(roiTopValue, WM_SETFONT, reinterpret_cast<WPARAM>(settingsFont_), TRUE);

    createLabelWithId(hwnd, L"JPEG中宽", IDC_STATIC_ROI_JPEG_CENTER_WIDTH_LABEL, 472, 472, 70, 20);
    HWND roiCenterWidthSlider = CreateWindowExW(0, TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS | TBS_TOOLTIPS,
        552, 464, 114, 30, hwnd, reinterpret_cast<HMENU>(static_cast<intptr_t>(IDC_SLIDER_ROI_JPEG_CENTER_WIDTH_PERCENT)), nullptr, nullptr);
    if (roiCenterWidthSlider) { SendMessageW(roiCenterWidthSlider, TBM_SETRANGE, TRUE, MAKELPARAM(VideoStreamTuning::kRoiJpegCenterWidthPercentMin, VideoStreamTuning::kRoiJpegCenterWidthPercentMax)); SendMessageW(roiCenterWidthSlider, TBM_SETTICFREQ, 5, 0); SendMessageW(roiCenterWidthSlider, TBM_SETPAGESIZE, 0, 2); }
    HWND roiCenterWidthValue = CreateWindowW(L"STATIC", L"30%", WS_CHILD | WS_VISIBLE | SS_CENTER,
        670, 472, 50, 20, hwnd, reinterpret_cast<HMENU>(static_cast<intptr_t>(IDC_STATIC_ROI_JPEG_CENTER_WIDTH_VALUE)), nullptr, nullptr);
    if (roiCenterWidthValue && settingsFont_) SendMessageW(roiCenterWidthValue, WM_SETFONT, reinterpret_cast<WPARAM>(settingsFont_), TRUE);

    createLabelWithId(hwnd, L"中心核心", IDC_STATIC_ROI_CENTER_WEIGHT_LABEL, 472, 504, 70, 20);
    HWND roiWeightSlider = CreateWindowExW(0, TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS | TBS_TOOLTIPS,
        552, 496, 114, 30, hwnd, reinterpret_cast<HMENU>(static_cast<intptr_t>(IDC_SLIDER_ROI_CENTER_WEIGHT_PERCENT)), nullptr, nullptr);
    if (roiWeightSlider) { SendMessageW(roiWeightSlider, TBM_SETRANGE, TRUE, MAKELPARAM(VideoStreamTuning::kRoiCenterCoreCountMin, VideoStreamTuning::kRoiCenterCoreCountMax)); SendMessageW(roiWeightSlider, TBM_SETTICFREQ, 1, 0); SendMessageW(roiWeightSlider, TBM_SETPAGESIZE, 0, 1); }
    HWND roiWeightValue = CreateWindowW(L"STATIC", L"4核", WS_CHILD | WS_VISIBLE | SS_CENTER,
        670, 504, 50, 20, hwnd, reinterpret_cast<HMENU>(static_cast<intptr_t>(IDC_STATIC_ROI_CENTER_WEIGHT_VALUE)), nullptr, nullptr);
    if (roiWeightValue && settingsFont_) SendMessageW(roiWeightValue, WM_SETFONT, reinterpret_cast<WPARAM>(settingsFont_), TRUE);

    createLabelWithId(hwnd, L"大核权重", IDC_STATIC_ROI_BIG_CORE_WEIGHT_LABEL, 472, 536, 70, 20);
    HWND bigCoreWeightSlider = CreateWindowExW(0, TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS | TBS_TOOLTIPS,
        552, 528, 114, 30, hwnd, reinterpret_cast<HMENU>(static_cast<intptr_t>(IDC_SLIDER_ROI_BIG_CORE_WEIGHT_PERCENT)), nullptr, nullptr);
    if (bigCoreWeightSlider) { SendMessageW(bigCoreWeightSlider, TBM_SETRANGE, TRUE, MAKELPARAM(VideoStreamTuning::kRoiBigCoreWeightPercentMin, VideoStreamTuning::kRoiBigCoreWeightPercentMax)); SendMessageW(bigCoreWeightSlider, TBM_SETTICFREQ, 10, 0); SendMessageW(bigCoreWeightSlider, TBM_SETPAGESIZE, 0, 10); }
    HWND bigCoreWeightValue = CreateWindowW(L"STATIC", L"120%", WS_CHILD | WS_VISIBLE | SS_CENTER,
        670, 536, 50, 20, hwnd, reinterpret_cast<HMENU>(static_cast<intptr_t>(IDC_STATIC_ROI_BIG_CORE_WEIGHT_VALUE)), nullptr, nullptr);
    if (bigCoreWeightValue && settingsFont_) SendMessageW(bigCoreWeightValue, WM_SETFONT, reinterpret_cast<WPARAM>(settingsFont_), TRUE);

    HWND centerOnlyCheck = CreateWindowW(L"BUTTON", L"只投中心区域", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        552, 558, 150, 22, hwnd, reinterpret_cast<HMENU>(static_cast<intptr_t>(IDC_CHECK_JPEG_CENTER_ONLY)), nullptr, nullptr);
    if (centerOnlyCheck && settingsFont_) SendMessageW(centerOnlyCheck, WM_SETFONT, reinterpret_cast<WPARAM>(settingsFont_), TRUE);

    createLabelWithId(hwnd, L"JPEG外围降Q", IDC_STATIC_ROI_EDGE_Q_LABEL, 472, 588, 70, 20);
    HWND roiQSlider = CreateWindowExW(0, TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS | TBS_TOOLTIPS,
        552, 580, 114, 30, hwnd, reinterpret_cast<HMENU>(static_cast<intptr_t>(IDC_SLIDER_ROI_EDGE_Q)), nullptr, nullptr);
    if (roiQSlider) { SendMessageW(roiQSlider, TBM_SETRANGE, TRUE, MAKELPARAM(VideoStreamTuning::kRoiEdgeQualityReductionMin, VideoStreamTuning::kRoiEdgeQualityReductionMax)); SendMessageW(roiQSlider, TBM_SETTICFREQ, 5, 0); SendMessageW(roiQSlider, TBM_SETPAGESIZE, 0, 5); }
    HWND roiQValue = CreateWindowW(L"STATIC", L"Q-10", WS_CHILD | WS_VISIBLE | SS_CENTER,
        670, 588, 50, 20, hwnd, reinterpret_cast<HMENU>(static_cast<intptr_t>(IDC_STATIC_ROI_EDGE_Q_VALUE)), nullptr, nullptr);
    if (roiQValue && settingsFont_) SendMessageW(roiQValue, WM_SETFONT, reinterpret_cast<WPARAM>(settingsFont_), TRUE);

    createLabelWithId(hwnd, L"JPEG外围缩放", IDC_STATIC_ROI_EDGE_SCALE_LABEL, 472, 620, 70, 20);
    HWND roiScaleSlider = CreateWindowExW(0, TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS | TBS_TOOLTIPS,
        552, 612, 114, 30, hwnd, reinterpret_cast<HMENU>(static_cast<intptr_t>(IDC_SLIDER_ROI_EDGE_SCALE)), nullptr, nullptr);
    if (roiScaleSlider) { SendMessageW(roiScaleSlider, TBM_SETRANGE, TRUE, MAKELPARAM(VideoStreamTuning::kRoiEdgeScalePercentMin, VideoStreamTuning::kRoiEdgeScalePercentMax)); SendMessageW(roiScaleSlider, TBM_SETTICFREQ, 10, 0); SendMessageW(roiScaleSlider, TBM_SETPAGESIZE, 0, 5); }
    HWND roiScaleValue = CreateWindowW(L"STATIC", L"75%", WS_CHILD | WS_VISIBLE | SS_CENTER,
        670, 620, 50, 20, hwnd, reinterpret_cast<HMENU>(static_cast<intptr_t>(IDC_STATIC_ROI_EDGE_SCALE_VALUE)), nullptr, nullptr);
    if (roiScaleValue && settingsFont_) SendMessageW(roiScaleValue, WM_SETFONT, reinterpret_cast<WPARAM>(settingsFont_), TRUE);

    createButton(hwnd, L"应用ROI策略", IDC_BUTTON_ROI_APPLY, 524, 646, 122, 30);
    HWND roiStatus = CreateWindowW(L"STATIC", L"ROI策略未发送", WS_CHILD | WS_VISIBLE,
        462, 678, 250, 34, hwnd, reinterpret_cast<HMENU>(static_cast<intptr_t>(IDC_STATIC_ROI_STATUS)), nullptr, nullptr);
    if (roiStatus && settingsFont_) SendMessageW(roiStatus, WM_SETFONT, reinterpret_cast<WPARAM>(settingsFont_), TRUE);

    // CPU 权重控制已从窗口移除：native 侧按 CPU 频率/图像复杂度自动分配。

    // 左下：快捷键 / HUD / 声音卡片
    createButton(hwnd, L"快捷键 / HUD / 声音", 0, 18, 370, 420, 152, BS_GROUPBOX);
    const int kx1 = 36, ex1 = 120, kx2 = 230, ex2 = 320;
    createLabel(hwnd, L"HUD", kx1, 396, 70, 20); createEdit(hwnd, IDC_EDIT_HUD_KEY, ex1, 392, 82, 24);
    createLabel(hwnd, L"全屏主键", kx2, 396, 80, 20); createEdit(hwnd, IDC_EDIT_FULLSCREEN_KEY1, ex2, 392, 82, 24);
    createLabel(hwnd, L"全屏备用", kx1, 426, 80, 20); createEdit(hwnd, IDC_EDIT_FULLSCREEN_KEY2, ex1, 422, 82, 24);
    createLabel(hwnd, L"设置", kx2, 426, 70, 20); createEdit(hwnd, IDC_EDIT_SETTINGS_KEY, ex2, 422, 82, 24);
    createLabel(hwnd, L"退出", kx1, 456, 70, 20); createEdit(hwnd, IDC_EDIT_EXIT_KEY, ex1, 452, 82, 24);

    createLabel(hwnd, L"HUD刷新", kx2, 456, 80, 20);
    HWND hudSlider = CreateWindowExW(0, TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS | TBS_TOOLTIPS,
        ex2, 448, 82, 26, hwnd, reinterpret_cast<HMENU>(static_cast<intptr_t>(IDC_SLIDER_HUD_INTERVAL)), nullptr, nullptr);
    if (hudSlider) { SendMessageW(hudSlider, TBM_SETRANGE, TRUE, MAKELPARAM(100, 1000)); SendMessageW(hudSlider, TBM_SETTICFREQ, 100, 0); }
    HWND hudValue = CreateWindowW(L"STATIC", L"200 ms", WS_CHILD | WS_VISIBLE | SS_CENTER,
        ex2, 474, 82, 18, hwnd, reinterpret_cast<HMENU>(static_cast<intptr_t>(IDC_STATIC_HUD_INTERVAL_VALUE)), nullptr, nullptr);
    if (hudValue && settingsFont_) SendMessageW(hudValue, WM_SETFONT, reinterpret_cast<WPARAM>(settingsFont_), TRUE);

    createLabel(hwnd, L"音量", kx1, 486, 70, 20);
    HWND audioSlider = CreateWindowExW(0, TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS | TBS_TOOLTIPS,
        ex1, 478, 150, 26, hwnd, reinterpret_cast<HMENU>(static_cast<intptr_t>(IDC_SLIDER_AUDIO_VOLUME)), nullptr, nullptr);
    if (audioSlider) { SendMessageW(audioSlider, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100)); SendMessageW(audioSlider, TBM_SETTICFREQ, 10, 0); }
    HWND audioValue = CreateWindowW(L"STATIC", L"30%", WS_CHILD | WS_VISIBLE | SS_CENTER,
        278, 486, 56, 18, hwnd, reinterpret_cast<HMENU>(static_cast<intptr_t>(IDC_STATIC_AUDIO_VOLUME_VALUE)), nullptr, nullptr);
    if (audioValue && settingsFont_) SendMessageW(audioValue, WM_SETFONT, reinterpret_cast<WPARAM>(settingsFont_), TRUE);

    HWND compactHud = CreateWindowW(L"BUTTON", L"精简模式", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        36, 504, 96, 20, hwnd, reinterpret_cast<HMENU>(static_cast<intptr_t>(IDC_CHECK_COMPACT_HUD_MODE)), nullptr, nullptr);
    HWND debugHud = CreateWindowW(L"BUTTON", L"调试模式", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        150, 504, 96, 20, hwnd, reinterpret_cast<HMENU>(static_cast<intptr_t>(IDC_CHECK_DEBUG_HUD_MODE)), nullptr, nullptr);
    if (compactHud && settingsFont_) SendMessageW(compactHud, WM_SETFONT, reinterpret_cast<WPARAM>(settingsFont_), TRUE);
    if (debugHud && settingsFont_) SendMessageW(debugHud, WM_SETFONT, reinterpret_cast<WPARAM>(settingsFont_), TRUE);

    HWND jpgStatus = CreateWindowW(L"STATIC", L"画面参数未发送", WS_CHILD | WS_VISIBLE,
        24, 528, 420, 20, hwnd, reinterpret_cast<HMENU>(static_cast<intptr_t>(IDC_STATIC_JPEG_STATUS)), nullptr, nullptr);
    if (jpgStatus && settingsFont_) SendMessageW(jpgStatus, WM_SETFONT, reinterpret_cast<WPARAM>(settingsFont_), TRUE);

    // 左下：ADB / WiFi 连接。独立成组，避免和标题、ROI 参数互相挤压。
    createButton(hwnd, L"ADB / WiFi 连接", 0, 18, 552, 420, 72, BS_GROUPBOX);
    createLabel(hwnd, L"WiFi IP", 36, 578, 58, 20);
    HWND wifiIp1 = CreateWindowW(L"STATIC", L"192.", WS_CHILD | WS_VISIBLE | SS_RIGHT,
        96, 578, 34, 20, hwnd, reinterpret_cast<HMENU>(static_cast<intptr_t>(IDC_STATIC_WIFI_IP_1)), nullptr, nullptr);
    if (wifiIp1 && settingsFont_) SendMessageW(wifiIp1, WM_SETFONT, reinterpret_cast<WPARAM>(settingsFont_), TRUE);
    HWND wifiIp2 = createValueEdit(hwnd, IDC_EDIT_WIFI_IP_2, 132, 574, 36, 24);
    createLabel(hwnd, L".", 170, 578, 8, 20);
    HWND wifiIp3 = createValueEdit(hwnd, IDC_EDIT_WIFI_IP_3, 180, 574, 34, 24);
    createLabel(hwnd, L".", 216, 578, 8, 20);
    HWND wifiIp4 = createValueEdit(hwnd, IDC_EDIT_WIFI_IP_4, 226, 574, 36, 24);
    if (wifiIp2) SendMessageW(wifiIp2, EM_LIMITTEXT, 3, 0);
    if (wifiIp3) SendMessageW(wifiIp3, EM_LIMITTEXT, 3, 0);
    if (wifiIp4) SendMessageW(wifiIp4, EM_LIMITTEXT, 3, 0);
    createButton(hwnd, L"USB转无线", IDC_BUTTON_WIFI_TCPIP, 276, 572, 82, 26);
    createButton(hwnd, L"连接", IDC_BUTTON_WIFI_CONNECT, 366, 572, 54, 26, BS_DEFPUSHBUTTON);
    createButton(hwnd, adbWifiMode_ ? L"切到有线" : L"切到无线", IDC_BUTTON_ADB_TOGGLE, 36, 602, 88, 20);
    HWND wifiStatus = CreateWindowW(L"STATIC", L"ADB：当前有线；无线端口固定5555", WS_CHILD | WS_VISIBLE,
        132, 602, 294, 20, hwnd, reinterpret_cast<HMENU>(static_cast<intptr_t>(IDC_STATIC_WIFI_STATUS)), nullptr, nullptr);
    if (wifiStatus && settingsFont_) SendMessageW(wifiStatus, WM_SETFONT, reinterpret_cast<WPARAM>(settingsFont_), TRUE);

    createButton(hwnd, L"应用全部", IDC_BUTTON_APPLY, 70, 636, 112, 30, BS_DEFPUSHBUTTON);
    createButton(hwnd, L"恢复默认", IDC_BUTTON_RESET, 194, 636, 112, 30);
    createButton(hwnd, L"关闭", IDC_BUTTON_CLOSE, 318, 636, 82, 30);


    fillSettingsControls();
    updateSettingsCompactLayout();
    tryAutoFillWifiIpFromUsb(false, false);
    SetFocus(GetDlgItem(hwnd, IDC_EDIT_HUD_KEY));
    armSettingsHotkeyCapture(hwnd);
}

