#include "d3d11_renderer.h"

// Renderer shutdown and owned resource release.

// cleanup
void D3D11Renderer::cleanup() {
        mappingToolbar_.Stop();
        lockRuntime_.Stop();
        pcUinput_.Stop();
        if (settingsTitleFont_) { DeleteObject(settingsTitleFont_); settingsTitleFont_ = nullptr; }
        if (settingsFont_) { DeleteObject(settingsFont_); settingsFont_ = nullptr; }
        if (settingsBgBrush_) { DeleteObject(settingsBgBrush_); settingsBgBrush_ = nullptr; }
        if (settingsCardBrush_) { DeleteObject(settingsCardBrush_); settingsCardBrush_ = nullptr; }
        if (settingsFieldBrush_) { DeleteObject(settingsFieldBrush_); settingsFieldBrush_ = nullptr; }
        if (settingsHwnd_) { DestroyWindow(settingsHwnd_); settingsHwnd_ = nullptr; }
        currentGpuFrame_.reset();
        currentCenterRoiGpuFrame_.reset();
        releaseFsrResources();
        if (alphaBlend_) alphaBlend_->Release();
        if (sampler_) sampler_->Release();
        if (fsrCb_) { fsrCb_->Release(); fsrCb_ = nullptr; }
        if (psFsrRcas_) { psFsrRcas_->Release(); psFsrRcas_ = nullptr; }
        if (psFsrEasu_) { psFsrEasu_->Release(); psFsrEasu_ = nullptr; }
        if (mappingOverlaySrv_) mappingOverlaySrv_->Release();
        if (mappingOverlayTex_) mappingOverlayTex_->Release();
        if (hudSrv_) hudSrv_->Release();
        if (hudTex_) hudTex_->Release();
        if (centerRoiSrv_) centerRoiSrv_->Release();
        if (centerRoiTex_) centerRoiTex_->Release();
        if (frameSrv_) frameSrv_->Release();
        if (frameTex_) frameTex_->Release();
        if (hudVertexBuffer_) hudVertexBuffer_->Release();
        if (roiVertexBuffer_) roiVertexBuffer_->Release();
        if (vertexBuffer_) vertexBuffer_->Release();
        if (inputLayout_) inputLayout_->Release();
        if (ps_) ps_->Release();
        if (vs_) vs_->Release();
        if (rtv_) rtv_->Release();
        if (swapChain2_) swapChain2_->Release();
        if (swapChain_) swapChain_->Release();
        if (ctx_) ctx_->Release();
        if (device_) device_->Release();
        if (hwnd_) DestroyWindow(hwnd_);
    }
