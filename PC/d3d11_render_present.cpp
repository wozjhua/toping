#include "d3d11_renderer.h"


namespace {
struct FsrUpscaleCBLocalRender {
    float srcSize[2];
    float dstSize[2];
    float srcTexelSize[2];
    float sharpness;
    float padding;
};
static_assert((sizeof(FsrUpscaleCBLocalRender) % 16) == 0, "D3D11 constant buffers must be 16-byte aligned.");
}


// Main D3D11 Present path, frame quad vertices, and render-time statistics.
// Frame upload and strict sync helpers live in d3d11_frame_upload.cpp.

// percentileFromSorted
double D3D11Renderer::percentileFromSorted(const std::vector<double>& values, double pct) {
    if (values.empty()) return 0.0;
    if (pct <= 0.0) return values.front();
    if (pct >= 100.0) return values.back();
    const double pos = (pct / 100.0) * double(values.size() - 1);
    const size_t lo = static_cast<size_t>(pos);
    const size_t hi = (std::min)(lo + 1, values.size() - 1);
    const double frac = pos - double(lo);
    return values[lo] * (1.0 - frac) + values[hi] * frac;
}
// Frame upload / strict sync / center ROI texture helpers moved to d3d11_frame_upload.cpp.

// updateVertices
void D3D11Renderer::updateVertices() {
    // 顶点坐标始终是 NDC 全屏四边形，和窗口大小无关；只需初始化一次。
    // 之前每帧 Map/Unmap 一次顶点缓冲，会给高刷显示增加一点不必要 CPU 开销。
    if (frameVerticesInitialized_) return;
    Vertex v[] = {
        { -1.0f,  1.0f, 0.0f, 0.0f, 0.0f },
        {  1.0f,  1.0f, 0.0f, 1.0f, 0.0f },
        {  1.0f, -1.0f, 0.0f, 1.0f, 1.0f },
        { -1.0f,  1.0f, 0.0f, 0.0f, 0.0f },
        {  1.0f, -1.0f, 0.0f, 1.0f, 1.0f },
        { -1.0f, -1.0f, 0.0f, 0.0f, 1.0f },
    };

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (SUCCEEDED(ctx_->Map(vertexBuffer_, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        std::memcpy(mapped.pData, v, sizeof(v));
        ctx_->Unmap(vertexBuffer_, 0);
        frameVerticesInitialized_ = true;
    }
}

// render
void D3D11Renderer::render(bool presentedNewFrame) {
    if (!rtv_) return;
    const int64_t renderBeginNs = NowNs();

    RECT rc{};
    GetClientRect(hwnd_, &rc);
    float clear[4] = { 0, 0, 0, 1 };
    D3D11_VIEWPORT vp{};
    vp.TopLeftX = 0;
    vp.TopLeftY = 0;
    vp.Width = float((std::max)(1L, rc.right - rc.left));
    vp.Height = float((std::max)(1L, rc.bottom - rc.top));
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;

    struct FrameViewportLayout {
        D3D11_VIEWPORT canvasVp{};
        D3D11_VIEWPORT contentVp{};
    };

    auto makeFrameViewports = [&](const D3D11_VIEWPORT& fullVp, int frameW, int frameH) -> FrameViewportLayout {
        FrameViewportLayout layout{ fullVp, fullVp };
        if (stretch_ || frameW <= 0 || frameH <= 0 ||
            fullVp.Width <= 1.0f || fullVp.Height <= 1.0f) {
            return layout;
        }

        // 关闭“拉伸填满”后的语义：
        // 1) 先得到一个标准 16:9 投屏画布，例如 1080P = 1920x1080。
        // 2) 安卓真实画面可能是 1920x820，这只是画布里的内容区，不能拿它当窗口比例。
        // 3) 窗口模式下不主动放大超过标准画布；全屏模式下按 16:9 画布等比放大到屏幕内。
        // 4) 安卓内容区在标准画布中居中显示，保持自身比例，不做纵向拉伸。
        constexpr float kCanvasAspect = 16.0f / 9.0f;
        float canvasW = static_cast<float>(frameW);
        float canvasH = static_cast<float>(frameH);

        const float frameAspect = static_cast<float>(frameW) / static_cast<float>(frameH);
        if (frameAspect > kCanvasAspect) {
            // 安卓内容比 16:9 更宽：例如 1920x820，画布应为 1920x1080。
            canvasW = static_cast<float>(frameW);
            canvasH = canvasW / kCanvasAspect;
            if (canvasH < static_cast<float>(frameH)) {
                canvasH = static_cast<float>(frameH);
            }
        }
        else if (frameAspect < kCanvasAspect) {
            // 安卓内容比 16:9 更窄：保持高度，补左右黑边。
            canvasH = static_cast<float>(frameH);
            canvasW = canvasH * kCanvasAspect;
            if (canvasW < static_cast<float>(frameW)) {
                canvasW = static_cast<float>(frameW);
            }
        }

        float scale = 1.0f;
        if (fullscreen_) {
            scale = (std::min)(fullVp.Width / canvasW, fullVp.Height / canvasH);
        }
        else if (canvasW > fullVp.Width || canvasH > fullVp.Height) {
            scale = (std::min)(fullVp.Width / canvasW, fullVp.Height / canvasH);
        }
        if (!(scale > 0.0f)) scale = 1.0f;

        const float drawCanvasW = (std::max)(1.0f, canvasW * scale);
        const float drawCanvasH = (std::max)(1.0f, canvasH * scale);
        const float canvasLeft = fullVp.TopLeftX + (fullVp.Width - drawCanvasW) * 0.5f;
        const float canvasTop = fullVp.TopLeftY + (fullVp.Height - drawCanvasH) * 0.5f;

        layout.canvasVp = fullVp;
        layout.canvasVp.TopLeftX = canvasLeft;
        layout.canvasVp.TopLeftY = canvasTop;
        layout.canvasVp.Width = drawCanvasW;
        layout.canvasVp.Height = drawCanvasH;

        const float drawContentW = (std::max)(1.0f, static_cast<float>(frameW) * scale);
        const float drawContentH = (std::max)(1.0f, static_cast<float>(frameH) * scale);
        layout.contentVp = fullVp;
        layout.contentVp.TopLeftX = canvasLeft + (drawCanvasW - drawContentW) * 0.5f;
        layout.contentVp.TopLeftY = canvasTop + (drawCanvasH - drawContentH) * 0.5f;
        layout.contentVp.Width = drawContentW;
        layout.contentVp.Height = drawContentH;

        return layout;
    };

    int displayFrameW = currentFrame_.width;
    int displayFrameH = currentFrame_.height;
    if ((displayFrameW <= 0 || displayFrameH <= 0) && validCenterRoiPlacement(currentCenterRoiFrame_)) {
        displayFrameW = currentCenterRoiFrame_.centerFullWidth;
        displayFrameH = currentCenterRoiFrame_.centerFullHeight;
    }
    const FrameViewportLayout frameLayout = makeFrameViewports(vp, displayFrameW, displayFrameH);
    const D3D11_VIEWPORT frameVp = frameLayout.contentVp;
    if (displayFrameW > 0 && displayFrameH > 0) {
        pcUinput_.SetMirrorFrameViewport(frameVp.TopLeftX, frameVp.TopLeftY, frameVp.Width, frameVp.Height, displayFrameW, displayFrameH);
    }

    updateVertices();

    UINT stride = sizeof(Vertex);
    UINT offset = 0;
    ctx_->OMSetRenderTargets(1, &rtv_, nullptr);
    ctx_->RSSetViewports(1, &vp);
    ctx_->ClearRenderTargetView(rtv_, clear);
    ctx_->RSSetViewports(1, &frameVp);

    // Main frame and H.264 ROI must be opaque copies. HUD rendering enables alpha
    // blending later, so explicitly disable blending at the start of every frame to avoid
    // stale D3D state making the center video look like a translucent glass overlay.
    float noBlendFactor[4] = { 0, 0, 0, 0 };
    ctx_->OMSetBlendState(nullptr, noBlendFactor, 0xffffffff);

    ctx_->IASetInputLayout(inputLayout_);
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx_->VSSetShader(vs_, nullptr, 0);
    ctx_->PSSetShader(ps_, nullptr, 0);
    ctx_->PSSetSamplers(0, 1, &sampler_);

    ID3D11ShaderResourceView* visibleFrameSrv = (currentGpuFrame_ && currentGpuFrame_->srv) ? currentGpuFrame_->srv : frameSrv_;
    if (visibleFrameSrv) {
        ctx_->IASetVertexBuffers(0, 1, &vertexBuffer_, &stride, &offset);

        const int srcW = (currentFrame_.width > 0) ? currentFrame_.width : displayFrameW;
        const int srcH = (currentFrame_.height > 0) ? currentFrame_.height : displayFrameH;
        const int dstW = (std::max)(1, static_cast<int>(frameVp.Width + 0.5f));
        const int dstH = (std::max)(1, static_cast<int>(frameVp.Height + 0.5f));
        const bool canUseFsr = enableFsrUpscale_ && psFsrEasu_ && psFsrRcas_ && fsrCb_ &&
            srcW > 0 && srcH > 0 && dstW > 1 && dstH > 1 &&
            ensureFsrResources(dstW, dstH);

        if (canUseFsr) {
            FsrUpscaleCBLocalRender cb{};
            cb.srcSize[0] = static_cast<float>(srcW);
            cb.srcSize[1] = static_cast<float>(srcH);
            cb.dstSize[0] = static_cast<float>(dstW);
            cb.dstSize[1] = static_cast<float>(dstH);
            cb.srcTexelSize[0] = 1.0f / static_cast<float>(srcW);
            cb.srcTexelSize[1] = 1.0f / static_cast<float>(srcH);
            cb.sharpness = (std::max)(0.0f, (std::min)(1.0f, fsrSharpness_));
            ctx_->UpdateSubresource(fsrCb_, 0, nullptr, &cb, 0, 0);

            D3D11_VIEWPORT upscaleVp{};
            upscaleVp.TopLeftX = 0.0f;
            upscaleVp.TopLeftY = 0.0f;
            upscaleVp.Width = static_cast<float>(dstW);
            upscaleVp.Height = static_cast<float>(dstH);
            upscaleVp.MinDepth = 0.0f;
            upscaleVp.MaxDepth = 1.0f;

            // Pass 1: EASU-like spatial upscale into an offscreen full-resolution content texture.
            ctx_->OMSetRenderTargets(1, &fsrRtv_, nullptr);
            ctx_->RSSetViewports(1, &upscaleVp);
            ctx_->ClearRenderTargetView(fsrRtv_, clear);
            ctx_->PSSetShader(psFsrEasu_, nullptr, 0);
            ctx_->PSSetConstantBuffers(0, 1, &fsrCb_);
            ctx_->PSSetShaderResources(0, 1, &visibleFrameSrv);
            ctx_->Draw(6, 0);

            ID3D11ShaderResourceView* nullSrv[1] = { nullptr };
            ctx_->PSSetShaderResources(0, 1, nullSrv);

            // Pass 2: RCAS-like adaptive sharpening back to the swapchain content viewport.
            ctx_->OMSetRenderTargets(1, &rtv_, nullptr);
            ctx_->RSSetViewports(1, &frameVp);
            ctx_->PSSetShader(psFsrRcas_, nullptr, 0);
            ctx_->PSSetConstantBuffers(0, 1, &fsrCb_);
            ctx_->PSSetShaderResources(0, 1, &fsrSrv_);
            ctx_->Draw(6, 0);
            ctx_->PSSetShaderResources(0, 1, nullSrv);
            ctx_->PSSetShader(ps_, nullptr, 0);
        }
        else {
            ctx_->OMSetRenderTargets(1, &rtv_, nullptr);
            ctx_->RSSetViewports(1, &frameVp);
            ctx_->PSSetShader(ps_, nullptr, 0);
            ctx_->PSSetShaderResources(0, 1, &visibleFrameSrv);
            ctx_->Draw(6, 0);
            ID3D11ShaderResourceView* nullSrv[1] = { nullptr };
            ctx_->PSSetShaderResources(0, 1, nullSrv);
        }
    }

    ctx_->OMSetRenderTargets(1, &rtv_, nullptr);
    ctx_->RSSetViewports(1, &frameVp);
    ctx_->PSSetShader(ps_, nullptr, 0);

    // Draw the H.264 center ROI only after a full JPEG background exists.
    // This avoids showing the cropped video alone and gives the final frame
    // the intended CPU-background + GPU-center composition.
    ID3D11ShaderResourceView* centerRoiSrv =
        (currentCenterRoiGpuFrame_ && currentCenterRoiGpuFrame_->srv) ? currentCenterRoiGpuFrame_->srv : centerRoiSrv_;
    const bool centerOnlyH264 = settings_.centerRoiUseVideo && settings_.jpegCenterOnly && settings_.roiRegion == ROI_REGION_CENTER;
    const bool canDrawCenterRoi = settings_.centerRoiUseVideo && centerRoiSrv && validCenterRoiPlacement(currentCenterRoiFrame_) &&
        ((visibleFrameSrv && currentFrame_.width > 0 && currentFrame_.height > 0 &&
          currentCenterRoiFrame_.centerFullWidth == currentFrame_.width &&
          currentCenterRoiFrame_.centerFullHeight == currentFrame_.height) ||
         centerOnlyH264);
    if (canDrawCenterRoi && updateCenterRoiVertices(currentCenterRoiFrame_)) {
        ctx_->OMSetBlendState(nullptr, noBlendFactor, 0xffffffff);
        ctx_->IASetVertexBuffers(0, 1, &roiVertexBuffer_, &stride, &offset);
        ctx_->PSSetShaderResources(0, 1, &centerRoiSrv);
        ctx_->Draw(6, 0);
    }

    updateMappingOverlayTextureIfNeeded(frameVp);
    drawMappingOverlay(frameVp);

    ctx_->RSSetViewports(1, &vp);
    if (hudVisible_) {
        // HUD must be drawn BEFORE Present, otherwise it will never be visible.
        updateHudTextureIfNeeded(false);
        drawHud(vp);
    }

    const int64_t presentBeginNs = NowNs();
    lastDrawCpuMs_ = (std::max)(0.0, double(presentBeginNs - renderBeginNs) / 1000000.0);
    swapChain_->Present(0, allowTearing_ ? DXGI_PRESENT_ALLOW_TEARING : 0);
    const int64_t presentDoneNs = NowNs();

    // CPU-side Present duration contributes to the lower bound latency.
    lastPresentCpuMs_ = (std::max)(0.0, double(presentDoneNs - presentBeginNs) / 1000000.0);
    lowerBoundMs_ = currentFrame_.captureMs + currentFrame_.encodeMs + currentFrame_.queueMs +
        currentFrame_.socketMs + currentFrame_.decodeMs +
        lastUploadCpuMs_ + lastDrawCpuMs_ + lastPresentCpuMs_;
    lowerBoundEqFps_ = lowerBoundMs_ > 0.0 ? 1000.0 / lowerBoundMs_ : 0.0;

    // 显示间隔只统计“新帧成功 Present”的间隔。
    // 普通窗口重绘 / HUD刷新 / F2窗口打开造成的重绘不能混进来，否则会出现 0.00ms、p95=0 这种假数据。
    if (presentedNewFrame) {
        if (lastPresentDoneNs_ > 0) {
            const double ivMs = (std::max)(0.0, double(presentDoneNs - lastPresentDoneNs_) / 1000000.0);
            if (ivMs >= 1.0 && ivMs <= 1000.0) {
                presentIntervalLastMs_ = ivMs;
                presentIntervalAccumMs_ += ivMs;
                presentIntervalSamples_ += 1;
                presentIntervalMaxMs_ = (std::max)(presentIntervalMaxMs_, ivMs);
                presentIntervalsWindow_.push_back(ivMs);
            }
        }
        lastPresentDoneNs_ = presentDoneNs;
        lastRenderBeginNs_ = renderBeginNs;

        presentIntervalShownAvgMs_ = presentIntervalSamples_ > 0
            ? (presentIntervalAccumMs_ / presentIntervalSamples_)
            : presentIntervalLastMs_;
        presentIntervalShownMaxMs_ = (std::max)(presentIntervalMaxMs_, presentIntervalLastMs_);
        skippedFramesLast_ = skippedFramesWindow_;

        if (!presentIntervalsWindow_.empty()) {
            std::vector<double> sorted = presentIntervalsWindow_;
            std::sort(sorted.begin(), sorted.end());
            presentIntervalP95Ms_ = percentileFromSorted(sorted, 95.0);
            presentIntervalP99Ms_ = percentileFromSorted(sorted, 99.0);
        }
    }

    if (presentedNewFrame && currentFrame_.generation > 0 && g_perfLog.isRecording()) {
        char row[1024]{};
        std::snprintf(row, sizeof(row),
            "0,%llu,0,%d,%d,-1,%d,%d,%d,0,0,%d,%d,%d,%d,0,%.3f,%.3f,%.3f,%.3f,0,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%d,%.3f,%.3f,-1,0,0,%d,%u,render_present",
            static_cast<unsigned long long>(currentFrame_.generation),
            currentFrame_.width,
            currentFrame_.height,
            (std::max)(1, currentFrame_.decodePartCount),
            currentFrame_.width,
            currentFrame_.height,
            currentFrame_.width,
            currentFrame_.height,
            currentFrame_.width,
            currentFrame_.height,
            currentFrame_.captureMs,
            currentFrame_.encodeMs,
            currentFrame_.queueMs,
            currentFrame_.socketMs,
            currentFrame_.decodeMs,
            currentFrame_.decodeCpuSumMs,
            currentFrame_.decodeMaxPartMs,
            currentFrame_.decodeTailWaitMs,
            currentFrame_.decodeOverlapSavedMs,
            lastUploadCpuMs_,
            lastDrawCpuMs_,
            lastPresentCpuMs_,
            presentIntervalLastMs_,
            skippedFramesLast_,
            cachedRecvMbps_,
            cachedDisplayFps_,
            lastUploadMode_,
            static_cast<unsigned int>(lastUploadRowPitch_));
        g_perfLog.recordRow("render_present", row);
    }

    auto statsNow = std::chrono::steady_clock::now();
    if (std::chrono::duration<double>(statsNow - presentStatsWindowStart_).count() >= 1.0) {
        // Reset the rolling window but keep the last shown values visible.
        uploadMapCountShown_ = uploadMapCountWindow_;
        uploadFallbackCountShown_ = uploadFallbackCountWindow_;
        uploadFailedCountShown_ = uploadFailedCountWindow_;
        uploadMapCountWindow_ = 0;
        uploadFallbackCountWindow_ = 0;
        uploadFailedCountWindow_ = 0;
        presentIntervalAccumMs_ = 0.0;
        presentIntervalSamples_ = 0;
        presentIntervalMaxMs_ = 0.0;
        skippedFramesWindow_ = 0;
        presentIntervalsWindow_.clear();
        presentStatsWindowStart_ = statsNow;
    }

    hudDirty_ = true;
    titleDirty_ = true;
}

// Upload mode text moved to d3d11_frame_upload.cpp.

