#include "d3d11_renderer.h"

// PC mapping overlay texture update and draw path.


// ensureMappingOverlayTexture
bool D3D11Renderer::ensureMappingOverlayTexture(int width, int height) {
    if (width <= 1 || height <= 1) return false;
    if (mappingOverlayTex_ && mappingOverlaySrv_ && mappingOverlayTexWidth_ == width && mappingOverlayTexHeight_ == height) {
        return true;
    }
    if (mappingOverlaySrv_) { mappingOverlaySrv_->Release(); mappingOverlaySrv_ = nullptr; }
    if (mappingOverlayTex_) { mappingOverlayTex_->Release(); mappingOverlayTex_ = nullptr; }
    mappingOverlayTexWidth_ = 0;
    mappingOverlayTexHeight_ = 0;

    D3D11_TEXTURE2D_DESC td{};
    td.Width = static_cast<UINT>(width);
    td.Height = static_cast<UINT>(height);
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device_->CreateTexture2D(&td, nullptr, &mappingOverlayTex_))) return false;
    if (FAILED(device_->CreateShaderResourceView(mappingOverlayTex_, nullptr, &mappingOverlaySrv_))) return false;
    mappingOverlayTexWidth_ = width;
    mappingOverlayTexHeight_ = height;
    mappingOverlayDirty_ = true;
    return true;
}


// updateMappingOverlayTextureIfNeeded
void D3D11Renderer::updateMappingOverlayTextureIfNeeded(const D3D11_VIEWPORT& vp) {
    const int width = static_cast<int>((std::max)(1.0f, vp.Width));
    const int height = static_cast<int>((std::max)(1.0f, vp.Height));
    const bool sizeChanged = width != mappingOverlayFrame_.width || height != mappingOverlayFrame_.height;

    // H.264 ROI 是在 JPEG 背景之后绘制的。如果 F4 文本/边框只画进 JPEG BGRA，
    // 正常视频帧一覆盖上来就会把它盖掉。这里把 H.264 的 F4 边框作为最终合成层绘制。
    // 但它仍然只是 F4 诊断文本/边框：F4 关闭时必须清掉旧 overlay texture，
    // F4 开启时只按 HUD 刷新间隔更新文字，避免每帧重建 overlay。
    const bool wantH264DebugOverlay = splitDebugOverlayVisible_ && settings_.centerRoiUseVideo && currentFrame_.width > 0 && currentFrame_.height > 0;
    static bool lastWantH264DebugOverlay = false;
    static auto lastH264DebugOverlayUpdate = std::chrono::steady_clock::now() - std::chrono::seconds(2);
    const bool h264DebugOverlayStateChanged = (lastWantH264DebugOverlay != wantH264DebugOverlay);
    const double h264DebugRefreshSec = clampInt(settings_.hudRefreshMs, 100, 1000) / 1000.0;
    const bool h264DebugOverlayRefreshDue = wantH264DebugOverlay &&
        std::chrono::duration<double>(std::chrono::steady_clock::now() - lastH264DebugOverlayUpdate).count() >= h264DebugRefreshSec;
    if (!mappingOverlayDirty_ && !sizeChanged && !h264DebugOverlayStateChanged && !h264DebugOverlayRefreshDue) return;

    PcMappingOverlayFrame frame;
    bool visible = false;
    if (!mappingOverlayHidden_) {
        PcMappingOverlayOptions overlayOptions;
        overlayOptions.editMode = mappingEditMode_;
        overlayOptions.selectedBindingIndex = selectedMappingBindingIndex_;
        overlayOptions.selectedMenuIndex = selectedMenuIndex_;
        overlayOptions.selectedCompass = compassSelected_;
        overlayOptions.collapsedAlpha = 0.30f;
        visible = BuildPcMappingOverlayFrame(width, height, mappingRuntime_.GetProfile(), overlayOptions, frame);
    }
    mappingOverlayDirty_ = false;

    if ((!visible || frame.bgra.empty()) && wantH264DebugOverlay) {
        frame.width = width;
        frame.height = height;
        frame.bgra.assign(size_t(width) * size_t(height) * 4u, 0);
        visible = true;
    }

    if (visible && !frame.bgra.empty() && wantH264DebugOverlay) {
        if (frame.width <= 0) frame.width = width;
        if (frame.height <= 0) frame.height = height;
        if (frame.width == width && frame.height == height) {
            drawH264DebugOverlayAfterComposite(frame.bgra.data(), width, height, width * 4, currentFrame_.width, currentFrame_.height);
        }
    }

    mappingOverlayFrame_ = std::move(frame);

    if (!visible || mappingOverlayFrame_.bgra.empty()) {
        if (mappingOverlaySrv_) { mappingOverlaySrv_->Release(); mappingOverlaySrv_ = nullptr; }
        if (mappingOverlayTex_) { mappingOverlayTex_->Release(); mappingOverlayTex_ = nullptr; }
        mappingOverlayTexWidth_ = 0;
        mappingOverlayTexHeight_ = 0;
        lastWantH264DebugOverlay = wantH264DebugOverlay;
        if (wantH264DebugOverlay) lastH264DebugOverlayUpdate = std::chrono::steady_clock::now();
        return;
    }
    if (!ensureMappingOverlayTexture(width, height)) {
        lastWantH264DebugOverlay = wantH264DebugOverlay;
        if (wantH264DebugOverlay) lastH264DebugOverlayUpdate = std::chrono::steady_clock::now();
        return;
    }
    ctx_->UpdateSubresource(mappingOverlayTex_, 0, nullptr, mappingOverlayFrame_.bgra.data(), width * 4, 0);
    lastWantH264DebugOverlay = wantH264DebugOverlay;
    if (wantH264DebugOverlay) lastH264DebugOverlayUpdate = std::chrono::steady_clock::now();
}


// drawMappingOverlay
void D3D11Renderer::drawMappingOverlay(const D3D11_VIEWPORT& vp) {
    if (!mappingOverlaySrv_ || !hudVertexBuffer_) return;

    Vertex v[] = {
        { -1.0f,  1.0f, 0.0f, 0.0f, 0.0f },
        {  1.0f,  1.0f, 0.0f, 1.0f, 0.0f },
        {  1.0f, -1.0f, 0.0f, 1.0f, 1.0f },
        { -1.0f,  1.0f, 0.0f, 0.0f, 0.0f },
        {  1.0f, -1.0f, 0.0f, 1.0f, 1.0f },
        { -1.0f, -1.0f, 0.0f, 0.0f, 1.0f },
    };

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(ctx_->Map(hudVertexBuffer_, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return;
    std::memcpy(mapped.pData, v, sizeof(v));
    ctx_->Unmap(hudVertexBuffer_, 0);

    UINT stride = sizeof(Vertex);
    UINT offset = 0;
    float blendFactor[4] = { 0, 0, 0, 0 };
    ctx_->OMSetBlendState(alphaBlend_, blendFactor, 0xffffffff);
    ctx_->IASetVertexBuffers(0, 1, &hudVertexBuffer_, &stride, &offset);
    ctx_->PSSetShaderResources(0, 1, &mappingOverlaySrv_);
    ctx_->Draw(6, 0);
    ctx_->OMSetBlendState(nullptr, blendFactor, 0xffffffff);
}

