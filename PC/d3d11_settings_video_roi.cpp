#include "d3d11_renderer.h"

// getIntFromEdit
int D3D11Renderer::getIntFromEdit(int id, int fallback, int lo, int hi) const {
    if (!settingsHwnd_) return clampInt(fallback, lo, hi);
    wchar_t buf[64]{};
    GetWindowTextW(GetDlgItem(settingsHwnd_, id), buf, 64);
    int value = _wtoi(buf);
    if (value == 0 && buf[0] != L'0') value = fallback;
    return clampInt(value, lo, hi);
}

// setIntEdit
void D3D11Renderer::setIntEdit(int id, int value) {
    if (!settingsHwnd_) return;
    wchar_t buf[64]{};
    std::swprintf(buf, 64, L"%d", value);
    SetWindowTextW(GetDlgItem(settingsHwnd_, id), buf);
}

// videoQualityLabel
const wchar_t* D3D11Renderer::videoQualityLabel(int mode) const {
    switch (mode) {
    case 0: return L"低延迟全I帧";
    default: return L"高画质全I帧";
    }
}

// videoPresetLabel
const wchar_t* D3D11Renderer::videoPresetLabel(int /*preset*/) const {
    return fullscreenPresetText(settings_.fullscreenPreset);
}

// selectedVideoQualityModeFromControls
int D3D11Renderer::selectedVideoQualityModeFromControls() const {
    if (!settingsHwnd_) return settings_.videoQualityMode;
    if (SendMessageW(GetDlgItem(settingsHwnd_, IDC_RADIO_VIDEO_Q_LOW), BM_GETCHECK, 0, 0) == BST_CHECKED) return 0;
    if (SendMessageW(GetDlgItem(settingsHwnd_, IDC_RADIO_VIDEO_Q_GOP), BM_GETCHECK, 0, 0) == BST_CHECKED) return 2;
    return 1;
}

// selectedVideoPresetFromControls
int D3D11Renderer::selectedVideoPresetFromControls() const {
    return clampInt(settings_.fullscreenPreset, 1, 5);
}

// updateVideoControlLabels
void D3D11Renderer::updateVideoControlLabels() {
    if (!settingsHwnd_) return;
    const bool showVideoControls = settings_.centerRoiUseVideo && !settings_.compactHudMode;

    const int videoIds[] = {
        IDC_STATIC_VIDEO_BITRATE_LABEL, IDC_EDIT_VIDEO_BITRATE,
        IDC_STATIC_VIDEO_FPS_LABEL, IDC_EDIT_VIDEO_FPS,
        IDC_STATIC_VIDEO_QUALITY_LABEL,
        IDC_RADIO_VIDEO_Q_LOW, IDC_RADIO_VIDEO_Q_HIGH, IDC_RADIO_VIDEO_Q_GOP,
        IDC_BUTTON_VIDEO_APPLY, IDC_STATIC_VIDEO_STATUS
    };
    for (int id : videoIds) setCtrlVisible(id, showVideoControls);

    CheckRadioButton(settingsHwnd_, IDC_RADIO_VIDEO_Q_LOW, IDC_RADIO_VIDEO_Q_GOP,
        settings_.videoQualityMode == 0 ? IDC_RADIO_VIDEO_Q_LOW :
        (settings_.videoQualityMode == 2 ? IDC_RADIO_VIDEO_Q_GOP : IDC_RADIO_VIDEO_Q_HIGH));

    HWND status = GetDlgItem(settingsHwnd_, IDC_STATIC_VIDEO_STATUS);
    if (status) {
        wchar_t buf[256]{};
        std::swprintf(buf, 256, L"视频：跟随画面%ls  %dMbps  %dfps  %ls",
            fullscreenPresetText(settings_.fullscreenPreset), settings_.videoBitrateMbps,
            settings_.videoFps, videoQualityLabel(settings_.videoQualityMode));
        SetWindowTextW(status, buf);
    }
}

// readVideoSettingsFromControlsIfOpen
//视频流 帧率和码率配置
void D3D11Renderer::readVideoSettingsFromControlsIfOpen() {
    if (!settingsHwnd_) return;
    settings_.videoPreset = selectedVideoPresetFromControls();
    settings_.videoBitrateMbps = getIntFromEdit(
        IDC_EDIT_VIDEO_BITRATE,
        settings_.videoBitrateMbps,
        VideoStreamTuning::kVideoBitrateMinMbps,
        VideoStreamTuning::kVideoBitrateMaxMbps);
    settings_.videoFps = getIntFromEdit(
        IDC_EDIT_VIDEO_FPS,
        settings_.videoFps,
        VideoStreamTuning::kVideoFpsMin,
        VideoStreamTuning::kVideoFpsMax);
    settings_.videoQualityMode = VideoStreamTuning::NormalizeVideoQualityMode(selectedVideoQualityModeFromControls());
}

// applyVideoSettingsFromControls
bool D3D11Renderer::applyVideoSettingsFromControls(bool showMessage) {
    if (settingsHwnd_) {
        HWND video = GetDlgItem(settingsHwnd_, IDC_RADIO_CENTER_ROI_VIDEO);
        settings_.centerRoiUseVideo = !video || SendMessageW(video, BM_GETCHECK, 0, 0) == BST_CHECKED;
    }
    if (!settings_.centerRoiUseVideo) {
        updateVideoControlLabels();
        return true;
    }

    readVideoSettingsFromControlsIfOpen();
    settings_.videoPreset = selectedVideoPresetFromControls();
    settings_.videoBitrateMbps = VideoStreamTuning::NormalizeVideoBitrateMbps(settings_.videoBitrateMbps);
    settings_.videoFps = VideoStreamTuning::NormalizeVideoFps(settings_.videoFps);
    settings_.videoQualityMode = VideoStreamTuning::NormalizeVideoQualityMode(settings_.videoQualityMode);
    char cmd[128]{};
    std::snprintf(cmd, sizeof(cmd), "HLVIDPRESET %d %d %d %d\n",
        settings_.videoPreset, settings_.videoBitrateMbps,
        settings_.videoFps, settings_.videoQualityMode);
    const bool ok = sendVideoControlLineToAndroid(cmd);
    updateVideoControlLabels();
    HWND status = GetDlgItem(settingsHwnd_, IDC_STATIC_VIDEO_STATUS);
    if (status) {
        wchar_t buf[260]{};
        std::swprintf(buf, 260, L"视频：跟随画面%ls %dMbps %dfps %ls  %ls",
            fullscreenPresetText(settings_.fullscreenPreset), settings_.videoBitrateMbps,
            settings_.videoFps, videoQualityLabel(settings_.videoQualityMode),
            ok ? L"已发送到控制+视频通道" : L"未连接，连接后再应用");
        SetWindowTextW(status, buf);
    }
    if (showMessage && !ok) {
        MessageBoxW(settingsHwnd_, L"当前没有可用的安卓控制/视频连接，视频参数已保存在PC窗口；连接后再点击应用视频参数。", L"H.264视频参数", MB_ICONINFORMATION);
    }
    return ok;
}

// updateRoiControlLabels
void D3D11Renderer::updateRoiControlLabels() {
    if (!settingsHwnd_) return;
    CheckRadioButton(settingsHwnd_, IDC_RADIO_CENTER_ROI_VIDEO, IDC_RADIO_CENTER_ROI_JPEG,
        settings_.centerRoiUseVideo ? IDC_RADIO_CENTER_ROI_VIDEO : IDC_RADIO_CENTER_ROI_JPEG);
    CheckRadioButton(settingsHwnd_, IDC_RADIO_ROI_REGION_CENTER, IDC_RADIO_ROI_REGION_BOTTOM,
        settings_.roiRegion == ROI_REGION_BOTTOM ? IDC_RADIO_ROI_REGION_BOTTOM : IDC_RADIO_ROI_REGION_CENTER);
    HWND sync = GetDlgItem(settingsHwnd_, IDC_CHECK_STRICT_HYBRID_SYNC);
    if (sync) SendMessageW(sync, BM_SETCHECK, settings_.strictHybridSync ? BST_CHECKED : BST_UNCHECKED, 0);
    HWND centerOnlyCheck = GetDlgItem(settingsHwnd_, IDC_CHECK_JPEG_CENTER_ONLY);
    if (centerOnlyCheck) SendMessageW(centerOnlyCheck, BM_SETCHECK, settings_.jpegCenterOnly ? BST_CHECKED : BST_UNCHECKED, 0);

    const bool panelVisible = !settings_.compactHudMode;
    const bool useVideo = settings_.centerRoiUseVideo && panelVisible;
    const bool bottomVideo = useVideo && settings_.roiRegion == ROI_REGION_BOTTOM;
    const bool centerVideo = useVideo && settings_.roiRegion == ROI_REGION_CENTER;
    const bool jpegLayout = panelVisible && (!settings_.centerRoiUseVideo || bottomVideo);

    // 这些控件是 ROI 面板的基础入口，不能只在创建窗口时显示。
    // 从精简模式或某些应用流程恢复后，它们可能已经被隐藏，必须每次刷新时恢复。
    setCtrlVisible(IDC_STATIC_ROI_CONTENT_LABEL, panelVisible);
    setCtrlVisible(IDC_RADIO_CENTER_ROI_VIDEO, panelVisible);
    setCtrlVisible(IDC_RADIO_CENTER_ROI_JPEG, panelVisible);
    setCtrlVisible(IDC_BUTTON_ROI_APPLY, panelVisible);
    setCtrlVisible(IDC_STATIC_ROI_STATUS, panelVisible);

    setCtrlVisible(IDC_STATIC_VIDEO_REGION_LABEL, useVideo);
    setCtrlVisible(IDC_RADIO_ROI_REGION_CENTER, useVideo);
    setCtrlVisible(IDC_RADIO_ROI_REGION_BOTTOM, useVideo);
    setCtrlVisible(IDC_CHECK_STRICT_HYBRID_SYNC, centerVideo && !settings_.jpegCenterOnly);

    setCtrlVisible(IDC_STATIC_ROI_VIDEO_WIDTH_LABEL, centerVideo);
    setCtrlVisible(IDC_SLIDER_ROI_WIDTH_PERCENT, centerVideo);
    setCtrlVisible(IDC_STATIC_ROI_WIDTH_VALUE, centerVideo);
    setCtrlVisible(IDC_STATIC_ROI_VIDEO_HEIGHT_LABEL, useVideo);
    setCtrlVisible(IDC_SLIDER_ROI_HEIGHT_PERCENT, useVideo);
    setCtrlVisible(IDC_STATIC_ROI_HEIGHT_VALUE, useVideo);
    setCtrlText(IDC_STATIC_ROI_VIDEO_HEIGHT_LABEL, bottomVideo ? L"视频流底部高度" : L"视频流高度");
    setCtrlText(IDC_STATIC_ROI_TOP_LOW_LABEL, L"JPEG中高");
    setCtrlText(IDC_STATIC_ROI_JPEG_CENTER_WIDTH_LABEL, L"JPEG中宽");

    setCtrlVisible(IDC_STATIC_ROI_TOP_LOW_LABEL, jpegLayout);
    setCtrlVisible(IDC_SLIDER_ROI_TOP_LOW_PERCENT, jpegLayout);
    setCtrlVisible(IDC_STATIC_ROI_TOP_LOW_VALUE, jpegLayout);
    setCtrlVisible(IDC_STATIC_ROI_JPEG_CENTER_WIDTH_LABEL, jpegLayout);
    setCtrlVisible(IDC_SLIDER_ROI_JPEG_CENTER_WIDTH_PERCENT, jpegLayout);
    setCtrlVisible(IDC_STATIC_ROI_JPEG_CENTER_WIDTH_VALUE, jpegLayout);
    // 新 JPEG 中心矩形方案：中心核心数由用户手动指定。
    // 继续复用旧的 CENTER_WEIGHT 控件 ID，但显示语义改为“中心核心”。
    setCtrlText(IDC_STATIC_ROI_CENTER_WEIGHT_LABEL, L"中心核心");
    setCtrlVisible(IDC_STATIC_ROI_CENTER_WEIGHT_LABEL, jpegLayout);
    setCtrlVisible(IDC_SLIDER_ROI_CENTER_WEIGHT_PERCENT, jpegLayout);
    setCtrlVisible(IDC_STATIC_ROI_CENTER_WEIGHT_VALUE, jpegLayout);
    setCtrlText(IDC_STATIC_ROI_BIG_CORE_WEIGHT_LABEL, L"大核权重");
    setCtrlVisible(IDC_STATIC_ROI_BIG_CORE_WEIGHT_LABEL, jpegLayout);
    setCtrlVisible(IDC_SLIDER_ROI_BIG_CORE_WEIGHT_PERCENT, jpegLayout);
    setCtrlVisible(IDC_STATIC_ROI_BIG_CORE_WEIGHT_VALUE, jpegLayout);
    setCtrlVisible(IDC_CHECK_JPEG_CENTER_ONLY, panelVisible);

    // 外围 JPEG 参数对正常模式有效；只投中心区域时 Android/native 不再编码外围，
    // 因此隐藏外围降Q/缩放，避免用户误以为还会影响画面。
    const bool showOuterJpegControls = panelVisible && !settings_.jpegCenterOnly;
    setCtrlVisible(IDC_STATIC_ROI_EDGE_Q_LABEL, showOuterJpegControls);
    setCtrlVisible(IDC_SLIDER_ROI_EDGE_Q, showOuterJpegControls);
    setCtrlVisible(IDC_STATIC_ROI_EDGE_Q_VALUE, showOuterJpegControls);
    setCtrlVisible(IDC_STATIC_ROI_EDGE_SCALE_LABEL, showOuterJpegControls);
    setCtrlVisible(IDC_SLIDER_ROI_EDGE_SCALE, showOuterJpegControls);
    setCtrlVisible(IDC_STATIC_ROI_EDGE_SCALE_VALUE, showOuterJpegControls);

    updateVideoControlLabels();

    auto setPercentLabel = [&](int sliderId, int labelId, int lo, int hi) {
        HWND s = GetDlgItem(settingsHwnd_, sliderId);
        HWND l = GetDlgItem(settingsHwnd_, labelId);
        if (!s || !l) return;
        int value = clampInt(static_cast<int>(SendMessageW(s, TBM_GETPOS, 0, 0)), lo, hi);
        wchar_t buf[64]{};
        std::swprintf(buf, 64, L"%d%%", value);
        SetWindowTextW(l, buf);
        };

    setPercentLabel(IDC_SLIDER_ROI_WIDTH_PERCENT, IDC_STATIC_ROI_WIDTH_VALUE, VideoStreamTuning::kRoiPercentMin, VideoStreamTuning::kRoiPercentMax);
    setPercentLabel(IDC_SLIDER_ROI_HEIGHT_PERCENT, IDC_STATIC_ROI_HEIGHT_VALUE, VideoStreamTuning::kRoiPercentMin, VideoStreamTuning::kRoiPercentMax);
    setPercentLabel(IDC_SLIDER_ROI_TOP_LOW_PERCENT, IDC_STATIC_ROI_TOP_LOW_VALUE, VideoStreamTuning::kRoiTopLowPercentMin, VideoStreamTuning::kRoiTopLowPercentMax);
    setPercentLabel(IDC_SLIDER_ROI_JPEG_CENTER_WIDTH_PERCENT, IDC_STATIC_ROI_JPEG_CENTER_WIDTH_VALUE, VideoStreamTuning::kRoiJpegCenterWidthPercentMin, VideoStreamTuning::kRoiJpegCenterWidthPercentMax);

    HWND q = GetDlgItem(settingsHwnd_, IDC_SLIDER_ROI_EDGE_Q);
    HWND qv = GetDlgItem(settingsHwnd_, IDC_STATIC_ROI_EDGE_Q_VALUE);
    if (q && qv) {
        int value = VideoStreamTuning::NormalizeRoiEdgeQualityReduction(static_cast<int>(SendMessageW(q, TBM_GETPOS, 0, 0)));
        wchar_t buf[64]{};
        std::swprintf(buf, 64, L"Q-%d", value);
        SetWindowTextW(qv, buf);
    }
    HWND sc = GetDlgItem(settingsHwnd_, IDC_SLIDER_ROI_EDGE_SCALE);
    HWND sv = GetDlgItem(settingsHwnd_, IDC_STATIC_ROI_EDGE_SCALE_VALUE);
    if (sc && sv) {
        int value = VideoStreamTuning::NormalizeRoiEdgeScalePercent(static_cast<int>(SendMessageW(sc, TBM_GETPOS, 0, 0)));
        wchar_t buf[64]{};
        std::swprintf(buf, 64, L"%d%%", value);
        SetWindowTextW(sv, buf);
    }
    HWND cw = GetDlgItem(settingsHwnd_, IDC_SLIDER_ROI_CENTER_WEIGHT_PERCENT);
    HWND cwv = GetDlgItem(settingsHwnd_, IDC_STATIC_ROI_CENTER_WEIGHT_VALUE);
    if (cw && cwv) {
        const int requestedFullCores = clampInt(settings_.fullscreenSplitParts, 1, MAX_RUNTIME_SPLIT_PARTS);
        const int maxCenterCores = settings_.jpegCenterOnly ? requestedFullCores : (std::max)(1, requestedFullCores - 2);
        SendMessageW(cw, TBM_SETRANGE, TRUE, MAKELPARAM(VideoStreamTuning::kRoiCenterCoreCountMin, maxCenterCores));
        int value = VideoStreamTuning::NormalizeRoiCenterCoreCount(static_cast<int>(SendMessageW(cw, TBM_GETPOS, 0, 0)));
        value = clampInt(value, VideoStreamTuning::kRoiCenterCoreCountMin, maxCenterCores);
        if (static_cast<int>(SendMessageW(cw, TBM_GETPOS, 0, 0)) != value) {
            SendMessageW(cw, TBM_SETPOS, TRUE, value);
        }
        settings_.roiCenterCpuWeightPercent = value;
        wchar_t buf[64]{};
        std::swprintf(buf, 64, L"%d核", value);
        SetWindowTextW(cwv, buf);
    }
    HWND bw = GetDlgItem(settingsHwnd_, IDC_SLIDER_ROI_BIG_CORE_WEIGHT_PERCENT);
    HWND bwv = GetDlgItem(settingsHwnd_, IDC_STATIC_ROI_BIG_CORE_WEIGHT_VALUE);
    if (bw && bwv) {
        int value = VideoStreamTuning::NormalizeRoiBigCoreWeightPercent(static_cast<int>(SendMessageW(bw, TBM_GETPOS, 0, 0)));
        wchar_t buf[64]{};
        std::swprintf(buf, 64, L"%d%%", value);
        SetWindowTextW(bwv, buf);
    }

    HWND status = GetDlgItem(settingsHwnd_, IDC_STATIC_ROI_STATUS);
    if (status) {
        wchar_t buf[320]{};
        if (useVideo) {
            if (bottomVideo) {
                if (settings_.jpegCenterOnly) {
                    std::swprintf(buf, 320, L"ROI：底部H.264 高%d%%；只投JPEG中心 %d%%x%d%% %d核 大核%d%%",
                        settings_.roiHeightPercent,
                        settings_.roiJpegCenterWidthPercent,
                        settings_.roiTopLowPercent,
                        settings_.roiCenterCpuWeightPercent,
                        settings_.roiBigCoreWeightPercent);
                } else {
                    std::swprintf(buf, 320, L"ROI：底部H.264 高%d%%；JPEG 中高%d%% 中宽%d%% 中心%d核 大核%d%% Q-%d 缩放%d%%",
                        settings_.roiHeightPercent,
                        settings_.roiTopLowPercent,
                        settings_.roiJpegCenterWidthPercent,
                        settings_.roiCenterCpuWeightPercent,
                        settings_.roiBigCoreWeightPercent,
                        settings_.roiEdgeQualityReduction,
                        settings_.roiEdgeScalePercent);
                }
            }
            else {
                if (settings_.jpegCenterOnly) {
                    std::swprintf(buf, 320, L"ROI：只投中心H.264 %d%%x%d%%",
                        settings_.roiWidthPercent,
                        settings_.roiHeightPercent);
                } else {
                    std::swprintf(buf, 320, L"ROI：中心H.264 %d%%x%d%% %ls；外围JPEG Q-%d 缩放%d%%",
                        settings_.roiWidthPercent,
                        settings_.roiHeightPercent,
                        settings_.strictHybridSync ? L"同步" : L"异步",
                        settings_.roiEdgeQualityReduction,
                        settings_.roiEdgeScalePercent);
                }
            }
        }
        else {
            if (settings_.jpegCenterOnly) {
                std::swprintf(buf, 320, L"ROI：只投JPEG中心 %d%%x%d%% %d核 大核%d%%",
                    settings_.roiJpegCenterWidthPercent,
                    settings_.roiTopLowPercent,
                    settings_.roiCenterCpuWeightPercent,
                    settings_.roiBigCoreWeightPercent);
            } else {
                std::swprintf(buf, 320, L"ROI：JPEG低延迟 中高%d%% 中宽%d%% 中心%d核 大核%d%% Q-%d 缩放%d%%",
                    settings_.roiTopLowPercent,
                    settings_.roiJpegCenterWidthPercent,
                    settings_.roiCenterCpuWeightPercent,
                    settings_.roiBigCoreWeightPercent,
                    settings_.roiEdgeQualityReduction,
                    settings_.roiEdgeScalePercent);
            }
        }
        SetWindowTextW(status, buf);
    }
}

// readRoiSettingsFromControlsIfOpen
void D3D11Renderer::readRoiSettingsFromControlsIfOpen() {
    if (!settingsHwnd_) return;
    HWND video = GetDlgItem(settingsHwnd_, IDC_RADIO_CENTER_ROI_VIDEO);
    settings_.centerRoiUseVideo = !video || SendMessageW(video, BM_GETCHECK, 0, 0) == BST_CHECKED;
    HWND bottomRegion = GetDlgItem(settingsHwnd_, IDC_RADIO_ROI_REGION_BOTTOM);
    settings_.roiRegion = (bottomRegion && SendMessageW(bottomRegion, BM_GETCHECK, 0, 0) == BST_CHECKED)
        ? ROI_REGION_BOTTOM
        : ROI_REGION_CENTER;
    HWND sync = GetDlgItem(settingsHwnd_, IDC_CHECK_STRICT_HYBRID_SYNC);
    if (sync) settings_.strictHybridSync = SendMessageW(sync, BM_GETCHECK, 0, 0) == BST_CHECKED;
    state_.centerRoiStrictSync.store(settings_.centerRoiUseVideo && settings_.strictHybridSync, std::memory_order_release);
    HWND rw = GetDlgItem(settingsHwnd_, IDC_SLIDER_ROI_WIDTH_PERCENT);
    if (rw) settings_.roiWidthPercent = VideoStreamTuning::NormalizeRoiPercent(static_cast<int>(SendMessageW(rw, TBM_GETPOS, 0, 0)));
    HWND rh = GetDlgItem(settingsHwnd_, IDC_SLIDER_ROI_HEIGHT_PERCENT);
    if (rh) settings_.roiHeightPercent = VideoStreamTuning::NormalizeRoiPercent(static_cast<int>(SendMessageW(rh, TBM_GETPOS, 0, 0)));
    HWND q = GetDlgItem(settingsHwnd_, IDC_SLIDER_ROI_EDGE_Q);
    if (q) settings_.roiEdgeQualityReduction = VideoStreamTuning::NormalizeRoiEdgeQualityReduction(static_cast<int>(SendMessageW(q, TBM_GETPOS, 0, 0)));
    HWND sc = GetDlgItem(settingsHwnd_, IDC_SLIDER_ROI_EDGE_SCALE);
    if (sc) settings_.roiEdgeScalePercent = VideoStreamTuning::NormalizeRoiEdgeScalePercent(static_cast<int>(SendMessageW(sc, TBM_GETPOS, 0, 0)));
    HWND top = GetDlgItem(settingsHwnd_, IDC_SLIDER_ROI_TOP_LOW_PERCENT);
    if (top) settings_.roiTopLowPercent = VideoStreamTuning::NormalizeRoiTopLowPercent(static_cast<int>(SendMessageW(top, TBM_GETPOS, 0, 0)));
    HWND centerW = GetDlgItem(settingsHwnd_, IDC_SLIDER_ROI_JPEG_CENTER_WIDTH_PERCENT);
    if (centerW) settings_.roiJpegCenterWidthPercent = VideoStreamTuning::NormalizeRoiJpegCenterWidthPercent(static_cast<int>(SendMessageW(centerW, TBM_GETPOS, 0, 0)));
    HWND centerOnly = GetDlgItem(settingsHwnd_, IDC_CHECK_JPEG_CENTER_ONLY);
    if (centerOnly) settings_.jpegCenterOnly = SendMessageW(centerOnly, BM_GETCHECK, 0, 0) == BST_CHECKED;
    HWND weight = GetDlgItem(settingsHwnd_, IDC_SLIDER_ROI_CENTER_WEIGHT_PERCENT);
    if (weight) {
        const int requestedFullCores = clampInt(settings_.fullscreenSplitParts, 1, MAX_RUNTIME_SPLIT_PARTS);
        const int maxCenterCores = settings_.jpegCenterOnly ? requestedFullCores : (std::max)(1, requestedFullCores - 2);
        settings_.roiCenterCpuWeightPercent = clampInt(
            VideoStreamTuning::NormalizeRoiCenterCpuWeightPercent(static_cast<int>(SendMessageW(weight, TBM_GETPOS, 0, 0))),
            VideoStreamTuning::kRoiCenterCoreCountMin,
            maxCenterCores);
    }
    HWND bigWeight = GetDlgItem(settingsHwnd_, IDC_SLIDER_ROI_BIG_CORE_WEIGHT_PERCENT);
    if (bigWeight) settings_.roiBigCoreWeightPercent = VideoStreamTuning::NormalizeRoiBigCoreWeightPercent(static_cast<int>(SendMessageW(bigWeight, TBM_GETPOS, 0, 0)));
}

// sendRoiSettingsToAndroid
bool D3D11Renderer::sendRoiSettingsToAndroid() {
    settings_.roiEdgeQualityReduction = VideoStreamTuning::NormalizeRoiEdgeQualityReduction(settings_.roiEdgeQualityReduction);
    settings_.roiEdgeScalePercent = VideoStreamTuning::NormalizeRoiEdgeScalePercent(settings_.roiEdgeScalePercent);
    settings_.roiWidthPercent = VideoStreamTuning::NormalizeRoiPercent(settings_.roiWidthPercent);
    settings_.roiHeightPercent = VideoStreamTuning::NormalizeRoiPercent(settings_.roiHeightPercent);
    settings_.roiTopLowPercent = VideoStreamTuning::NormalizeRoiTopLowPercent(settings_.roiTopLowPercent);
    settings_.roiJpegCenterWidthPercent = VideoStreamTuning::NormalizeRoiJpegCenterWidthPercent(settings_.roiJpegCenterWidthPercent);
    settings_.roiCenterCpuWeightPercent = VideoStreamTuning::NormalizeRoiCenterCpuWeightPercent(settings_.roiCenterCpuWeightPercent);
    {
        const int requestedFullCores = clampInt(settings_.fullscreenSplitParts, 1, MAX_RUNTIME_SPLIT_PARTS);
        const int maxCenterCores = settings_.jpegCenterOnly ? requestedFullCores : (std::max)(1, requestedFullCores - 2);
        settings_.roiCenterCpuWeightPercent = clampInt(settings_.roiCenterCpuWeightPercent,
            VideoStreamTuning::kRoiCenterCoreCountMin, maxCenterCores);
    }
    settings_.roiBigCoreWeightPercent = VideoStreamTuning::NormalizeRoiBigCoreWeightPercent(settings_.roiBigCoreWeightPercent);
    settings_.roiRegion = VideoStreamTuning::NormalizeRoiRegion(settings_.roiRegion);
    const char* regionText = settings_.roiRegion == ROI_REGION_BOTTOM ? "bottom" : "center";
    char cmd[256]{};
    std::snprintf(cmd, sizeof(cmd), "HLROI %s %d %d %d %d %s %d %d %d %d %d\n",
        settings_.centerRoiUseVideo ? "video" : "jpeg",
        settings_.roiEdgeQualityReduction,
        settings_.roiEdgeScalePercent,
        settings_.roiWidthPercent,
        settings_.roiHeightPercent,
        regionText,
        settings_.roiTopLowPercent,
        settings_.roiJpegCenterWidthPercent,
        settings_.roiCenterCpuWeightPercent,
        settings_.jpegCenterOnly ? 1 : 0,
        settings_.roiBigCoreWeightPercent);

    // HLROI must be handled by PcScreenMirrorCoordinator on the main JPEG/control
    // socket because it changes both the native JPEG ROI strategy and whether the
    // Android capture pipeline punches the center hole.  Sending only to the H.264
    // video socket is not enough: CenterRoiVideoStreamSender only understands video
    // keyframe/preset commands and will ignore HLROI.
    bool okMain = sendControlLineToAndroid(cmd);

    // Also broadcast to the video socket for logging/future compatibility, but do
    // not treat that as success.  If okMain is false, Android did not apply HLROI.
    (void)sendVideoControlLineToAndroid(cmd);
    return okMain;
}

// applyRoiSettingsFromControls
bool D3D11Renderer::applyRoiSettingsFromControls(bool showMessage) {
    readRoiSettingsFromControlsIfOpen();
    const bool ok = sendRoiSettingsToAndroid();
    if (!settings_.centerRoiUseVideo) {
        clearCenterRoiOverlay();
    }
    updateRoiControlLabels();
    HWND status = GetDlgItem(settingsHwnd_, IDC_STATIC_ROI_STATUS);
    if (status) {
        wchar_t buf[320]{};
        const wchar_t* result = ok ? L"已发送" : L"未连接，连接后再应用";
        if (settings_.centerRoiUseVideo && settings_.roiRegion == ROI_REGION_CENTER) {
            if (settings_.jpegCenterOnly) {
                std::swprintf(buf, 320, L"ROI：只投中心H.264 %d%%x%d%%  %ls",
                    settings_.roiWidthPercent,
                    settings_.roiHeightPercent,
                    result);
            } else {
                std::swprintf(buf, 320, L"ROI：中心H.264 %d%%x%d%% 外围Q-%d 缩放%d%%  %ls",
                    settings_.roiWidthPercent,
                    settings_.roiHeightPercent,
                    settings_.roiEdgeQualityReduction,
                    settings_.roiEdgeScalePercent,
                    result);
            }
        }
        else if (settings_.centerRoiUseVideo) {
            if (settings_.jpegCenterOnly) {
                std::swprintf(buf, 320, L"ROI：底部H.264 高%d%%；只投JPEG中心 %d%%x%d%% %d核 大核%d%%  %ls",
                    settings_.roiHeightPercent,
                    settings_.roiJpegCenterWidthPercent,
                    settings_.roiTopLowPercent,
                    settings_.roiCenterCpuWeightPercent,
                    settings_.roiBigCoreWeightPercent,
                    result);
            } else {
                std::swprintf(buf, 320, L"ROI：底部H.264 高%d%%；JPEG 中高%d%% 中宽%d%% 中心%d核 大核%d%% Q-%d 缩放%d%%  %ls",
                    settings_.roiHeightPercent,
                    settings_.roiTopLowPercent,
                    settings_.roiJpegCenterWidthPercent,
                    settings_.roiCenterCpuWeightPercent,
                    settings_.roiBigCoreWeightPercent,
                    settings_.roiEdgeQualityReduction,
                    settings_.roiEdgeScalePercent,
                    result);
            }
        }
        else {
            if (settings_.jpegCenterOnly) {
                std::swprintf(buf, 320, L"ROI：只投JPEG中心 %d%%x%d%% %d核 大核%d%%  %ls",
                    settings_.roiJpegCenterWidthPercent,
                    settings_.roiTopLowPercent,
                    settings_.roiCenterCpuWeightPercent,
                    settings_.roiBigCoreWeightPercent,
                    result);
            } else {
                std::swprintf(buf, 320, L"ROI：JPEG低延迟 中高%d%% 中宽%d%% 中心%d核 大核%d%% Q-%d 缩放%d%%  %ls",
                    settings_.roiTopLowPercent,
                    settings_.roiJpegCenterWidthPercent,
                    settings_.roiCenterCpuWeightPercent,
                    settings_.roiBigCoreWeightPercent,
                    settings_.roiEdgeQualityReduction,
                    settings_.roiEdgeScalePercent,
                    result);
            }
        }
        SetWindowTextW(status, buf);
    }
    if (showMessage && !ok) {
        MessageBoxW(settingsHwnd_, L"当前没有可用的安卓控制/视频连接，ROI策略已保存在PC窗口；连接后再点击应用ROI策略。", L"ROI策略", MB_ICONINFORMATION);
    }
    return ok;
}

// selectedJpegSubsamplingModeFromControls
int D3D11Renderer::selectedJpegSubsamplingModeFromControls() {
    if (!settingsHwnd_) return settings_.jpegSubsamplingMode;
    HWND r444 = GetDlgItem(settingsHwnd_, IDC_RADIO_JPEG_444);
    if (r444 && SendMessageW(r444, BM_GETCHECK, 0, 0) == BST_CHECKED) return 444;
    return 420;
}

// jpegSubsamplingLabel
const wchar_t* D3D11Renderer::jpegSubsamplingLabel() const {
    return settings_.jpegSubsamplingMode == 444 ? L"极限" : L"高质";
}

// updateJpegSubsamplingControls
void D3D11Renderer::updateJpegSubsamplingControls() {
    if (!settingsHwnd_) return;
    CheckRadioButton(settingsHwnd_, IDC_RADIO_JPEG_420, IDC_RADIO_JPEG_444,
        settings_.jpegSubsamplingMode == 444 ? IDC_RADIO_JPEG_444 : IDC_RADIO_JPEG_420);
    HWND status = GetDlgItem(settingsHwnd_, IDC_STATIC_JPEG_STATUS);
    if (status) {
        SetWindowTextW(status,
            settings_.jpegSubsamplingMode == 444
            ? L"采样：极限保真（测试用，码率/编码/解码压力更高）"
            : L"采样：高质量（推荐默认，2K高刷更稳）");
    }
}

// applyJpegSubsamplingFromControls
bool D3D11Renderer::applyJpegSubsamplingFromControls(bool showMessage) {
    const int mode = selectedJpegSubsamplingModeFromControls();
    settings_.jpegSubsamplingMode = (mode == 444) ? 444 : 420;

    char cmd[64]{};
    std::snprintf(cmd, sizeof(cmd), "HLJPGSUB %d\n", settings_.jpegSubsamplingMode);
    const bool ok = sendControlLineToAndroid(cmd);
    updateJpegSubsamplingControls();

    HWND status = GetDlgItem(settingsHwnd_, IDC_STATIC_JPEG_STATUS);
    if (status) {
        wchar_t buf[180]{};
        std::swprintf(buf, 180, L"采样：%ls  %ls",
            settings_.jpegSubsamplingMode == 444 ? L"极限保真" : L"高质量",
            ok ? L"已发送到安卓" : L"未连接安卓，设置仅保存在PC窗口");
        SetWindowTextW(status, buf);
    }
    if (showMessage && !ok) {
        MessageBoxW(settingsHwnd_, L"当前没有可用的画面连接，采样设置已保存在PC窗口；连接后再点击应用设置。", L"JPEG采样", MB_ICONINFORMATION);
    }
    return ok;
}

