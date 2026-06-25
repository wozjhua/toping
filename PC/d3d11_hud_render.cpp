#include "d3d11_renderer.h"

// HUD texture creation, text composition and HUD quad drawing.


// createHudResources
bool D3D11Renderer::createHudResources() {
    D3D11_BUFFER_DESC vb{};
    vb.ByteWidth = sizeof(Vertex) * 6;
    vb.Usage = D3D11_USAGE_DYNAMIC;
    vb.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    vb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device_->CreateBuffer(&vb, nullptr, &hudVertexBuffer_))) {
        return false;
    }

    D3D11_TEXTURE2D_DESC td{};
    td.Width = hudTexWidth_;
    td.Height = hudTexHeight_;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device_->CreateTexture2D(&td, nullptr, &hudTex_))) {
        return false;
    }
    if (FAILED(device_->CreateShaderResourceView(hudTex_, nullptr, &hudSrv_))) {
        return false;
    }

    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(device_->CreateBlendState(&bd, &alphaBlend_))) {
        return false;
    }

    hudPixels_.resize(size_t(hudTexWidth_) * size_t(hudTexHeight_) * 4u);
    return true;
}


// drawChineseHudLines
void D3D11Renderer::drawChineseHudLines(const std::vector<std::wstring>& lines) {
    std::fill(hudPixels_.begin(), hudPixels_.end(), 0);
    hudUsedWidth_ = 0;
    hudUsedHeight_ = 0;

    bool hasAnyLine = false;
    for (const auto& line : lines) {
        if (!line.empty()) {
            hasAnyLine = true;
            break;
        }
    }
    if (!hasAnyLine) return;

    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) return;

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = hudTexWidth_;
    bmi.bmiHeader.biHeight = -hudTexHeight_;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp || !bits) {
        if (bmp) DeleteObject(bmp);
        DeleteDC(dc);
        return;
    }

    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    std::memset(bits, 0, size_t(hudTexWidth_) * size_t(hudTexHeight_) * 4u);
    //HUD 字体大小 -15
    HFONT font = CreateFontW(-15, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, HLW(L"Microsoft YaHei UI"));
    HGDIOBJ oldFont = nullptr;
    if (font) oldFont = SelectObject(dc, font);

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));

    const int padX = 8;
    const int padY = 6;
    const int lineGap = 4;

    std::vector<SIZE> sizes(lines.size());
    int usedTextW = 0;
    int usedTextH = 0;
    int lineCount = 0;

    for (size_t i = 0; i < lines.size(); ++i) {
        if (lines[i].empty()) continue;
        GetTextExtentPoint32W(dc, lines[i].c_str(), lstrlenW(lines[i].c_str()), &sizes[i]);
        usedTextW = (std::max)(usedTextW, static_cast<int>(sizes[i].cx));
        usedTextH += (std::max)(1, static_cast<int>(sizes[i].cy));
        ++lineCount;
    }
    usedTextH += (std::max)(0, lineCount - 1) * lineGap;

    hudUsedWidth_ = (std::min)(hudTexWidth_, (std::max)(1, usedTextW + padX * 2));
    hudUsedHeight_ = (std::min)(hudTexHeight_, (std::max)(1, usedTextH + padY * 2));

    int y = padY;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (lines[i].empty()) continue;
        const int textH = (std::max)(1, static_cast<int>(sizes[i].cy));
        RECT rc{ padX, y, hudUsedWidth_ - padX, y + textH + 2 };
        DrawTextW(dc, lines[i].c_str(), -1, &rc, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        y += textH + lineGap;
    }

    uint8_t* dib = static_cast<uint8_t*>(bits);
    for (int y2 = 0; y2 < hudUsedHeight_; ++y2) {
        for (int x = 0; x < hudUsedWidth_; ++x) {
            const size_t idx = (size_t(y2) * size_t(hudTexWidth_) + size_t(x)) * 4u;
            hudPixels_[idx + 0] = 0;
            hudPixels_[idx + 1] = 0;
            hudPixels_[idx + 2] = 0;
            hudPixels_[idx + 3] = 255;

            const uint8_t b = dib[idx + 0];
            const uint8_t g = dib[idx + 1];
            const uint8_t r = dib[idx + 2];
            if (r > 8 || g > 8 || b > 8) {
                hudPixels_[idx + 0] = b;
                hudPixels_[idx + 1] = g;
                hudPixels_[idx + 2] = r;
                hudPixels_[idx + 3] = 255;
            }
        }
    }

    if (oldFont) SelectObject(dc, oldFont);
    if (font) DeleteObject(font);
    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(dc);
}


// appendPartStatsHudLines
void D3D11Renderer::appendPartStatsHudLines(std::vector<std::wstring>& lines) const {
    const int count = (std::min)(MAX_RUNTIME_SPLIT_PARTS, (std::max)(0, cachedPartStatCount_));
    if (count <= 0) return;

    for (int i = 0; i < count; i += 2) {
        wchar_t line[256]{};
        int pos = std::swprintf(line, 256, HLW(L"核心耗时 "));
        if (pos < 0) continue;

        for (int j = 0; j < 2 && i + j < count && pos > 0 && pos < 250; ++j) {
            const int k = i + j;
            wchar_t item[128]{};
            const double ghz = cachedPartCpuFreqKhz_[k] > 0 ? double(cachedPartCpuFreqKhz_[k]) / 1000000.0 : 0.0;
            const double share = cachedPartSharePermille_[k] > 0 ? double(cachedPartSharePermille_[k]) / 10.0 : 0.0;
            if (cachedPartMs_[k] > 0.0) {
                if (ghz > 0.0 && share > 0.0) {
                    std::swprintf(item, 128,
                        j == 0 ? HLW(L"核心%d CPU%d %.2fGHz %.1f%% %.1fms %.0fKB") : HLW(L" | 核心%d CPU%d %.2fGHz %.1f%% %.1fms %.0fKB"),
                        k, cachedPartCpu_[k], ghz, share, cachedPartMs_[k], cachedPartKb_[k]);
                }
                else {
                    std::swprintf(item, 128,
                        j == 0 ? HLW(L"核心%d CPU%d %.1fms %.0fKB") : HLW(L" | 核心%d CPU%d %.1fms %.0fKB"),
                        k, cachedPartCpu_[k], cachedPartMs_[k], cachedPartKb_[k]);
                }
            }
            else {
                std::swprintf(item, 128,
                    j == 0 ? HLW(L"核心%d CPU%d -- %.0fKB") : HLW(L" | 核心%d CPU%d -- %.0fKB"),
                    k, cachedPartCpu_[k], cachedPartKb_[k]);
            }

            const int n = std::swprintf(line + pos, 256 - static_cast<size_t>(pos), HLW(L"%ls"), item);
            if (n <= 0) break;
            pos += n;
        }

        lines.emplace_back(line);
    }
}


// totalHudKb
double D3D11Renderer::totalHudKb() const {
    return (std::max)(0.0, cachedAvgJpegKb_) + (std::max)(0.0, cachedCenterVideoAvgKb_);
}


// totalHudMbps
double D3D11Renderer::totalHudMbps() const {
    return (std::max)(0.0, cachedRecvMbps_) + (std::max)(0.0, cachedCenterVideoMbps_);
}


// updateHudTextureIfNeeded
void D3D11Renderer::updateHudTextureIfNeeded(bool force) {
    auto now = std::chrono::steady_clock::now();
    // HUD 文本和 HUD 纹理按设置窗口中的间隔刷新。
    const double hudRefreshSec = clampInt(settings_.hudRefreshMs, 100, 1000) / 1000.0;
    if (!force &&
        std::chrono::duration<double>(now - lastHudUpdate_).count() < hudRefreshSec) {
        return;
    }

    if (!hudTex_ || !hudSrv_) {
        return;
    }

    std::vector<std::wstring> hudLines;
    hudLines.reserve(16);

    if (currentFrame_.width > 0 && currentFrame_.height > 0) {
        wchar_t line[512]{};
        const bool hasVideoStats = (cachedCenterVideoFps_ > 0.1 ||
            cachedCenterVideoMbps_ > 0.01 ||
            cachedCenterVideoAvgKb_ > 0.1);
        // HUD 首行显示真实总负载：CPU/JPEG + H.264 视频流。
        // 不管精简/调试模式，都用同一套总 KB / 总 Mbps，避免精简模式误显示成纯 CPU/JPEG。
        const bool fullHud = settings_.debugHudMode && !settings_.compactHudMode;
        // 精简模式显示总负载：CPU/JPEG + H.264；调试/完整HUD保持分开，第一行只显示CPU/JPEG。
        const double lineKb = settings_.compactHudMode ? totalHudKb() : cachedAvgJpegKb_;
        const double lineMbps = settings_.compactHudMode ? totalHudMbps() : cachedRecvMbps_;
        const int actualCpuCores = actualCpuCoreCountForHud();

        std::swprintf(
            line,
            sizeof(line) / sizeof(line[0]),
            HLW(L" 灰狼至尊版-群主专属 画面 %dx%d |  采样 %ls |  %.0fKB %.1fMbps | 多核心%d"),
            currentFrame_.width,
            currentFrame_.height,
            jpegSubsamplingLabel(),
            lineKb,
            lineMbps,
            actualCpuCores
        );
        hudLines.emplace_back(line);

        std::swprintf(
            line,
            sizeof(line) / sizeof(line[0]),
            HLW(L"实际帧率 接收 %.1f | 解码发布 %.1f | 新画面 %.1f"),
            cachedRecvFps_,
            cachedDecodeFps_,
            cachedDisplayFps_
        );
        hudLines.emplace_back(line);

        if (fullHud) {
            if (hasVideoStats) {
                std::swprintf(
                    line,
                    sizeof(line) / sizeof(line[0]),
                    HLW(L"视频区 H264 %.0fKB %.1fMbps | PC %ls %.2fms %.1ffps 丢弃 %.1ffps | ROI %d,%d %dx%d codec %dx%d"),
                    cachedCenterVideoAvgKb_,
                    cachedCenterVideoMbps_,
                    centerVideoDecoderModeText(),
                    cachedCenterVideoDecodeMs_,
                    cachedCenterVideoFps_,
                    cachedCenterVideoDropFps_,
                    cachedCenterVideoRoiLeft_,
                    cachedCenterVideoRoiTop_,
                    cachedCenterVideoRoiWidth_,
                    cachedCenterVideoRoiHeight_,
                    cachedCenterVideoCodecWidth_,
                    cachedCenterVideoCodecHeight_
                );
            }
            else {
                std::swprintf(
                    line,
                    sizeof(line) / sizeof(line[0]),
                    HLW(L"中心视频 H264 等待数据 | ROI由视频流接管，外圈为CPU/JPEG")
                );
            }
            hudLines.emplace_back(line);
            if (currentFrame_.frameProducedNs > 0 && currentCenterRoiFrame_.frameProducedNs > 0) {
                const double deltaMs =
                    double(currentCenterRoiFrame_.frameProducedNs - currentFrame_.frameProducedNs) / 1000000.0;

                const double absDeltaMs = std::fabs(deltaMs);

                double baseFps = cachedCenterVideoFps_;
                if (baseFps <= 1.0) baseFps = cachedDisplayFps_;
                if (baseFps <= 1.0) baseFps = double(settings_.targetFps);
                if (baseFps <= 1.0) baseFps = 60.0;

                const double frames = absDeltaMs * baseFps / 1000.0;

                const wchar_t* relationText = HLW(L"基本同步");
                if (deltaMs < -2.0) {
                    relationText = HLW(L"视频ROI比JPEG慢");
                }
                else if (deltaMs > 2.0) {
                    relationText = HLW(L"视频ROI比JPEG快");
                }

                wchar_t syncLine[512]{};

                if (absDeltaMs <= 2.0) {
                    std::swprintf(
                        syncLine,
                        sizeof(syncLine) / sizeof(syncLine[0]),
                        HLW(L"混合同步 %ls %.1fms | video-jpeg %.1fms | videoGen %llu | jpegGen %llu"),
                        relationText,
                        absDeltaMs,
                        deltaMs,
                        static_cast<unsigned long long>(currentCenterRoiFrame_.generation),
                        static_cast<unsigned long long>(currentFrame_.generation)
                    );
                }
                else {
                    std::swprintf(
                        syncLine,
                        sizeof(syncLine) / sizeof(syncLine[0]),
                        HLW(L"混合同步 %ls %.1fms ≈ %.1f帧 | video-jpeg %.1fms | videoGen %llu | jpegGen %llu"),
                        relationText,
                        absDeltaMs,
                        frames,
                        deltaMs,
                        static_cast<unsigned long long>(currentCenterRoiFrame_.generation),
                        static_cast<unsigned long long>(currentFrame_.generation)
                    );
                }

                hudLines.emplace_back(syncLine);
            }
            else {
                hudLines.emplace_back(HLW(L"混合同步 等待 JPEG/H264 时间戳"));
            }
            if (hasVideoStats || cachedCenterVideoTimingSamples_ > 0) {
                std::swprintf(
                    line,
                    sizeof(line) / sizeof(line[0]),
                    HLW(L"H264时序 提交前 %.1fms | 上传 %.1fms | swap %.1fms | 硬编管线 %.1fms | 写前 %.1fms | 安卓到写 %.1fms | 样本 %d"),
                    cachedCenterVideoBeforeQueueMs_,
                    cachedCenterVideoUploadMs_,
                    cachedCenterVideoSwapMs_,
                    cachedCenterVideoCodecPipeMs_,
                    cachedCenterVideoWriteWaitMs_ + cachedCenterVideoSocketWriteMs_,
                    cachedCenterVideoAndroidTotalMs_,
                    cachedCenterVideoTimingSamples_
                );
                hudLines.emplace_back(line);
            }

            std::swprintf(
                line,
                sizeof(line) / sizeof(line[0]),
                HLW(L"采集到解码 采集 %.1fms | 安卓编码 %.1fms | 等待发送 %.1fms | 收齐跨度 %.1fms | 收齐+解码 %.1fms"),
                cachedCaptureMs_,
                cachedEncodeMs_,
                cachedQueueMs_,
                cachedSocketMs_,
                cachedDecodeWallMs_
            );
            hudLines.emplace_back(line);

            std::swprintf(
                line,
                sizeof(line) / sizeof(line[0]),
                HLW(L"PC纯解码 | 累计 %.1fms | 最慢 %.1fms | 等最后块 %.1fms | 并行省 %.1fms"),
                cachedDecodeCpuSumMs_,
                cachedDecodeMaxPartMs_,
                cachedDecodeTailWaitMs_,
                cachedDecodeOverlapSavedMs_
            );
            hudLines.emplace_back(line);

            std::swprintf(
                line,
                sizeof(line) / sizeof(line[0]),
                HLW(L"PC显示 新帧间隔 %.2fms | p95 %.2f | p99 %.2f | 近1秒漏显%d"),
                presentIntervalLastMs_,
                presentIntervalP95Ms_,
                presentIntervalP99Ms_,
                skippedFramesLast_
            );
            hudLines.emplace_back(line);

            appendPartStatsHudLines(hudLines);
        }
    }
    else {
        wchar_t line[512]{};

        std::swprintf(
            line,
            sizeof(line) / sizeof(line[0]),
            HLW(L"状态 %ls"),
            currentStatus_.c_str()
        );
        hudLines.emplace_back(line);

        std::swprintf(
            line,
            sizeof(line) / sizeof(line[0]),
            HLW(L"帧率 接收 %.1f | 发布 %.1f | 显示 %.1f"),
            cachedRecvFps_,
            cachedDecodeFps_,
            cachedDisplayFps_
        );
        hudLines.emplace_back(line);

        std::swprintf(
            line,
            sizeof(line) / sizeof(line[0]),
            HLW(L"快捷键 %ls隐藏/显示HUD | %ls或%ls切换全屏/窗口 | %ls打开设置 | %ls退出"),
            keyName(settings_.hudKey).c_str(),
            keyName(settings_.fullscreenKey1).c_str(),
            keyName(settings_.fullscreenKey2).c_str(),
            keyName(settings_.settingsKey).c_str(),
            keyName(settings_.exitKey).c_str()
        );
        hudLines.emplace_back(line);
    }

    drawChineseHudLines(hudLines);
    ctx_->UpdateSubresource(hudTex_, 0, nullptr, hudPixels_.data(), hudTexWidth_ * 4, 0);
    lastHudUpdate_ = now;
    hudDirty_ = false;
}


// drawHud
void D3D11Renderer::drawHud(const D3D11_VIEWPORT& vp) {
    if (!hudSrv_ || !hudVertexBuffer_ || hudUsedWidth_ <= 0 || hudUsedHeight_ <= 0) return;

    const float marginX = 12.0f;
    const float marginY = 12.0f;

    // HUD 位于左上角
    const float left = -1.0f + (2.0f * marginX / vp.Width);
    const float top = 1.0f - (2.0f * marginY / vp.Height);
    const float right = left + (2.0f * float(hudUsedWidth_) / vp.Width);
    const float bottom = top - (2.0f * float(hudUsedHeight_) / vp.Height);

    Vertex v[] = {
        { left,  top,    0.0f, 0.0f, 0.0f },
        { right, top,    0.0f, float(hudUsedWidth_) / float(hudTexWidth_), 0.0f },
        { right, bottom, 0.0f, float(hudUsedWidth_) / float(hudTexWidth_), float(hudUsedHeight_) / float(hudTexHeight_) },
        { left,  top,    0.0f, 0.0f, 0.0f },
        { right, bottom, 0.0f, float(hudUsedWidth_) / float(hudTexWidth_), float(hudUsedHeight_) / float(hudTexHeight_) },
        { left,  bottom, 0.0f, 0.0f, float(hudUsedHeight_) / float(hudTexHeight_) },
    };

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(ctx_->Map(hudVertexBuffer_, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        return;
    }
    std::memcpy(mapped.pData, v, sizeof(v));
    ctx_->Unmap(hudVertexBuffer_, 0);

    UINT stride = sizeof(Vertex);
    UINT offset = 0;
    float blendFactor[4] = { 0, 0, 0, 0 };

    ctx_->OMSetBlendState(alphaBlend_, blendFactor, 0xffffffff);
    ctx_->IASetVertexBuffers(0, 1, &hudVertexBuffer_, &stride, &offset);
    ctx_->PSSetShaderResources(0, 1, &hudSrv_);
    ctx_->Draw(6, 0);
    ctx_->OMSetBlendState(nullptr, blendFactor, 0xffffffff);
}


// centerVideoDecoderModeText
const wchar_t* D3D11Renderer::centerVideoDecoderModeText() const {
    switch (cachedCenterVideoDecoderMode_) {
    case 1: return HLW(L"FFmpeg/GPU");
    case 2: return HLW(L"FFmpeg失败");
    case 3: return HLW(L"MF");
    case 4: return HLW(L"FFmpeg非GPU");
    default: return HLW(L"未知");
    }
}

// actualCpuCoreCountForHud
int D3D11Renderer::actualCpuCoreCountForHud() const {
    bool seen[MAX_RUNTIME_SPLIT_PARTS]{};
    int unique = 0;
    const int n = (std::max)(0, (std::min)(cachedPartStatCount_, MAX_RUNTIME_SPLIT_PARTS));
    for (int i = 0; i < n; ++i) {
        const int cpu = cachedPartCpu_[i];
        if (cpu < 0 || cpu >= MAX_RUNTIME_SPLIT_PARTS) continue;
        if (!seen[cpu]) {
            seen[cpu] = true;
            ++unique;
        }
    }
    if (unique > 0) return unique;

    const int requested = settings_.captureMode == 0
        ? settings_.cropSplitParts
        : settings_.fullscreenSplitParts;
    const int safeRequested = (std::max)(1, (std::min)(requested, MAX_RUNTIME_SPLIT_PARTS));
    if (cachedAvailableEncodeCpuCount_ > 0) {
        return (std::max)(1, (std::min)(safeRequested, cachedAvailableEncodeCpuCount_));
    }
    return safeRequested;
}
