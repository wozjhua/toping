#include "d3d11_renderer.h"

// CPU-side debug drawing helpers for HUD/F4 overlays.


// blendBgraPixel
void D3D11Renderer::blendBgraPixel(uint8_t* px, uint8_t b, uint8_t g, uint8_t r, uint8_t alpha) {
    const uint32_t inv = 255u - alpha;
    px[0] = static_cast<uint8_t>((uint32_t(px[0]) * inv + uint32_t(b) * alpha) / 255u);
    px[1] = static_cast<uint8_t>((uint32_t(px[1]) * inv + uint32_t(g) * alpha) / 255u);
    px[2] = static_cast<uint8_t>((uint32_t(px[2]) * inv + uint32_t(r) * alpha) / 255u);
    px[3] = 255;
}


// fillRectBlend
void D3D11Renderer::fillRectBlend(uint8_t* bgra, int width, int height, int pitch, int left, int top, int right, int bottom,
    uint8_t b, uint8_t g, uint8_t r, uint8_t alpha) {
    left = (std::max)(0, left); top = (std::max)(0, top); right = (std::min)(width, right); bottom = (std::min)(height, bottom);
    if (!bgra || left >= right || top >= bottom) return;
    for (int y = top; y < bottom; ++y) {
        uint8_t* row = bgra + size_t(y) * size_t(pitch) + size_t(left) * 4u;
        for (int x = left; x < right; ++x, row += 4) blendBgraPixel(row, b, g, r, alpha);
    }
}


// drawVerticalLine
void D3D11Renderer::drawVerticalLine(uint8_t* bgra, int width, int height, int pitch, int x, int thickness,
    uint8_t b, uint8_t g, uint8_t r) {
    fillRectBlend(bgra, width, height, pitch, x, 0, x + thickness, height, b, g, r, 200);
}


// drawVerticalLineSpan
void D3D11Renderer::drawVerticalLineSpan(uint8_t* bgra, int width, int height, int pitch, int x, int top, int bottom, int thickness,
    uint8_t b, uint8_t g, uint8_t r, uint8_t alpha) {
    fillRectBlend(bgra, width, height, pitch, x, top, x + thickness, bottom, b, g, r, alpha);
}


// drawHorizontalLineSpan
void D3D11Renderer::drawHorizontalLineSpan(uint8_t* bgra, int width, int height, int pitch, int y, int left, int right, int thickness,
    uint8_t b, uint8_t g, uint8_t r, uint8_t alpha) {
    fillRectBlend(bgra, width, height, pitch, left, y, right, y + thickness, b, g, r, alpha);
}


// drawRectOutline
void D3D11Renderer::drawRectOutline(uint8_t* bgra, int width, int height, int pitch, int left, int top, int right, int bottom, int thickness,
    uint8_t b, uint8_t g, uint8_t r, uint8_t alpha) {
    fillRectBlend(bgra, width, height, pitch, left, top, right, top + thickness, b, g, r, alpha);
    fillRectBlend(bgra, width, height, pitch, left, bottom - thickness, right, bottom, b, g, r, alpha);
    fillRectBlend(bgra, width, height, pitch, left, top, left + thickness, bottom, b, g, r, alpha);
    fillRectBlend(bgra, width, height, pitch, right - thickness, top, right, bottom, b, g, r, alpha);
}


// drawSplitDebugOverlay
void D3D11Renderer::drawSplitDebugOverlay(uint8_t* bgra, int width, int height, int pitch) {
    if (!splitDebugOverlayVisible_ || !bgra || width <= 0 || height <= 0) return;

    // 区域文字使用快照，刷新频率跟随 HUD 刷新间隔；分割线仍然每帧画，避免画面切换后残留。
    const auto now = std::chrono::steady_clock::now();
    const double hudRefreshSec = clampInt(settings_.hudRefreshMs, 100, 1000) / 1000.0;
    if (!regionOverlaySnapshotValid_ ||
        std::chrono::duration<double>(now - lastRegionOverlayUpdate_).count() >= hudRefreshSec ||
        overlayPartStatCount_ != cachedPartStatCount_) {
        overlayPartStatCount_ = cachedPartStatCount_;
        for (int k = 0; k < MAX_RUNTIME_SPLIT_PARTS; ++k) {
            overlayPartKb_[k] = cachedPartKb_[k];
            overlayPartMs_[k] = cachedPartMs_[k];
            overlayPartCpu_[k] = cachedPartCpu_[k];
            overlayPartCpuFreqKhz_[k] = cachedPartCpuFreqKhz_[k];
            overlayPartLeft_[k] = cachedPartLeft_[k];
            overlayPartTop_[k] = cachedPartTop_[k];
            overlayPartWidth_[k] = cachedPartWidth_[k];
            overlayPartHeight_[k] = cachedPartHeight_[k];
            overlayPartSharePermille_[k] = cachedPartSharePermille_[k];
        }
        lastRegionOverlayUpdate_ = now;
        regionOverlaySnapshotValid_ = true;
    }

    enum class RoiDebugRegion {
        JpegLow,
        JpegHigh,
        Video
    };

    struct DebugRect {
        int left = 0;
        int top = 0;
        int right = 0;
        int bottom = 0;
    };

    auto pixelsFromPercent = [](int total, int percent) -> int {
        if (total <= 0) return 0;
        const long long v = static_cast<long long>(total) * static_cast<long long>(percent);
        int px = static_cast<int>((v + 50LL) / 100LL);
        return (std::max)(0, (std::min)(total, px));
    };

    auto makeCenteredRect = [&](int w, int h) -> DebugRect {
        w = (std::max)(0, (std::min)(width, w));
        h = (std::max)(0, (std::min)(height, h));
        const int l = (width - w) / 2;
        const int t = (height - h) / 2;
        return DebugRect{ l, t, l + w, t + h };
    };

    auto rectArea = [](const DebugRect& r) -> long long {
        const int w = (std::max)(0, r.right - r.left);
        const int h = (std::max)(0, r.bottom - r.top);
        return static_cast<long long>(w) * static_cast<long long>(h);
    };

    auto intersectArea = [](const DebugRect& a, const DebugRect& b) -> long long {
        const int l = (std::max)(a.left, b.left);
        const int t = (std::max)(a.top, b.top);
        const int r = (std::min)(a.right, b.right);
        const int bot = (std::min)(a.bottom, b.bottom);
        if (r <= l || bot <= t) return 0LL;
        return static_cast<long long>(r - l) * static_cast<long long>(bot - t);
    };

    const bool useVideoRoi = settings_.centerRoiUseVideo;
    const bool bottomVideoRoi = useVideoRoi && settings_.roiRegion == ROI_REGION_BOTTOM;
    const bool centerVideoRoi = useVideoRoi && settings_.roiRegion == ROI_REGION_CENTER;

    DebugRect videoRect{};
    bool hasVideoRect = false;
    if (bottomVideoRoi) {
        const int videoH = pixelsFromPercent(height, VideoStreamTuning::NormalizeRoiPercent(settings_.roiHeightPercent));
        videoRect = DebugRect{ 0, height - videoH, width, height };
        hasVideoRect = videoH > 0;
    }
    else if (centerVideoRoi) {
        const int videoW = pixelsFromPercent(width, VideoStreamTuning::NormalizeRoiPercent(settings_.roiWidthPercent));
        const int videoH = pixelsFromPercent(height, VideoStreamTuning::NormalizeRoiPercent(settings_.roiHeightPercent));
        videoRect = makeCenteredRect(videoW, videoH);
        hasVideoRect = videoW > 0 && videoH > 0;
    }

    DebugRect highJpegRect{};
    bool hasHighJpegRect = false;
    if (!centerVideoRoi) {
        const int jpegBottom = bottomVideoRoi && hasVideoRect ? videoRect.top : height;
        const int jpegHeight = (std::max)(0, jpegBottom);
        const int centerW = pixelsFromPercent(width, VideoStreamTuning::NormalizeRoiJpegCenterWidthPercent(settings_.roiJpegCenterWidthPercent));
        const int centerH = pixelsFromPercent(jpegHeight, VideoStreamTuning::NormalizeRoiTopLowPercent(settings_.roiTopLowPercent));
        if (centerW > 0 && centerH > 0 && jpegHeight > 0) {
            const int l = (width - centerW) / 2;
            // 底部 H.264 模式：高质量 JPEG 贴住视频上边缘，不能在两者之间再画低质量块。
            const int t = bottomVideoRoi ? (jpegBottom - centerH) : ((height - centerH) / 2);
            highJpegRect = DebugRect{ l, t, l + centerW, t + centerH };
            hasHighJpegRect = highJpegRect.right > highJpegRect.left && highJpegRect.bottom > highJpegRect.top;
        }
    }

    auto classifyPart = [&](int left, int top, int right, int bottom) -> RoiDebugRegion {
        DebugRect part{
            (std::max)(0, (std::min)(width, left)),
            (std::max)(0, (std::min)(height, top)),
            (std::max)(0, (std::min)(width, right)),
            (std::max)(0, (std::min)(height, bottom))
        };
        if (part.right <= part.left || part.bottom <= part.top) return RoiDebugRegion::JpegLow;

        const long long area = (std::max)(1LL, rectArea(part));

        // F4 的颜色/文字只按当前 ROI 定义判断：
        // 1) 视频 ROI 区域 = H.264；
        // 2) JPEG 中间高价值区域 = 当前主 Q；
        // 3) 其余 JPEG 外围区域 = Q-设置值。
        // 不再根据 part 下标、top/bottom 或旧统计值猜测。
        if (hasVideoRect && intersectArea(part, videoRect) * 2LL >= area) {
            return RoiDebugRegion::Video;
        }
        if (hasHighJpegRect && intersectArea(part, highJpegRect) * 2LL >= area) {
            return RoiDebugRegion::JpegHigh;
        }
        return RoiDebugRegion::JpegLow;
    };

    const int partCount = (std::min)(MAX_RUNTIME_SPLIT_PARTS, (std::max)(0, overlayPartStatCount_));
    const int mainJpegQ = clampInt(settings_.jpegQuality, 1, 100);
    const int edgeQDrop = VideoStreamTuning::NormalizeRoiEdgeQualityReduction(settings_.roiEdgeQualityReduction);

    for (int i = 0; i < partCount; ++i) {
        const int left = overlayPartLeft_[i];
        const int top = overlayPartTop_[i];
        const int right = overlayPartLeft_[i] + overlayPartWidth_[i];
        const int bottom = overlayPartTop_[i] + overlayPartHeight_[i];
        const RoiDebugRegion region = classifyPart(left, top, right, bottom);
        if (region == RoiDebugRegion::Video) {
            // H.264 ROI 会在视频纹理合成之后单独绘制；JPEG 背景层不要再画一份，
            // 否则切换/重建视频流时会看到短暂残影或重复标签。
            continue;
        }
        const bool highQ = region == RoiDebugRegion::JpegHigh;
        if (settings_.jpegCenterOnly && !highQ) {
            continue;
        }

        const uint8_t lineB = highQ ? 64 : 255;
        const uint8_t lineG = highQ ? 220 : 96;
        const uint8_t lineR = highQ ? 255 : 96;
        const uint8_t fillB = highQ ? 64 : 255;
        const uint8_t fillG = highQ ? 90 : 72;
        const uint8_t fillR = highQ ? 90 : 72;

        fillRectBlend(bgra, width, height, pitch, left, top, right, bottom, fillB, fillG, fillR, 18);
        drawRectOutline(bgra, width, height, pitch, left, top, right, bottom, 2, lineB, lineG, lineR, 180);

        char lineCpu[32]{};
        char lineQ[32]{};
        char lineKb[32]{};
        char lineMs[32]{};
        if (overlayPartCpu_[i] >= 0) {
            std::snprintf(lineCpu, sizeof(lineCpu), "cpu%d", overlayPartCpu_[i]);
        }
        else {
            std::snprintf(lineCpu, sizeof(lineCpu), "cpu?");
        }

        if (highQ) {
            std::snprintf(lineQ, sizeof(lineQ), "Q%d", mainJpegQ);
        }
        else if (edgeQDrop > 0) {
            std::snprintf(lineQ, sizeof(lineQ), "Q-%d", edgeQDrop);
        }
        else {
            std::snprintf(lineQ, sizeof(lineQ), "Q%d", mainJpegQ);
        }

        std::snprintf(lineKb, sizeof(lineKb), "%.0fkb", overlayPartKb_[i]);
        if (overlayPartMs_[i] > 0.0) {
            std::snprintf(lineMs, sizeof(lineMs), "%.1fms", overlayPartMs_[i]);
        }
        else {
            std::snprintf(lineMs, sizeof(lineMs), "--ms");
        }

        int scale = 2;
        const int lineGap = 3;
        auto calcBoxW = [&](int s) {
            return (std::max)({ debugTextWidth(lineCpu, s), debugTextWidth(lineQ, s), debugTextWidth(lineKb, s), debugTextWidth(lineMs, s) }) + 10;
            };
        auto calcBoxH = [&](int s) {
            return 4 * debugTextHeight(s) + 3 * lineGap + 10;
            };
        int boxW = calcBoxW(scale);
        int boxH = calcBoxH(scale);
        if (overlayPartWidth_[i] < boxW + 8 || overlayPartHeight_[i] < boxH + 8) {
            scale = 1;
            boxW = calcBoxW(scale);
            boxH = calcBoxH(scale);
        }
        if (overlayPartWidth_[i] < boxW + 4 || overlayPartHeight_[i] < boxH + 4) {
            continue;
        }

        int boxLeft = left + 6;
        int boxTop = top + 6;
        if (boxLeft + boxW > right - 2) boxLeft = (std::max)(left + 2, right - boxW - 2);
        if (boxTop + boxH > bottom - 2) boxTop = (std::max)(top + 2, bottom - boxH - 2);

        fillRectBlend(bgra, width, height, pitch, boxLeft, boxTop, boxLeft + boxW, boxTop + boxH, 0, 0, 0, 170);
        drawRectOutline(bgra, width, height, pitch, boxLeft, boxTop, boxLeft + boxW, boxTop + boxH, 1, 255, 255, 255, 100);

        int ty = boxTop + 5;
        drawDebugTextRaw(bgra, width, height, pitch, boxLeft + 5, ty, lineCpu, scale, 255, 255, 255, 255);
        ty += debugTextHeight(scale) + lineGap;
        drawDebugTextRaw(bgra, width, height, pitch, boxLeft + 5, ty, lineQ, scale, lineB, lineG, lineR, 255);
        ty += debugTextHeight(scale) + lineGap;
        drawDebugTextRaw(bgra, width, height, pitch, boxLeft + 5, ty, lineKb, scale, 200, 255, 200, 255);
        ty += debugTextHeight(scale) + lineGap;
        drawDebugTextRaw(bgra, width, height, pitch, boxLeft + 5, ty, lineMs, scale, 200, 220, 255, 255);
    }

}


// currentH264DebugRect
bool D3D11Renderer::currentH264DebugRect(int frameW, int frameH, int& left, int& top, int& right, int& bottom) const {
    left = top = right = bottom = 0;
    if (!settings_.centerRoiUseVideo || frameW <= 0 || frameH <= 0) return false;

    // 优先使用当前已经收到的视频帧元数据。这样“应用视频参数”后，F4 边框会跟真实 H.264 ROI 一致，
    // 不依赖设置窗口里的旧值，也不会因为视频纹理覆盖 JPEG 背景而消失。
    if (validCenterRoiPlacement(currentCenterRoiFrame_) &&
        currentCenterRoiFrame_.centerFullWidth == frameW &&
        currentCenterRoiFrame_.centerFullHeight == frameH) {
        left = currentCenterRoiFrame_.centerRoiLeft;
        top = currentCenterRoiFrame_.centerRoiTop;
        right = currentCenterRoiFrame_.centerRoiLeft + currentCenterRoiFrame_.centerRoiWidth;
        bottom = currentCenterRoiFrame_.centerRoiTop + currentCenterRoiFrame_.centerRoiHeight;
    }
    else {
        auto pixelsFromPercent = [](int total, int percent) -> int {
            if (total <= 0) return 0;
            const long long v = static_cast<long long>(total) * static_cast<long long>(percent);
            int px = static_cast<int>((v + 50LL) / 100LL);
            return (std::max)(0, (std::min)(total, px));
        };

        const int region = VideoStreamTuning::NormalizeRoiRegion(settings_.roiRegion);
        if (region == ROI_REGION_BOTTOM) {
            const int videoH = pixelsFromPercent(frameH, VideoStreamTuning::NormalizeRoiPercent(settings_.roiHeightPercent));
            if (videoH <= 0) return false;
            left = 0;
            top = frameH - videoH;
            right = frameW;
            bottom = frameH;
        }
        else {
            const int videoW = pixelsFromPercent(frameW, VideoStreamTuning::NormalizeRoiPercent(settings_.roiWidthPercent));
            const int videoH = pixelsFromPercent(frameH, VideoStreamTuning::NormalizeRoiPercent(settings_.roiHeightPercent));
            if (videoW <= 0 || videoH <= 0) return false;
            left = (frameW - videoW) / 2;
            top = (frameH - videoH) / 2;
            right = left + videoW;
            bottom = top + videoH;
        }
    }

    left = (std::max)(0, (std::min)(frameW, left));
    top = (std::max)(0, (std::min)(frameH, top));
    right = (std::max)(0, (std::min)(frameW, right));
    bottom = (std::max)(0, (std::min)(frameH, bottom));
    return right > left + 4 && bottom > top + 4;
}


// fillRectSetBgra
void D3D11Renderer::fillRectSetBgra(uint8_t* bgra, int width, int height, int pitch, int left, int top, int right, int bottom,
    uint8_t b, uint8_t g, uint8_t r, uint8_t alpha) {
    left = (std::max)(0, left);
    top = (std::max)(0, top);
    right = (std::min)(width, right);
    bottom = (std::min)(height, bottom);
    if (!bgra || left >= right || top >= bottom || alpha == 0) return;
    for (int y = top; y < bottom; ++y) {
        uint8_t* row = bgra + size_t(y) * size_t(pitch) + size_t(left) * 4u;
        for (int x = left; x < right; ++x, row += 4) {
            row[0] = b;
            row[1] = g;
            row[2] = r;
            row[3] = alpha;
        }
    }
}


// drawRectOutlineSetBgra
void D3D11Renderer::drawRectOutlineSetBgra(uint8_t* bgra, int width, int height, int pitch, int left, int top, int right, int bottom, int thickness,
    uint8_t b, uint8_t g, uint8_t r, uint8_t alpha) {
    fillRectSetBgra(bgra, width, height, pitch, left, top, right, top + thickness, b, g, r, alpha);
    fillRectSetBgra(bgra, width, height, pitch, left, bottom - thickness, right, bottom, b, g, r, alpha);
    fillRectSetBgra(bgra, width, height, pitch, left, top, left + thickness, bottom, b, g, r, alpha);
    fillRectSetBgra(bgra, width, height, pitch, right - thickness, top, right, bottom, b, g, r, alpha);
}


// drawH264DebugOverlayAfterComposite
void D3D11Renderer::drawH264DebugOverlayAfterComposite(uint8_t* bgra, int overlayW, int overlayH, int pitch, int frameW, int frameH) const {
    if (!splitDebugOverlayVisible_ || !settings_.centerRoiUseVideo || !bgra || overlayW <= 0 || overlayH <= 0 || frameW <= 0 || frameH <= 0) return;

    int fl = 0, ft = 0, fr = 0, fb = 0;
    if (!currentH264DebugRect(frameW, frameH, fl, ft, fr, fb)) return;

    auto mapX = [&](int x) -> int {
        return static_cast<int>((static_cast<long long>(x) * overlayW + frameW / 2) / frameW);
    };
    auto mapY = [&](int y) -> int {
        return static_cast<int>((static_cast<long long>(y) * overlayH + frameH / 2) / frameH);
    };

    const int vl = (std::max)(0, (std::min)(overlayW, mapX(fl)));
    const int vt = (std::max)(0, (std::min)(overlayH, mapY(ft)));
    const int vr = (std::max)(0, (std::min)(overlayW, mapX(fr)));
    const int vb = (std::max)(0, (std::min)(overlayH, mapY(fb)));
    if (vr <= vl + 4 || vb <= vt + 4) return;

    static constexpr uint8_t videoLineB = 255;
    static constexpr uint8_t videoLineG = 210;
    static constexpr uint8_t videoLineR = 48;

    fillRectSetBgra(bgra, overlayW, overlayH, pitch, vl, vt, vr, vb, videoLineB, 120, 48, 32);
    drawRectOutlineSetBgra(bgra, overlayW, overlayH, pitch, vl, vt, vr, vb, 3, videoLineB, videoLineG, videoLineR, 235);

    const int codecW = cachedCenterVideoCodecWidth_ > 0
        ? cachedCenterVideoCodecWidth_
        : (currentCenterRoiFrame_.centerCodecWidth > 0 ? currentCenterRoiFrame_.centerCodecWidth : (fr - fl));
    const int codecH = cachedCenterVideoCodecHeight_ > 0
        ? cachedCenterVideoCodecHeight_
        : (currentCenterRoiFrame_.centerCodecHeight > 0 ? currentCenterRoiFrame_.centerCodecHeight : (fb - ft));
    const int bitrateMbps = VideoStreamTuning::NormalizeVideoBitrateMbps(settings_.videoBitrateMbps);

    char lineCodec[32]{};
    char lineBitrate[32]{};
    char lineRes[32]{};
    std::snprintf(lineCodec, sizeof(lineCodec), "H264");
    if (cachedCenterVideoMbps_ > 0.05) {
        std::snprintf(lineBitrate, sizeof(lineBitrate), "%.1fMbps", cachedCenterVideoMbps_);
    }
    else {
        std::snprintf(lineBitrate, sizeof(lineBitrate), "%dMbps", bitrateMbps);
    }
    std::snprintf(lineRes, sizeof(lineRes), "%dx%d", codecW, codecH);

    int scale = 2;
    const int lineGap = 3;
    auto calcVideoBoxW = [&](int s) {
        return (std::max)({ debugTextWidth(lineCodec, s), debugTextWidth(lineBitrate, s), debugTextWidth(lineRes, s) }) + 10;
    };
    auto calcVideoBoxH = [&](int s) {
        return 3 * debugTextHeight(s) + 2 * lineGap + 10;
    };
    int boxW = calcVideoBoxW(scale);
    int boxH = calcVideoBoxH(scale);
    if ((vr - vl) < boxW + 8 || (vb - vt) < boxH + 8) {
        scale = 1;
        boxW = calcVideoBoxW(scale);
        boxH = calcVideoBoxH(scale);
    }
    if ((vr - vl) < boxW + 4 || (vb - vt) < boxH + 4) return;

    int boxLeft = vl + 6;
    int boxTop = vt + 6;
    if (boxLeft + boxW > vr - 2) boxLeft = (std::max)(vl + 2, vr - boxW - 2);
    if (boxTop + boxH > vb - 2) boxTop = (std::max)(vt + 2, vb - boxH - 2);

    fillRectSetBgra(bgra, overlayW, overlayH, pitch, boxLeft, boxTop, boxLeft + boxW, boxTop + boxH, 0, 0, 0, 190);
    drawRectOutlineSetBgra(bgra, overlayW, overlayH, pitch, boxLeft, boxTop, boxLeft + boxW, boxTop + boxH, 1, videoLineB, videoLineG, videoLineR, 160);

    int ty = boxTop + 5;
    drawDebugTextRaw(bgra, overlayW, overlayH, pitch, boxLeft + 5, ty, lineCodec, scale, videoLineB, videoLineG, videoLineR, 255);
    ty += debugTextHeight(scale) + lineGap;
    drawDebugTextRaw(bgra, overlayW, overlayH, pitch, boxLeft + 5, ty, lineBitrate, scale, 220, 255, 220, 255);
    ty += debugTextHeight(scale) + lineGap;
    drawDebugTextRaw(bgra, overlayW, overlayH, pitch, boxLeft + 5, ty, lineRes, scale, 200, 220, 255, 255);
}


// debugTextWidth
int D3D11Renderer::debugTextWidth(const char* text, int scale) {
        return static_cast<int>(std::strlen(text)) * 6 * scale;
    }


// debugTextHeight
int D3D11Renderer::debugTextHeight(int scale) {
        return 7 * scale;
    }


// drawDebugTextRaw
int D3D11Renderer::drawDebugTextRaw(uint8_t* bgra, int width, int height, int pitch, int x, int y,
        const char* text, int scale,
        uint8_t b, uint8_t g, uint8_t r, uint8_t alpha) {
        int cursorX = x;
        for (const char* p = text; *p; ++p) {
            const char ch = static_cast<char>(std::tolower(static_cast<unsigned char>(*p)));
            const uint8_t* rows = FindHudGlyph(ch);
            for (int row = 0; row < 7; ++row) {
                for (int col = 0; col < 5; ++col) {
                    if (rows[row] & (1 << (4 - col))) {
                        fillRectBlend(bgra, width, height, pitch,
                            cursorX + col * scale,
                            y + row * scale,
                            cursorX + col * scale + scale,
                            y + row * scale + scale,
                            b, g, r, alpha);
                    }
                }
            }
            cursorX += 6 * scale;
        }
        return cursorX - x;
    }


// roiEdgePartsForCount
int D3D11Renderer::roiEdgePartsForCount(int partCount) {
        if (partCount < 4) return 0;
        // Keep this in sync with Android native balanced ROI:
        // 4 -> 1|2|1, 5 -> 1|3|1, 6 -> 2|2|2, 7 -> 2|3|2, 8 -> 2|4|2.
        int edge = (partCount + 2) / 4;
        if (edge < 1) edge = 1;
        const int maxEdge = (partCount - 2) / 2;
        if (edge > maxEdge) edge = maxEdge;
        return edge;
    }

