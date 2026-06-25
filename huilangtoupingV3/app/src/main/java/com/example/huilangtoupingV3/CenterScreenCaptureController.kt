package com.example.huilangtoupingV3

import android.content.Context
import android.content.Intent
import android.graphics.ImageFormat
import android.graphics.PixelFormat
import android.hardware.display.DisplayManager
import android.view.Display
import android.hardware.display.VirtualDisplay
import android.media.Image
import android.media.ImageReader
import android.media.projection.MediaProjection
import android.media.projection.MediaProjectionManager
import android.os.Build
import android.os.Handler
import android.os.HandlerThread
import android.os.Process
import android.os.SystemClock
import android.util.DisplayMetrics
import android.util.Log
import android.view.WindowManager
import java.util.ArrayDeque
import java.util.concurrent.CountDownLatch
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.TimeUnit
import kotlin.math.max
import kotlin.math.min

class CenterScreenCaptureController(
    context: Context,
    private val resultCode: Int,
    private val resultData: Intent,
    private var captureMode: Int,
    private var cropSize: Int,
    private var fullscreenPreset: Int,
    private var jpegQuality: Int,
    private var jpegSubsamplingMode: Int = AppSettingsStore.SCREEN_MIRROR_JPEG_SUBSAMPLING_420,
    private var keepFrameRate: Boolean = true,
    fps: Int,
    private var fullscreenSplitParts: Int = 2,
    private var cropSplitParts: Int = 2,
    private val onJpegFrame: ((jpeg: ByteArray, width: Int, height: Int, timing: FrameTimingMeta) -> Unit)? = null,
    private val isFrameSinkReady: (() -> Boolean)? = null,
    private val onPerfText: ((String) -> Unit)? = null,
    private val onCoreStatusText: ((String) -> Unit)? = null,
    private val onAutoFallbackTo720p: (() -> Unit)? = null,
) {
    private val appContext = context.applicationContext
    private val projectionManager =
        appContext.getSystemService(Context.MEDIA_PROJECTION_SERVICE) as MediaProjectionManager
    private val windowManager =
        appContext.getSystemService(Context.WINDOW_SERVICE) as WindowManager

    @Volatile
    private var targetFrameIntervalNs = frameIntervalNsForFps(fps)

    private var handlerThread: HandlerThread? = null
    private var handler: Handler? = null
    private var mediaProjection: MediaProjection? = null
    private var imageReader: ImageReader? = null
    private var virtualDisplay: VirtualDisplay? = null
    private var audioSender: AudioPlaybackCaptureSender? = null
    private var centerRoiVideoSender: CenterRoiVideoStreamSender? = null
    private var localAudioSilencer: AndroidLocalAudioSilencer? = null
    private var nextFrameDueNs = 0L
    // FPS 节流改成 token bucket：允许回调有抖动/补帧，避免 120fps 源头平均只有 117~119fps 时，
    // 因为某两个回调间隔略小于 8.33ms 就误杀真实帧。
    private var pacingLastCallbackNs = 0L
    private var pacingCreditNs = 0L
    private var projectionCallbackRegistered = false
    @Volatile
    private var isReleasing = false
    private var captureSpec: CaptureSpec? = null
    private var lastScreenSize: ScreenSize? = null
    private var isReconfiguringCapture = false
    private var autoFallbackTriggered = false
    private var consecutiveHighSkipWindows = 0
    private var activeReaderFormat: Int = PixelFormat.RGBA_8888

    // 单 VirtualDisplay 混合模式：只保留主 ImageReader VirtualDisplay。
    // 中心 ROI H.264 从同一个 RGBA Image 拷贝后送 MediaCodec，外圈由 native CPU/JPEG 分块发送。
    private val enableCenterRoiVideoOnlyProbe = false
    private val enableCenterRoiVideoExperiment = true

    // PC 控制台运行时 ROI 策略：视频 ROI 可选中心/底部，其他区域走 JPEG。
    // true  = 视频 ROI + JPEG；false = 整帧都走 JPEG，便于排查“玻璃感”/颜色差异。
    @Volatile
    private var centerRoiVideoEnabled = CenterRoiVideoConfig.USE_VIDEO_STREAM
    @Volatile
    private var splitEdgeQualityReduction = CenterRoiVideoConfig.OUTER_JPEG_QUALITY_REDUCTION
    @Volatile
    private var splitEdgeEncodeScalePercent = CenterRoiVideoConfig.OUTER_JPEG_SCALE_PERCENT
    @Volatile
    private var splitTopLowPercent = 20
    @Volatile
    private var splitCenterWidthPercent = 60
    @Volatile
    private var splitCenterCpuWeightPercent = 4
    @Volatile
    private var splitBigCoreWeightPercent = 120
    @Volatile
    private var jpegCenterOnly = false

    @Volatile
    private var centerRoiWidthPercent = CenterRoiVideoConfig.ROI_DEFAULT_WIDTH_SOURCE_PERCENT

    @Volatile
    private var centerRoiHeightPercent = CenterRoiVideoConfig.ROI_DEFAULT_HEIGHT_SOURCE_PERCENT

    @Volatile
    private var videoRoiRegion = CenterRoiVideoConfig.ROI_DEFAULT_REGION

    // 分辨率升档时部分设备的 VirtualDisplay.resize()+setSurface(new) 会偶发不再产出新帧。
    // 记录重建后的第一帧时间，并用 watchdog 在无帧时重新 detach/attach Surface。
    private var captureReconfigureGeneration = 0
    private var lastImageCallbackAfterReconfigureMs = 0L
    private var lastSuccessfulEncodeAfterReconfigureMs = 0L
    private var lastReconfigureAppliedAtMs = 0L
    private var reconfigureWatchdogAttempts = 0

    // 最近一次成功启动/切换的采集参数。切换 2K/3K/极限模式失败时用于恢复旧画面，避免黑屏/停帧。
    private var lastStableScreen: ScreenSize? = null
    private var lastStableSpec: CaptureSpec? = null

    // nativeEncodeAndSend 直接写 LocalSocket fd。
    // 高刷低等待策略：
    // 1) 最多准备 2 个编码线程，但只有输出分辨率达到 1080P 级别时才启用第 2 个。
    // 2) 双线程不是让系统随便调度，而是在 native 里按 CPU 最高频率选两个性能核，
    //    CenterScreenEncode0/1 启动时分别绑定到不同 CPU，避免两个编码线程抢同一个核心。
    // 3) 编码线程忙时保留极短 pending 队列：允许 1~2 帧预算内的抖动，不再一忙就覆盖丢帧。
    // 不在 Kt 侧 synchronized 包住整个 encodeAndSend；fd 的 header + jpeg 完整写入顺序，
    // 继续由 native C++ 内部只对 write(fd) 那一小段加锁。
    private val maxEncodeWorkerCount = 2

    // MediaProjection 屏幕来源在多数设备上不能稳定产出 YUV_420_888，
    // 默认固定 RGBA，避免启动时先走 YUV 再 fallback 导致重建和抖动。
    @Volatile
    private var adaptiveJpegQuality = if (keepFrameRate) {
        jpegQuality.coerceIn(60, 100)
    } else {
        jpegQuality.coerceIn(30, 100)
    }
    private var adaptiveWindowFrames = 0
    private var adaptiveWindowTotalEncodeNs = 0L
    private var adaptiveStableWindows = 0

    private val encodeLock = Any()
    private val encodeThreads = arrayOfNulls<HandlerThread>(maxEncodeWorkerCount)
    private val encodeHandlers = arrayOfNulls<Handler>(maxEncodeWorkerCount)
    private val encodeWorkers = Array(maxEncodeWorkerCount) { EncodeWorkerState() }
    // 预创建 drain Runnable，避免高帧率下每次直发都分配匿名 Runnable。
    private val encodeDrainRunnables: Array<Runnable> by lazy {
        Array(maxEncodeWorkerCount) { index -> encodeDrainRunnable(index) }
    }
    private val emptyJpegBytes = ByteArray(0)
    private val lastSendStatsArray = LongArray(7)
    private val lastSplitPartStatsArray = LongArray(1 + MAX_NATIVE_SPLIT_PARTS * 3)
    private var warmupDropFrames = 0
    private var nextEncodeDispatchIndex = 0
    private var statDroppedByOverwriteCount = 0
    private var nextFrameSeq = 0L
    // 非 inline 路径保留的 pending 队列。
    // inline 低延迟路径在 native 分块模式下不再走这里，直接在采集回调线程里编码。
    private val pendingFrameQueue = ArrayDeque<PendingFrame>()

    private val deliveryLock = Any()
    private var lastDeliveredSeq = 0L
    private val perfLock = Any()

    private val coreStatusLock = Any()
    private var callbackTargetCpu = -1
    private var callbackBindOk = false
    private var callbackCurrentCpu = -1
    private var callbackLastCpuSampleAtMs = 0L
    private val encodeTargetCpuIds = IntArray(maxEncodeWorkerCount) { -1 }
    private val encodeBindOk = BooleanArray(maxEncodeWorkerCount)
    private val encodeCurrentCpuIds = IntArray(maxEncodeWorkerCount) { -1 }
    @Volatile
    private var lastCoreStatusText = ""

    private val projectionCallback = object : MediaProjection.Callback() {
        override fun onStop() {
            Log.w("CenterScreenCapture", "MediaProjection stopped by system")
            release()
        }
    }

    private var lastPerfReportAtMs = 0L
    private var lastHybridVideoQueueFailLogMs = 0L
    private var perfFrameCount = 0
    private var perfTotalCaptureNs = 0L
    private var perfTotalEncodeNs = 0L
    private var perfTotalEmitNs = 0L
    private var perfTotalFrameNs = 0L
    private var perfTotalJpegBytes = 0L
    private var perfTotalPart0Bytes = 0L
    private var perfTotalPart1Bytes = 0L
    private var perfTotalSocketWriteNs = 0L
    private var perfTotalPart0EncodeNs = 0L
    private var perfTotalPart1EncodeNs = 0L
    private var perfTotalQueueWaitNs = 0L
    private var perfTotalCenterVideoQueueNs = 0L
    private var perfMaxCenterVideoQueueNs = 0L
    private var perfCenterVideoQueueAttempts = 0
    private var perfCenterVideoQueueOk = 0
    private var perfMaxQueueWaitNs = 0L
    private var perfMaxNativeNs = 0L
    private var perfMaxTotalNs = 0L
    private var perfMaxSocketWriteNs = 0L
    private var perfTotalLaneMaxEncodeNs = 0L
    private var perfTotalLaneMinEncodeNs = 0L
    private val perfTotalNativePartBytes = LongArray(MAX_NATIVE_SPLIT_PARTS)
    private val perfTotalNativePartEncodeNs = LongArray(MAX_NATIVE_SPLIT_PARTS)
    private val perfLastNativePartCpuIds = IntArray(MAX_NATIVE_SPLIT_PARTS) { -1 }
    private var perfLastNativePartCount = 0
    private var perfLastSplitParts = 1
    private var perfLastModeText = ""
    private var perfLastFormatText = ""
    private var perfLastRowStride = 0
    private var perfLastPixelStride = 0
    private var perfLastBufferKb = 0
    private var perfLastReaderMaxImages = 0

    private var statWindowStartMs = 0L
    private var statCallbackCount = 0
    private var statAcquireNullCount = 0
    private var statProcessedCount = 0
    private var statSkippedByIntervalCount = 0
    private var statLastCallbackAtMs = 0L
    private var statMaxCallbackGapMs = 0L
    private var statDirectDispatchCount = 0
    private var statQueuedLatestCount = 0

    // 细分丢帧原因：之前 ScreenSourcePerf 只有一个 skip，无法判断到底是 pacing、sink、去重还是编码失败。
    // 这些计数有些来自编码线程，所以用 AtomicInteger，避免多线程统计互相覆盖。
    private val statSkippedByReleaseCount = AtomicInteger(0)
    private val statSkippedByReconfigureCount = AtomicInteger(0)
    private val statSkippedBySinkNotReadyCount = AtomicInteger(0)
    private val statSkippedByDeliverySeqCount = AtomicInteger(0)
    private val statSkippedByEncodeFailCount = AtomicInteger(0)

    private data class EncodeWorkerState(
        var pendingImage: Image? = null,
        var pendingCallbackNs: Long = 0L,
        var pendingFrameNs: Long = 0L,
        var pendingSeq: Long = 0L,
        var running: Boolean = false,
        var drainPosted: Boolean = false,
    )

    private data class PendingFrame(
        val image: Image,
        val callbackNs: Long,
        val frameNs: Long,
        val seq: Long,
    )

    private fun normalizedJpegSubsamplingMode(): Int {
        return AppSettingsStore.normalizeJpegSubsamplingMode(jpegSubsamplingMode)
    }

    private fun isExtremeJpegMode(): Boolean {
        return normalizedJpegSubsamplingMode() == AppSettingsStore.SCREEN_MIRROR_JPEG_SUBSAMPLING_444
    }

    private fun shouldEncodeInlineOnCaptureThread(spec: CaptureSpec?): Boolean {
        // native 分块模式直接在 ImageReader 回调线程里调用 native 编码。
        // 目标是把 Handler.post 到 CenterScreenEncode0 的调度等待压到接近 0。
        // reader=3 配合 inline，用更少缓存换更低延迟。
        return splitPartsForSpec(spec) > 1
    }

    private fun warmupNativeForSpec(spec: CaptureSpec?) {
        spec ?: return
        val parts = splitPartsForSpec(spec).coerceAtLeast(1)
        runCatching { NativeCenterCropJpegEncoder.setJpegSubsamplingMode(normalizedJpegSubsamplingMode()) }
        runCatching { NativeCenterCropJpegEncoder.setSplitRoiQualityParams(splitEdgeQualityReduction, splitEdgeEncodeScalePercent) }
        runCatching { NativeCenterCropJpegEncoder.setSplitLayoutParams(splitTopLowPercent, splitCenterWidthPercent, splitCenterCpuWeightPercent, jpegCenterOnly, splitBigCoreWeightPercent) }
        if (parts > 1) {
            runCatching {
                NativeCenterCropJpegEncoder.warmupSplitEncoder(
                    width = spec.outputWidth,
                    height = spec.outputHeight,
                    splitParts = parts,
                    jpegSubsamplingMode = normalizedJpegSubsamplingMode(),
                )
            }
        }
    }

    private fun resetWarmupDropFrames() {
        warmupDropFrames = if (splitPartsForSpec(captureSpec) > 1) 3 else 1
    }

    private var directVideoWidth = CenterRoiVideoConfig.WIDTH
    private var directVideoHeight = CenterRoiVideoConfig.HEIGHT
    private var directVideoBitrate = CenterRoiVideoConfig.BITRATE
    private var directVideoFps = CenterRoiVideoConfig.FPS
    // 0=低延迟全I帧，1=高画质全I帧，2=高压缩GOP
    @Volatile
    private var directVideoQualityMode = CenterRoiVideoConfig.QUALITY_MODE


    fun updateDirectVideoPresetRuntimeConfig(
        preset: Int,
        bitrateMbps: Int,
        fps: Int,
        qualityMode: Int,
    ): Boolean {
        // H.264 center ROI no longer has an independent resolution preset.
        // Its size is derived from the active JPEG capture resolution by centerRoiRectForSpec().
        return applyCenterVideoRuntimeParams(bitrateMbps, fps, qualityMode)
    }

    fun updateDirectVideoRuntimeConfig(
        codecWidth: Int,
        codecHeight: Int,
        bitrateMbps: Int,
        fps: Int,
        qualityMode: Int,
    ): Boolean {
        // Width/height are accepted only for backward compatibility with older PC builds.
        // Runtime resolution changes are intentionally ignored; JPEG resolution controls the
        // active capture size, and centerRoiRectForSpec() clamps the video ROI to that size.
        return applyCenterVideoRuntimeParams(bitrateMbps, fps, qualityMode)
    }

    private fun applyCenterVideoRuntimeParams(
        bitrateMbps: Int,
        fps: Int,
        qualityMode: Int,
    ): Boolean {
        directVideoBitrate = CenterRoiVideoConfig.normalizeBitrateMbps(bitrateMbps) * 1_000_000
        directVideoFps = CenterRoiVideoConfig.normalizeFps(fps)
        directVideoQualityMode = CenterRoiVideoConfig.normalizeQualityMode(qualityMode)

        val spec = captureSpec ?: lastStableSpec
        val roi = spec?.let { centerRoiRectForSpec(it) }
        if (roi != null) {
            directVideoWidth = roi.width
            directVideoHeight = roi.height
        }

        val sender = centerRoiVideoSender
        return if (sender != null && roi != null) {
            sender.updateRuntimeConfig(
                codecWidth = roi.width,
                codecHeight = roi.height,
                bitrate = directVideoBitrate,
                frameRate = directVideoFps,
                quality = directVideoQualityMode,
                roiLeft = roi.left,
                roiTop = roi.top,
            )
            true
        } else {
            false
        }
    }

    fun updateCenterRoiRuntimeConfig(
        useVideoStream: Boolean,
        edgeQualityReduction: Int,
        edgeEncodeScalePercent: Int,
        roiWidthPercent: Int = centerRoiWidthPercent,
        roiHeightPercent: Int = centerRoiHeightPercent,
        roiRegion: Int = videoRoiRegion,
        topLowPercent: Int = splitTopLowPercent,
        centerWidthPercent: Int = splitCenterWidthPercent,
        centerCoreCount: Int = splitCenterCpuWeightPercent,
        centerOnly: Boolean = jpegCenterOnly,
        bigCoreWeightPercent: Int = splitBigCoreWeightPercent,
    ): Boolean {
        val newWidthPercent = roiWidthPercent.coerceIn(
            CenterRoiVideoConfig.ROI_MIN_SOURCE_PERCENT,
            CenterRoiVideoConfig.ROI_MAX_SOURCE_PERCENT,
        )
        val newHeightPercent = roiHeightPercent.coerceIn(
            CenterRoiVideoConfig.ROI_MIN_SOURCE_PERCENT,
            CenterRoiVideoConfig.ROI_MAX_SOURCE_PERCENT,
        )
        val newRegion = when (roiRegion) {
            CenterRoiVideoConfig.ROI_REGION_BOTTOM -> CenterRoiVideoConfig.ROI_REGION_BOTTOM
            else -> CenterRoiVideoConfig.ROI_REGION_CENTER
        }
        val geometryChanged = centerRoiVideoEnabled != useVideoStream ||
                centerRoiWidthPercent != newWidthPercent ||
                centerRoiHeightPercent != newHeightPercent ||
                videoRoiRegion != newRegion

        centerRoiVideoEnabled = useVideoStream
        centerRoiWidthPercent = newWidthPercent
        centerRoiHeightPercent = newHeightPercent
        videoRoiRegion = newRegion
        splitEdgeQualityReduction = edgeQualityReduction.coerceIn(0, 40)
        splitEdgeEncodeScalePercent = edgeEncodeScalePercent.coerceIn(50, 100)
        splitTopLowPercent = topLowPercent.coerceIn(10, 80)
        splitCenterWidthPercent = centerWidthPercent.coerceIn(10, 80)
        splitCenterCpuWeightPercent = centerCoreCount.coerceIn(1, 24)
        splitBigCoreWeightPercent = bigCoreWeightPercent.coerceIn(50, 200)
        jpegCenterOnly = centerOnly
        NativeCenterCropJpegEncoder.setSplitRoiQualityParams(
            edgeQualityReduction = splitEdgeQualityReduction,
            edgeEncodeScalePercent = splitEdgeEncodeScalePercent,
        )
        NativeCenterCropJpegEncoder.setSplitLayoutParams(
            topLowPercent = splitTopLowPercent,
            centerWidthPercent = splitCenterWidthPercent,
            centerCoreCount = splitCenterCpuWeightPercent,
            centerOnly = jpegCenterOnly,
            bigCoreWeightPercent = splitBigCoreWeightPercent,
        )
        val screen = lastScreenSize
        val spec = captureSpec ?: lastStableSpec
        if (useVideoStream) {
            if (centerRoiVideoSender == null || geometryChanged) {
                restartCenterRoiVideoExperiment(mediaProjection, screen, spec)
            }
        } else {
            centerRoiVideoSender?.close()
            centerRoiVideoSender = null
        }
        val regionText = if (videoRoiRegion == CenterRoiVideoConfig.ROI_REGION_BOTTOM) "bottom" else "center"
        Log.w(
            "CenterScreenCapture",
            "video ROI runtime mode=${if (useVideoStream) "H264" else "JPEG"} region=$regionText " +
                    "roiPercent=${centerRoiWidthPercent}x${centerRoiHeightPercent} " +
                    "edgeQDrop=$splitEdgeQualityReduction edgeScale=$splitEdgeEncodeScalePercent " +
                    "jpegCenter=${splitCenterWidthPercent}x${splitTopLowPercent} centerCores=$splitCenterCpuWeightPercent bigWeight=$splitBigCoreWeightPercent centerOnly=$jpegCenterOnly"
        )
        return true
    }

    private data class CenterRoiRect(val left: Int, val top: Int, val width: Int, val height: Int)


    private fun centerRoiRectForSpec(spec: CaptureSpec): CenterRoiRect {
        val sourceWidth = evenSize(spec.captureWidth.coerceAtLeast(2))
        val sourceHeight = evenSize(spec.captureHeight.coerceAtLeast(2))

        if (videoRoiRegion == CenterRoiVideoConfig.ROI_REGION_BOTTOM) {
            val requestedHeight = evenSize((sourceHeight * centerRoiHeightPercent) / 100)
            val minHeight = min(CenterRoiVideoConfig.MIN_CODEC_HEIGHT, sourceHeight).coerceAtLeast(2)
            val targetHeight = evenSize(
                requestedHeight
                    .coerceAtLeast(minHeight)
                    .coerceAtMost(sourceHeight)
            )
            val top = evenSize((sourceHeight - targetHeight).coerceAtLeast(0))
            return CenterRoiRect(
                left = 0,
                top = top,
                width = sourceWidth,
                height = targetHeight.coerceAtMost(sourceHeight - top),
            )
        }

        val maxWidth = evenSize((sourceWidth * CenterRoiVideoConfig.ROI_MAX_SOURCE_PERCENT) / 100).coerceAtLeast(2)
        val maxHeight = evenSize((sourceHeight * CenterRoiVideoConfig.ROI_MAX_SOURCE_PERCENT) / 100).coerceAtLeast(2)
        val minWidth = min(CenterRoiVideoConfig.MIN_CODEC_WIDTH, maxWidth).coerceAtLeast(2)
        val minHeight = min(CenterRoiVideoConfig.MIN_CODEC_HEIGHT, maxHeight).coerceAtLeast(2)

        val requestedWidth = evenSize((sourceWidth * centerRoiWidthPercent) / 100)
        val requestedHeight = evenSize((sourceHeight * centerRoiHeightPercent) / 100)

        val targetWidth = evenSize(
            requestedWidth
                .coerceIn(minWidth, maxWidth)
                .coerceAtMost(sourceWidth)
        )

        val targetHeight = evenSize(
            requestedHeight
                .coerceIn(minHeight, maxHeight)
                .coerceAtMost(sourceHeight)
        )

        val left = evenSize(((sourceWidth - targetWidth) / 2).coerceAtLeast(0))
        val top = evenSize(((sourceHeight - targetHeight) / 2).coerceAtLeast(0))

        return CenterRoiRect(
            left = left,
            top = top,
            width = targetWidth.coerceAtMost(sourceWidth - left),
            height = targetHeight.coerceAtMost(sourceHeight - top),
        )
    }

    private fun isBottomVideoRoiMode(): Boolean =
        videoRoiRegion == CenterRoiVideoConfig.ROI_REGION_BOTTOM

    private fun restartCenterRoiVideoExperiment(projection: MediaProjection?, screen: ScreenSize?, spec: CaptureSpec?) {
        if (!enableCenterRoiVideoExperiment || !centerRoiVideoEnabled) {
            centerRoiVideoSender?.close()
            centerRoiVideoSender = null
            return
        }
        val p = projection ?: return
        val s = screen ?: return
        val c = spec ?: return

        if (c.mode != AppSettingsStore.SCREEN_MIRROR_CAPTURE_MODE_FULLSCREEN_SCALED) {
            centerRoiVideoSender?.close()
            centerRoiVideoSender = null
            return
        }

        val sourceWidth = evenSize(c.captureWidth.coerceAtLeast(2))
        val sourceHeight = evenSize(c.captureHeight.coerceAtLeast(2))
        val roi = centerRoiRectForSpec(c)
        directVideoWidth = roi.width
        directVideoHeight = roi.height

        centerRoiVideoSender?.close()
        centerRoiVideoSender = CenterRoiVideoStreamSender(
            mediaProjection = p,
            densityDpi = s.densityDpi,
            sourceWidth = sourceWidth,
            sourceHeight = sourceHeight,
            codecWidth = roi.width,
            codecHeight = roi.height,
            initialRoiLeft = roi.left,
            initialRoiTop = roi.top,
            bitrate = directVideoBitrate,
            frameRate = directVideoFps,
            initialQualityMode = directVideoQualityMode,
        ).also { it.start() }
        Log.w(
            "CenterScreenCapture",
            "single-VD hybrid started: source=${sourceWidth}x$sourceHeight " +
                    "videoRoi=${if (isBottomVideoRoiMode()) "bottom" else "center"} ${roi.left},${roi.top} ${roi.width}x${roi.height}; outside=CPU/JPEG"
        )
    }

    private fun pendingFrameQueueMaxForSpec(spec: CaptureSpec?): Int {
        // 分块模式才需要短队列吸收回调抖动。
        // 极限模式统一 3 帧；高质量模式统一 2 帧。
        // 这里不再看 JPEG 质量数值，避免把策略做得过复杂。
        if (splitPartsForSpec(spec) <= 1) return 1
        return if (isExtremeJpegMode()) 3 else 2
    }

    private fun pendingFrameHardWaitNs(): Long {
        // 120fps 时一帧约 8.33ms：
        // 高质量模式最多允许 20帧预算；极限模式最多允许 20 帧预算。
        // 不再和 Q 值绑定，设备性能强弱交给用户通过模式/质量自行取舍。
        val frames = if (isExtremeJpegMode()) 10L else 10L
        return targetFrameIntervalNs.coerceAtLeast(1L) * frames
    }

    private fun clearPendingFrameQueueLocked() {
        while (!pendingFrameQueue.isEmpty()) {
            runCatching { pendingFrameQueue.removeFirst().image.close() }
        }
    }

    private fun dropOldestPendingFrameLocked() {
        if (pendingFrameQueue.isEmpty()) return
        runCatching { pendingFrameQueue.removeFirst().image.close() }
        statDroppedByOverwriteCount += 1
    }

    private fun prunePendingFrameQueueLocked(nowNs: Long, spec: CaptureSpec?) {
        val hardWaitNs = pendingFrameHardWaitNs()
        while (!pendingFrameQueue.isEmpty()) {
            val oldest = pendingFrameQueue.peekFirst()
            val waitNs = (nowNs - oldest.callbackNs).coerceAtLeast(0L)
            if (waitNs <= hardWaitNs) break
            dropOldestPendingFrameLocked()
        }

        val maxQueue = pendingFrameQueueMaxForSpec(spec).coerceAtLeast(1)
        while (pendingFrameQueue.size > maxQueue) {
            dropOldestPendingFrameLocked()
        }
    }

    fun start() {
        if (handlerThread != null) return

        val screen = readScreenSize()
        val spec = createCaptureSpec(screen)
        captureSpec = spec
        lastScreenSize = screen

        val suggestedCpus = NativeCenterCropJpegEncoder.getSuggestedEncodeCpuIds(
            (NativeCenterCropJpegEncoder.availableEncodeCpuCount() + 1).coerceAtLeast(2)
        )
        val callbackTargetCpu = chooseCallbackCpu(suggestedCpus, spec)
        // native 分块编码不能再占用回调线程核心；先用目标核心预留，绑定完成后再用实际核心校正。
        NativeCenterCropJpegEncoder.setReservedCallbackCpu(callbackTargetCpu)
        updateCallbackCoreStatus(targetCpu = callbackTargetCpu, bindOk = false, currentCpu = -1)
        warmupNativeForSpec(spec)
        resetWarmupDropFrames()

        val thread = HandlerThread("CenterScreenCapture", Process.THREAD_PRIORITY_URGENT_DISPLAY)
        thread.start()
        handlerThread = thread
        handler = Handler(thread.looper)

        handler?.post {
            runCatching { Process.setThreadPriority(Process.THREAD_PRIORITY_URGENT_DISPLAY) }
            val bindOk = if (callbackTargetCpu >= 0) {
                NativeCenterCropJpegEncoder.bindCurrentThreadToCpu(callbackTargetCpu)
            } else {
                false
            }
            val currentCpu = NativeCenterCropJpegEncoder.getCurrentCpu()
            NativeCenterCropJpegEncoder.setReservedCallbackCpu(if (bindOk && currentCpu >= 0) currentCpu else callbackTargetCpu)
            updateCallbackCoreStatus(targetCpu = callbackTargetCpu, bindOk = bindOk, currentCpu = currentCpu)
            emitCoreStatus(force = true)
            Log.w("CenterScreenCapture", "callback thread targetCpu=$callbackTargetCpu currentCpu=$currentCpu bind=$bindOk")
        }

        val suggestedEncodeCpus = suggestedCpus
        for (index in 0 until encodeThreads.size) {
            val encThread = HandlerThread("CenterScreenEncode${index}", Process.THREAD_PRIORITY_URGENT_DISPLAY)
            encThread.start()
            encodeThreads[index] = encThread
            encodeHandlers[index] = Handler(encThread.looper)
            val targetCpu = suggestedEncodeCpus.getOrNull(index) ?: -1
            updateEncodeCoreStatus(index, targetCpu = targetCpu, bindOk = false, currentCpu = -1)
            emitCoreStatus(force = true)
            encodeHandlers[index]?.post {
                runCatching { Process.setThreadPriority(Process.THREAD_PRIORITY_URGENT_DISPLAY) }
                val bindOk = if (targetCpu >= 0) {
                    NativeCenterCropJpegEncoder.bindCurrentThreadToCpu(targetCpu)
                } else {
                    false
                }
                runCatching { NativeCenterCropJpegEncoder.prepareCurrentThreadForEncoding() }
                val currentCpu = NativeCenterCropJpegEncoder.getCurrentCpu()
                updateEncodeCoreStatus(index, targetCpu = targetCpu, bindOk = bindOk, currentCpu = currentCpu)
                emitCoreStatus(force = true)
                Log.w("CenterScreenCapture", "encode thread[$index] targetCpu=$targetCpu currentCpu=$currentCpu bind=$bindOk")
            }
        }

        val callbackHandler = handler ?: error("capture handler not ready")
        val projection = projectionManager.getMediaProjection(resultCode, resultData)
            ?: error("MediaProjection is null")

        projection.registerCallback(projectionCallback, callbackHandler)
        projectionCallbackRegistered = true
        mediaProjection = projection

        // AudioPlaybackCapture 只是复制声音，不会把安卓本机扬声器切走。
        // 投屏期间自动把安卓本机媒体音量压到最低，停止投屏时恢复。
        localAudioSilencer = AndroidLocalAudioSilencer(appContext).also { it.enable() }

        // 音频和画面共用同一个 MediaProjection，单独走 localabstract:huilang_screen_audio。
        // PC 端会 adb forward tcp:27184 -> 这个本地通道，不影响原来的 27183/JPEG 视频链路。
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            audioSender = AudioPlaybackCaptureSender(mediaProjection = projection).also { it.start() }
        }

        if (!enableCenterRoiVideoOnlyProbe) {
            restartCenterRoiVideoExperiment(projection, screen, spec)
        }

        val reader = ImageReader.newInstance(
            spec.captureWidth,
            spec.captureHeight,
            activeReaderFormat,
            imageReaderMaxImagesForSpec(spec),
        )
        imageReader = reader
        reader.setOnImageAvailableListener({ source -> handleImageAvailable(source) }, callbackHandler)

        virtualDisplay = projection.createVirtualDisplay(
            "huilangtouping-screen-capture",
            spec.captureWidth,
            spec.captureHeight,
            screen.densityDpi,
            DisplayManager.VIRTUAL_DISPLAY_FLAG_AUTO_MIRROR,
            reader.surface,
            null,
            callbackHandler,
        ) ?: error("createVirtualDisplay returned null")
        lastStableScreen = screen
        lastStableSpec = spec
    }

    fun updateTargetFps(fps: Int) {
        val normalized = AppSettingsStore.normalizeFpsPreset(fps)
        targetFrameIntervalNs = frameIntervalNsForFps(normalized)
        handler?.post { resetPacingState() }
    }

    fun updateRuntimeConfig(
        newCaptureMode: Int? = null,
        newCropSize: Int? = null,
        newFullscreenPreset: Int? = null,
        newJpegQuality: Int? = null,
        newJpegSubsamplingMode: Int? = null,
        newKeepFrameRate: Boolean? = null,
        newFps: Int? = null,
        newFullscreenSplitParts: Int? = null,
        newCropSplitParts: Int? = null,
    ): Boolean {
        val oldMode = captureMode
        val oldCropSize = cropSize
        val oldPreset = fullscreenPreset
        val oldFullParts = fullscreenSplitParts
        val oldCropParts = cropSplitParts
        val oldJpegQuality = jpegQuality
        val oldKeepFrameRate = keepFrameRate
        val oldAdaptiveJpegQuality = adaptiveJpegQuality
        val oldJpegSubsamplingMode = normalizedJpegSubsamplingMode()
        val oldTargetFrameIntervalNs = targetFrameIntervalNs

        // 先只计算目标值，不立刻写入会影响 CaptureSpec 的字段。
        // 否则 ImageReader 回调线程可能在 applyCaptureSpec() 之前看到“新 preset + 旧 captureSpec”，
        // 从 maybeReconfigureForRotation() 再触发一次重建，形成双重 resize/setSurface，偶发停帧。
        val targetMode = (newCaptureMode ?: oldMode)
            .coerceIn(AppSettingsStore.MIN_SCREEN_MIRROR_CAPTURE_MODE, AppSettingsStore.MAX_SCREEN_MIRROR_CAPTURE_MODE)
        val targetCropSize = (newCropSize ?: oldCropSize)
            .coerceIn(AppSettingsStore.MIN_SCREEN_MIRROR_CROP_SIZE, AppSettingsStore.MAX_SCREEN_MIRROR_CROP_SIZE)
        val targetPreset = (newFullscreenPreset ?: oldPreset)
            .coerceIn(AppSettingsStore.MIN_SCREEN_MIRROR_FULLSCREEN_PRESET, AppSettingsStore.MAX_SCREEN_MIRROR_FULLSCREEN_PRESET)
        val targetJpegQuality = (newJpegQuality ?: oldJpegQuality)
            .coerceIn(AppSettingsStore.MIN_SCREEN_MIRROR_JPEG_QUALITY, AppSettingsStore.MAX_SCREEN_MIRROR_JPEG_QUALITY)
        val targetKeepFrameRate = newKeepFrameRate ?: oldKeepFrameRate
        val targetJpegSubsamplingMode = newJpegSubsamplingMode
            ?.let { AppSettingsStore.normalizeJpegSubsamplingMode(it) }
            ?: oldJpegSubsamplingMode
        val targetFullParts = newFullscreenSplitParts
            ?.let { AppSettingsStore.normalizeFullscreenSplitParts(it) }
            ?: oldFullParts
        val targetCropParts = newCropSplitParts
            ?.let { AppSettingsStore.normalizeCropSplitParts(it) }
            ?: oldCropParts
        val targetFps = newFps?.let { AppSettingsStore.normalizeFpsPreset(it) }

        fun applyRuntimeFields() {
            captureMode = targetMode
            cropSize = targetCropSize
            fullscreenPreset = targetPreset
            jpegQuality = targetJpegQuality
            jpegSubsamplingMode = targetJpegSubsamplingMode
            keepFrameRate = targetKeepFrameRate
            fullscreenSplitParts = targetFullParts
            cropSplitParts = targetCropParts
            adaptiveJpegQuality = if (keepFrameRate) jpegQuality.coerceIn(60, 100) else jpegQuality.coerceIn(30, 100)
            adaptiveWindowFrames = 0
            adaptiveWindowTotalEncodeNs = 0L
            adaptiveStableWindows = 0
            if (targetFps != null) {
                targetFrameIntervalNs = frameIntervalNsForFps(targetFps)
                resetPacingState()
            }
        }

        fun restoreRuntimeFields() {
            captureMode = oldMode
            cropSize = oldCropSize
            fullscreenPreset = oldPreset
            fullscreenSplitParts = oldFullParts
            cropSplitParts = oldCropParts
            jpegQuality = oldJpegQuality
            keepFrameRate = oldKeepFrameRate
            adaptiveJpegQuality = oldAdaptiveJpegQuality
            adaptiveWindowFrames = 0
            adaptiveWindowTotalEncodeNs = 0L
            adaptiveStableWindows = 0
            jpegSubsamplingMode = oldJpegSubsamplingMode
            targetFrameIntervalNs = oldTargetFrameIntervalNs
            resetPacingState()
        }

        if (enableCenterRoiVideoOnlyProbe) {
            val captureHandler = handler ?: return false
            val latch = CountDownLatch(1)
            var switched = false
            captureHandler.post {
                try {
                    applyRuntimeFields()
                    val screen = readScreenSize()
                    val newSpec = createCaptureSpec(screen)
                    captureSpec = newSpec
                    lastScreenSize = screen
                    lastStableScreen = screen
                    lastStableSpec = newSpec
                    resetPacingState()
                    resetSourceStats()
                    resetPerfWindowForReconfigure()
                    restartCenterRoiVideoExperiment(mediaProjection, screen, newSpec)
                    emitCoreStatus(force = true)
                    Log.w(
                        "CenterScreenCapture",
                        "direct full-screen video-only runtime updated ${newSpec.captureWidth}x${newSpec.captureHeight}"
                    )
                    switched = true
                } catch (t: Throwable) {
                    Log.e("CenterScreenCapture", "center ROI video-only updateRuntimeConfig failed", t)
                    restoreRuntimeFields()
                    switched = false
                } finally {
                    latch.countDown()
                }
            }
            latch.await(3000, TimeUnit.MILLISECONDS)
            return switched
        }

        val splitOnlyChanged = oldMode == targetMode &&
                oldCropSize == targetCropSize &&
                oldPreset == targetPreset &&
                oldJpegSubsamplingMode == targetJpegSubsamplingMode &&
                (oldFullParts != targetFullParts || oldCropParts != targetCropParts)
        val jpegOnlyChanged = oldMode == targetMode &&
                oldCropSize == targetCropSize &&
                oldPreset == targetPreset &&
                (oldJpegQuality != targetJpegQuality || oldJpegSubsamplingMode != targetJpegSubsamplingMode)

        val mustRebuildReader = oldMode != targetMode ||
                oldCropSize != targetCropSize ||
                oldPreset != targetPreset

        if (!mustRebuildReader) {
            applyRuntimeFields()
            if (splitOnlyChanged || jpegOnlyChanged || newJpegQuality != null || newJpegSubsamplingMode != null || newFps != null) {
                handler?.post {
                    warmupNativeForSpec(captureSpec)
                    perfLastSplitParts = splitPartsForSpec(captureSpec).coerceAtLeast(1)
                    emitCoreStatus(force = true)
                }
            }
            return true
        }

        val captureHandler = handler ?: return false
        val latch = CountDownLatch(1)
        var switched = false
        captureHandler.post {
            try {
                // 从这里开始在采集回调线程内原子地切换运行时字段和 ImageReader/VirtualDisplay。
                // 同一个 Looper 上不会插入新的 ImageReader 回调，因此不会再被 maybeReconfigureForRotation() 抢跑。
                applyRuntimeFields()
                runCatching { NativeCenterCropJpegEncoder.resetFrameTimeline() }
                val screen = readScreenSize()
                val newSpec = createCaptureSpec(screen)
                applyCaptureSpec(screen, newSpec)
                switched = true
            } catch (t: Throwable) {
                Log.e("CenterScreenCapture", "updateRuntimeConfig applyCaptureSpec failed; keep old capture alive", t)
                restoreRuntimeFields()
                runCatching { NativeCenterCropJpegEncoder.resetFrameTimeline() }
                switched = false
            } finally {
                latch.countDown()
            }
        }
        latch.await(3000, TimeUnit.MILLISECONDS)
        return switched
    }

    fun requestFullscreenPreset(newPreset: Int): Boolean {
        if (captureMode != AppSettingsStore.SCREEN_MIRROR_CAPTURE_MODE_FULLSCREEN_SCALED) return false
        val captureHandler = handler ?: return false

        if (enableCenterRoiVideoOnlyProbe) {
            val safePreset = newPreset.coerceIn(
                AppSettingsStore.MIN_SCREEN_MIRROR_FULLSCREEN_PRESET,
                AppSettingsStore.MAX_SCREEN_MIRROR_FULLSCREEN_PRESET
            )
            val latch = CountDownLatch(1)
            var switched = false
            captureHandler.post {
                try {
                    fullscreenPreset = safePreset
                    val screen = readScreenSize()
                    val newSpec = createCaptureSpecForPreset(screen, safePreset)
                    captureSpec = newSpec
                    lastScreenSize = screen
                    lastStableScreen = screen
                    lastStableSpec = newSpec
                    restartCenterRoiVideoExperiment(mediaProjection, screen, newSpec)
                    Log.w(
                        "CenterScreenCapture",
                        "center ROI video-only preset updated ${newSpec.captureWidth}x${newSpec.captureHeight}"
                    )
                    switched = true
                } catch (t: Throwable) {
                    Log.e("CenterScreenCapture", "center ROI video-only requestFullscreenPreset failed", t)
                    switched = false
                } finally {
                    latch.countDown()
                }
            }
            latch.await(3000, TimeUnit.MILLISECONDS)
            return switched
        }

        val initialScreen = readScreenSize()
        val initialSpec = createCaptureSpecForPreset(initialScreen, newPreset)
        val oldSpec = captureSpec ?: return false

        if (oldSpec.outputWidth == initialSpec.outputWidth && oldSpec.outputHeight == initialSpec.outputHeight) {
            fullscreenPreset = newPreset
            return true
        }

        val latch = CountDownLatch(1)
        var switched = false
        val oldPreset = fullscreenPreset

        captureHandler.post {
            try {
                fullscreenPreset = newPreset
                runCatching { NativeCenterCropJpegEncoder.resetFrameTimeline() }
                val screen = readScreenSize()
                val newSpec = createCaptureSpecForPreset(screen, newPreset)
                applyCaptureSpec(screen, newSpec)
                switched = true
            } catch (t: Throwable) {
                fullscreenPreset = oldPreset
                runCatching { NativeCenterCropJpegEncoder.resetFrameTimeline() }
                Log.e("CenterScreenCapture", "requestFullscreenPreset failed; keep old capture alive", t)
                switched = false
            } finally {
                latch.countDown()
            }
        }

        latch.await(3000, TimeUnit.MILLISECONDS)
        return switched
    }

    fun release() {
        if (isReleasing) return
        isReleasing = true
        try {
            // 先停止 ImageReader 回调和所有尚未执行的任务，避免 stop 时继续拿新帧。
            runCatching { imageReader?.setOnImageAvailableListener(null, null) }
            runCatching { handler?.removeCallbacksAndMessages(null) }
            for (index in encodeHandlers.indices) {
                runCatching { encodeHandlers[index]?.removeCallbacksAndMessages(null) }
            }

            // 只关闭“还没开始编码”的 Image。正在 native tjCompress2 中使用的 Image 不能在这里 close，
            // 否则停止投屏时 ImageReader/底层 GraphicBuffer 可能被释放，native 会崩在 libturbojpeg。
            synchronized(encodeLock) {
                clearPendingFrameQueueLocked()

                for (worker in encodeWorkers) {
                    worker.pendingImage?.close()
                    worker.pendingImage = null
                    worker.pendingCallbackNs = 0L
                    worker.pendingFrameNs = 0L
                    worker.pendingSeq = 0L
                    worker.drainPosted = false
                }
            }

            // 等正在编码的 Image 正常跑完并在 encodeDrainRunnable.finally 里 close。
            // 关键：必须等它们结束以后，才能 close ImageReader / VirtualDisplay / MediaProjection。
            if (!isOnEncodeThread()) {
                waitForEncodeWorkersIdle(timeoutMs = 1200L)
            }

            runCatching { virtualDisplay?.release() }
            virtualDisplay = null
            runCatching { imageReader?.close() }
            imageReader = null

            centerRoiVideoSender?.close()
            centerRoiVideoSender = null
            audioSender?.close()
            audioSender = null
            localAudioSilencer?.disable()
            localAudioSilencer = null

            val projection = mediaProjection
            mediaProjection = null
            if (projection != null) {
                if (projectionCallbackRegistered) {
                    runCatching { projection.unregisterCallback(projectionCallback) }
                    projectionCallbackRegistered = false
                }
                runCatching { projection.stop() }
            }

            runCatching { handlerThread?.quitSafely() }
            for (index in encodeThreads.indices) {
                runCatching { encodeThreads[index]?.quitSafely() }
                encodeThreads[index] = null
                encodeHandlers[index] = null
                encodeWorkers[index].running = false
            }
            handlerThread = null
            handler = null
            resetPacingState()
            nextFrameSeq = 0L
            synchronized(deliveryLock) { lastDeliveredSeq = 0L }
            captureSpec = null
            lastScreenSize = null
            lastStableScreen = null
            lastStableSpec = null
            synchronized(coreStatusLock) {
                callbackTargetCpu = -1
                callbackBindOk = false
                callbackCurrentCpu = -1
                callbackLastCpuSampleAtMs = 0L
                for (index in 0 until maxEncodeWorkerCount) {
                    encodeTargetCpuIds[index] = -1
                    encodeBindOk[index] = false
                    encodeCurrentCpuIds[index] = -1
                }
            }
            lastCoreStatusText = ""
            onCoreStatusText?.invoke(AppSettingsStore.DEFAULT_SCREEN_MIRROR_CORE_STATUS)
        } finally {
            isReleasing = false
        }
    }

    private fun isOnEncodeThread(): Boolean {
        val current = Thread.currentThread()
        return encodeThreads.any { it === current }
    }

    private fun waitForEncodeWorkersIdle(timeoutMs: Long) {
        val deadline = SystemClock.elapsedRealtime() + timeoutMs
        while (SystemClock.elapsedRealtime() < deadline) {
            val busy = synchronized(encodeLock) {
                !pendingFrameQueue.isEmpty() || encodeWorkers.any { worker ->
                    worker.running || worker.pendingImage != null || worker.drainPosted
                }
            }
            if (!busy) return
            try {
                Thread.sleep(2L)
            } catch (_: InterruptedException) {
                Thread.currentThread().interrupt()
                return
            }
        }
        Log.w("CenterScreenCapture", "wait encode workers idle timeout")
    }

    private fun clearQueuedImagesForReconfigureLocked() {
        clearPendingFrameQueueLocked()

        for (worker in encodeWorkers) {
            // running=true 的 Image 已经交给 native tjCompress2，不能 close。
            if (!worker.running) {
                worker.pendingImage?.close()
                worker.pendingImage = null
                worker.pendingCallbackNs = 0L
                worker.pendingFrameNs = 0L
                worker.pendingSeq = 0L
                worker.drainPosted = false
            }
        }
    }


    private fun formatCpuId(cpuId: Int): String {
        return if (cpuId >= 0) "CPU$cpuId" else "-"
    }

    private fun imageFormatName(format: Int): String {
        return when (format) {
            PixelFormat.RGBA_8888 -> "rgba"
            ImageFormat.YUV_420_888 -> "yuv"
            else -> format.toString()
        }
    }

    private fun safeRatio(maxValue: Double, minValue: Double): Double {
        return if (minValue <= 0.0) 0.0 else maxValue / minValue
    }

    private fun nsToMs(ns: Long): Double {
        return ns.coerceAtLeast(0L) / 1_000_000.0
    }

    private fun chooseCallbackCpu(suggestedCpus: IntArray, spec: CaptureSpec?): Int {
        // 分块模式下，真正吃 CPU 的是 native 内部分块 worker。
        // native 分块 worker 固定使用最高两个性能核心，所以回调线程不能再绑 CPU7/CPU6。
        val cpus = suggestedCpus.filter { it >= 0 }.distinct()
        if (cpus.isEmpty()) return -1

        val splitParts = splitPartsForSpec(spec)
        if (splitParts > 1) {
            // cpus 已按性能从高到低排序：0/1 留给 native 分块编码；
            // 回调优先使用剩余里的最低/中核。只有两个核心可用时不绑定，让系统调度。
            return cpus.drop(2).lastOrNull() ?: -1
        }

        val activeEncodeCount = activeEncodeWorkerCountForSpec(spec).coerceIn(1, maxEncodeWorkerCount)
        return cpus.drop(activeEncodeCount).lastOrNull() ?: -1
    }


    private fun updateCallbackCoreStatus(
        targetCpu: Int? = null,
        bindOk: Boolean? = null,
        currentCpu: Int? = null,
    ) {
        synchronized(coreStatusLock) {
            if (targetCpu != null) callbackTargetCpu = targetCpu
            if (bindOk != null) callbackBindOk = bindOk
            if (currentCpu != null) callbackCurrentCpu = currentCpu
        }
    }

    private fun updateCallbackCpuSample(nowMs: Long = SystemClock.elapsedRealtime()) {
        if (nowMs - callbackLastCpuSampleAtMs < 500L) return
        val cpu = NativeCenterCropJpegEncoder.getCurrentCpu()
        synchronized(coreStatusLock) {
            callbackCurrentCpu = cpu
            callbackLastCpuSampleAtMs = nowMs
        }
    }

    private fun updateEncodeCoreStatus(
        workerIndex: Int,
        targetCpu: Int? = null,
        bindOk: Boolean? = null,
        currentCpu: Int? = null,
    ) {
        if (workerIndex !in 0 until maxEncodeWorkerCount) return
        synchronized(coreStatusLock) {
            if (targetCpu != null) encodeTargetCpuIds[workerIndex] = targetCpu
            if (bindOk != null) encodeBindOk[workerIndex] = bindOk
            if (currentCpu != null) encodeCurrentCpuIds[workerIndex] = currentCpu
        }
    }

    private fun emitCoreStatus(force: Boolean = false) {
        val schedulingEncodeCount = activeEncodeWorkerCount().coerceIn(1, maxEncodeWorkerCount)
        val splitParts = splitPartsForSpec(captureSpec)
        // 分块模式下 Kotlin 只保留 1 个“调度线程”，真正吃 CPU 的是 native 分块 worker。
        // UI 不再写 enc=2，避免把“预备绑定核心”误解成“每帧两个 Kotlin 编码线程”。
        val visibleEncodeCoreCount = if (splitParts > 1) {
            min(2, maxEncodeWorkerCount)
        } else {
            schedulingEncodeCount
        }
        val text = synchronized(coreStatusLock) {
            buildString {
                append("核心状态：回调线程 目标=")
                append(formatCpuId(callbackTargetCpu))
                append(" 实际=")
                append(formatCpuId(callbackCurrentCpu))
                append(" 绑定=")
                append(if (callbackBindOk) "成功" else "失败")
                append(" / Kotlin调度线程=")
                append(schedulingEncodeCount)
                append(" / native分块=")
                val actualSplitParts = perfLastSplitParts.takeIf { it > 1 } ?: splitParts
                append(actualSplitParts)
                if (actualSplitParts != splitParts) {
                    append("(请求=")
                    append(splitParts)
                    append(")")
                }
                append(" / 预备编码核心=")
                append(visibleEncodeCoreCount)
                for (index in 0 until visibleEncodeCoreCount) {
                    append(" | 预备")
                    append(index)
                    append(" 目标=")
                    append(formatCpuId(encodeTargetCpuIds[index]))
                    append(" 实际=")
                    append(formatCpuId(encodeCurrentCpuIds[index]))
                    append(" 绑定=")
                    append(if (encodeBindOk[index]) "成功" else "失败")
                }
            }
        }
        if (!force && text == lastCoreStatusText) return
        lastCoreStatusText = text
        onCoreStatusText?.invoke(text)
    }


    private fun activeEncodeWorkerCount(): Int {
        return activeEncodeWorkerCountForSpec(captureSpec)
    }

    private fun fullscreenSplitPartsForSpec(spec: CaptureSpec?): Int {
        if (spec == null) return 1
        if (spec.mode != AppSettingsStore.SCREEN_MIRROR_CAPTURE_MODE_FULLSCREEN_SCALED) return 1
        val nativeMax = NativeCenterCropJpegEncoder.availableEncodeCpuCount().coerceAtLeast(1)
        if (nativeMax < 2) return 1
        return fullscreenSplitParts.coerceIn(2, nativeMax)
    }

    private fun cropSplitPartsForSpec(spec: CaptureSpec?): Int {
        if (spec == null) return 1
        if (spec.mode != AppSettingsStore.SCREEN_MIRROR_CAPTURE_MODE_CENTER_CROP) return 1
        return cropSplitParts.coerceIn(1, 4)
    }

    private fun splitPartsForSpec(spec: CaptureSpec?): Int {
        return if (spec?.mode == AppSettingsStore.SCREEN_MIRROR_CAPTURE_MODE_FULLSCREEN_SCALED) {
            fullscreenSplitPartsForSpec(spec)
        } else {
            cropSplitPartsForSpec(spec)
        }
    }

    private fun activeEncodeWorkerCountForSpec(spec: CaptureSpec?): Int {
        if (spec == null) return 1
        if (splitPartsForSpec(spec) > 1) return 1

        val longEdge = max(spec.outputWidth, spec.outputHeight)
        val shortEdge = min(spec.outputWidth, spec.outputHeight)
        return if (longEdge >= 1920 || shortEdge >= 1080) {
            min(2, maxEncodeWorkerCount)
        } else {
            1
        }
    }

    private fun imageReaderMaxImagesForSpec(spec: CaptureSpec): Int {
        val parts = splitPartsForSpec(spec)
        if (parts > 1) {
            // inline 实验版不再保留 pending 队列，ImageReader 只需要少量 buffer。
            // 这样可以避免 24 缓存把延迟隐藏成排队，并更容易观察真实调度等待是否接近 0。
            return 3
        }
        return if (activeEncodeWorkerCountForSpec(spec) >= 2) 4 else 3
    }

    private fun resetPacingState() {
        nextFrameDueNs = 0L
        pacingLastCallbackNs = 0L
        pacingCreditNs = 0L
    }

    private fun handleImageAvailable(source: ImageReader) {
        if (isReleasing) {
            runCatching { source.acquireLatestImage()?.close() }
            statSkippedByReleaseCount.incrementAndGet()
            return
        }
        if (maybeReconfigureForRotation(source)) {
            statSkippedByReconfigureCount.incrementAndGet()
            return
        }

        val callbackNowMs = SystemClock.elapsedRealtime()
        val callbackNowNs = System.nanoTime()
        updateCallbackCpuSample(callbackNowMs)

        if (statWindowStartMs == 0L) statWindowStartMs = callbackNowMs
        statCallbackCount += 1

        if (statLastCallbackAtMs != 0L) {
            val gap = callbackNowMs - statLastCallbackAtMs
            if (gap > statMaxCallbackGapMs) statMaxCallbackGapMs = gap
        }
        statLastCallbackAtMs = callbackNowMs

        var image: Image? = null
        try {
            image = try {
                source.acquireLatestImage()
            } catch (t: UnsupportedOperationException) {
                val msg = t.message ?: ""
                val mismatch = msg.contains("doesn't match", ignoreCase = true)
                if (mismatch && activeReaderFormat == ImageFormat.YUV_420_888) {
                    Log.w(
                        "CenterScreenCapture",
                        "YUV ImageReader not supported by producer, fallback to RGBA_8888: $msg"
                    )
                    activeReaderFormat = PixelFormat.RGBA_8888
                    val screen = lastScreenSize ?: readScreenSize().also { lastScreenSize = it }
                    val spec = captureSpec ?: createCaptureSpec(screen)
                    applyCaptureSpec(screen, spec)
                    return
                }
                throw t
            } catch (_: IllegalStateException) {
                statAcquireNullCount += 1
                maybeReportSourcePerf(callbackNowMs)
                return
            }
            if (image == null) {
                statAcquireNullCount += 1
                maybeReportSourcePerf(callbackNowMs)
                return
            }
            lastImageCallbackAfterReconfigureMs = callbackNowMs

            if (warmupDropFrames > 0) {
                warmupDropFrames -= 1
                image.close()
                image = null
                return
            }

            // FPS 节流使用 token bucket，而不是“下一帧绝对到期时间”。
            // 旧逻辑在 120fps 下会因为回调抖动误杀：源头平均 117~119fps，
            // 但某两个回调偶尔挤在一起时被 pace 掉，导致每秒固定丢 15~25 帧。
            // token bucket 会把前面延迟产生的时间余额保留下来，允许后续补帧。
            val pacingNowNs = callbackNowNs
            val intervalNs = targetFrameIntervalNs.coerceAtLeast(1L)
            val pacingToleranceNs = (intervalNs / 4L).coerceIn(250_000L, 750_000L)

            if (pacingLastCallbackNs == 0L) {
                pacingLastCallbackNs = pacingNowNs
                pacingCreditNs = intervalNs
            } else {
                val elapsedNs = (pacingNowNs - pacingLastCallbackNs).coerceAtLeast(0L)
                pacingLastCallbackNs = pacingNowNs
                pacingCreditNs = (pacingCreditNs + elapsedNs).coerceAtMost(intervalNs * 2L)
            }

            if (pacingCreditNs + pacingToleranceNs < intervalNs) {
                statSkippedByIntervalCount += 1
                return
            }
            pacingCreditNs = (pacingCreditNs - intervalNs).coerceAtLeast(0L)

            statProcessedCount += 1
            val currentSpec = captureSpec
            if (shouldEncodeInlineOnCaptureThread(currentSpec)) {
                val seq = synchronized(encodeLock) { ++nextFrameSeq }
                statDirectDispatchCount += 1
                try {
                    processImage(0, image, callbackNowNs, seq)
                } catch (t: Throwable) {
                    Log.e("CenterScreenCapture", "inline processImage failed", t)
                    statSkippedByEncodeFailCount.incrementAndGet()
                } finally {
                    image.close()
                    image = null
                }
            } else {
                enqueueImageForEncoding(image, callbackNowNs)
                image = null
            }
        } finally {
            image?.close()
            maybeReportSourcePerf(callbackNowMs)
        }
    }

    private fun enqueueImageForEncoding(image: Image, callbackNowNs: Long) {
        if (isReleasing) {
            image.close()
            return
        }
        synchronized(encodeLock) {
            if (isReleasing) {
                image.close()
                return
            }
            val frameNs = image.timestamp.takeIf { it > 0L } ?: callbackNowNs
            val seq = ++nextFrameSeq
            val activeWorkerCount = activeEncodeWorkerCount()

            var idleIndex = -1
            val startIndex = nextEncodeDispatchIndex % activeWorkerCount
            for (offset in 0 until activeWorkerCount) {
                val index = (startIndex + offset) % activeWorkerCount
                val worker = encodeWorkers[index]
                if (!worker.running && worker.pendingImage == null && !worker.drainPosted) {
                    idleIndex = index
                    break
                }
            }

            if (idleIndex >= 0) {
                statDirectDispatchCount += 1
                val worker = encodeWorkers[idleIndex]
                worker.pendingImage = image
                worker.pendingCallbackNs = callbackNowNs
                worker.pendingFrameNs = frameNs
                worker.pendingSeq = seq
                worker.drainPosted = true
                nextEncodeDispatchIndex = (idleIndex + 1) % activeWorkerCount

                val handler = encodeHandlers[idleIndex]
                if (handler == null) {
                    worker.pendingImage?.close()
                    worker.pendingImage = null
                    worker.pendingCallbackNs = 0L
                    worker.pendingFrameNs = 0L
                    worker.pendingSeq = 0L
                    worker.drainPosted = false
                    return
                }
                handler.post(encodeDrainRunnables[idleIndex])
                return
            }

            // 编码线程忙时进入“时间预算短队列”，不再一忙就覆盖掉上一帧。
            // 例如 120fps 下每帧预算 8.33ms，这里允许最多约 2 帧预算的瞬时排队。
            // 简单场景会很快追回；只有队列超过上限或最旧帧等待超过 2 帧预算时，才丢最旧帧。
            statQueuedLatestCount += 1
            prunePendingFrameQueueLocked(callbackNowNs, captureSpec)
            pendingFrameQueue.addLast(PendingFrame(image, callbackNowNs, frameNs, seq))
            prunePendingFrameQueueLocked(callbackNowNs, captureSpec)
        }
    }

    private fun encodeDrainRunnable(workerIndex: Int): Runnable = object : Runnable {
        override fun run() {
            while (true) {
                val image: Image
                val callbackNs: Long
                val seq: Long
                synchronized(encodeLock) {
                    val worker = encodeWorkers[workerIndex]
                    if (isReleasing) {
                        clearPendingFrameQueueLocked()
                        worker.pendingImage?.close()
                        worker.pendingImage = null
                        worker.pendingCallbackNs = 0L
                        worker.pendingFrameNs = 0L
                        worker.pendingSeq = 0L
                        worker.drainPosted = false
                        worker.running = false
                        return
                    }
                    var pending = worker.pendingImage
                    if (pending == null) {
                        prunePendingFrameQueueLocked(System.nanoTime(), captureSpec)
                        if (!pendingFrameQueue.isEmpty()) {
                            val queued = pendingFrameQueue.removeFirst()
                            pending = queued.image
                            worker.pendingImage = pending
                            worker.pendingCallbackNs = queued.callbackNs
                            worker.pendingFrameNs = queued.frameNs
                            worker.pendingSeq = queued.seq
                        }
                    }
                    if (pending == null) {
                        worker.drainPosted = false
                        worker.running = false
                        return
                    }
                    image = pending
                    callbackNs = worker.pendingCallbackNs
                    seq = worker.pendingSeq
                    worker.pendingImage = null
                    worker.pendingCallbackNs = 0L
                    worker.pendingFrameNs = 0L
                    worker.pendingSeq = 0L
                    worker.running = true
                }
                try {
                    processImage(workerIndex, image, callbackNs, seq)
                } catch (t: Throwable) {
                    Log.e("CenterScreenCapture", "encodeDrainRunnable[$workerIndex] failed", t)
                } finally {
                    image.close()
                    synchronized(encodeLock) {
                        encodeWorkers[workerIndex].running = false
                    }
                }
            }
        }
    }

    private fun maybeReconfigureForRotation(source: ImageReader): Boolean {
        if (source !== imageReader) {
            runCatching { source.acquireLatestImage()?.close() }
            return true
        }
        if (isReconfiguringCapture) {
            // 切换分辨率/极限模式时，可能已经排队了旧 reader 的回调。
            // 这里必须把最新 Image 取出并关闭，否则 ImageReader 的 3 个 buffer 很容易被填满，
            // 之后 producer 报 BufferQueue abandoned/画面停住。
            runCatching { source.acquireLatestImage()?.close() }
            return true
        }

        val oldSpec = captureSpec ?: return false
        val oldScreen = lastScreenSize ?: readScreenSize().also { lastScreenSize = it }
        val newScreen = readScreenSize()
        val screenChanged =
            oldScreen.width != newScreen.width ||
                    oldScreen.height != newScreen.height ||
                    oldScreen.densityDpi != newScreen.densityDpi

        // 这里仅处理真实屏幕旋转/尺寸变化。运行时参数变化必须由 updateRuntimeConfig()
        // 在采集线程内统一重建，不能让回调线程因为字段先变而抢先重建一次。
        if (!screenChanged) return false

        val newSpec = createCaptureSpec(newScreen)

        isReconfiguringCapture = true
        return try {
            Log.w(
                "CenterScreenCapture",
                "display changed oldScreen=${oldScreen.width}x${oldScreen.height} " +
                        "newScreen=${newScreen.width}x${newScreen.height} " +
                        "oldSpec=${oldSpec.captureWidth}x${oldSpec.captureHeight}/${oldSpec.outputWidth}x${oldSpec.outputHeight} " +
                        "newSpec=${newSpec.captureWidth}x${newSpec.captureHeight}/${newSpec.outputWidth}x${newSpec.outputHeight}"
            )
            applyCaptureSpec(newScreen, newSpec)
            true
        } catch (t: Throwable) {
            Log.e("CenterScreenCapture", "applyCaptureSpec on rotation failed", t)
            false
        } finally {
            isReconfiguringCapture = false
        }
    }

    private fun applyCaptureSpec(screen: ScreenSize, newSpec: CaptureSpec, forceSurfaceReset: Boolean = false) {
        if (enableCenterRoiVideoOnlyProbe) {
            captureSpec = newSpec
            lastScreenSize = screen
            lastStableScreen = screen
            lastStableSpec = newSpec
            resetPacingState()
            resetSourceStats()
            resetPerfWindowForReconfigure()
            restartCenterRoiVideoExperiment(mediaProjection, screen, newSpec)
            Log.w(
                "CenterScreenCapture",
                "center ROI video-only applyCaptureSpec ${newSpec.captureWidth}x${newSpec.captureHeight}"
            )
            return
        }
        val callbackHandler = handler ?: return
        val vd = virtualDisplay ?: return
        val oldReader = imageReader
        val oldSpec = captureSpec
        val oldScreen = lastScreenSize

        isReconfiguringCapture = true
        var newReader: ImageReader? = null
        var surfaceSwitched = false

        try {
            // 先停旧 reader 回调，再清掉还没开始编码的旧 Image。
            // 正在 native tjCompress2 使用的 Image 不能强行 close，所以先等 worker 空闲。
            runCatching { oldReader?.setOnImageAvailableListener(null, null) }
            synchronized(encodeLock) {
                clearQueuedImagesForReconfigureLocked()
            }
            if (!isOnEncodeThread()) {
                waitForEncodeWorkersIdle(timeoutMs = 1200L)
            }

            val createdReader = ImageReader.newInstance(
                newSpec.captureWidth,
                newSpec.captureHeight,
                activeReaderFormat,
                imageReaderMaxImagesForSpec(newSpec),
            )
            newReader = createdReader

            // 先把新 reader 的 listener 挂好，再把 VD 切到新 Surface。
            // 升分辨率时 producer 有概率马上投递第一帧，先挂 listener 可以避免首批 buffer 堆满后不再推进。
            createdReader.setOnImageAvailableListener({ src -> handleImageAvailable(src) }, callbackHandler)

            // ImageReader / VirtualDisplay 切换后 Image.timestamp 可能重启或回退。
            // 清 native 的旧帧时间线，避免 stale-frame 保护把新尺寸的帧全部判旧。
            runCatching { NativeCenterCropJpegEncoder.resetFrameTimeline() }

            // 不再 createVirtualDisplay；但升分辨率时仅 resize+setSurface(new) 在部分机型上会偶发不出帧。
            // 稳定顺序：detach 旧 surface -> resize -> attach 新 surface。watchdog 触发时强制再走一次 detach。
            runCatching { vd.setSurface(null) }
            if (forceSurfaceReset) {
                // 给 BufferQueue 一次明确断开机会；只在 watchdog 恢复路径使用，避免正常切换增加明显延迟。
                SystemClock.sleep(16L)
            }
            if (oldSpec == null ||
                oldSpec.captureWidth != newSpec.captureWidth ||
                oldSpec.captureHeight != newSpec.captureHeight ||
                oldScreen?.densityDpi != screen.densityDpi ||
                forceSurfaceReset
            ) {
                vd.resize(newSpec.captureWidth, newSpec.captureHeight, screen.densityDpi)
            }
            vd.setSurface(createdReader.surface)
            surfaceSwitched = true

            imageReader = createdReader
            virtualDisplay = vd
            captureSpec = newSpec
            lastScreenSize = screen
            lastStableScreen = screen
            lastStableSpec = newSpec
            runCatching { NativeCenterCropJpegEncoder.resetFrameTimeline() }
            warmupNativeForSpec(newSpec)
            resetWarmupDropFrames()
            resetPacingState()
            resetSourceStats()
            resetPerfWindowForReconfigure()
            markCaptureReconfigured(screen, newSpec, forceSurfaceReset)
            if (!enableCenterRoiVideoOnlyProbe) {
                restartCenterRoiVideoExperiment(mediaProjection, screen, newSpec)
            }

            // 最后释放旧 reader.此时系统可能打印旧 BufferQueue abandoned，这是旧 Surface 断开，不代表新画面失败。
            runCatching { oldReader?.close() }

            Log.w(
                "CenterScreenCapture",
                "capture reconfigured ok ${newSpec.captureWidth}x${newSpec.captureHeight} reader=${imageReaderMaxImagesForSpec(newSpec)} reuseVD=1 forceReset=$forceSurfaceReset"
            )
        } catch (t: Throwable) {
            Log.e("CenterScreenCapture", "applyCaptureSpec failed; trying to restore previous capture", t)
            runCatching { newReader?.setOnImageAvailableListener(null, null) }

            if (oldReader != null && oldSpec != null && oldScreen != null) {
                imageReader = oldReader
                captureSpec = oldSpec
                lastScreenSize = oldScreen
                lastStableScreen = oldScreen
                lastStableSpec = oldSpec
                resetWarmupDropFrames()
                resetPacingState()
                resetSourceStats()
                resetPerfWindowForReconfigure()

                if (surfaceSwitched) {
                    // 如果已经把 VD 切到了新 Surface，但后续失败，尽量切回旧 Surface。
                    runCatching {
                        vd.resize(oldSpec.captureWidth, oldSpec.captureHeight, oldScreen.densityDpi)
                        vd.setSurface(oldReader.surface)
                    }.onFailure { restoreError ->
                        Log.e("CenterScreenCapture", "restore old VirtualDisplay surface failed", restoreError)
                    }
                }

                val restored = runCatching {
                    oldReader.setOnImageAvailableListener({ src -> handleImageAvailable(src) }, callbackHandler)
                }.isSuccess
                if (restored) {
                    Log.w(
                        "CenterScreenCapture",
                        "restore previous capture ok ${oldSpec.captureWidth}x${oldSpec.captureHeight}"
                    )
                }
            }

            runCatching { newReader?.close() }
            throw t
        } finally {
            isReconfiguringCapture = false
        }
    }

    private fun markCaptureReconfigured(screen: ScreenSize, spec: CaptureSpec, forceSurfaceReset: Boolean) {
        val token = ++captureReconfigureGeneration
        val nowMs = SystemClock.elapsedRealtime()
        lastReconfigureAppliedAtMs = nowMs
        lastImageCallbackAfterReconfigureMs = 0L
        lastSuccessfulEncodeAfterReconfigureMs = 0L
        reconfigureWatchdogAttempts = if (forceSurfaceReset) reconfigureWatchdogAttempts + 1 else 0

        val captureHandler = handler ?: return
        captureHandler.postDelayed({
            if (isReleasing) return@postDelayed
            if (token != captureReconfigureGeneration) return@postDelayed
            if (isReconfiguringCapture) return@postDelayed

            val current = captureSpec ?: return@postDelayed
            val sameSpec = current.captureWidth == spec.captureWidth &&
                    current.captureHeight == spec.captureHeight &&
                    current.outputWidth == spec.outputWidth &&
                    current.outputHeight == spec.outputHeight &&
                    current.mode == spec.mode
            if (!sameSpec) return@postDelayed

            val gotAnyFrame = lastImageCallbackAfterReconfigureMs >= nowMs ||
                    lastSuccessfulEncodeAfterReconfigureMs >= nowMs
            if (gotAnyFrame) return@postDelayed

            if (reconfigureWatchdogAttempts >= 2) {
                Log.e(
                    "CenterScreenCapture",
                    "capture reconfigure watchdog no frames after ${spec.captureWidth}x${spec.captureHeight}, attempts=$reconfigureWatchdogAttempts"
                )
                return@postDelayed
            }

            Log.w(
                "CenterScreenCapture",
                "capture reconfigure watchdog refresh surface ${spec.captureWidth}x${spec.captureHeight} attempt=${reconfigureWatchdogAttempts + 1}"
            )
            runCatching { applyCaptureSpec(screen, spec, forceSurfaceReset = true) }
                .onFailure { t ->
                    Log.e("CenterScreenCapture", "watchdog applyCaptureSpec failed", t)
                }
        }, 450L)
    }

    private fun resetPerfWindowForReconfigure() {
        synchronized(perfLock) {
            perfFrameCount = 0
            perfTotalCaptureNs = 0L
            perfTotalEncodeNs = 0L
            perfTotalEmitNs = 0L
            perfTotalFrameNs = 0L
            perfTotalJpegBytes = 0L
            perfTotalPart0Bytes = 0L
            perfTotalPart1Bytes = 0L
            perfTotalSocketWriteNs = 0L
            perfTotalPart0EncodeNs = 0L
            perfTotalPart1EncodeNs = 0L
            perfTotalQueueWaitNs = 0L
            perfMaxQueueWaitNs = 0L
            perfMaxNativeNs = 0L
            perfMaxTotalNs = 0L
            perfMaxSocketWriteNs = 0L
            perfTotalLaneMaxEncodeNs = 0L
            perfTotalLaneMinEncodeNs = 0L
            resetNativePartPerfWindow()
            perfLastSplitParts = 1
            perfLastFormatText = ""
            perfLastRowStride = 0
            perfLastPixelStride = 0
            perfLastBufferKb = 0
            perfLastReaderMaxImages = 0
            lastPerfReportAtMs = 0L
        }
    }

    private fun resetSourceStats() {
        statWindowStartMs = 0L
        statCallbackCount = 0
        statAcquireNullCount = 0
        statProcessedCount = 0
        statSkippedByIntervalCount = 0
        statLastCallbackAtMs = 0L
        statMaxCallbackGapMs = 0L
        statDroppedByOverwriteCount = 0
        statDirectDispatchCount = 0
        statQueuedLatestCount = 0
        statSkippedByReleaseCount.set(0)
        statSkippedByReconfigureCount.set(0)
        statSkippedBySinkNotReadyCount.set(0)
        statSkippedByDeliverySeqCount.set(0)
        statSkippedByEncodeFailCount.set(0)
    }

    private fun maybeReportSourcePerf(nowMs: Long) {
        if (statWindowStartMs == 0L) {
            statWindowStartMs = nowMs
            return
        }
        val elapsed = nowMs - statWindowStartMs
        if (elapsed < 1000L) return

        val observedFrameIntMs = if (statCallbackCount > 1) elapsed.toDouble() / statCallbackCount else 0.0
        val targetIntervalMs = targetFrameIntervalNs / 1_000_000.0
        val targetFpsApprox = if (targetFrameIntervalNs > 0L) 1_000_000_000.0 / targetFrameIntervalNs else 0.0
        val skipRelease = statSkippedByReleaseCount.getAndSet(0)
        val skipReconfigure = statSkippedByReconfigureCount.getAndSet(0)
        val skipSink = statSkippedBySinkNotReadyCount.getAndSet(0)
        val skipSeq = statSkippedByDeliverySeqCount.getAndSet(0)
        val skipEncodeFail = statSkippedByEncodeFailCount.getAndSet(0)
        val spec = captureSpec
        val requestedSplitParts = splitPartsForSpec(spec)
        val actualSplitParts = perfLastSplitParts.takeIf { it > 1 } ?: requestedSplitParts
        val workerCount = activeEncodeWorkerCountForSpec(spec)
        val readerMax = spec?.let { imageReaderMaxImagesForSpec(it) } ?: 0
        val text = "画面来源 回调=$statCallbackCount 成功=$statProcessedCount 空图=$statAcquireNullCount " +
                "节流跳过=$statSkippedByIntervalCount 编码忙=$statQueuedLatestCount 直发=$statDirectDispatchCount 覆盖丢帧=$statDroppedByOverwriteCount " +
                "原因:节流=$statSkippedByIntervalCount 接收端未就绪=$skipSink 旧帧=$skipSeq 编码失败=$skipEncodeFail 释放=$skipRelease 重建=$skipReconfigure " +
                "Kotlin调度线程=$workerCount native分块=$actualSplitParts(请求=$requestedSplitParts) reader缓存=$readerMax 格式=${imageFormatName(activeReaderFormat)} " +
                "目标=%.1ffps 每帧预算=%.2fms 最大回调间隔=${statMaxCallbackGapMs}ms 实际回调间隔=%.2fms"
                    .format(targetFpsApprox, targetIntervalMs, observedFrameIntMs)

        Log.e("ScreenSourcePerf", text)
        // 每秒强制刷新一次核心状态，避免 UI 被外部状态覆盖后一直停留在等待采集线程上报。
        emitCoreStatus(force = true)

        if (
            captureMode == AppSettingsStore.SCREEN_MIRROR_CAPTURE_MODE_FULLSCREEN_SCALED &&
            false &&
            fullscreenPreset == AppSettingsStore.SCREEN_MIRROR_FULLSCREEN_PRESET_900P &&
            !autoFallbackTriggered &&
            onAutoFallbackTo720p != null
        ) {
            if (statSkippedByIntervalCount >= 3 || statProcessedCount < (statCallbackCount * 0.97f)) {
                consecutiveHighSkipWindows += 1
            } else {
                consecutiveHighSkipWindows = 0
            }
            if (consecutiveHighSkipWindows >= 3) {
                autoFallbackTriggered = true
                Log.w("ScreenSourcePerf", "auto fallback: 1600x900 -> 1280x720")
                onAutoFallbackTo720p?.invoke()
            }
        }

        statWindowStartMs = nowMs
        statCallbackCount = 0
        statAcquireNullCount = 0
        statProcessedCount = 0
        statSkippedByIntervalCount = 0
        statDroppedByOverwriteCount = 0
        statDirectDispatchCount = 0
        statQueuedLatestCount = 0
        statMaxCallbackGapMs = 0L
    }

    private fun processImage(workerIndex: Int, image: Image, callbackNowNs: Long, frameSeq: Long) {
        updateEncodeCoreStatus(workerIndex, currentCpu = NativeCenterCropJpegEncoder.getCurrentCpu())
        val spec = captureSpec ?: return
        val frameProducedNs = image.timestamp.takeIf { it > 0L } ?: callbackNowNs
        var centerVideoQueueNsThisFrame = 0L
        var centerVideoQueueAttemptsThisFrame = 0
        var centerVideoQueueOkThisFrame = 0

        // Hybrid rule: feed the selected video ROI pipeline before checking the JPEG sink.
        // The video socket/MediaCodec path must not be blocked by the JPEG
        // LocalSocket fd state; otherwise H.264 only receives a few frames whenever the JPEG
        // sender happens to be ready, which was observed as renderFps=0~4 even though the
        // hardware AVC encoder was selected correctly.
        var preTriedCenterVideo = false
        var preQueuedCenterVideo = false
        var preCenterRoi: CenterRoiRect? = null
        if (
            centerRoiVideoEnabled &&
            spec.mode == AppSettingsStore.SCREEN_MIRROR_CAPTURE_MODE_FULLSCREEN_SCALED &&
            image.format == PixelFormat.RGBA_8888 &&
            image.planes.isNotEmpty()
        ) {
            val videoSender = centerRoiVideoSender
            val plane = image.planes[0]
            if (videoSender != null && plane.pixelStride == 4) {
                val roi = centerRoiRectForSpec(spec)
                preCenterRoi = roi
                preTriedCenterVideo = true
                val videoQueueStartNs = System.nanoTime()
                preQueuedCenterVideo = videoSender.queueRgbaFrame(
                    rgbaBuffer = plane.buffer,
                    bufferCapacity = plane.buffer.capacity(),
                    srcWidth = image.width,
                    srcHeight = image.height,
                    rowStride = plane.rowStride,
                    pixelStride = plane.pixelStride,
                    frameProducedNs = frameProducedNs,
                )
                centerVideoQueueNsThisFrame += (System.nanoTime() - videoQueueStartNs).coerceAtLeast(0L)
                centerVideoQueueAttemptsThisFrame += 1
                if (preQueuedCenterVideo) centerVideoQueueOkThisFrame += 1
                if (!preQueuedCenterVideo) {
                    val nowMs = SystemClock.elapsedRealtime()
                    if (nowMs - lastHybridVideoQueueFailLogMs >= 1000L) {
                        lastHybridVideoQueueFailLogMs = nowMs
                        Log.w(
                            "CenterScreenCapture",
                            "hybrid video ROI prequeue failed: " +
                                    "connected=${videoSender.isConnected} src=${image.width}x${image.height} " +
                                    "stride=${plane.rowStride}/${plane.pixelStride} roi=${roi.left},${roi.top} ${roi.width}x${roi.height}"
                        )
                    }
                }
            }
        }

        val sinkReady = isFrameSinkReady?.invoke() ?: true
        if (!sinkReady) {
            statSkippedBySinkNotReadyCount.incrementAndGet()
            return
        }

        val captureLatencyNs = (callbackNowNs - frameProducedNs).coerceAtLeast(0L)

        val shouldDeliver = synchronized(deliveryLock) {
            if (frameSeq <= lastDeliveredSeq) {
                false
            } else {
                lastDeliveredSeq = frameSeq
                true
            }
        }
        if (!shouldDeliver) {
            statSkippedByDeliverySeqCount.incrementAndGet()
            return
        }

        val nativeStartNs = System.nanoTime()
        val effectiveQuality = if (keepFrameRate) {
            adaptiveJpegQuality
        } else {
            jpegQuality.coerceIn(30, 100)
        }
        var outWidth = spec.outputWidth
        var outHeight = spec.outputHeight
        var planeRowStride = 0
        var planePixelStride = 0
        var planeBufferKb = 0
        val sent = when (image.format) {
            ImageFormat.YUV_420_888 -> {
                if (image.planes.size < 3) {
                    statSkippedByEncodeFailCount.incrementAndGet()
                    return
                }
                val yPlane = image.planes[0]
                val uPlane = image.planes[1]
                val vPlane = image.planes[2]
                planeRowStride = yPlane.rowStride
                planePixelStride = yPlane.pixelStride
                planeBufferKb = yPlane.buffer.capacity() / 1024
                when (spec.mode) {
                    AppSettingsStore.SCREEN_MIRROR_CAPTURE_MODE_FULLSCREEN_SCALED -> {
                        outWidth = image.width
                        outHeight = image.height
                        NativeCenterCropJpegEncoder.encodeAndSendFullFrameYuv420888(
                            yBuffer = yPlane.buffer,
                            uBuffer = uPlane.buffer,
                            vBuffer = vPlane.buffer,
                            srcWidth = image.width,
                            srcHeight = image.height,
                            yRowStride = yPlane.rowStride,
                            uRowStride = uPlane.rowStride,
                            vRowStride = vPlane.rowStride,
                            uPixelStride = uPlane.pixelStride,
                            vPixelStride = vPlane.pixelStride,
                            jpegQuality = effectiveQuality,
                            frameProducedNs = frameProducedNs,
                            callbackStartNs = callbackNowNs,
                        )
                    }
                    else -> {
                        outWidth = spec.outputWidth
                        outHeight = spec.outputHeight
                        NativeCenterCropJpegEncoder.encodeAndSendCenterCropYuv420888(
                            yBuffer = yPlane.buffer,
                            uBuffer = uPlane.buffer,
                            vBuffer = vPlane.buffer,
                            srcWidth = image.width,
                            srcHeight = image.height,
                            yRowStride = yPlane.rowStride,
                            uRowStride = uPlane.rowStride,
                            vRowStride = vPlane.rowStride,
                            uPixelStride = uPlane.pixelStride,
                            vPixelStride = vPlane.pixelStride,
                            cropSize = spec.outputWidth,
                            jpegQuality = effectiveQuality,
                            frameProducedNs = frameProducedNs,
                            callbackStartNs = callbackNowNs,
                        )
                    }
                }
            }
            PixelFormat.RGBA_8888 -> {
                val plane = image.planes.firstOrNull() ?: run {
                    statSkippedByEncodeFailCount.incrementAndGet()
                    return
                }
                planeRowStride = plane.rowStride
                planePixelStride = plane.pixelStride
                planeBufferKb = plane.buffer.capacity() / 1024
                when (spec.mode) {
                    AppSettingsStore.SCREEN_MIRROR_CAPTURE_MODE_FULLSCREEN_SCALED -> {
                        outWidth = image.width
                        outHeight = image.height
                        val selectedSplitParts = if (plane.pixelStride == 4) fullscreenSplitPartsForSpec(spec) else 1
                        val centerRoi = preCenterRoi ?: centerRoiRectForSpec(spec)
                        val videoSender = centerRoiVideoSender

                            // Hard hybrid rule:
                        // if the video sender exists for this fullscreen RGBA capture, JPEG must NOT
                        // encode the video-owned region. The frame has already been queued before the
                        // JPEG sinkReady check, so do not queue it again here.
                        val forceRoiOwnedByVideo = centerRoiVideoEnabled && videoSender != null && plane.pixelStride == 4
                        val queuedCenterVideo: Boolean = if (forceRoiOwnedByVideo) {
                            if (preTriedCenterVideo) {
                                preQueuedCenterVideo
                            } else {
                                val videoQueueStartNs = System.nanoTime()
                                val ok = videoSender?.queueRgbaFrame(
                                    rgbaBuffer = plane.buffer,
                                    bufferCapacity = plane.buffer.capacity(),
                                    srcWidth = image.width,
                                    srcHeight = image.height,
                                    rowStride = plane.rowStride,
                                    pixelStride = plane.pixelStride,
                                    frameProducedNs = frameProducedNs,
                                ) == true
                                centerVideoQueueNsThisFrame += (System.nanoTime() - videoQueueStartNs).coerceAtLeast(0L)
                                centerVideoQueueAttemptsThisFrame += 1
                                if (ok) centerVideoQueueOkThisFrame += 1
                                ok
                            }
                        } else {
                            false
                        }

                        val sentNow = if (forceRoiOwnedByVideo) {
                            if (isBottomVideoRoiMode()) {
                                NativeCenterCropJpegEncoder.encodeAndSendFrameWithBottomVideoRgba8888SplitN(
                                    buffer = plane.buffer,
                                    srcWidth = image.width,
                                    srcHeight = image.height,
                                    rowStride = plane.rowStride,
                                    pixelStride = plane.pixelStride,
                                    videoTop = centerRoi.top,
                                    videoHeight = centerRoi.height,
                                    jpegQuality = effectiveQuality,
                                    frameProducedNs = frameProducedNs,
                                    callbackStartNs = callbackNowNs,
                                    splitParts = selectedSplitParts,
                                )
                            } else {
                                if (jpegCenterOnly) {
                                    // 只投中心区域 + 中心 H.264：JPEG 外圈在 Android 端完全不编码，
                                    // 只保留已经投递给 MediaCodec 的中心 H.264 视频区域。
                                    queuedCenterVideo
                                } else {
                                    NativeCenterCropJpegEncoder.encodeAndSendFrameExceptCenterRgba8888SplitN(
                                        buffer = plane.buffer,
                                        srcWidth = image.width,
                                        srcHeight = image.height,
                                        rowStride = plane.rowStride,
                                        pixelStride = plane.pixelStride,
                                        centerLeft = centerRoi.left,
                                        centerTop = centerRoi.top,
                                        centerWidth = centerRoi.width,
                                        centerHeight = centerRoi.height,
                                        jpegQuality = effectiveQuality,
                                        frameProducedNs = frameProducedNs,
                                        callbackStartNs = callbackNowNs,
                                        splitParts = selectedSplitParts,
                                    )
                                }
                            }
                        } else {
                            NativeCenterCropJpegEncoder.encodeAndSendFullFrameRgba8888SplitN(
                                buffer = plane.buffer,
                                srcWidth = image.width,
                                srcHeight = image.height,
                                rowStride = plane.rowStride,
                                pixelStride = plane.pixelStride,
                                jpegQuality = effectiveQuality,
                                frameProducedNs = frameProducedNs,
                                callbackStartNs = callbackNowNs,
                                splitParts = selectedSplitParts,
                            )
                        }

                        if (forceRoiOwnedByVideo && queuedCenterVideo == false) {
                            val nowMs = SystemClock.elapsedRealtime()
                            if (nowMs - lastHybridVideoQueueFailLogMs >= 1000L) {
                                lastHybridVideoQueueFailLogMs = nowMs
                                Log.w(
                                    "CenterScreenCapture",
                                    "hybrid video ROI is reserved for H264 but queue failed: " +
                                            "connected=${videoSender?.isConnected} src=${image.width}x${image.height} " +
                                            "stride=${plane.rowStride}/${plane.pixelStride} region=${if (isBottomVideoRoiMode()) "bottom" else "center"} roi=${centerRoi.left},${centerRoi.top} ${centerRoi.width}x${centerRoi.height}"
                                )
                            }
                        }

                        sentNow
                    }
                    else -> {
                        outWidth = spec.outputWidth
                        outHeight = spec.outputHeight
                        val selectedCropParts = if (plane.pixelStride == 4) cropSplitPartsForSpec(spec) else 1
                        NativeCenterCropJpegEncoder.encodeAndSendCenterCropRgba8888SplitN(
                            buffer = plane.buffer,
                            srcWidth = image.width,
                            srcHeight = image.height,
                            rowStride = plane.rowStride,
                            pixelStride = plane.pixelStride,
                            cropSize = spec.outputWidth,
                            jpegQuality = effectiveQuality,
                            frameProducedNs = frameProducedNs,
                            callbackStartNs = callbackNowNs,
                            splitParts = selectedCropParts,
                        )
                    }
                }
            }
            else -> {
                Log.w("CenterScreenCapture", "unsupported image.format=${image.format}")
                false
            }
        }
        val nativeEndNs = System.nanoTime()
        if (!sent) {
            statSkippedByEncodeFailCount.incrementAndGet()
            NativeCenterCropJpegEncoder.clearOutputFileDescriptor()
            return
        }
        lastSuccessfulEncodeAfterReconfigureMs = SystemClock.elapsedRealtime()

        if (!NativeCenterCropJpegEncoder.fillLastSendStats(lastSendStatsArray)) {
            lastSendStatsArray.fill(0L)
            lastSendStatsArray[6] = 1L
        }
        if (!NativeCenterCropJpegEncoder.fillLastSplitPartStats(lastSplitPartStatsArray)) {
            lastSplitPartStatsArray.fill(0L)
        }
        val sendTotalBytes = lastSendStatsArray[0].coerceAtLeast(0L)
        val sendPart0Bytes = lastSendStatsArray[1].coerceAtLeast(0L)
        val sendPart1Bytes = lastSendStatsArray[2].coerceAtLeast(0L)
        val sendSocketWriteNs = lastSendStatsArray[3].coerceAtLeast(0L)
        val sendPart0EncodeNs = lastSendStatsArray[4].coerceAtLeast(0L)
        val sendPart1EncodeNs = lastSendStatsArray[5].coerceAtLeast(0L)
        val sendSplitParts = lastSendStatsArray[6].toInt().coerceAtLeast(1)
        val nativeEncodeNs = (nativeEndNs - nativeStartNs).coerceAtLeast(0L)
        val queueWaitNs = (nativeStartNs - callbackNowNs).coerceAtLeast(0L)
        val totalFrameNs = (nativeEndNs - callbackNowNs).coerceAtLeast(0L)
        val socketWriteNs = sendSocketWriteNs
        val lane0EncodeNs = sendPart0EncodeNs
        val lane1EncodeNs = sendPart1EncodeNs
        val laneMaxEncodeNs = max(lane0EncodeNs, lane1EncodeNs)
        val laneMinEncodeNs = min(lane0EncodeNs, lane1EncodeNs)

        updateAdaptiveQuality(nativeEncodeNs)

        val timing = FrameTimingMeta(
            frameProducedNs = frameProducedNs,
            callbackStartNs = callbackNowNs,
            encodeStartNs = nativeStartNs,
            encodeEndNs = nativeEndNs,
            sequence = frameSeq,
        )
        onJpegFrame?.invoke(emptyJpegBytes, outWidth, outHeight, timing)

        synchronized(perfLock) {
            perfFrameCount += 1
            perfTotalCaptureNs += captureLatencyNs
            perfTotalEncodeNs += nativeEncodeNs
            perfTotalEmitNs += 0L
            perfTotalFrameNs += totalFrameNs
            perfTotalQueueWaitNs += queueWaitNs
            perfTotalCenterVideoQueueNs += centerVideoQueueNsThisFrame
            perfCenterVideoQueueAttempts += centerVideoQueueAttemptsThisFrame
            perfCenterVideoQueueOk += centerVideoQueueOkThisFrame
            if (centerVideoQueueNsThisFrame > perfMaxCenterVideoQueueNs) perfMaxCenterVideoQueueNs = centerVideoQueueNsThisFrame
            if (queueWaitNs > perfMaxQueueWaitNs) perfMaxQueueWaitNs = queueWaitNs
            if (nativeEncodeNs > perfMaxNativeNs) perfMaxNativeNs = nativeEncodeNs
            if (totalFrameNs > perfMaxTotalNs) perfMaxTotalNs = totalFrameNs
            if (socketWriteNs > perfMaxSocketWriteNs) perfMaxSocketWriteNs = socketWriteNs
            perfTotalJpegBytes += sendTotalBytes
            perfTotalPart0Bytes += sendPart0Bytes
            perfTotalPart1Bytes += sendPart1Bytes
            perfTotalSocketWriteNs += socketWriteNs
            perfTotalPart0EncodeNs += lane0EncodeNs
            perfTotalPart1EncodeNs += lane1EncodeNs
            perfTotalLaneMaxEncodeNs += laneMaxEncodeNs
            perfTotalLaneMinEncodeNs += laneMinEncodeNs

            val nativePartCountFromStats = lastSplitPartStatsArray[0].toInt().coerceIn(0, MAX_NATIVE_SPLIT_PARTS)
            val nativePartCount = (if (nativePartCountFromStats > 0) nativePartCountFromStats else sendSplitParts)
                .coerceIn(1, MAX_NATIVE_SPLIT_PARTS)
            perfLastNativePartCount = nativePartCount
            if (nativePartCountFromStats > 0) {
                for (index in 0 until nativePartCount) {
                    val base = 1 + index * 3
                    perfTotalNativePartBytes[index] += lastSplitPartStatsArray.getOrElse(base) { 0L }.coerceAtLeast(0L)
                    perfTotalNativePartEncodeNs[index] += lastSplitPartStatsArray.getOrElse(base + 1) { 0L }.coerceAtLeast(0L)
                    perfLastNativePartCpuIds[index] = lastSplitPartStatsArray.getOrElse(base + 2) { -1L }.toInt()
                }
            } else {
                // 兼容旧 native：没有逐块接口时，至少把旧的上/下半组统计放进前两项。
                perfTotalNativePartBytes[0] += sendPart0Bytes
                perfTotalNativePartEncodeNs[0] += lane0EncodeNs
                perfTotalNativePartBytes[1] += sendPart1Bytes
                perfTotalNativePartEncodeNs[1] += lane1EncodeNs
            }

            perfLastSplitParts = sendSplitParts
            perfLastFormatText = imageFormatName(image.format)
            perfLastRowStride = planeRowStride
            perfLastPixelStride = planePixelStride
            perfLastBufferKb = planeBufferKb
            perfLastReaderMaxImages = imageReaderMaxImagesForSpec(spec)
            val ktDispatchCount = activeEncodeWorkerCountForSpec(spec)
            perfLastModeText = if (spec.mode == AppSettingsStore.SCREEN_MIRROR_CAPTURE_MODE_FULLSCREEN_SCALED) {
                if (image.format == ImageFormat.YUV_420_888) {
                    "模式=全屏 分辨率=${outWidth}x${outHeight} 格式=YUV 质量=$effectiveQuality Kotlin调度=$ktDispatchCount native分块=1"
                } else {
                    val requestedSplitParts = fullscreenSplitPartsForSpec(spec)
                    val actualSplitParts = sendSplitParts
                    "模式=全屏 分辨率=${outWidth}x${outHeight} 格式=RGBA 质量=$effectiveQuality Kotlin调度=$ktDispatchCount native分块=$actualSplitParts(请求=$requestedSplitParts)"
                }
            } else {
                if (image.format == ImageFormat.YUV_420_888) {
                    "模式=中心裁剪 分辨率=${outWidth}x${outHeight} 格式=YUV 质量=$effectiveQuality Kotlin调度=$ktDispatchCount native分块=1"
                } else {
                    "模式=中心裁剪 分辨率=${outWidth}x${outHeight} 格式=RGBA 质量=$effectiveQuality Kotlin调度=$ktDispatchCount native分块=${cropSplitPartsForSpec(spec)}"
                }
            }

            val nowMs = SystemClock.elapsedRealtime()
            if (nowMs - lastPerfReportAtMs >= 1000L) {
                val count = perfFrameCount.coerceAtLeast(1)
                val captureMs = perfTotalCaptureNs / 1_000_000.0 / count
                val encodeMs = perfTotalEncodeNs / 1_000_000.0 / count
                val emitMs = perfTotalEmitNs / 1_000_000.0 / count
                val totalMs = perfTotalFrameNs / 1_000_000.0 / count
                val queueWaitMs = perfTotalQueueWaitNs / 1_000_000.0 / count
                val centerVideoQueueMs = perfTotalCenterVideoQueueNs / 1_000_000.0 / count
                val centerVideoQueueMaxMs = nsToMs(perfMaxCenterVideoQueueNs)
                val centerVideoQueueAttempts = perfCenterVideoQueueAttempts
                val centerVideoQueueOk = perfCenterVideoQueueOk
                val windowSec = if (lastPerfReportAtMs > 0L) {
                    ((nowMs - lastPerfReportAtMs).coerceAtLeast(1L)) / 1000.0
                } else {
                    1.0
                }
                val sendFps = perfFrameCount / windowSec
                val avgJpegKb = perfTotalJpegBytes / 1024.0 / count
                val avgPart0Kb = perfTotalPart0Bytes / 1024.0 / count
                val avgPart1Kb = perfTotalPart1Bytes / 1024.0 / count
                val mbps = (perfTotalJpegBytes * 8.0 / 1_000_000.0) / windowSec
                val writeMs = perfTotalSocketWriteNs / 1_000_000.0 / count
                val p0Ms = perfTotalPart0EncodeNs / 1_000_000.0 / count
                val p1Ms = perfTotalPart1EncodeNs / 1_000_000.0 / count
                val laneMaxMs = perfTotalLaneMaxEncodeNs / 1_000_000.0 / count
                val laneMinMs = perfTotalLaneMinEncodeNs / 1_000_000.0 / count
                val budgetMs = targetFrameIntervalNs / 1_000_000.0
                val loadPct = if (budgetMs > 0.0) totalMs / budgetMs * 100.0 else 0.0
                val skewBytes = safeRatio(max(avgPart0Kb, avgPart1Kb), min(avgPart0Kb, avgPart1Kb))
                val skewTime = safeRatio(max(p0Ms, p1Ms), min(p0Ms, p1Ms))
                val maxQueueMs = nsToMs(perfMaxQueueWaitNs)
                val maxNativeMs = nsToMs(perfMaxNativeNs)
                val maxTotalMs = nsToMs(perfMaxTotalNs)
                val maxWriteMs = nsToMs(perfMaxSocketWriteNs)
                val groupBytesSkew = skewBytes
                val groupTimeSkew = skewTime
                val nativePartDetailText = buildNativePartDetailText(perfLastNativePartCount, count)

                val text = "画面性能 帧数=$count 发送帧率=%.1f $perfLastModeText ".format(sendFps) +
                        "采集延迟=%.1fms 调度等待=%.1fms native总耗时=%.1fms 回调输出=%.1fms 单帧总耗时=%.1fms ".format(
                            captureMs,
                            queueWaitMs,
                            encodeMs,
                            emitMs,
                            totalMs,
                        ) +
                        "中心视频queue=%.2fms(ok=%d/%d max=%.2fms) ".format(
                            centerVideoQueueMs,
                            centerVideoQueueOk,
                            centerVideoQueueAttempts,
                            centerVideoQueueMaxMs,
                        ) +
                        "每帧预算=%.2fms 负载=%.0f%% JPG=%.0fKB 带宽=%.1fMbps socket写=%.2fms ".format(
                            budgetMs,
                            loadPct,
                            avgJpegKb,
                            mbps,
                            writeMs,
                        ) +
                        "旧分组统计:上半=%.0fKB/%.1fms 下半=%.0fKB/%.1fms 大小不均=%.2fx 耗时不均=%.2fx".format(
                            avgPart0Kb,
                            p0Ms,
                            avgPart1Kb,
                            p1Ms,
                            groupBytesSkew,
                            groupTimeSkew,
                        )

                val detailText = "画面细分 Kotlin调度线程=${activeEncodeWorkerCountForSpec(spec)} native分块=$perfLastSplitParts " +
                        "reader缓存=$perfLastReaderMaxImages 图像格式=$perfLastFormatText stride=${perfLastRowStride}/${perfLastPixelStride} " +
                        "buffer=${perfLastBufferKb}KB 调度等待最大=%.1fms native最大=%.1fms 单帧最大=%.1fms socket写最大=%.2fms ".format(
                            maxQueueMs,
                            maxNativeMs,
                            maxTotalMs,
                            maxWriteMs,
                        ) +
                        "旧分组最大均值=%.1fms 旧分组最小均值=%.1fms 旧分组不均=%.2fx $nativePartDetailText".format(
                            laneMaxMs,
                            laneMinMs,
                            safeRatio(laneMaxMs, laneMinMs),
                        )

                Log.e("ScreenPerf", text)
                Log.e("ScreenPerfDetail", detailText)
                onPerfText?.invoke(text)
                emitCoreStatus()

                perfFrameCount = 0
                perfTotalCaptureNs = 0L
                perfTotalEncodeNs = 0L
                perfTotalEmitNs = 0L
                perfTotalFrameNs = 0L
                perfTotalJpegBytes = 0L
                perfTotalPart0Bytes = 0L
                perfTotalPart1Bytes = 0L
                perfTotalSocketWriteNs = 0L
                perfTotalPart0EncodeNs = 0L
                perfTotalPart1EncodeNs = 0L
                perfTotalQueueWaitNs = 0L
                perfTotalCenterVideoQueueNs = 0L
                perfMaxCenterVideoQueueNs = 0L
                perfCenterVideoQueueAttempts = 0
                perfCenterVideoQueueOk = 0
                perfMaxQueueWaitNs = 0L
                perfMaxNativeNs = 0L
                perfMaxTotalNs = 0L
                perfMaxSocketWriteNs = 0L
                perfTotalLaneMaxEncodeNs = 0L
                perfTotalLaneMinEncodeNs = 0L
                resetNativePartPerfWindow()
                perfLastSplitParts = 1
                perfLastFormatText = ""
                perfLastRowStride = 0
                perfLastPixelStride = 0
                perfLastBufferKb = 0
                perfLastReaderMaxImages = 0
                lastPerfReportAtMs = nowMs
            }
        }
    }

    private fun updateAdaptiveQuality(encodeNs: Long) {
        if (!keepFrameRate) {
            return
        }
        // 复杂画面（草丛/人多）会让 JPEG 编码时间升高。
        // 优先保帧率：编码耗时接近一帧预算时自动降质量；稳定后再慢慢恢复。
        synchronized(perfLock) {
            adaptiveWindowFrames += 1
            adaptiveWindowTotalEncodeNs += encodeNs.coerceAtLeast(0L)
            if (adaptiveWindowFrames < 30) return

            val avgEncodeMs = adaptiveWindowTotalEncodeNs / 1_000_000.0 / adaptiveWindowFrames.coerceAtLeast(1)
            val frameBudgetMs = targetFrameIntervalNs / 1_000_000.0
            val minQuality = 60
            val maxQuality = jpegQuality.coerceIn(minQuality, 100)

            if (avgEncodeMs > frameBudgetMs * 0.82 && adaptiveJpegQuality > minQuality) {
                adaptiveJpegQuality = (adaptiveJpegQuality - 5).coerceAtLeast(minQuality)
                adaptiveStableWindows = 0
                Log.w(
                    "ScreenPerf",
                    "自动降JPEG质量 q=$adaptiveJpegQuality 平均编码=%.1fms 每帧预算=%.1fms"
                        .format(avgEncodeMs, frameBudgetMs)
                )
            } else if (avgEncodeMs < frameBudgetMs * 0.58 && adaptiveJpegQuality < maxQuality) {
                adaptiveStableWindows += 1
                if (adaptiveStableWindows >= 2) {
                    adaptiveJpegQuality = (adaptiveJpegQuality + 2).coerceAtMost(maxQuality)
                    adaptiveStableWindows = 0
                    Log.w(
                        "ScreenPerf",
                        "自动升JPEG质量 q=$adaptiveJpegQuality 平均编码=%.1fms 每帧预算=%.1fms"
                            .format(avgEncodeMs, frameBudgetMs)
                    )
                }
            } else {
                adaptiveStableWindows = 0
            }

            adaptiveWindowFrames = 0
            adaptiveWindowTotalEncodeNs = 0L
        }
    }

    private data class ScreenSize(
        val width: Int,
        val height: Int,
        val densityDpi: Int,
    )

    private data class CaptureSpec(
        val mode: Int,
        val captureWidth: Int,
        val captureHeight: Int,
        val outputWidth: Int,
        val outputHeight: Int,
    )

    private fun createCaptureSpec(screen: ScreenSize): CaptureSpec {
        return if (captureMode == AppSettingsStore.SCREEN_MIRROR_CAPTURE_MODE_FULLSCREEN_SCALED) {
            createCaptureSpecForPreset(screen, fullscreenPreset)
        } else {
            val size = min(cropSize, min(screen.width, screen.height)).coerceAtLeast(1)
            CaptureSpec(
                mode = AppSettingsStore.SCREEN_MIRROR_CAPTURE_MODE_CENTER_CROP,
                captureWidth = screen.width,
                captureHeight = screen.height,
                outputWidth = size,
                outputHeight = size,
            )
        }
    }

    private fun createCaptureSpecForPreset(
        screen: ScreenSize,
        presetValue: Int,
    ): CaptureSpec {
        if (presetValue == AppSettingsStore.SCREEN_MIRROR_FULLSCREEN_PRESET_DEVICE_MAX) {
            val width = evenSize(screen.width)
            val height = evenSize(screen.height)
            return CaptureSpec(
                mode = AppSettingsStore.SCREEN_MIRROR_CAPTURE_MODE_FULLSCREEN_SCALED,
                captureWidth = width,
                captureHeight = height,
                outputWidth = width,
                outputHeight = height,
            )
        }

        // PC 端发来的“1080P / 2K / 3K”是目标标准档位，不能再用手机屏幕比例去算对边。
        // 旧逻辑：1920 * screenHeight / screenWidth，在 3040x1904 这种 16:10 平板上会得到 1920x1202。
        // 新逻辑：按标准 16:9 输出，1080P 固定 1920x1080，2K 固定 2560x1440。
        val screenLongEdge = evenSize(max(screen.width, screen.height))
        val supports2k = screenLongEdge >= 2560 && min(screen.width, screen.height) >= 1440
        val targetLongEdge = when (presetValue) {
            AppSettingsStore.SCREEN_MIRROR_FULLSCREEN_PRESET_900P -> 1600
            AppSettingsStore.SCREEN_MIRROR_FULLSCREEN_PRESET_1080P -> 1920
            AppSettingsStore.SCREEN_MIRROR_FULLSCREEN_PRESET_2K -> if (supports2k) 2560 else 1920
            // 3K 没有唯一行业标准。这里优先 3200x1800；如果设备长边不足 3200，
            // 用设备长边做 16:9 标准高度，例如 Y700 3040x1904 -> 3040x1710。
            AppSettingsStore.SCREEN_MIRROR_FULLSCREEN_PRESET_3K -> min(3200, screenLongEdge).coerceAtLeast(2560)
            else -> 1920
        }
        val targetShortEdge = evenSize(((targetLongEdge.toLong() * 9L + 8L) / 16L).toInt())

        val width: Int
        val height: Int
        if (screen.width >= screen.height) {
            width = evenSize(targetLongEdge)
            height = targetShortEdge
        } else {
            width = targetShortEdge
            height = evenSize(targetLongEdge)
        }

        return CaptureSpec(
            mode = AppSettingsStore.SCREEN_MIRROR_CAPTURE_MODE_FULLSCREEN_SCALED,
            captureWidth = width,
            captureHeight = height,
            outputWidth = width,
            outputHeight = height,
        )
    }

    private fun evenSize(value: Int): Int {
        val clamped = value.coerceAtLeast(2)
        return if (clamped and 1 == 0) clamped else clamped - 1
    }

    private fun buildNativePartDetailText(partCount: Int, frameCount: Int): String {
        val count = frameCount.coerceAtLeast(1)
        val shownCount = partCount.coerceIn(0, MAX_NATIVE_SPLIT_PARTS)
        if (shownCount <= 0) return "native分块明细=无"

        val parts = ArrayList<String>(shownCount)
        for (index in 0 until shownCount) {
            val avgKb = perfTotalNativePartBytes[index] / 1024.0 / count
            val avgMs = perfTotalNativePartEncodeNs[index] / 1_000_000.0 / count
            val cpuText = formatCpuId(perfLastNativePartCpuIds[index])
            parts += "块$index=%.0fKB/%.1fms/$cpuText".format(avgKb, avgMs)
        }
        return "native分块明细=" + parts.joinToString(" ")
    }

    private fun resetNativePartPerfWindow() {
        for (index in 0 until MAX_NATIVE_SPLIT_PARTS) {
            perfTotalNativePartBytes[index] = 0L
            perfTotalNativePartEncodeNs[index] = 0L
            perfLastNativePartCpuIds[index] = -1
        }
        perfLastNativePartCount = 0
    }

    private fun readScreenSize(): ScreenSize {
        val displayManager = appContext.getSystemService(Context.DISPLAY_SERVICE) as DisplayManager
        val metrics = DisplayMetrics()
        val display = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            displayManager.getDisplay(Display.DEFAULT_DISPLAY)
        } else {
            @Suppress("DEPRECATION")
            windowManager.defaultDisplay
        }
        if (display != null) {
            @Suppress("DEPRECATION")
            display.getRealMetrics(metrics)
            return ScreenSize(
                width = metrics.widthPixels.coerceAtLeast(1),
                height = metrics.heightPixels.coerceAtLeast(1),
                densityDpi = metrics.densityDpi,
            )
        }
        val fallback = appContext.resources.displayMetrics
        return ScreenSize(
            width = fallback.widthPixels.coerceAtLeast(1),
            height = fallback.heightPixels.coerceAtLeast(1),
            densityDpi = fallback.densityDpi,
        )
    }
    companion object {
        private const val MAX_NATIVE_SPLIT_PARTS = 16

        private fun frameIntervalNsForFps(fps: Int): Long {
            val normalized = AppSettingsStore.normalizeFpsPreset(fps).coerceAtLeast(1)
            return (1_000_000_000.0 / normalized.toDouble()).toLong().coerceAtLeast(1L)
        }
    }

}
