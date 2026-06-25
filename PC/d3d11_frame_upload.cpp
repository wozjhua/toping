#include "d3d11_renderer.h"

// Frame texture upload, strict hybrid sync, frame consumption, and center ROI texture helpers.

bool D3D11Renderer::ensureFrameTexture(int width, int height) {
    if (frameTex_ && currentFrame_.width == width && currentFrame_.height == height) {
        return true;
    }
    if (currentFrame_.width != width || currentFrame_.height != height) {
        windowSizedToFrame_ = false;
    }
    currentGpuFrame_.reset();
    currentCenterRoiGpuFrame_.reset();
    pendingSyncJpegQueue_.clear();
    pendingSyncCenterQueue_.clear();
    if (centerRoiSrv_) { centerRoiSrv_->Release(); centerRoiSrv_ = nullptr; }
    if (centerRoiTex_) { centerRoiTex_->Release(); centerRoiTex_ = nullptr; }
    if (frameSrv_) { frameSrv_->Release(); frameSrv_ = nullptr; }
    if (frameTex_) { frameTex_->Release(); frameTex_ = nullptr; }

    D3D11_TEXTURE2D_DESC td{};
    td.Width = width;
    td.Height = height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    // 高频投屏每帧都要上传整张 BGRA。动态纹理 + WRITE_DISCARD 通常比
    // UpdateSubresource 的隐式 staging 路径更稳定，减少 PC 端上传长尾。
    td.Usage = D3D11_USAGE_DYNAMIC;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    HRESULT hr = device_->CreateTexture2D(&td, nullptr, &frameTex_);
    if (FAILED(hr)) return false;
    hr = device_->CreateShaderResourceView(frameTex_, nullptr, &frameSrv_);
    return SUCCEEDED(hr);
}

// uploadFrameTextureDynamic
bool D3D11Renderer::uploadFrameTextureDynamic(const DecodedFrame& frame) {
    if (!frameTex_ || frame.width <= 0 || frame.height <= 0 || (!frame.pixelsBGRA || frame.pixelsBGRA->empty())) {
        lastUploadMode_ = 3;
        lastUploadRowPitch_ = 0;
        ++uploadFailedCountWindow_;
        return false;
    }

    // 避免上一帧 draw 后 shader resource 仍绑定，部分驱动 Map 动态纹理时会产生隐式等待。
    ID3D11ShaderResourceView* nullSrv[1] = { nullptr };
    ctx_->PSSetShaderResources(0, 1, nullSrv);

    const int srcPitch = frame.width * 4;
    D3D11_MAPPED_SUBRESOURCE mapped{};
    HRESULT hr = ctx_->Map(frameTex_, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (SUCCEEDED(hr)) {
        lastUploadMode_ = 1;
        lastUploadRowPitch_ = mapped.RowPitch;
        ++uploadMapCountWindow_;

        uint8_t* srcMutable = frame.pixelsBGRA->data();
        drawSplitDebugOverlay(srcMutable, frame.width, frame.height, srcPitch);
        const uint8_t* src = srcMutable;
        uint8_t* dst = static_cast<uint8_t*>(mapped.pData);
        if (mapped.RowPitch == static_cast<UINT>(srcPitch)) {
            std::memcpy(dst, src, static_cast<size_t>(srcPitch) * static_cast<size_t>(frame.height));
        }
        else {
            for (int y = 0; y < frame.height; ++y) {
                std::memcpy(
                    dst + static_cast<size_t>(y) * static_cast<size_t>(mapped.RowPitch),
                    src + static_cast<size_t>(y) * static_cast<size_t>(srcPitch),
                    static_cast<size_t>(srcPitch));
            }
        }
        ctx_->Unmap(frameTex_, 0);
        return true;
    }

    // 极少数驱动不喜欢动态纹理时兜底，不影响兼容性。HUD 会显示 Fallback 次数。
    lastUploadMode_ = 2;
    lastUploadRowPitch_ = static_cast<UINT>(srcPitch);
    ++uploadFallbackCountWindow_;
    if (uploadFallbackCountWindow_ == 1) {
        wchar_t dbg[160]{};
        std::swprintf(dbg, 160, HLW(L"[HuiLang] D3D frame upload Map failed hr=0x%08X, fallback UpdateSubresource\n"), static_cast<unsigned int>(hr));
        OutputDebugStringW(dbg);
    }
    ctx_->UpdateSubresource(frameTex_, 0, nullptr, frame.pixelsBGRA->data(), srcPitch, 0);
    return true;
}

// clearCenterRoiOverlay
void D3D11Renderer::clearCenterRoiOverlay() {
    {
        std::lock_guard<std::mutex> lk(state_.mutex);
        state_.latestCenterRoi = DecodedFrame{};
        state_.hasCenterRoiFrame = false;
    }
    currentCenterRoiFrame_ = DecodedFrame{};
    currentCenterRoiGpuFrame_.reset();
    uploadedCenterRoiGeneration_ = 0;
    pendingSyncJpegQueue_.clear();
    pendingSyncCenterQueue_.clear();
    if (centerRoiSrv_) { centerRoiSrv_->Release(); centerRoiSrv_ = nullptr; }
    if (centerRoiTex_) { centerRoiTex_->Release(); centerRoiTex_ = nullptr; }
    dirtyWindow_ = true;
    hudDirty_ = true;
    titleDirty_ = true;
}

// clearDisplayedFrameAfterStreamReset
void D3D11Renderer::clearDisplayedFrameAfterStreamReset() {
    currentFrame_ = DecodedFrame{};
    currentCenterRoiFrame_ = DecodedFrame{};
    uploadedGeneration_ = 0;
    uploadedCenterRoiGeneration_ = 0;
    lastUploadCpuMs_ = 0.0;
    lastDrawCpuMs_ = 0.0;
    lastPresentCpuMs_ = 0.0;
    lowerBoundMs_ = 0.0;
    lowerBoundEqFps_ = 0.0;
    lastPresentDoneNs_ = 0;
    lastRenderBeginNs_ = 0;
    presentIntervalAvgMs_ = 0.0;
    presentIntervalMaxMs_ = 0.0;
    presentIntervalAccumMs_ = 0.0;
    presentIntervalLastMs_ = 0.0;
    presentIntervalShownAvgMs_ = 0.0;
    presentIntervalShownMaxMs_ = 0.0;
    presentIntervalP95Ms_ = 0.0;
    presentIntervalP99Ms_ = 0.0;
    presentIntervalSamples_ = 0;
    presentIntervalsWindow_.clear();
    presentStatsWindowStart_ = std::chrono::steady_clock::now();
    skippedFramesWindow_ = 0;
    skippedFramesLast_ = 0;
    regionOverlaySnapshotValid_ = false;
    overlayPartStatCount_ = 0;
    lastRegionOverlayUpdate_ = std::chrono::steady_clock::now() - std::chrono::seconds(2);
    currentGpuFrame_.reset();
    currentCenterRoiGpuFrame_.reset();
    pendingSyncJpegQueue_.clear();
    pendingSyncCenterQueue_.clear();
    if (centerRoiSrv_) { centerRoiSrv_->Release(); centerRoiSrv_ = nullptr; }
    if (centerRoiTex_) { centerRoiTex_->Release(); centerRoiTex_ = nullptr; }
    if (frameSrv_) { frameSrv_->Release(); frameSrv_ = nullptr; }
    if (frameTex_) { frameTex_->Release(); frameTex_ = nullptr; }
    dirtyWindow_ = true;
    hudDirty_ = true;
    titleDirty_ = true;
}

// framesAreSyncMatch
bool D3D11Renderer::framesAreSyncMatch(const DecodedFrame& jpeg, const DecodedFrame& center, int64_t toleranceNs) const {
    if (jpeg.generation == 0 || center.generation == 0) return false;
    if (jpeg.frameProducedNs <= 0 || center.frameProducedNs <= 0) return false;
    if (!validCenterRoiPlacement(center)) return false;
    if (center.centerFullWidth != jpeg.width || center.centerFullHeight != jpeg.height) return false;
    return absI64(jpeg.frameProducedNs - center.frameProducedNs) <= toleranceNs;
}

// pruneStrictSyncQueues
void D3D11Renderer::pruneStrictSyncQueues() {
    static constexpr size_t kMaxQueue = 8;
    while (pendingSyncJpegQueue_.size() > kMaxQueue) {
        pendingSyncJpegQueue_.pop_front();
        ++droppedSyncJpegFrames_;
    }
    while (pendingSyncCenterQueue_.size() > kMaxQueue) {
        pendingSyncCenterQueue_.pop_front();
        ++droppedSyncCenterFrames_;
    }

    // If both queues exist but their oldest frames are far apart, drop only
    // frames that are certainly older than any possible match.  This avoids
    // the old single-slot ping-pong behavior.
    const int64_t toleranceNs = strictSyncToleranceNs();
    while (!pendingSyncJpegQueue_.empty() && !pendingSyncCenterQueue_.empty()) {
        const int64_t j = pendingSyncJpegQueue_.front().frameProducedNs;
        const int64_t c = pendingSyncCenterQueue_.front().frameProducedNs;
        if (j <= 0 || c <= 0) break;
        if (j + toleranceNs < c) {
            pendingSyncJpegQueue_.pop_front();
            ++droppedSyncJpegFrames_;
            continue;
        }
        if (c + toleranceNs < j) {
            pendingSyncCenterQueue_.pop_front();
            ++droppedSyncCenterFrames_;
            continue;
        }
        break;
    }
}

// popMatchedStrictSyncPair
bool D3D11Renderer::popMatchedStrictSyncPair(DecodedFrame& outJpeg, DecodedFrame& outCenter) {
    if (pendingSyncJpegQueue_.empty() || pendingSyncCenterQueue_.empty()) return false;
    const int64_t toleranceNs = strictSyncToleranceNs();

    int bestJ = -1;
    int bestC = -1;
    int64_t bestDiff = INT64_MAX;
    for (int ji = 0; ji < static_cast<int>(pendingSyncJpegQueue_.size()); ++ji) {
        const auto& j = pendingSyncJpegQueue_[static_cast<size_t>(ji)];
        for (int ci = 0; ci < static_cast<int>(pendingSyncCenterQueue_.size()); ++ci) {
            const auto& c = pendingSyncCenterQueue_[static_cast<size_t>(ci)];
            if (!framesAreSyncMatch(j, c, toleranceNs)) continue;
            const int64_t d = absI64(j.frameProducedNs - c.frameProducedNs);
            if (d < bestDiff) {
                bestDiff = d;
                bestJ = ji;
                bestC = ci;
            }
        }
    }
    if (bestJ < 0 || bestC < 0) return false;

    outJpeg = std::move(pendingSyncJpegQueue_[static_cast<size_t>(bestJ)]);
    outCenter = std::move(pendingSyncCenterQueue_[static_cast<size_t>(bestC)]);

    // Drop all older/unmatched frames up to the matched pair.  They cannot be
    // displayed without breaking strict sync.
    for (int i = 0; i <= bestJ && !pendingSyncJpegQueue_.empty(); ++i) {
        if (i < bestJ) ++droppedSyncJpegFrames_;
        pendingSyncJpegQueue_.pop_front();
    }
    for (int i = 0; i <= bestC && !pendingSyncCenterQueue_.empty(); ++i) {
        if (i < bestC) ++droppedSyncCenterFrames_;
        pendingSyncCenterQueue_.pop_front();
    }
    return true;
}

// consumeLatestFrame
bool D3D11Renderer::consumeLatestFrame() {
    const bool centerOnlyH264 = settings_.centerRoiUseVideo &&
        settings_.jpegCenterOnly && settings_.roiRegion == ROI_REGION_CENTER;
    state_.centerRoiStrictSync.store(settings_.centerRoiUseVideo && settings_.strictHybridSync && !centerOnlyH264, std::memory_order_release);
    DecodedFrame newest;
    DecodedFrame newestCenterRoi;
    std::wstring status;
    double recvFps = 0.0;
    double decodeFps = 0.0;
    double displayFps = 0.0;
    double recvMbps = 0.0;
    double avgJpegKb = 0.0;
    double centerVideoFps = 0.0;
    double centerVideoMbps = 0.0;
    double centerVideoAvgKb = 0.0;
    double centerVideoDropFps = 0.0;
    double centerVideoDecodeMs = 0.0;
    double centerVideoBeforeQueueMs = 0.0;
    double centerVideoUploadMs = 0.0;
    double centerVideoSwapMs = 0.0;
    double centerVideoCodecPipeMs = 0.0;
    double centerVideoWriteWaitMs = 0.0;
    double centerVideoSocketWriteMs = 0.0;
    double centerVideoAndroidTotalMs = 0.0;
    int centerVideoTimingSamples = 0;
    int centerVideoDecoderMode = 0;
    int centerVideoFullWidth = 0;
    int centerVideoFullHeight = 0;
    int centerVideoRoiLeft = 0;
    int centerVideoRoiTop = 0;
    int centerVideoRoiWidth = 0;
    int centerVideoRoiHeight = 0;
    int centerVideoCodecWidth = 0;
    int centerVideoCodecHeight = 0;
    double avgPart0Kb = 0.0;
    double avgPart1Kb = 0.0;
    int recvParts = 1;
    int partStatCount = 0;
    double partKb[MAX_RUNTIME_SPLIT_PARTS]{};
    double partMs[MAX_RUNTIME_SPLIT_PARTS]{};
    int partCpu[MAX_RUNTIME_SPLIT_PARTS]{};
    int partCpuFreqKhz[MAX_RUNTIME_SPLIT_PARTS]{};
    int partLeft[MAX_RUNTIME_SPLIT_PARTS]{};
    int partTop[MAX_RUNTIME_SPLIT_PARTS]{};
    int partWidth[MAX_RUNTIME_SPLIT_PARTS]{};
    int partHeight[MAX_RUNTIME_SPLIT_PARTS]{};
    int partSharePermille[MAX_RUNTIME_SPLIT_PARTS]{};
    int availableEncodeCpuCount = cachedAvailableEncodeCpuCount_;
    uint64_t streamResetGeneration = observedStreamResetGeneration_;
    bool gotFrame = false;
    bool gotCenterRoiFrame = false;

    {
        std::lock_guard<std::mutex> lk(state_.mutex);
        status = state_.status;
        recvFps = state_.recvFps;
        decodeFps = state_.decodeFps;
        displayFps = state_.displayFps;
        recvMbps = state_.recvMbps;
        avgJpegKb = state_.avgJpegKb;
        centerVideoFps = state_.centerVideoFps;
        centerVideoMbps = state_.centerVideoMbps;
        centerVideoAvgKb = state_.centerVideoAvgKb;
        centerVideoDropFps = state_.centerVideoDropFps;
        centerVideoDecodeMs = state_.centerVideoDecodeMs;
        centerVideoBeforeQueueMs = state_.centerVideoBeforeQueueMs;
        centerVideoUploadMs = state_.centerVideoUploadMs;
        centerVideoSwapMs = state_.centerVideoSwapMs;
        centerVideoCodecPipeMs = state_.centerVideoCodecPipeMs;
        centerVideoWriteWaitMs = state_.centerVideoWriteWaitMs;
        centerVideoSocketWriteMs = state_.centerVideoSocketWriteMs;
        centerVideoAndroidTotalMs = state_.centerVideoAndroidTotalMs;
        centerVideoTimingSamples = state_.centerVideoTimingSamples;
        centerVideoDecoderMode = state_.centerVideoDecoderMode;
        centerVideoFullWidth = state_.centerVideoFullWidth;
        centerVideoFullHeight = state_.centerVideoFullHeight;
        centerVideoRoiLeft = state_.centerVideoRoiLeft;
        centerVideoRoiTop = state_.centerVideoRoiTop;
        centerVideoRoiWidth = state_.centerVideoRoiWidth;
        centerVideoRoiHeight = state_.centerVideoRoiHeight;
        centerVideoCodecWidth = state_.centerVideoCodecWidth;
        centerVideoCodecHeight = state_.centerVideoCodecHeight;
        avgPart0Kb = state_.avgPart0Kb;
        avgPart1Kb = state_.avgPart1Kb;
        recvParts = state_.recvParts;
        partStatCount = state_.latestPartStatCount;
        availableEncodeCpuCount = state_.availableEncodeCpuCount;
        for (int i = 0; i < MAX_RUNTIME_SPLIT_PARTS; ++i) {
            partKb[i] = state_.latestPartKb[i];
            partMs[i] = state_.latestPartMs[i];
            partCpu[i] = state_.latestPartCpu[i];
            partCpuFreqKhz[i] = state_.latestPartCpuFreqKhz[i];
            partLeft[i] = state_.latestPartLeft[i];
            partTop[i] = state_.latestPartTop[i];
            partWidth[i] = state_.latestPartWidth[i];
            partHeight[i] = state_.latestPartHeight[i];
            partSharePermille[i] = state_.latestPartSharePermille[i];
        }
        streamResetGeneration = state_.streamResetGeneration;
        if (state_.hasFrame && state_.latest.generation != uploadedGeneration_) {
            // Move the decoded BGRA buffer out instead of copying ~9MB per 1080p+ frame.
            newest = std::move(state_.latest);
            state_.hasFrame = false;
            gotFrame = true;
        }
        if (state_.hasCenterRoiFrame && state_.latestCenterRoi.generation != uploadedCenterRoiGeneration_) {
            newestCenterRoi = std::move(state_.latestCenterRoi);
            state_.hasCenterRoiFrame = false;
            gotCenterRoiFrame = true;
        }
    }

    if (streamResetGeneration != observedStreamResetGeneration_) {
        observedStreamResetGeneration_ = streamResetGeneration;
        clearDisplayedFrameAfterStreamReset();
    }

    const bool hasVisibleFrame = (((frameSrv_ != nullptr && frameTex_ != nullptr) || (currentGpuFrame_ && currentGpuFrame_->srv)) && currentFrame_.generation > 0);
    if (currentStatus_ != status) {
        currentStatus_ = status;
        titleDirty_ = true;
        // 没有任何视频帧时也必须重绘 HUD，否则 adb/forward/socket 错误只停留在旧画面，
        // 用户会只看到黑底快捷键提示，无法判断真正的连接状态。
        hudDirty_ = true;
        dirtyWindow_ = true;
    }
    if (cachedRecvFps_ != recvFps || cachedDecodeFps_ != decodeFps || cachedDisplayFps_ != displayFps ||
        cachedRecvMbps_ != recvMbps || cachedAvgJpegKb_ != avgJpegKb ||
        cachedCenterVideoFps_ != centerVideoFps || cachedCenterVideoMbps_ != centerVideoMbps ||
        cachedCenterVideoAvgKb_ != centerVideoAvgKb || cachedCenterVideoDropFps_ != centerVideoDropFps ||
        cachedCenterVideoDecodeMs_ != centerVideoDecodeMs || cachedCenterVideoBeforeQueueMs_ != centerVideoBeforeQueueMs ||
        cachedCenterVideoUploadMs_ != centerVideoUploadMs || cachedCenterVideoSwapMs_ != centerVideoSwapMs ||
        cachedCenterVideoCodecPipeMs_ != centerVideoCodecPipeMs || cachedCenterVideoWriteWaitMs_ != centerVideoWriteWaitMs ||
        cachedCenterVideoSocketWriteMs_ != centerVideoSocketWriteMs || cachedCenterVideoAndroidTotalMs_ != centerVideoAndroidTotalMs ||
        cachedCenterVideoTimingSamples_ != centerVideoTimingSamples || cachedCenterVideoDecoderMode_ != centerVideoDecoderMode ||
        cachedCenterVideoFullWidth_ != centerVideoFullWidth || cachedCenterVideoFullHeight_ != centerVideoFullHeight ||
        cachedCenterVideoRoiLeft_ != centerVideoRoiLeft || cachedCenterVideoRoiTop_ != centerVideoRoiTop ||
        cachedCenterVideoRoiWidth_ != centerVideoRoiWidth || cachedCenterVideoRoiHeight_ != centerVideoRoiHeight ||
        cachedCenterVideoCodecWidth_ != centerVideoCodecWidth || cachedCenterVideoCodecHeight_ != centerVideoCodecHeight ||
        cachedPart0Kb_ != avgPart0Kb || cachedPart1Kb_ != avgPart1Kb || cachedRecvParts_ != recvParts) {
        cachedRecvFps_ = recvFps;
        cachedDecodeFps_ = decodeFps;
        cachedDisplayFps_ = displayFps;
        cachedRecvMbps_ = recvMbps;
        cachedAvgJpegKb_ = avgJpegKb;
        cachedCenterVideoFps_ = centerVideoFps;
        cachedCenterVideoMbps_ = centerVideoMbps;
        cachedCenterVideoAvgKb_ = centerVideoAvgKb;
        cachedCenterVideoDropFps_ = centerVideoDropFps;
        cachedCenterVideoDecodeMs_ = centerVideoDecodeMs;
        cachedCenterVideoBeforeQueueMs_ = centerVideoBeforeQueueMs;
        cachedCenterVideoUploadMs_ = centerVideoUploadMs;
        cachedCenterVideoSwapMs_ = centerVideoSwapMs;
        cachedCenterVideoCodecPipeMs_ = centerVideoCodecPipeMs;
        cachedCenterVideoWriteWaitMs_ = centerVideoWriteWaitMs;
        cachedCenterVideoSocketWriteMs_ = centerVideoSocketWriteMs;
        cachedCenterVideoAndroidTotalMs_ = centerVideoAndroidTotalMs;
        cachedCenterVideoTimingSamples_ = centerVideoTimingSamples;
        cachedCenterVideoDecoderMode_ = centerVideoDecoderMode;
        cachedCenterVideoFullWidth_ = centerVideoFullWidth;
        cachedCenterVideoFullHeight_ = centerVideoFullHeight;
        cachedCenterVideoRoiLeft_ = centerVideoRoiLeft;
        cachedCenterVideoRoiTop_ = centerVideoRoiTop;
        cachedCenterVideoRoiWidth_ = centerVideoRoiWidth;
        cachedCenterVideoRoiHeight_ = centerVideoRoiHeight;
        cachedCenterVideoCodecWidth_ = centerVideoCodecWidth;
        cachedCenterVideoCodecHeight_ = centerVideoCodecHeight;
        cachedPart0Kb_ = avgPart0Kb;
        cachedPart1Kb_ = avgPart1Kb;
        cachedRecvParts_ = recvParts;
        cachedPartStatCount_ = partStatCount;
        cachedAvailableEncodeCpuCount_ = availableEncodeCpuCount;
        const int currentMaxParts = maxFullscreenSplitPartsForSettings();
        if (settings_.fullscreenSplitParts > currentMaxParts) {
            settings_.fullscreenSplitParts = currentMaxParts;
            pendingRuntimeSettingsSync_ = true;
            if (settingsHwnd_) fillSettingsControls();
        }
        for (int i = 0; i < MAX_RUNTIME_SPLIT_PARTS; ++i) {
            cachedPartKb_[i] = partKb[i];
            cachedPartMs_[i] = partMs[i];
            cachedPartCpu_[i] = partCpu[i];
            cachedPartCpuFreqKhz_[i] = partCpuFreqKhz[i];
            cachedPartLeft_[i] = partLeft[i];
            cachedPartTop_[i] = partTop[i];
            cachedPartWidth_[i] = partWidth[i];
            cachedPartHeight_[i] = partHeight[i];
            cachedPartSharePermille_[i] = partSharePermille[i];
        }
        titleDirty_ = true;
        hudDirty_ = hasVisibleFrame || gotFrame;
        if (hasVisibleFrame || gotFrame) dirtyWindow_ = true;
    }
    cachedPartStatCount_ = partStatCount;
    cachedAvailableEncodeCpuCount_ = availableEncodeCpuCount;
    for (int i = 0; i < MAX_RUNTIME_SPLIT_PARTS; ++i) {
        cachedPartKb_[i] = partKb[i];
        cachedPartMs_[i] = partMs[i];
        cachedPartCpu_[i] = partCpu[i];
        cachedPartCpuFreqKhz_[i] = partCpuFreqKhz[i];
        cachedPartLeft_[i] = partLeft[i];
        cachedPartTop_[i] = partTop[i];
        cachedPartWidth_[i] = partWidth[i];
        cachedPartHeight_[i] = partHeight[i];
        cachedPartSharePermille_[i] = partSharePermille[i];
    }
    // CPU 权重控制已移除，不再在渲染循环里刷新 F2 权重控件。
    if (gotFrame && newest.generation != 0) {
        cachedCaptureMs_ = newest.captureMs;
        cachedEncodeMs_ = newest.encodeMs;
        cachedQueueMs_ = newest.queueMs;
        cachedSocketMs_ = newest.socketMs;
        cachedDecodeWallMs_ = newest.decodeMs;
        cachedDecodeCpuSumMs_ = newest.decodeCpuSumMs;
        cachedDecodeMaxPartMs_ = newest.decodeMaxPartMs;
        cachedDecodeTailWaitMs_ = newest.decodeTailWaitMs;
        cachedDecodeOverlapSavedMs_ = newest.decodeOverlapSavedMs;
        cachedDecodePartCount_ = newest.decodePartCount > 0 ? newest.decodePartCount : 1;
    }

    if (settings_.centerRoiUseVideo && settings_.strictHybridSync && !centerOnlyH264) {
        if (gotFrame && newest.generation != 0) {
            pendingSyncJpegQueue_.push_back(std::move(newest));
            gotFrame = false;
        }
        if (gotCenterRoiFrame && newestCenterRoi.generation != 0 && validCenterRoiPlacement(newestCenterRoi)) {
            pendingSyncCenterQueue_.push_back(std::move(newestCenterRoi));
            gotCenterRoiFrame = false;
        }

        pruneStrictSyncQueues();
        if (popMatchedStrictSyncPair(newest, newestCenterRoi)) {
            gotFrame = true;
            gotCenterRoiFrame = true;
        }
        else {
            // Strict sync mode is only meaningful when the H.264 ROI stream has
            // actually produced frames.  During first connection / before the
            // center-video socket is ready, blocking here also blocks the JPEG
            // base stream, leaving the window black even though JPEG is healthy.
            //
            // Keep the original strict behavior once ROI frames exist: hold the
            // previous displayed synchronized pair until a new pair is available.
            // Only fall back to live JPEG when there is currently no ROI evidence.
            const bool centerRoiStreamHasFrames = gotCenterRoiFrame ||
                !pendingSyncCenterQueue_.empty() ||
                centerVideoFps > 0.01;

            if (!centerRoiStreamHasFrames && !pendingSyncJpegQueue_.empty()) {
                // Display the newest JPEG base frame while the optional ROI stream
                // has not started yet.  This fixes the first-connection black
                // screen without changing the paired-sync path after ROI starts.
                const size_t last = pendingSyncJpegQueue_.size() - 1;
                if (last > 0) droppedSyncJpegFrames_ += static_cast<int>(last);
                newest = std::move(pendingSyncJpegQueue_.back());
                pendingSyncJpegQueue_.clear();
                gotFrame = newest.generation != 0;
                gotCenterRoiFrame = false;
            }
            else {
                // Original strict-sync behavior: keep the old displayed frame
                // until a nearest JPEG/H.264 pair is available.  Do not clear the
                // previous ROI here; clearing on every missed match causes the
                // center-video area to blink black.
                return false;
            }
        }
    }
    else if (!settings_.strictHybridSync) {
        pendingSyncJpegQueue_.clear();
        pendingSyncCenterQueue_.clear();
    }

    bool updatedCenterRoi = false;
    if (gotCenterRoiFrame && newestCenterRoi.generation != 0 &&
        newestCenterRoi.generation != uploadedCenterRoiGeneration_ &&
        validCenterRoiPlacement(newestCenterRoi)) {
        const bool centerIsGpuFrame = newestCenterRoi.gpuFrame && newestCenterRoi.gpuFrame->srv;
        bool centerUploadOk = true;
        if (!centerIsGpuFrame) {
            currentCenterRoiGpuFrame_.reset();
            centerUploadOk = ensureCenterRoiTexture(newestCenterRoi.width, newestCenterRoi.height);
            if (centerUploadOk) {
                currentCenterRoiFrame_ = std::move(newestCenterRoi);
                centerUploadOk = uploadCenterRoiTextureDynamic(currentCenterRoiFrame_);
            }
        }
        else {
            currentCenterRoiFrame_ = std::move(newestCenterRoi);
            currentCenterRoiGpuFrame_ = currentCenterRoiFrame_.gpuFrame;
        }
        if (centerUploadOk) {
            uploadedCenterRoiGeneration_ = currentCenterRoiFrame_.generation;
            updatedCenterRoi = true;
            dirtyWindow_ = true;
            titleDirty_ = true;
            hudDirty_ = true;
        }
    }

    if (!gotFrame || newest.generation == 0 || newest.generation == uploadedGeneration_) {
        return updatedCenterRoi;
    }

    const bool newestIsGpuFrame = newest.gpuFrame && newest.gpuFrame->srv;
    if (!newestIsGpuFrame) {
        currentGpuFrame_.reset();
        if (!ensureFrameTexture(newest.width, newest.height)) {
            return false;
        }
    }

    if (uploadedGeneration_ != 0 && newest.generation > uploadedGeneration_ + 1) {
        skippedFramesWindow_ += int(newest.generation - uploadedGeneration_ - 1);
    }
    currentFrame_ = std::move(newest);
    currentGpuFrame_ = currentFrame_.gpuFrame;
    uploadedGeneration_ = currentFrame_.generation;
    pcUinput_.UpdateFrameGeometry(currentFrame_.width, currentFrame_.height);
    fitWindowToFrameIfNeeded();
    if (currentGpuFrame_ && currentGpuFrame_->srv) {
        lastUploadMode_ = currentFrame_.diagnosticFrame ? 5 : 4;
        lastUploadRowPitch_ = 0;
        lastUploadCpuMs_ = 0.0;
    }
    else {
        const int64_t uploadBeginNs = NowNs();
        const bool uploadOk = uploadFrameTextureDynamic(currentFrame_);
        const int64_t uploadDoneNs = NowNs();
        lastUploadCpuMs_ = (std::max)(0.0, double(uploadDoneNs - uploadBeginNs) / 1000000.0);
        if (!uploadOk) {
            return false;
        }
    }
    dirtyWindow_ = true;
    titleDirty_ = true;
    hudDirty_ = true;
    return true;
}

// ensureCenterRoiTexture
bool D3D11Renderer::ensureCenterRoiTexture(int width, int height) {
    if (centerRoiTex_ && currentCenterRoiFrame_.width == width && currentCenterRoiFrame_.height == height) {
        return true;
    }
    currentCenterRoiGpuFrame_.reset();
    pendingSyncJpegQueue_.clear();
    pendingSyncCenterQueue_.clear();
    if (centerRoiSrv_) { centerRoiSrv_->Release(); centerRoiSrv_ = nullptr; }
    if (centerRoiTex_) { centerRoiTex_->Release(); centerRoiTex_ = nullptr; }

    D3D11_TEXTURE2D_DESC td{};
    td.Width = width;
    td.Height = height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DYNAMIC;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    HRESULT hr = device_->CreateTexture2D(&td, nullptr, &centerRoiTex_);
    if (FAILED(hr)) return false;
    hr = device_->CreateShaderResourceView(centerRoiTex_, nullptr, &centerRoiSrv_);
    return SUCCEEDED(hr);
}

// uploadCenterRoiTextureDynamic
bool D3D11Renderer::uploadCenterRoiTextureDynamic(const DecodedFrame& frame) {
    if (!centerRoiTex_ || frame.width <= 0 || frame.height <= 0 || (!frame.pixelsBGRA || frame.pixelsBGRA->empty())) {
        return false;
    }
    ID3D11ShaderResourceView* nullSrv[1] = { nullptr };
    ctx_->PSSetShaderResources(0, 1, nullSrv);

    const int srcPitch = frame.width * 4;
    D3D11_MAPPED_SUBRESOURCE mapped{};
    HRESULT hr = ctx_->Map(centerRoiTex_, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr)) return false;
    const uint8_t* src = frame.pixelsBGRA->data();
    uint8_t* dst = static_cast<uint8_t*>(mapped.pData);
    if (mapped.RowPitch == static_cast<UINT>(srcPitch)) {
        std::memcpy(dst, src, static_cast<size_t>(srcPitch) * static_cast<size_t>(frame.height));
    }
    else {
        for (int y = 0; y < frame.height; ++y) {
            std::memcpy(dst + static_cast<size_t>(y) * static_cast<size_t>(mapped.RowPitch),
                src + static_cast<size_t>(y) * static_cast<size_t>(srcPitch),
                static_cast<size_t>(srcPitch));
        }
    }
    ctx_->Unmap(centerRoiTex_, 0);
    return true;
}

// validCenterRoiPlacement
bool D3D11Renderer::validCenterRoiPlacement(const DecodedFrame& f) {
    return f.centerRoiOverlay &&
        f.centerFullWidth > 0 && f.centerFullHeight > 0 &&
        f.centerRoiWidth > 0 && f.centerRoiHeight > 0 &&
        f.centerRoiLeft >= 0 && f.centerRoiTop >= 0 &&
        f.centerRoiLeft + f.centerRoiWidth <= f.centerFullWidth &&
        f.centerRoiTop + f.centerRoiHeight <= f.centerFullHeight;
}

// updateCenterRoiVertices
bool D3D11Renderer::updateCenterRoiVertices(const DecodedFrame& f) {
    if (!roiVertexBuffer_ || !validCenterRoiPlacement(f)) return false;
    // H.264 ROI 和 JPEG 背景来自两条解码/缩放路径，边界处一旦有 1px 四舍五入
    // 或 JPEG 挖洞边缘残留，就会看到黑色矩形线。绘制时让视频区域向外覆盖 2px，
    // 不改变编码尺寸，只用来盖住接缝。
    static constexpr int kRoiSeamCoverPx = 2;
    const int drawLeft = (std::max)(0, f.centerRoiLeft - kRoiSeamCoverPx);
    const int drawTop = (std::max)(0, f.centerRoiTop - kRoiSeamCoverPx);
    const int drawRight = (std::min)(f.centerFullWidth, f.centerRoiLeft + f.centerRoiWidth + kRoiSeamCoverPx);
    const int drawBottom = (std::min)(f.centerFullHeight, f.centerRoiTop + f.centerRoiHeight + kRoiSeamCoverPx);
    const float x0 = -1.0f + 2.0f * float(drawLeft) / float(f.centerFullWidth);
    const float x1 = -1.0f + 2.0f * float(drawRight) / float(f.centerFullWidth);
    const float y0 = 1.0f - 2.0f * float(drawTop) / float(f.centerFullHeight);
    const float y1 = 1.0f - 2.0f * float(drawBottom) / float(f.centerFullHeight);
    Vertex v[] = {
        { x0, y0, 0.0f, 0.0f, 0.0f },
        { x1, y0, 0.0f, 1.0f, 0.0f },
        { x1, y1, 0.0f, 1.0f, 1.0f },
        { x0, y0, 0.0f, 0.0f, 0.0f },
        { x1, y1, 0.0f, 1.0f, 1.0f },
        { x0, y1, 0.0f, 0.0f, 1.0f },
    };
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(ctx_->Map(roiVertexBuffer_, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return false;
    std::memcpy(mapped.pData, v, sizeof(v));
    ctx_->Unmap(roiVertexBuffer_, 0);
    return true;
}

// uploadModeText
const wchar_t* D3D11Renderer::uploadModeText() const {
        switch (lastUploadMode_) {
        case 1: return HLW(L"Map");
        case 2: return HLW(L"Fallback");
        case 3: return HLW(L"失败");
        case 4: return HLW(L"FFmpeg/GPU");
        case 5: return HLW(L"GPU诊断");
        default: return HLW(L"无");
        }
    }

// absI64
int64_t D3D11Renderer::absI64(int64_t v) {
    return v < 0 ? -v : v;
}

// strictSyncToleranceNs
int64_t D3D11Renderer::strictSyncToleranceNs() const {
    // Use about half a frame as the nearest-neighbor tolerance.  A fixed 3ms
    // tolerance was too strict when H.264 PTS is rounded/retimed or when JPEG
    // and video arrive with slightly different subsets.
    const int fps = (std::max)(30, (std::min)(240, settings_.targetFps));
    const int64_t halfFrameNs = 1000000000LL / fps / 2;
    const int64_t tolerance = halfFrameNs + 1000000LL;
    return (std::max)(4LL * 1000000LL, (std::min)(12LL * 1000000LL, tolerance));
}
