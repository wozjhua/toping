package com.example.huilangtoupingV3

import android.content.Context
import android.content.Intent

import java.util.Locale

class PcScreenMirrorCoordinator(
    context: Context,
    private val onRequireForeground: () -> Unit,
    private val onReleaseForeground: () -> Unit,
) {
    private val appContext = context.applicationContext
    private val settingsStore = AppSettingsStore(appContext)

    private var captureController: CenterScreenCaptureController? = null
    private var frameSender: JpegFrameSender? = null
    private var activeResultCode: Int? = null
    private var activeResultData: Intent? = null
    private var activeSettings: AppSettingsSnapshot? = null

    companion object {
        const val LOCAL_SOCKET_NAME = "huilang_screen_mirror"
    }

    fun start(
        resultCode: Int,
        resultData: Intent,
        settings: AppSettingsSnapshot = settingsStore.read(),
    ) {
        stop("restart_before_start")

        val normalized = normalizeRuntimeSettings(settings)

        activeResultCode = resultCode
        activeResultData = Intent(resultData)
        activeSettings = normalized

        persistRuntimeSettings(normalized)
        settingsStore.setScreenMirrorEnabled(true)
        settingsStore.setScreenMirrorSummary(summaryFor(normalized, prefix = "画面传输：初始化中"))
        settingsStore.setScreenMirrorCoreStatus("核心状态：初始化中")
        onRequireForeground()

        val sender = JpegFrameSender(
            socketName = LOCAL_SOCKET_NAME,
            onStatusChanged = { status -> settingsStore.setScreenMirrorSummary(status) },
            onPerfText = null,
            onControlCommand = { command -> handlePcControlCommand(command) },
        )
        frameSender = sender

        try {
            settingsStore.setScreenMirrorSummary("画面传输：正在启动采集")
            sender.start()
            startCaptureWithSettings(normalized)
        } catch (t: Throwable) {
            settingsStore.setScreenMirrorEnabled(false)
            settingsStore.setScreenMirrorSummary(
                buildString {
                    append("画面传输：启动失败 ")
                    append(t.javaClass.simpleName)
                    if (!t.message.isNullOrBlank()) {
                        append(": ")
                        append(t.message)
                    }
                }
            )
            release("start_failed")
            throw t
        }
    }

    private fun normalizeRuntimeSettings(settings: AppSettingsSnapshot): AppSettingsSnapshot {
        return settings.copy(
            screenMirrorCaptureMode = settings.screenMirrorCaptureMode
                .coerceIn(AppSettingsStore.Companion.MIN_SCREEN_MIRROR_CAPTURE_MODE, AppSettingsStore.Companion.MAX_SCREEN_MIRROR_CAPTURE_MODE),
            screenMirrorCropSize = settings.screenMirrorCropSize.takeIf { it > 0 }
                ?.coerceIn(AppSettingsStore.Companion.MIN_SCREEN_MIRROR_CROP_SIZE, AppSettingsStore.Companion.MAX_SCREEN_MIRROR_CROP_SIZE)
                ?: AppSettingsStore.Companion.DEFAULT_SCREEN_MIRROR_CROP_SIZE,
            // Resolution is controlled by the unified JPEG/fullscreen preset.
            // 720p is no longer used; default to 1080p when an old/invalid value is seen.
            screenMirrorFullscreenPreset = settings.screenMirrorFullscreenPreset
                .takeIf { it >= AppSettingsStore.Companion.SCREEN_MIRROR_FULLSCREEN_PRESET_900P }
                ?.coerceAtMost(AppSettingsStore.Companion.MAX_SCREEN_MIRROR_FULLSCREEN_PRESET)
                ?: AppSettingsStore.Companion.SCREEN_MIRROR_FULLSCREEN_PRESET_1080P,
            screenMirrorJpegQuality = settings.screenMirrorJpegQuality.takeIf { it > 0 }
                ?.coerceIn(AppSettingsStore.Companion.MIN_SCREEN_MIRROR_JPEG_QUALITY, AppSettingsStore.Companion.MAX_SCREEN_MIRROR_JPEG_QUALITY)
                ?: AppSettingsStore.Companion.DEFAULT_SCREEN_MIRROR_JPEG_QUALITY,
            screenMirrorJpegSubsamplingMode = AppSettingsStore.Companion.normalizeJpegSubsamplingMode(settings.screenMirrorJpegSubsamplingMode),
            screenMirrorFullscreenSplitParts = AppSettingsStore.Companion.normalizeFullscreenSplitParts(settings.screenMirrorFullscreenSplitParts),
            screenMirrorCropSplitParts = AppSettingsStore.Companion.normalizeCropSplitParts(settings.screenMirrorCropSplitParts),
            screenMirrorFps = AppSettingsStore.Companion.normalizeFpsPreset(
                settings.screenMirrorFps.takeIf { it > 0 } ?: AppSettingsStore.Companion.DEFAULT_SCREEN_MIRROR_FPS
            ),
        )
    }

    private fun persistRuntimeSettings(settings: AppSettingsSnapshot) {
        settingsStore.setScreenMirrorConfig(
            captureMode = settings.screenMirrorCaptureMode,
            cropSize = settings.screenMirrorCropSize,
            fullscreenPreset = settings.screenMirrorFullscreenPreset,
            jpegQuality = settings.screenMirrorJpegQuality,
            jpegSubsamplingMode = settings.screenMirrorJpegSubsamplingMode,
            keepFrameRate = settings.screenMirrorKeepFrameRate,
            fullscreenSplitParts = settings.screenMirrorFullscreenSplitParts,
            cropSplitParts = settings.screenMirrorCropSplitParts,
            fps = settings.screenMirrorFps,
            port = settings.screenMirrorPort,
        )
    }

    private fun startCaptureWithSettings(settings: AppSettingsSnapshot) {
        val resultCode = activeResultCode ?: error("missing resultCode")
        val resultData = activeResultData ?: error("missing resultData")
        val sender = frameSender ?: error("missing frameSender")
        val capture = CenterScreenCaptureController(
            context = appContext,
            resultCode = resultCode,
            resultData = Intent(resultData),
            captureMode = settings.screenMirrorCaptureMode,
            cropSize = settings.screenMirrorCropSize,
            fullscreenPreset = settings.screenMirrorFullscreenPreset,
            jpegQuality = settings.screenMirrorJpegQuality,
            jpegSubsamplingMode = settings.screenMirrorJpegSubsamplingMode,
            keepFrameRate = settings.screenMirrorKeepFrameRate,
            fps = settings.screenMirrorFps,
            fullscreenSplitParts = settings.screenMirrorFullscreenSplitParts,
            cropSplitParts = settings.screenMirrorCropSplitParts,
            isFrameSinkReady = {
                sender.isConnected && NativeCenterCropJpegEncoder.hasOutputFileDescriptor()
            },
            onPerfText = null,
            onCoreStatusText = { text -> settingsStore.setScreenMirrorCoreStatus(text) },
            onAutoFallbackTo720p = null,
        )
        NativeCenterCropJpegEncoder.setJpegSubsamplingMode(settings.screenMirrorJpegSubsamplingMode)
        NativeCenterCropJpegEncoder.setSplitRoiQualityParams(
            CenterRoiVideoConfig.OUTER_JPEG_QUALITY_REDUCTION,
            CenterRoiVideoConfig.OUTER_JPEG_SCALE_PERCENT,
        )
        NativeCenterCropJpegEncoder.setSplitLayoutParams(30, 30, 4, false)
        settingsStore.setScreenMirrorCoreStatus("核心状态：启动采集线程 / JPEG${AppSettingsStore.Companion.jpegSubsamplingLabel(settings.screenMirrorJpegSubsamplingMode)}")
        capture.start()
        captureController = capture
        activeSettings = settings
        settingsStore.setScreenMirrorSummary(summaryFor(settings, prefix = "画面传输：等待 PC 连接"))
    }

    private fun applyPcRuntimeSettings(settings: AppSettingsSnapshot, reason: String) {
        val normalized = normalizeRuntimeSettings(settings)
        activeSettings = normalized
        persistRuntimeSettings(normalized)
        NativeCenterCropJpegEncoder.setJpegSubsamplingMode(normalized.screenMirrorJpegSubsamplingMode)
        val ok = captureController?.updateRuntimeConfig(
            newCaptureMode = normalized.screenMirrorCaptureMode,
            newCropSize = normalized.screenMirrorCropSize,
            newFullscreenPreset = normalized.screenMirrorFullscreenPreset,
            newJpegQuality = normalized.screenMirrorJpegQuality,
            newJpegSubsamplingMode = normalized.screenMirrorJpegSubsamplingMode,
            newKeepFrameRate = normalized.screenMirrorKeepFrameRate,
            newFps = normalized.screenMirrorFps,
            newFullscreenSplitParts = normalized.screenMirrorFullscreenSplitParts,
            newCropSplitParts = normalized.screenMirrorCropSplitParts,
        ) ?: false
        settingsStore.setScreenMirrorSummary(summaryFor(normalized, prefix = if (ok) "画面传输：PC已更新参数" else "画面传输：PC参数已保存"))
        settingsStore.setScreenMirrorCoreStatus(
            if (ok) "核心状态：$reason 已动态应用"
            else "核心状态：$reason 已保存，等待采集启动后生效"
        )
    }

    private fun handlePcControlCommand(command: String) {
        val parts = command.trim().split(Regex("\\s+")).filter { it.isNotBlank() }
        if (parts.isEmpty()) return
        val op = parts[0].toUpperCase(Locale.US)
        val current = activeSettings ?: settingsStore.read()

        when (op) {
            "HLJPGSUB" -> {
                val safe = if (parts.getOrNull(1)?.toIntOrNull() == 444) 444 else 420
                val updated = current.copy(screenMirrorJpegSubsamplingMode = safe)
                val label = if (safe == 444) "极限保真 4:4:4" else "高质量 4:2:0"
                applyPcRuntimeSettings(updated, "PC已切换JPEG采样 $label")
                return
            }

            "HLJPGQ" -> {
                val q = parts.getOrNull(1)?.toIntOrNull()?.coerceIn(AppSettingsStore.Companion.MIN_SCREEN_MIRROR_JPEG_QUALITY, AppSettingsStore.Companion.MAX_SCREEN_MIRROR_JPEG_QUALITY) ?: return
                applyPcRuntimeSettings(current.copy(screenMirrorJpegQuality = q), "PC已切换JPEG质量Q$q")
                return
            }
            "HLFPS" -> {
                val fps = AppSettingsStore.Companion.normalizeFpsPreset(parts.getOrNull(1)?.toIntOrNull() ?: return)
                applyPcRuntimeSettings(current.copy(screenMirrorFps = fps), "PC已切换FPS $fps")
                return
            }
            "HLPRESET" -> {
                val preset = parts.getOrNull(1)?.toIntOrNull()
                    ?.coerceIn(
                        AppSettingsStore.Companion.MIN_SCREEN_MIRROR_FULLSCREEN_PRESET,
                        AppSettingsStore.Companion.MAX_SCREEN_MIRROR_FULLSCREEN_PRESET,
                    )
                    ?: current.screenMirrorFullscreenPreset
                applyPcRuntimeSettings(
                    current.copy(
                        screenMirrorCaptureMode = AppSettingsStore.Companion.SCREEN_MIRROR_CAPTURE_MODE_FULLSCREEN_SCALED,
                        screenMirrorFullscreenPreset = preset,
                    ),
                    "PC已切换分辨率为${AppSettingsStore.Companion.fullscreenPresetLabel(preset)}"
                )
                return
            }
            "HLSPLIT" -> {
                val full = parts.getOrNull(1)?.toIntOrNull()?.let { AppSettingsStore.Companion.normalizeFullscreenSplitParts(it) } ?: current.screenMirrorFullscreenSplitParts
                val crop = parts.getOrNull(2)?.toIntOrNull()?.let { AppSettingsStore.Companion.normalizeCropSplitParts(it) } ?: current.screenMirrorCropSplitParts
                applyPcRuntimeSettings(current.copy(screenMirrorFullscreenSplitParts = full, screenMirrorCropSplitParts = crop), "PC已切换分块 全屏${full}/裁剪${crop}")
                return
            }
            "HLCAP" -> {
                val mode = parts.getOrNull(1)?.toIntOrNull()?.coerceIn(AppSettingsStore.Companion.MIN_SCREEN_MIRROR_CAPTURE_MODE, AppSettingsStore.Companion.MAX_SCREEN_MIRROR_CAPTURE_MODE) ?: return
                applyPcRuntimeSettings(current.copy(screenMirrorCaptureMode = mode), "PC已切换采集模式")
                return
            }
            "HLCROP" -> {
                val size = parts.getOrNull(1)?.toIntOrNull()?.coerceIn(AppSettingsStore.Companion.MIN_SCREEN_MIRROR_CROP_SIZE, AppSettingsStore.Companion.MAX_SCREEN_MIRROR_CROP_SIZE) ?: return
                applyPcRuntimeSettings(
                    current.copy(
                        screenMirrorCaptureMode = AppSettingsStore.Companion.SCREEN_MIRROR_CAPTURE_MODE_CENTER_CROP,
                        screenMirrorCropSize = size,
                    ),
                    "PC已切换裁剪尺寸 ${size}x${size}"
                )
                return
            }
            "HLKEEP" -> {
                val keep = parts.getOrNull(1)?.let { it == "1" || it.equals("true", ignoreCase = true) } ?: return
                applyPcRuntimeSettings(current.copy(screenMirrorKeepFrameRate = keep), if (keep) "PC已开启保帧率" else "PC已关闭保帧率")
                return
            }
            "HLROI" -> {
                // HLROI <video|jpeg|1|0> <edgeQDrop 0..40> <edgeScalePercent 50..100>
                //       [roiWidthPercent] [roiHeightPercent] [center|bottom]
                //       [jpegCenterHeightPercent 10..80] [jpegCenterWidthPercent 10..80] [centerCoreCount 1..24] [centerOnly] [bigCoreWeightPercent 50..200]
                val modeToken = parts.getOrNull(1)?.lowercase(Locale.US) ?: return
                val useVideo = when (modeToken) {
                    "1", "video", "h264", "on", "true" -> true
                    "0", "jpeg", "jpg", "off", "false" -> false
                    else -> return
                }
                val edgeQDrop = parts.getOrNull(2)?.toIntOrNull()?.coerceIn(0, 40)
                    ?: CenterRoiVideoConfig.OUTER_JPEG_QUALITY_REDUCTION
                val edgeScale = parts.getOrNull(3)?.toIntOrNull()?.coerceIn(50, 100)
                    ?: CenterRoiVideoConfig.OUTER_JPEG_SCALE_PERCENT
                val roiWidthPercent = parts.getOrNull(4)?.toIntOrNull()
                    ?.coerceIn(CenterRoiVideoConfig.ROI_MIN_SOURCE_PERCENT, CenterRoiVideoConfig.ROI_MAX_SOURCE_PERCENT)
                    ?: CenterRoiVideoConfig.ROI_DEFAULT_WIDTH_SOURCE_PERCENT
                val roiHeightPercent = parts.getOrNull(5)?.toIntOrNull()
                    ?.coerceIn(CenterRoiVideoConfig.ROI_MIN_SOURCE_PERCENT, CenterRoiVideoConfig.ROI_MAX_SOURCE_PERCENT)
                    ?: CenterRoiVideoConfig.ROI_DEFAULT_HEIGHT_SOURCE_PERCENT
                val roiRegion = when (parts.getOrNull(6)?.lowercase(Locale.US)) {
                    "bottom", "bottom2", "low", "lower" -> CenterRoiVideoConfig.ROI_REGION_BOTTOM
                    "center", "middle", "mid" -> CenterRoiVideoConfig.ROI_REGION_CENTER
                    else -> CenterRoiVideoConfig.ROI_DEFAULT_REGION
                }
                val topLowPercent = parts.getOrNull(7)?.toIntOrNull()?.coerceIn(10, 80) ?: 30
                val jpegCenterWidthPercent = parts.getOrNull(8)?.toIntOrNull()?.coerceIn(10, 80) ?: 30
                val centerCoreCount = parts.getOrNull(9)?.toIntOrNull()?.coerceIn(1, 24) ?: 4
                val centerOnly = parts.getOrNull(10)?.let { raw ->
                    raw == "1" || raw.equals("true", ignoreCase = true) || raw.equals("center", ignoreCase = true) || raw.equals("only", ignoreCase = true)
                } ?: false
                val bigCoreWeightPercent = parts.getOrNull(11)?.toIntOrNull()?.coerceIn(50, 200) ?: 120
                val ok = captureController?.updateCenterRoiRuntimeConfig(
                    useVideoStream = useVideo,
                    edgeQualityReduction = edgeQDrop,
                    edgeEncodeScalePercent = edgeScale,
                    roiWidthPercent = roiWidthPercent,
                    roiHeightPercent = roiHeightPercent,
                    roiRegion = roiRegion,
                    topLowPercent = topLowPercent,
                    centerWidthPercent = jpegCenterWidthPercent,
                    centerCoreCount = centerCoreCount,
                    centerOnly = centerOnly,
                    bigCoreWeightPercent = bigCoreWeightPercent,
                ) ?: (NativeCenterCropJpegEncoder.setSplitRoiQualityParams(edgeQDrop, edgeScale) &&
                        NativeCenterCropJpegEncoder.setSplitLayoutParams(topLowPercent, jpegCenterWidthPercent, centerCoreCount, centerOnly, bigCoreWeightPercent))
                val regionText = if (roiRegion == CenterRoiVideoConfig.ROI_REGION_BOTTOM) "底部" else "中心"
                settingsStore.setScreenMirrorCoreStatus(
                    if (ok) "核心状态：PC已应用ROI策略 ${regionText} ${if (useVideo) "H264视频" else "JPEG"} ROI=${roiWidthPercent}%x${roiHeightPercent}% 外围Q-${edgeQDrop} 外围缩放=${edgeScale}% 中高=${topLowPercent}% 中宽=${jpegCenterWidthPercent}% 中心=${centerCoreCount}核 只投中心=$centerOnly 大核权重=${bigCoreWeightPercent}%"
                    else "核心状态：PC ROI策略应用失败"
                )
                return
            }
            "HLVIDPRESET" -> {
                val preset = parts.getOrNull(1)?.toIntOrNull()?.coerceIn(1, 5) ?: return
                val bitrateMbps = parts.getOrNull(2)?.toIntOrNull()?.coerceIn(10, 800) ?: return
                val fps = parts.getOrNull(3)?.toIntOrNull()
                    ?.let { CenterRoiVideoConfig.normalizeFps(it) }
                    ?: return
                val quality = parts.getOrNull(4)?.toIntOrNull()
                    ?.let { CenterRoiVideoConfig.normalizeQualityMode(it) }
                    ?: CenterRoiVideoConfig.QUALITY_MODE
                val ok = captureController?.updateDirectVideoPresetRuntimeConfig(
                    preset = preset,
                    bitrateMbps = bitrateMbps,
                    fps = fps,
                    qualityMode = quality,
                ) ?: false
                val pLabel = when (preset) {
                    1 -> "900p"
                    2 -> "1080p"
                    3 -> "2K"
                    4 -> "3K"
                    5 -> "设备最大"
                    else -> "1080p"
                }
                val qLabel = when (quality) {
                    0 -> "低延迟全I帧"
                    2 -> "高压缩GOP"
                    else -> "高画质全I帧"
                }
                settingsStore.setScreenMirrorCoreStatus(
                    if (ok) "核心状态：PC已应用视频预设 $pLabel ${bitrateMbps}Mbps ${fps}fps $qLabel"
                    else "核心状态：PC视频预设已收到，等待视频发送器启动 $pLabel ${bitrateMbps}Mbps ${fps}fps $qLabel"
                )
                return
            }
            "HLVIDEO" -> {
                val width = parts.getOrNull(1)?.toIntOrNull()?.coerceIn(320, 3840) ?: return
                val height = parts.getOrNull(2)?.toIntOrNull()?.coerceIn(180, 2160) ?: return
                val bitrateMbps = parts.getOrNull(3)?.toIntOrNull()?.coerceIn(10, 800) ?: return
                val fps = parts.getOrNull(4)?.toIntOrNull()
                    ?.let { CenterRoiVideoConfig.normalizeFps(it) }
                    ?: return
                val quality = parts.getOrNull(5)?.toIntOrNull()
                    ?.let { CenterRoiVideoConfig.normalizeQualityMode(it) }
                    ?: CenterRoiVideoConfig.QUALITY_MODE
                val ok = captureController?.updateDirectVideoRuntimeConfig(
                    codecWidth = width,
                    codecHeight = height,
                    bitrateMbps = bitrateMbps,
                    fps = fps,
                    qualityMode = quality,
                ) ?: false
                val qLabel = when (quality) {
                    0 -> "低延迟全I帧"
                    2 -> "高压缩GOP"
                    else -> "高画质全I帧"
                }
                settingsStore.setScreenMirrorCoreStatus(
                    if (ok) "核心状态：PC已应用视频参数 ${width}x${height} ${bitrateMbps}Mbps ${fps}fps $qLabel"
                    else "核心状态：PC视频参数已收到，等待视频发送器启动 ${width}x${height} ${bitrateMbps}Mbps ${fps}fps $qLabel"
                )
                return
            }
            "HLAPPLY" -> {
                if (parts.size >= 9) {
                    val mode = parts[1].toIntOrNull() ?: current.screenMirrorCaptureMode
                    val preset = parts[2].toIntOrNull() ?: current.screenMirrorFullscreenPreset
                    val crop = parts[3].toIntOrNull() ?: current.screenMirrorCropSize
                    val quality = parts[4].toIntOrNull() ?: current.screenMirrorJpegQuality
                    val keep = parts[5] == "1" || parts[5].equals("true", ignoreCase = true)
                    val fps = parts[6].toIntOrNull() ?: current.screenMirrorFps
                    val fullParts = parts[7].toIntOrNull() ?: current.screenMirrorFullscreenSplitParts
                    val cropParts = parts[8].toIntOrNull() ?: current.screenMirrorCropSplitParts
                    val subsampling = parts.getOrNull(9)?.toIntOrNull() ?: current.screenMirrorJpegSubsamplingMode
                    applyPcRuntimeSettings(
                        current.copy(
                            screenMirrorCaptureMode = mode,
                            screenMirrorFullscreenPreset = preset,
                            screenMirrorCropSize = crop,
                            screenMirrorJpegQuality = quality,
                            screenMirrorKeepFrameRate = keep,
                            screenMirrorFps = fps,
                            screenMirrorFullscreenSplitParts = fullParts,
                            screenMirrorCropSplitParts = cropParts,
                            screenMirrorJpegSubsamplingMode = subsampling,
                        ),
                        "PC已应用整套画面参数"
                    )
                }
                return
            }
            "HLWGTCPU" -> {
                if (parts.size < 2) return
                val count = parts[1].toIntOrNull()?.coerceIn(2, 16) ?: return
                if (parts.size < 2 + count * 2) return
                val cpuIds = IntArray(count)
                val weights = FloatArray(count)
                for (i in 0 until count) {
                    cpuIds[i] = parts[2 + i * 2].toIntOrNull() ?: return
                    val raw = parts[3 + i * 2].toFloatOrNull() ?: 1f
                    weights[i] = if (raw.isFinite() && raw > 0f) raw.coerceIn(0.70f, 1.30f) else 1f
                }
                val ok = NativeCenterCropJpegEncoder.setManualCpuSplitWeights(cpuIds, weights)
                if (ok) {
                    val text = cpuIds.indices.joinToString(" ") { i -> "CPU${cpuIds[i]}=${"%.2f".format(
                        Locale.US, weights[i])}" }
                    settingsStore.setScreenMirrorCoreStatus("核心状态：PC按CPU权重已应用 $text")
                } else {
                    settingsStore.setScreenMirrorCoreStatus("核心状态：PC按CPU权重应用失败")
                }
                return
            }
            "HLWGT" -> {
                if (parts.size >= 2 && parts[1].equals("RESET", ignoreCase = true)) {
                    NativeCenterCropJpegEncoder.clearManualSplitWeights()
                    settingsStore.setScreenMirrorCoreStatus("核心状态：PC已恢复默认分块权重")
                    return
                }
                if (parts.size < 4) return
                val count = parts[1].toIntOrNull()?.coerceIn(2, 16) ?: return
                if (parts.size < 2 + count) return
                val weights = FloatArray(count) { i ->
                    val raw = parts[2 + i].toFloatOrNull() ?: 1f
                    if (raw.isFinite() && raw > 0f) raw.coerceIn(0.70f, 1.30f) else 1f
                }
                val ok = NativeCenterCropJpegEncoder.setManualSplitWeights(weights)
                if (ok) {
                    val text = weights.joinToString(",") { "%.2f".format(Locale.US, it) }
                    settingsStore.setScreenMirrorCoreStatus("核心状态：PC手动分块权重已应用 ${count}块 [$text]")
                } else {
                    settingsStore.setScreenMirrorCoreStatus("核心状态：PC手动分块权重应用失败")
                }
                return
            }
        }
    }

    fun stop(reason: String) {
        captureController?.release()
        captureController = null
        frameSender?.close()
        frameSender = null
        activeResultCode = null
        activeResultData = null
        activeSettings = null
        settingsStore.setScreenMirrorEnabled(false)
        if (reason != "release") {
            settingsStore.setScreenMirrorSummary("画面传输：已停止")
        }
        settingsStore.setScreenMirrorCoreStatus(AppSettingsStore.Companion.DEFAULT_SCREEN_MIRROR_CORE_STATUS)
        onReleaseForeground()
    }

    fun release(reason: String) {
        captureController?.release()
        captureController = null
        frameSender?.close()
        frameSender = null
        activeResultCode = null
        activeResultData = null
        activeSettings = null
        settingsStore.setScreenMirrorEnabled(false)
        if (reason != "service_onDestroy") {
            settingsStore.setScreenMirrorSummary("画面传输：未启动")
        }
        onReleaseForeground()
    }

    private fun summaryFor(settings: AppSettingsSnapshot, prefix: String): String {
        return if (settings.screenMirrorCaptureMode == AppSettingsStore.Companion.SCREEN_MIRROR_CAPTURE_MODE_FULLSCREEN_SCALED) {
            val qMode = if (settings.screenMirrorKeepFrameRate) "保帧" else "固定"
            "$prefix (全屏 ${AppSettingsStore.Companion.fullscreenPresetLabel(settings.screenMirrorFullscreenPreset)}   Q${settings.screenMirrorJpegQuality} $qMode ${AppSettingsStore.Companion.jpegSubsamplingLabel(settings.screenMirrorJpegSubsamplingMode)} 全屏${settings.screenMirrorFullscreenSplitParts}块 ${settings.screenMirrorFps}fps)"
        } else {
            val qMode = if (settings.screenMirrorKeepFrameRate) "保帧" else "固定"
            "$prefix (中心裁剪 ${settings.screenMirrorCropSize}x${settings.screenMirrorCropSize}   Q${settings.screenMirrorJpegQuality} $qMode ${AppSettingsStore.Companion.jpegSubsamplingLabel(settings.screenMirrorJpegSubsamplingMode)} 裁剪${settings.screenMirrorCropSplitParts}块 ${settings.screenMirrorFps}fps)"
        }
    }
}
