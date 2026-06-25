#pragma once

// PC-side defaults and valid ranges for the center ROI H.264 video stream and
// hybrid ROI/JPEG policy. Keep UI defaults, reset values, validation, and
// Android control command clamping in this file so the C++ side has one source
// of truth.
namespace VideoStreamTuning {

// ========================
// H.264 ROI video defaults
// ========================

static constexpr int kVideoPresetDefault = 2; // Compatibility/status only; ROI follows the JPEG/fullscreen preset.
static constexpr int kVideoWidthDefault = 1600;
static constexpr int kVideoHeightDefault = 1024;

//码率范围 
static constexpr int kVideoBitrateMinMbps = 10; //最小10M
static constexpr int kVideoBitrateMaxMbps = 500; //最大500M
static constexpr int kVideoBitrateDefaultMbps = 40;//默认40M

// FPS  
static constexpr int kVideoFpsMin = 30;  //最小30
static constexpr int kVideoFpsMax = 240;  //最大240
static constexpr int kVideoFpsDefault = 144;//默认144

// 0=低延迟顺帧，1=高画质顺帧，2=高压缩 GOP。
// 注意：这里只定义 PC 侧传给 Android 的 qualityMode 值；实际编码器参数由 Android 端按该值配置。
static constexpr int kVideoQualityLowLatency = 0;
static constexpr int kVideoQualityHighQuality = 1;
static constexpr int kVideoQualityGop = 2;

// 默认视频质量模式：高画质顺帧。
// 如果想默认优先低延迟，把这里改成 kVideoQualityLowLatency。
static constexpr int kVideoQualityDefault = kVideoQualityHighQuality;

// H.264 ROI 编码尺寸允许范围。
// 实际 ROI 尺寸通常跟随采集/JPEG 分辨率和 ROI 百分比计算，这里主要用于旧协议兼容和输入保护。
static constexpr int kVideoCodecWidthMin = 320;
static constexpr int kVideoCodecWidthMax = 3840;
static constexpr int kVideoCodecHeightMin = 180;
static constexpr int kVideoCodecHeightMax = 2160;

// ========================
// ROI / hybrid stream
// ROI / 混合流参数
// ========================

// 默认启用 H.264 视频 ROI。
// true = ROI 区域走 H.264，其余区域走 JPEG；false = 全部走 JPEG。
static constexpr bool kCenterRoiUseVideoDefault = true;

// 视频 ROI 位置。
// Center：中心区域走 H.264；Bottom：底部区域走 H.264，上方/中心高价值区域保留 JPEG。
static constexpr int kRoiRegionCenter = 0;
static constexpr int kRoiRegionBottom = 1;

// 默认底部视频 ROI：把低价值底部区域交给 H.264，中心高价值区域继续 JPEG。
static constexpr int kRoiRegionDefault = kRoiRegionBottom;

// ROI 外围 JPEG 质量降低范围。
// 数值越大，外围 JPEG 质量越低、码率/编码压力越小。
static constexpr int kRoiEdgeQualityReductionMin = 0;
static constexpr int kRoiEdgeQualityReductionMax = 90;
static constexpr int kRoiEdgeQualityReductionDefault = 50;

// ROI 外围 JPEG 横向缩放百分比范围。
// 100 = 不缩放；75 = 外围区域按 75% 编码后再显示，可降低 CPU/JPEG 压力。
static constexpr int kRoiEdgeScalePercentMin = 10;
static constexpr int kRoiEdgeScalePercentMax = 100;
static constexpr int kRoiEdgeScalePercentDefault = 75;

// ROI 尺寸百分比范围。
// Center 模式下同时影响视频 ROI 宽高；Bottom 模式下主要使用高度百分比。
static constexpr int kRoiPercentMin = 20;
static constexpr int kRoiPercentMax = 90;

// 默认 ROI 宽高百分比。
// 当前默认宽 63%，高 20%；Bottom 模式下高 20% 表示底部约 20% 屏幕走 H.264。
static constexpr int kRoiWidthPercentDefault = 63;
static constexpr int kRoiHeightPercentDefault = 20;

// JPEG 中心高质量矩形：高度百分比。
// 旧 UI/协议字段名仍叫 TopLow，但新版语义是“JPEG 中心高”。
static constexpr int kRoiTopLowPercentMin = 10;
static constexpr int kRoiTopLowPercentMax = 80;
static constexpr int kRoiTopLowPercentDefault = 50;

// JPEG 中心高质量矩形：宽度百分比。
// 纯 JPEG / 底部 H.264 模式共用；中心 H.264 模式下 JPEG 只负责外圈，不使用该中心矩形。
static constexpr int kRoiJpegCenterWidthPercentMin = 10;
static constexpr int kRoiJpegCenterWidthPercentMax = 80;
static constexpr int kRoiJpegCenterWidthPercentDefault = 30;

// JPEG 中心高质量区域使用的核心数量。
// 继续复用旧字段/控件名 roiCenterCpuWeightPercent，但新版语义是“中心核心数”。
// 1 表示中心区域切成 1 块；N 表示中心区域切成 N 块。
// PC 界面会按“全屏核心数”动态限制；Android native 会再按实际可用 worker 数夹取。
static constexpr int kRoiCenterCoreCountMin = 1;
static constexpr int kRoiCenterCoreCountMax = 24;
static constexpr int kRoiCenterCoreCountDefault = 4;

// 大核心权重百分比：
// 100 = 中心区域平均切块；
// >100 = 最高频核心对应块更大；
// <100 = 最高频核心对应块更小。
static constexpr int kRoiBigCoreWeightPercentMin = 50;
static constexpr int kRoiBigCoreWeightPercentMax = 200;
static constexpr int kRoiBigCoreWeightPercentDefault = 120;

// 只投中心区域：
// false = 中心高质量 + 外围低质量；true = Android/native 只编码中心高质量矩形，外围不编码不发送。
// 中心 H.264 模式下 true 表示只发送 H.264 中心视频，不再编码 JPEG 外圈。
static constexpr bool kJpegCenterOnlyDefault = false;

// 兼容旧代码命名。
static constexpr int kRoiCenterCpuWeightPercentMin = kRoiCenterCoreCountMin;
static constexpr int kRoiCenterCpuWeightPercentMax = kRoiCenterCoreCountMax;
static constexpr int kRoiCenterCpuWeightPercentDefault = kRoiCenterCoreCountDefault;

// ========================
// Helpers
// ========================

constexpr int ClampInt(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

constexpr int NormalizeVideoBitrateMbps(int v) {
    return ClampInt(v, kVideoBitrateMinMbps, kVideoBitrateMaxMbps);
}

constexpr int NormalizeVideoFps(int v) {
    return ClampInt(v, kVideoFpsMin, kVideoFpsMax);
}

constexpr int NormalizeVideoQualityMode(int v) {
    return ClampInt(v, kVideoQualityLowLatency, kVideoQualityGop);
}

constexpr int NormalizeVideoWidth(int v) {
    return ClampInt(v, kVideoCodecWidthMin, kVideoCodecWidthMax);
}

constexpr int NormalizeVideoHeight(int v) {
    return ClampInt(v, kVideoCodecHeightMin, kVideoCodecHeightMax);
}

constexpr int NormalizeRoiEdgeQualityReduction(int v) {
    return ClampInt(v, kRoiEdgeQualityReductionMin, kRoiEdgeQualityReductionMax);
}

constexpr int NormalizeRoiEdgeScalePercent(int v) {
    return ClampInt(v, kRoiEdgeScalePercentMin, kRoiEdgeScalePercentMax);
}

constexpr int NormalizeRoiPercent(int v) {
    return ClampInt(v, kRoiPercentMin, kRoiPercentMax);
}

constexpr int NormalizeRoiTopLowPercent(int v) {
    return ClampInt(v, kRoiTopLowPercentMin, kRoiTopLowPercentMax);
}

constexpr int NormalizeRoiJpegCenterWidthPercent(int v) {
    return ClampInt(v, kRoiJpegCenterWidthPercentMin, kRoiJpegCenterWidthPercentMax);
}

constexpr int NormalizeRoiCenterCoreCount(int v) {
    // 兼容旧配置：旧版中心权重默认 100。如果读到明显超出核心数范围的旧值，
    // 不要夹成 16 核，而是回到新版默认中心核心数。
    if (v > kRoiCenterCoreCountMax) return kRoiCenterCoreCountDefault;
    return ClampInt(v, kRoiCenterCoreCountMin, kRoiCenterCoreCountMax);
}

constexpr int NormalizeRoiCenterCpuWeightPercent(int v) {
    return NormalizeRoiCenterCoreCount(v);
}

constexpr int NormalizeRoiBigCoreWeightPercent(int v) {
    return ClampInt(v, kRoiBigCoreWeightPercentMin, kRoiBigCoreWeightPercentMax);
}

constexpr int NormalizeRoiRegion(int v) {
    return v == kRoiRegionCenter ? kRoiRegionCenter : kRoiRegionBottom;
}

} // namespace VideoStreamTuning
