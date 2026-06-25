// CenterRoiVideoConfig.kt
package com.example.huilangtoupingV3

object CenterRoiVideoConfig {
    // H.264 ROI / codec 目标尺寸。新感知优先模式下，视频流默认放到底部低价值区域；
    // 实际尺寸由当前 JPEG/采集分辨率动态裁剪。
    const val WIDTH = 1600
    const val HEIGHT = 1024

    // 默认中心视频码率。PC 控制台“应用视频参数”会通过 MediaCodec.setParameters()
    // 动态更新运行中的编码器码率。
    const val BITRATE = 120_000_000

    // 编码器目标 fps。实际输出帧率受游戏/VirtualDisplay 源帧率限制。
    const val FPS = 144
    const val MIN_FPS = 30
    const val MAX_FPS = 240

    fun normalizeFps(fps: Int): Int {
        return fps.coerceIn(MIN_FPS, MAX_FPS)
    }
    // 可选：质量模式也统一掉
    const val QUALITY_MODE = 0
    const val MIN_QUALITY_MODE = 0
    const val MAX_QUALITY_MODE = 2

    fun normalizeQualityMode(mode: Int): Int {
        return mode.coerceIn(MIN_QUALITY_MODE, MAX_QUALITY_MODE)
    }

    // 默认 ROI 策略：true=视频 ROI + 其余区域 JPEG；false=整帧 JPEG。
    const val USE_VIDEO_STREAM = true

    // 视频 ROI 位置。
    // CENTER：旧方案，中心区域 H.264，外围 JPEG。
    // BOTTOM：新方案，底部低价值区域 H.264，上部/中心高价值区域继续 JPEG。
    const val ROI_REGION_CENTER = 0
    const val ROI_REGION_BOTTOM = 1
    const val ROI_DEFAULT_REGION = ROI_REGION_BOTTOM

    // 外围低价值区域的 JPEG 策略，由 PC 控制台 HLROI 动态覆盖。
    const val OUTER_JPEG_QUALITY_REDUCTION = 10
    const val OUTER_JPEG_SCALE_PERCENT = 75

    const val MIN_BITRATE = 10_000_000
    const val MAX_BITRATE = 500_000_000
    const val MIN_BITRATE_MBPS = MIN_BITRATE / 1_000_000
    const val MAX_BITRATE_MBPS = MAX_BITRATE / 1_000_000

    fun normalizeBitrateMbps(mbps: Int): Int {
        return mbps.coerceIn(MIN_BITRATE_MBPS, MAX_BITRATE_MBPS)
    }

    fun normalizeBitrateBps(bps: Int): Int {
        return bps.coerceIn(MIN_BITRATE, MAX_BITRATE)
    }

    const val MIN_CODEC_WIDTH = 640
    const val MIN_CODEC_HEIGHT = 180
    const val MAX_CODEC_WIDTH = 3840
    const val MAX_CODEC_HEIGHT = 2160

    // 视频 ROI 手动尺寸按当前采集分辨率百分比控制。
    // 新底部视频模式主要使用高度百分比；范围 20%..90%。
    const val ROI_MIN_SOURCE_PERCENT = 20
    const val ROI_MAX_SOURCE_PERCENT = 90
    // 底部视频默认占屏幕高度 25%，把高价值中心/上半区域留给 CPU/JPEG。
    const val ROI_DEFAULT_WIDTH_SOURCE_PERCENT = 70
    const val ROI_DEFAULT_HEIGHT_SOURCE_PERCENT = 25
}
