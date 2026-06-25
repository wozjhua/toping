#include "d3d11_renderer.h"

// maxFullscreenSplitPartsForSettings
int D3D11Renderer::maxFullscreenSplitPartsForSettings() const {
    const int nativeMax = cachedAvailableEncodeCpuCount_ > 1 ? cachedAvailableEncodeCpuCount_ : 7;
    // F2 窗口会按安卓回传的可用编码核心数动态限制上限；未知设备连接前先按7核心展示。
    return clampInt(nativeMax, 4, MAX_RUNTIME_SPLIT_PARTS);
}

// computeDefaultSplitWeightPercent
void D3D11Renderer::computeDefaultSplitWeightPercent(int count, int outPercent[MAX_RUNTIME_SPLIT_PARTS]) {
    count = clampInt(count, 4, MAX_RUNTIME_SPLIT_PARTS);
    double weights[MAX_RUNTIME_SPLIT_PARTS]{};
    for (int i = 0; i < MAX_RUNTIME_SPLIT_PARTS; ++i) {
        weights[i] = 1.0;
        outPercent[i] = 100;
    }
    // 与安卓当前固定加权保持一致的简化展示：前两个高性能核心略大，其余核心标准权重。
    // UI 这里展示的是“整幅图像按比例分给每个核心”的百分比，而不是 1.0x 这种难理解的倍数。
    if (count >= 3) {
        weights[0] = 1.16;
        weights[1] = 1.16;
    }
    double total = 0.0;
    for (int i = 0; i < count; ++i) total += weights[i];
    if (total <= 0.0) total = static_cast<double>(count);
    for (int i = 0; i < count; ++i) {
        const double normalized = weights[i] * static_cast<double>(count) / total;
        outPercent[i] = clampInt(static_cast<int>(normalized * 100.0 + 0.5), 70, 130);
    }
    for (int i = count; i < MAX_RUNTIME_SPLIT_PARTS; ++i) outPercent[i] = 100;
}

// ensureSplitWeightDefaultsForCount
void D3D11Renderer::ensureSplitWeightDefaultsForCount(int count, bool forceReset) {
    count = clampInt(count, 4, MAX_RUNTIME_SPLIT_PARTS);
    if (!forceReset && splitWeightCustomized_ && splitWeightInitializedCount_ == count) {
        return;
    }
    computeDefaultSplitWeightPercent(count, settings_.splitWeightPercent);
    splitWeightInitializedCount_ = count;
    splitWeightCustomized_ = false;
}

// currentSplitWeightCountForSettings
int D3D11Renderer::currentSplitWeightCountForSettings() {
    int parts = cachedRecvParts_;
    {
        std::lock_guard<std::mutex> lk(state_.mutex);
        if (state_.recvParts > 0) parts = state_.recvParts;
    }
    if (parts < 2) parts = 6;
    return clampInt(parts, 4, maxFullscreenSplitPartsForSettings());
}

// readSplitWeightSliders
void D3D11Renderer::readSplitWeightSliders(int count, int outPercent[MAX_RUNTIME_SPLIT_PARTS]) {
    for (int i = 0; i < MAX_RUNTIME_SPLIT_PARTS; ++i) {
        int value = settings_.splitWeightPercent[i];
        HWND slider = GetDlgItem(settingsHwnd_, IDC_SLIDER_WEIGHT_BASE + i);
        if (slider) value = static_cast<int>(SendMessageW(slider, TBM_GETPOS, 0, 0));
        outPercent[i] = clampInt(value, 70, 130);
    }
    for (int i = count; i < MAX_RUNTIME_SPLIT_PARTS; ++i) outPercent[i] = 100;
}

// formatCpuFreqText
std::wstring D3D11Renderer::formatCpuFreqText(int khz) const {
    if (khz <= 0) return L"--GHz";
    wchar_t buf[32]{};
    std::swprintf(buf, 32, L"%.2fGHz", double(khz) / 1000000.0);
    return buf;
}

// updateSplitWeightLabels
void D3D11Renderer::updateSplitWeightLabels() {
    // CPU 权重控制已移除。
}

// applySplitWeightsFromControls
bool D3D11Renderer::applySplitWeightsFromControls(bool /*showMessage*/) {
    // CPU 权重控制已移除，PC 不再下发 HLWGT/HLWGTCPU。
    return false;
}

// resetSplitWeightsToDefault
void D3D11Renderer::resetSplitWeightsToDefault(bool /*sendToAndroid*/) {
    // CPU 权重控制已移除。
}

