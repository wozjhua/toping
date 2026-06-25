package com.example.huilangtoupingV3

import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaFormat
import android.media.projection.MediaProjection
import android.net.LocalServerSocket
import android.net.LocalSocket
import android.opengl.EGL14
import android.opengl.EGLConfig
import android.opengl.EGLContext
import android.opengl.EGLDisplay
import android.opengl.EGLExt
import android.opengl.EGLSurface
import android.opengl.GLES20
import android.opengl.GLES30
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.HandlerThread
import android.os.Process
import android.os.SystemClock
import android.util.Log
import android.view.Surface
import java.io.BufferedOutputStream
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.ArrayDeque
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.CountDownLatch

/**
 * Single-VirtualDisplay center ROI H.264 sender.
 *
 * This class intentionally does NOT create MediaProjection.createVirtualDisplay().  The only
 * VirtualDisplay in the app remains the main ImageReader capture owned by
 * CenterScreenCaptureController.  For every acquired RGBA_8888 Image, the controller copies the
 * center ROI into this sender, this sender uploads that ROI to the MediaCodec input Surface with
 * GLES, and the encoded H.264 packets are sent over localabstract:huilang_center_roi_video.
 *
 * Pipeline:
 *   one full-screen VirtualDisplay -> ImageReader RGBA
 *     -> center ROI copy -> GLES texture -> MediaCodec inputSurface -> H.264 socket
 *     -> outside ROI native CPU/JPEG split -> JPEG socket
 */
class CenterRoiVideoStreamSender(
    @Suppress("UNUSED_PARAMETER") private val mediaProjection: MediaProjection,
    @Suppress("UNUSED_PARAMETER") private val densityDpi: Int,
    private val sourceWidth: Int,
    private val sourceHeight: Int,
    private val socketName: String = LOCAL_SOCKET_NAME,
    @Suppress("UNUSED_PARAMETER") private val roiWidthRatio: Float = 1.0f,
    @Suppress("UNUSED_PARAMETER") private val roiHeightRatio: Float = 1.0f,
    private val codecWidth: Int = CenterRoiVideoConfig.WIDTH,
    private val codecHeight: Int = CenterRoiVideoConfig.HEIGHT,
    private val initialRoiLeft: Int = -1,
    private val initialRoiTop: Int = -1,
    private val bitrate: Int = CenterRoiVideoConfig.BITRATE,
    private val frameRate: Int = CenterRoiVideoConfig.FPS,
    private val initialQualityMode: Int = CenterRoiVideoConfig.QUALITY_MODE,
) {
    companion object {
        const val LOCAL_SOCKET_NAME = "huilang_center_roi_video"
        const val PORT = 27185
        private const val TAG = "CenterRoiVideo"
        private const val PACKET_MAGIC = 0x484C5631 // HLV1
        private const val PACKET_VERSION = 2
        private const val FLAG_CODEC_CONFIG = 1
        private const val FLAG_KEY_FRAME = 2
        private const val EGL_RECORDABLE_ANDROID = 0x3142
    }

    private val lock = Object()
    private val outputLock = Object()
    private val frameLock = Object()

    @Volatile
    private var closed = false

    @Volatile
    var isConnected: Boolean = false
        private set

    private var thread: HandlerThread? = null
    private var handler: Handler? = null
    private var acceptThread: Thread? = null

    private var server: LocalServerSocket? = null
    private var client: LocalSocket? = null
    private var output: BufferedOutputStream? = null

    private var inputSurface: Surface? = null
    private var codec: MediaCodec? = null
    private var egl: EncoderEgl? = null
    private var renderer: RgbaTextureRenderer? = null

    private var latestCodecConfig: ByteArray? = null
    private var encodedFrames = 0
    private var renderedFrames = 0
    private var droppedFrames = 0
    private var sentBytes = 0L
    private var sentPackets = 0
    private var sentVideoFrames = 0
    private var directUploadFrames = 0
    private var copyUploadFrames = 0
    private var directUploadFallbackFrames = 0

    // Diagnostic counters for locating whether center H.264 is limited by
    // ImageReader/caller wait, GL upload, encoder input Surface swap, MediaCodec output, or socket write.
    private var queuedCalls = 0
    private var directCallerWaitNs = 0L
    private var directDrawNs = 0L
    private var directSwapNs = 0L
    private var directDrainNs = 0L
    private var directTaskNs = 0L
    private var compactCopyNs = 0L
    private var renderDrawNs = 0L
    private var renderSwapNs = 0L
    private var renderDrainNs = 0L
    private var renderTaskNs = 0L
    private var codecDequeueNs = 0L
    private var codecDequeueCalls = 0
    private var codecOutputBuffers = 0
    private var socketWriteNs = 0L
    private var socketWritePackets = 0

    private data class H264Timing(
        val frameProducedNs: Long,
        val queueStartNs: Long,

        // GL upload / draw
        val drawStartNs: Long,
        val uploadDoneNs: Long,

        // MediaCodec input Surface swap
        val swapStartNs: Long,
        val swapDoneNs: Long,
    )
    private val timingByPtsUs = ConcurrentHashMap<Long, H264Timing>()

    private fun rememberTiming(ptsNs: Long, timing: H264Timing) {
        val ptsUs = ptsNs / 1000L
        if (timingByPtsUs.size > 256) timingByPtsUs.clear()
        timingByPtsUs[ptsUs] = timing
    }

    private var lastLogMs = 0L
    private var lastPtsNs = 0L
    private var lastSegmentLogMs = 0L

    @Volatile
    private var lastSentVideoFrameWallMs = 0L

    @Volatile
    private var lastQueuedVideoFrameWallMs = 0L

    @Volatile
    private var lastRenderedVideoFrameWallMs = 0L

    @Volatile
    private var safeSourceWidth = even(sourceWidth.coerceIn(320, 3840))
    @Volatile
    private var safeSourceHeight = even(sourceHeight.coerceIn(180, 2160))


    @Volatile
    private var safeCodecWidth =
        even(codecWidth.coerceIn(CenterRoiVideoConfig.MIN_CODEC_WIDTH, CenterRoiVideoConfig.MAX_CODEC_WIDTH))
            .coerceAtMost(safeSourceWidth)

    @Volatile
    private var safeCodecHeight =
        even(codecHeight.coerceIn(CenterRoiVideoConfig.MIN_CODEC_HEIGHT, CenterRoiVideoConfig.MAX_CODEC_HEIGHT))
            .coerceAtMost(safeSourceHeight)
    @Volatile
    private var safeFrameRate = CenterRoiVideoConfig.normalizeFps(frameRate)
    @Volatile
    private var safeBitrate =
        bitrate.coerceIn(CenterRoiVideoConfig.MIN_BITRATE, CenterRoiVideoConfig.MAX_BITRATE)
    @Volatile
    private var qualityMode = initialQualityMode.coerceIn(0, 2)

    @Volatile
    private var requestedRoiLeft = initialRoiLeft

    @Volatile
    private var requestedRoiTop = initialRoiTop

    val roiWidth: Int get() = safeCodecWidth.coerceAtMost(safeSourceWidth)
    val roiHeight: Int get() = safeCodecHeight.coerceAtMost(safeSourceHeight)
    val roiLeft: Int
        get() {
            val maxLeft = (safeSourceWidth - roiWidth).coerceAtLeast(0)
            return if (requestedRoiLeft >= 0) {
                even(requestedRoiLeft).coerceIn(0, maxLeft)
            } else {
                even(maxLeft / 2).coerceAtLeast(0)
            }
        }
    val roiTop: Int
        get() {
            val maxTop = (safeSourceHeight - roiHeight).coerceAtLeast(0)
            return if (requestedRoiTop >= 0) {
                even(requestedRoiTop).coerceIn(0, maxTop)
            } else {
                even(maxTop / 2).coerceAtLeast(0)
            }
        }

    private data class RgbaFrame(
        val pixels: ByteBuffer,
        val ptsNs: Long,
        val queueStartNs: Long,
    )

    private val pool = ArrayDeque<ByteBuffer>(3)
    private var pendingFrame: RgbaFrame? = null
    private var renderPosted = false

    private val renderRunnable = object : Runnable {
        override fun run() {
            renderPosted = false
            renderPendingFrame()
        }
    }

    private val drainRunnable = object : Runnable {
        override fun run() {
            if (closed) return
            drainEncoder(false)
            handler?.postDelayed(this, 1L)
        }
    }

    fun start() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.LOLLIPOP) return
        synchronized(lock) {
            if (thread != null) return
            closed = false
            val ht = HandlerThread("CenterRoiVideo", Process.THREAD_PRIORITY_DISPLAY)
            ht.start()
            thread = ht
            handler = Handler(ht.looper)
            handler?.post { startPipelineImmediately() }
            acceptThread = Thread({ acceptLoop() }, "CenterRoiVideoAccept").apply {
                isDaemon = true
                start()
            }
        }
    }

    fun close() {
        val ht: HandlerThread?
        val accept: Thread?
        synchronized(lock) {
            if (closed) return
            closed = true
            runCatching { client?.close() }
            runCatching { server?.close() }
            synchronized(outputLock) {
                runCatching { output?.close() }
                output = null
                client = null
                isConnected = false
            }
            synchronized(frameLock) {
                pendingFrame = null
                pool.clear()
            }
            handler?.removeCallbacksAndMessages(null)
            handler?.post { releasePipeline() }
            accept = acceptThread
            acceptThread = null
            ht = thread
            handler = null
            thread = null
        }
        runCatching { accept?.join(700) }
        runCatching { ht?.quitSafely() }
        runCatching { ht?.join(900) }
    }

    /**
     * Copy center ROI from the current ImageReader RGBA buffer.  The copy happens before the Image
     * is closed, then the GL/MediaCodec work runs asynchronously on this sender's HandlerThread.
     */
    fun queueRgbaFrame(
        rgbaBuffer: ByteBuffer,
        bufferCapacity: Int,
        srcWidth: Int,
        srcHeight: Int,
        rowStride: Int,
        pixelStride: Int,
        frameProducedNs: Long,
    ): Boolean {
        queuedCalls += 1
        val queueStartNs = System.nanoTime()
        if (closed || pixelStride != 4 || srcWidth != safeSourceWidth || srcHeight != safeSourceHeight) return false
        if (rowStride <= 0 || bufferCapacity <= 0) return false
        // Do not gate the encoder input on the PC socket connection.
        // Hybrid mode must keep feeding MediaCodec so SPS/PPS + IDR are ready as soon as the
        // PC receiver connects.  The controller decides whether JPEG punches the center hole;
        // this function only answers whether the current center RGBA was accepted by the video path.

        val w = roiWidth
        val h = roiHeight
        val left = roiLeft
        val top = roiTop
        val bytesPerRow = w * 4
        val required = bytesPerRow * h
        if (required <= 0) return false
        if (top < 0 || left < 0 || top + h > srcHeight || left + w > srcWidth) return false
        val lastByte = (top + h - 1) * rowStride + left * pixelStride + bytesPerRow
        if (lastByte > bufferCapacity || lastByte > rgbaBuffer.capacity()) return false

        // Fast path: do not compact-copy the ROI into another ByteBuffer.
        // While the ImageReader Image is still open, ask the EGL thread to upload directly from
        // the original RGBA buffer using a byte-offset plus GL_UNPACK_ROW_LENGTH.  This is the GL
        // equivalent of the native JPEG crop path using pointer offset + pitch.
        if (queueRgbaFrameDirectUpload(
                rgbaBuffer = rgbaBuffer,
                bufferCapacity = bufferCapacity,
                rowStride = rowStride,
                pixelStride = pixelStride,
                left = left,
                top = top,
                width = w,
                height = h,
                frameProducedNs = frameProducedNs,
                queueStartNs = queueStartNs,
            )) {
            return true
        }
        directUploadFallbackFrames += 1

        val dst = synchronized(frameLock) {
            var b = if (pool.isNotEmpty()) pool.removeFirst() else null
            if (b == null || b.capacity() < required) {
                b = ByteBuffer.allocateDirect(required).order(ByteOrder.nativeOrder())
            }
            b.clear()
            b.limit(required)
            b
        }

        val copyStartNs = System.nanoTime()
        try {
            val src = rgbaBuffer.duplicate()
            for (yy in 0 until h) {
                val pos = (top + yy) * rowStride + left * pixelStride
                val end = pos + bytesPerRow
                // IMPORTANT: limit must be raised before position.
                // ImageReader rowStride is normally larger than roiWidth*4; after the previous
                // row set a small limit, calling position(nextRowPos) first throws
                // IllegalArgumentException because nextRowPos > oldLimit. That made every
                // center ROI copy fail and left only the punched black JPEG hole visible.
                src.limit(end)
                src.position(pos)
                dst.put(src)
            }
            dst.position(0)
            dst.limit(required)
            compactCopyNs += (System.nanoTime() - copyStartNs).coerceAtLeast(0L)
        } catch (t: Throwable) {
            compactCopyNs += (System.nanoTime() - copyStartNs).coerceAtLeast(0L)
            synchronized(frameLock) { pool.addLast(dst) }
            Log.w(TAG, "copy center ROI frame failed", t)
            return false
        }

        val postNow: Boolean
        synchronized(frameLock) {
            pendingFrame?.let {
                droppedFrames += 1
                pool.addLast(it.pixels)
            }
            pendingFrame = RgbaFrame(dst, frameProducedNs, queueStartNs)
            copyUploadFrames += 1
            lastQueuedVideoFrameWallMs = SystemClock.elapsedRealtime()
            postNow = !renderPosted
            if (postNow) renderPosted = true
        }
        if (postNow) handler?.post(renderRunnable)
        return true
    }

    /**
     * True when the center video path should own the ROI.
     *
     * The previous black-screen guard waited for a non-config H.264 frame to be written before
     * allowing JPEG to punch the center hole. On some devices/codecs the first output frame is
     * delayed until several input-surface swaps have happened, so that guard kept falling back to
     * full-frame JPEG forever and the video path never became visible.
     *
     * For hybrid mode we only require that the PC video socket is connected and the current RGBA
     * center frame has recently entered the video pipeline. If the sender/codec really dies,
     * queueRgbaFrame() returns false or isConnected becomes false, and JPEG falls back to full frame.
     */
    fun hasUsableOverlayFrame(nowMs: Long = SystemClock.elapsedRealtime()): Boolean {
        if (!isConnected) return false
        val lastQueued = lastQueuedVideoFrameWallMs
        val lastRendered = lastRenderedVideoFrameWallMs
        val lastSent = lastSentVideoFrameWallMs
        return (lastQueued > 0L && nowMs - lastQueued <= 500L) ||
                (lastRendered > 0L && nowMs - lastRendered <= 1000L) ||
                (lastSent > 0L && nowMs - lastSent <= 1000L)
    }

    fun updateRuntimeConfig(
        codecWidth: Int,
        codecHeight: Int,
        bitrate: Int,
        frameRate: Int,
        quality: Int,
        roiLeft: Int = requestedRoiLeft,
        roiTop: Int = requestedRoiTop,
    ) {
        val newBitrate = bitrate.coerceIn(
            CenterRoiVideoConfig.MIN_BITRATE,
            CenterRoiVideoConfig.MAX_BITRATE
        )
        val newFrameRate = CenterRoiVideoConfig.normalizeFps(frameRate)
        val newQuality = CenterRoiVideoConfig.normalizeQualityMode(quality)
        val newCodecWidth = even(
            codecWidth.coerceIn(
                CenterRoiVideoConfig.MIN_CODEC_WIDTH,
                CenterRoiVideoConfig.MAX_CODEC_WIDTH
            )
        ).coerceAtMost(safeSourceWidth)

        val newCodecHeight = even(
            codecHeight.coerceIn(
                CenterRoiVideoConfig.MIN_CODEC_HEIGHT,
                CenterRoiVideoConfig.MAX_CODEC_HEIGHT
            )
        ).coerceAtMost(safeSourceHeight)

        val oldBitrate = safeBitrate
        val oldFrameRate = safeFrameRate
        val oldQuality = qualityMode
        val oldWidth = safeCodecWidth
        val oldHeight = safeCodecHeight

        val sizeChanged = newCodecWidth != oldWidth || newCodecHeight != oldHeight
        val fpsChanged = newFrameRate != oldFrameRate
        val qualityChanged = newQuality != oldQuality

        safeBitrate = newBitrate
        safeFrameRate = newFrameRate
        qualityMode = newQuality
        safeCodecWidth = newCodecWidth
        safeCodecHeight = newCodecHeight
        requestedRoiLeft = roiLeft
        requestedRoiTop = roiTop

        // 只有码率变化可以安全走 MediaCodec.setParameters()
        val onlyBitrateChanged =
            !sizeChanged &&
                    !fpsChanged &&
                    !qualityChanged &&
                    newBitrate != oldBitrate

        if (onlyBitrateChanged) {
            val enc = codec
            if (enc != null) {
                runCatching {
                    enc.setParameters(Bundle().apply {
                        putInt(MediaCodec.PARAMETER_KEY_VIDEO_BITRATE, safeBitrate)
                    })
                }.onFailure { t ->
                    if (!closed) Log.w(TAG, "update center ROI bitrate failed", t)
                }
            }
            requestSyncFrame()
            Log.w(
                TAG,
                "runtime video control applied LIVE bitrate=$safeBitrate fps=$safeFrameRate " +
                        "quality=$qualityMode codec=${safeCodecWidth}x$safeCodecHeight"
            )
            return
        }

        // quality / fps / size 都是 configure-time 参数，必须重建 MediaCodec
        val h = handler
        if (h != null) {
            h.post {
                if (closed) return@post
                try {
                    Log.w(
                        TAG,
                        "runtime video control REBUILD " +
                                "bitrate $oldBitrate->$safeBitrate " +
                                "fps $oldFrameRate->$safeFrameRate " +
                                "quality $oldQuality->$qualityMode " +
                                "codec ${oldWidth}x${oldHeight}->${safeCodecWidth}x${safeCodecHeight}"
                    )

                    releasePipeline()
                    startPipeline()
                    requestSyncFrame()
                } catch (t: Throwable) {
                    Log.e(TAG, "rebuild center ROI video pipeline failed", t)
                    closeCurrentClient()
                }
            }
        } else {
            Log.w(
                TAG,
                "runtime video control saved before handler ready bitrate=$safeBitrate " +
                        "fps=$safeFrameRate quality=$qualityMode codec=${safeCodecWidth}x$safeCodecHeight"
            )
        }
    }
    private fun startPipelineImmediately() {
        runCatching { Process.setThreadPriority(Process.THREAD_PRIORITY_DISPLAY) }
        try {
            server = LocalServerSocket(socketName)
            startPipeline()
            Log.w(
                TAG,
                "single-VD center ROI video started source=${safeSourceWidth}x$safeSourceHeight " +
                        "roi=$roiLeft,$roiTop ${roiWidth}x${roiHeight} codec=${safeCodecWidth}x$safeCodecHeight " +
                        "bitrate=$safeBitrate fps=$safeFrameRate quality=$qualityMode"
            )
            handler?.post(drainRunnable)
        } catch (t: Throwable) {
            if (!closed) {
                Log.e(TAG, "start single-VD center ROI video failed", t)
                closed = true
                releasePipeline()
                runCatching { server?.close() }
            }
        }
    }

    private fun acceptLoop() {
        runCatching { Process.setThreadPriority(Process.THREAD_PRIORITY_DISPLAY) }
        while (!closed) {
            var accepted: LocalSocket? = null
            try {
                while (!closed && server == null) Thread.sleep(5)
                accepted = server?.accept() ?: break
                if (closed) break

                accepted.sendBufferSize = 2 * 1024 * 1024
                accepted.receiveBufferSize = 16 * 1024
                val newOutput = BufferedOutputStream(accepted.outputStream, 512 * 1024)

                synchronized(outputLock) {
                    runCatching { output?.close() }
                    runCatching { client?.close() }
                    client = accepted
                    output = newOutput
                    isConnected = true
                }

                latestCodecConfig?.let { config ->
                    runCatching { writePacket(newOutput, FLAG_CODEC_CONFIG, 0L, config) }
                }
                requestSyncFrame()
                Log.w(TAG, "center ROI video client connected")

                val controlThread = Thread({ readVideoControlLoop(accepted) }, "CenterRoiVideoControl").apply {
                    isDaemon = true
                    start()
                }
                @Suppress("UNUSED_VARIABLE") val unused = controlThread

                while (!closed && isConnected) Thread.sleep(100)
            } catch (t: Throwable) {
                if (!closed) {
                    Log.e(TAG, "center ROI video accept loop failed", t)
                    Thread.sleep(250)
                }
            } finally {
                synchronized(outputLock) {
                    if (client === accepted) {
                        runCatching { output?.close() }
                        output = null
                        client = null
                        isConnected = false
                    }
                }
                runCatching { accepted?.close() }
            }
        }
    }

    private fun handleVideoControlLine(line: String) {
        val trimmed = line.trim()
        Log.w(TAG, "video control recv raw='$line' trimmed='$trimmed'")
        if (trimmed.isEmpty()) return

        val parts = trimmed.split(' ', '\t').filter { it.isNotBlank() }
        Log.w(TAG, "video control parts=$parts")

        when (parts.firstOrNull()) {
            "HLVIDPRESET" -> {
                val bitrateMbps = parts.getOrNull(2)?.toIntOrNull() ?: return requestSyncFrame()
                val fps = parts.getOrNull(3)?.toIntOrNull() ?: safeFrameRate
                val quality = parts.getOrNull(4)?.toIntOrNull() ?: qualityMode
                updateRuntimeConfig(
                    codecWidth = safeCodecWidth,
                    codecHeight = safeCodecHeight,
                    bitrate = CenterRoiVideoConfig.normalizeBitrateMbps(bitrateMbps) * 1_000_000,
                    frameRate = fps,
                    quality = quality,
                    roiLeft = requestedRoiLeft,
                    roiTop = requestedRoiTop,
                )
            }

            "HLVIDEO" -> {
                val bitrateMbps = parts.getOrNull(3)?.toIntOrNull() ?: return requestSyncFrame()
                val fps = parts.getOrNull(4)?.toIntOrNull() ?: safeFrameRate
                val quality = parts.getOrNull(5)?.toIntOrNull() ?: qualityMode
                updateRuntimeConfig(
                    codecWidth = safeCodecWidth,
                    codecHeight = safeCodecHeight,
                    bitrate = CenterRoiVideoConfig.normalizeBitrateMbps(bitrateMbps) * 1_000_000,
                    frameRate = fps,
                    quality = quality,
                    roiLeft = requestedRoiLeft,
                    roiTop = requestedRoiTop,
                )
            }

            else -> {
                Log.w(TAG, "unknown video control command parts=$parts")
            }
        }
    }

    private fun readVideoControlLoop(socket: LocalSocket) {
        runCatching { Process.setThreadPriority(Process.THREAD_PRIORITY_DISPLAY) }
        val sb = StringBuilder(128)
        val buf = ByteArray(256)
        try {
            val input = socket.inputStream
            while (!closed) {
                val n = input.read(buf)
                if (n <= 0) break
                for (i in 0 until n) {
                    val ch = buf[i].toInt().toChar()
                    if (ch == '\n' || ch == '\r') {
                        if (sb.isNotEmpty()) {
                            val line = sb.toString()
                            sb.setLength(0)
                            handleVideoControlLine(line)
                        }
                    } else if (sb.length < 512) {
                        sb.append(ch)
                    } else {
                        sb.setLength(0)
                    }
                }
            }
        } catch (t: Throwable) {
            if (!closed) Log.w(TAG, "video control loop ended", t)
        }
    }
    private fun isSoftwareEncoderName(name: String): Boolean {
        return AvcEncoderInspector.isSoftwareEncoderName(name)
    }

    private fun selectHardwareAvcEncoderName(): String? {
        val report = AvcEncoderInspector.inspect()
        Log.w(
            TAG,
            "AVC encoder candidates: ${report.toLogString()} selected=${report.selectedName} " +
                    "priority=device-aware qcom=c2.qti-first mtk=c2.mtk-first unknown=c2-hw-first"
        )
        return report.selectedName
    }

    private data class EncoderFormatProfile(
        val label: String,
        val fps: Int,
        val bitrate: Int,
        val bitrateMode: Int,
        val iframeInterval: Int,
        val profile: Int?,
        val level: Int?,
        val useVendorLowLatencyKeys: Boolean,
        val useOperatingRate: Boolean,
        val useColorAspects: Boolean,
    )

    private fun isMtkEncoderName(name: String): Boolean {
        val n = name.lowercase()
        return n.contains("mtk") || n.contains("mediatek")
    }

    private fun isQcomEncoderName(name: String): Boolean {
        val n = name.lowercase()
        return n.contains("qcom") || n.contains("qti") || n.contains("qualcomm")
    }

    private fun encoderFormatProfiles(codecName: String, preferredBitrateMode: Int): List<EncoderFormatProfile> {
        val isMtk = isMtkEncoderName(codecName)
        val isQcom = isQcomEncoderName(codecName)
        val mtkFps = safeFrameRate.coerceAtMost(120).coerceAtLeast(30)
        val mtkBitrate = safeBitrate.coerceAtMost(80_000_000).coerceAtLeast(CenterRoiVideoConfig.MIN_BITRATE)
        val qcomIframeInterval = if (qualityMode >= 2) 1 else 0

        if (isMtk) {
            // MTK/C2 对 High@5.1、全 I 帧、CBR/operating-rate 的组合兼容性不如 QTI。
            // 优先走“自动 profile + GOP=1 + VBR”，这是大多数天玑设备更容易稳定出码流的组合。
            return listOf(
                EncoderFormatProfile(
                    label = "mtk-auto-gop-vbr-120cap",
                    fps = mtkFps,
                    bitrate = mtkBitrate,
                    bitrateMode = MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_VBR,
                    iframeInterval = 1,
                    profile = null,
                    level = null,
                    useVendorLowLatencyKeys = false,
                    useOperatingRate = false,
                    useColorAspects = false,
                ),
                EncoderFormatProfile(
                    label = "mtk-main-gop-vbr-120cap",
                    fps = mtkFps,
                    bitrate = mtkBitrate,
                    bitrateMode = MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_VBR,
                    iframeInterval = 1,
                    profile = MediaCodecInfo.CodecProfileLevel.AVCProfileMain,
                    level = null,
                    useVendorLowLatencyKeys = false,
                    useOperatingRate = false,
                    useColorAspects = false,
                ),
                EncoderFormatProfile(
                    label = "mtk-baseline-gop-vbr-60cap",
                    fps = mtkFps.coerceAtMost(60),
                    bitrate = mtkBitrate.coerceAtMost(50_000_000),
                    bitrateMode = MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_VBR,
                    iframeInterval = 1,
                    profile = MediaCodecInfo.CodecProfileLevel.AVCProfileBaseline,
                    level = null,
                    useVendorLowLatencyKeys = false,
                    useOperatingRate = false,
                    useColorAspects = false,
                ),
                EncoderFormatProfile(
                    label = "mtk-auto-allidr-vbr-60cap",
                    fps = mtkFps.coerceAtMost(60),
                    bitrate = mtkBitrate.coerceAtMost(50_000_000),
                    bitrateMode = MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_VBR,
                    iframeInterval = 0,
                    profile = null,
                    level = null,
                    useVendorLowLatencyKeys = false,
                    useOperatingRate = false,
                    useColorAspects = false,
                ),
            )
        }

        return listOf(
            EncoderFormatProfile(
                label = if (isQcom) "qti-high-lowlatency" else "generic-high-lowlatency",
                fps = safeFrameRate,
                bitrate = safeBitrate,
                bitrateMode = preferredBitrateMode,
                iframeInterval = qcomIframeInterval,
                profile = MediaCodecInfo.CodecProfileLevel.AVCProfileHigh,
                level = MediaCodecInfo.CodecProfileLevel.AVCLevel51,
                useVendorLowLatencyKeys = true,
                useOperatingRate = true,
                useColorAspects = true,
            ),
            EncoderFormatProfile(
                label = "generic-auto-gop-vbr",
                fps = safeFrameRate.coerceAtMost(120).coerceAtLeast(30),
                bitrate = safeBitrate,
                bitrateMode = MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_VBR,
                iframeInterval = 1,
                profile = null,
                level = null,
                useVendorLowLatencyKeys = false,
                useOperatingRate = false,
                useColorAspects = false,
            ),
            EncoderFormatProfile(
                label = "generic-baseline-gop-vbr-60cap",
                fps = safeFrameRate.coerceAtMost(60).coerceAtLeast(30),
                bitrate = safeBitrate.coerceAtMost(50_000_000),
                bitrateMode = MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_VBR,
                iframeInterval = 1,
                profile = MediaCodecInfo.CodecProfileLevel.AVCProfileBaseline,
                level = null,
                useVendorLowLatencyKeys = false,
                useOperatingRate = false,
                useColorAspects = false,
            ),
        )
    }

    private fun buildEncoderFormat(p: EncoderFormatProfile): MediaFormat {
        return MediaFormat.createVideoFormat(
            MediaFormat.MIMETYPE_VIDEO_AVC,
            safeCodecWidth,
            safeCodecHeight,
        ).apply {
            setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface)
            setInteger(MediaFormat.KEY_BIT_RATE, p.bitrate)
            setInteger(MediaFormat.KEY_FRAME_RATE, p.fps)
            setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, p.iframeInterval)
            setInteger(MediaFormat.KEY_BITRATE_MODE, p.bitrateMode)

            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
                setIntegerSafe(MediaFormat.KEY_PRIORITY, 0)
                if (p.useOperatingRate) setIntegerSafe(MediaFormat.KEY_OPERATING_RATE, p.fps)
            }

            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                setIntegerSafe(MediaFormat.KEY_LATENCY, 0)
            }

            setIntegerSafe("max-bframes", 0)
            setIntegerSafe("max-b-frames", 0)
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                runCatching { setInteger(MediaFormat.KEY_MAX_B_FRAMES, 0) }
                    .onFailure { Log.w(TAG, "MediaFormat set KEY_MAX_B_FRAMES failed", it) }
            }

            if (p.useVendorLowLatencyKeys) {
                setIntegerSafe("vendor.qti-ext-enc-low-latency.enable", 1)
                setIntegerSafe("vendor.qti-ext-enc-venc-low-latency.enable", 1)
                setIntegerSafe("vendor.qti-ext-enc-preprocess-rotate.enable", 0)
            }
            setIntegerSafe("low-latency", 1)
            setIntegerSafe("latency", 0)
            setIntegerSafe("priority", 0)
            setIntegerSafe("max-bitrate", p.bitrate)
            setIntegerSafe(MediaFormat.KEY_CAPTURE_RATE, p.fps)

            if (p.useColorAspects && Build.VERSION.SDK_INT >= Build.VERSION_CODES.N) {
                setIntegerSafe(MediaFormat.KEY_COLOR_STANDARD, MediaFormat.COLOR_STANDARD_BT709)
                setIntegerSafe(MediaFormat.KEY_COLOR_TRANSFER, MediaFormat.COLOR_TRANSFER_SDR_VIDEO)
                setIntegerSafe(MediaFormat.KEY_COLOR_RANGE, MediaFormat.COLOR_RANGE_LIMITED)
            }

            if (p.profile != null) setIntegerSafe(MediaFormat.KEY_PROFILE, p.profile)
            if (p.level != null) setIntegerSafe(MediaFormat.KEY_LEVEL, p.level)

            // 每个 IDR 前带 SPS/PPS；部分 MTK 不认这个私有 key，但忽略即可。
            setIntegerSafe("prepend-sps-pps-to-idr-frames", 1)
        }
    }

    private fun startPipeline() {
        val encoderName = selectHardwareAvcEncoderName()
            ?: throw IllegalStateException("No hardware AVC encoder with COLOR_FormatSurface found")

        val probe = MediaCodec.createByCodecName(encoderName)
        val codecName = probe.name
        val isLikelySoftware = isSoftwareEncoderName(codecName)
        val preferredBitrateMode = runCatching {
            val caps = probe.codecInfo
                .getCapabilitiesForType(MediaFormat.MIMETYPE_VIDEO_AVC)
                .encoderCapabilities

            when {
                !isMtkEncoderName(codecName) && qualityMode >= 1 &&
                        caps.isBitrateModeSupported(MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CBR) ->
                    MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CBR

                caps.isBitrateModeSupported(MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_VBR) ->
                    MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_VBR

                caps.isBitrateModeSupported(MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CBR) ->
                    MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CBR

                else -> MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_VBR
            }
        }.getOrDefault(MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_VBR)
        runCatching { probe.release() }

        Log.w(
            TAG,
            "当前编码类型：AVC encoder selected: name=$codecName softwareLikely=$isLikelySoftware forcedHardware=true " +
                    "mtk=${isMtkEncoderName(codecName)} qcom=${isQcomEncoderName(codecName)}"
        )

        var configuredCodecTmp: MediaCodec? = null
        var configuredFormat: MediaFormat? = null
        var configuredProfile: EncoderFormatProfile? = null
        var lastError: Throwable? = null

        for (profile in encoderFormatProfiles(codecName, preferredBitrateMode)) {
            val candidate = MediaCodec.createByCodecName(encoderName)
            val format = buildEncoderFormat(profile)
            try {
                Log.w(
                    TAG,
                    "encoder configure attempt=${profile.label} format=$format " +
                            "qualityMode=$qualityMode bitrate=${profile.bitrate} fps=${profile.fps} " +
                            "bitrateMode=${profile.bitrateMode} iframeInterval=${profile.iframeInterval}"
                )
                candidate.configure(format, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
                configuredCodecTmp = candidate
                configuredFormat = format
                configuredProfile = profile
                break
            } catch (t: Throwable) {
                lastError = t
                Log.e(TAG, "encoder configure failed attempt=${profile.label}", t)
                runCatching { candidate.release() }
            }
        }

        val enc = configuredCodecTmp ?: throw IllegalStateException(
            "AVC encoder configure failed name=$codecName last=${lastError?.javaClass?.simpleName}: ${lastError?.message}",
            lastError
        )

        Log.w(
            TAG,
            "encoder configured ok attempt=${configuredProfile?.label} actualFormat=$configuredFormat"
        )

        val encSurface = enc.createInputSurface()
        inputSurface = encSurface

        val eglLocal = EncoderEgl(encSurface)
        egl = eglLocal

        val rendererLocal = RgbaTextureRenderer(safeCodecWidth, safeCodecHeight)
        renderer = rendererLocal
        rendererLocal.init()

        enc.start()
        codec = enc

        runCatching {
            Log.w(TAG, "encoder started outputFormat=${enc.outputFormat}")
        }.onFailure {
            Log.w(TAG, "encoder outputFormat not available immediately after start", it)
        }

        requestSyncFrame()
    }

    private fun MediaFormat.setIntegerSafe(key: String, value: Int) {
        runCatching {
            setInteger(key, value)
        }.onFailure {
            Log.w(TAG, "MediaFormat set $key=$value failed", it)
        }
    }

    private fun renderPendingFrame() {
        if (closed) return
        val frame = synchronized(frameLock) { pendingFrame.also { pendingFrame = null } } ?: return
        val eglLocal = egl
        val rendererLocal = renderer
        if (eglLocal == null || rendererLocal == null) {
            recycle(frame.pixels)
            return
        }
        try {
            val taskStartNs = System.nanoTime()
            eglLocal.makeCurrent()
            val drawStartNs = System.nanoTime()
            rendererLocal.draw(frame.pixels)
            val drawEndNs = System.nanoTime()
            val ptsNs = nextPresentationTimeNs(frame.ptsNs)
            eglLocal.setPresentationTime(ptsNs)
            val swapStartNs = System.nanoTime()
            eglLocal.swapBuffers()
            val swapEndNs = System.nanoTime()
            rememberTiming(
                ptsNs,
                H264Timing(
                    frameProducedNs = frame.ptsNs,
                    queueStartNs = frame.queueStartNs,
                    drawStartNs = drawStartNs,
                    uploadDoneNs = drawEndNs,
                    swapStartNs = swapStartNs,
                    swapDoneNs = swapEndNs,
                )
            )
            lastRenderedVideoFrameWallMs = SystemClock.elapsedRealtime()
            ++renderedFrames
            val drainStartNs = System.nanoTime()
            drainEncoder(false)
            val drainEndNs = System.nanoTime()
            renderDrawNs += (drawEndNs - drawStartNs).coerceAtLeast(0L)
            renderSwapNs += (swapEndNs - swapStartNs).coerceAtLeast(0L)
            renderDrainNs += (drainEndNs - drainStartNs).coerceAtLeast(0L)
            renderTaskNs += (drainEndNs - taskStartNs).coerceAtLeast(0L)
        } catch (t: Throwable) {
            Log.e(TAG, "render center ROI frame failed", t)
            closeCurrentClient()
        } finally {
            recycle(frame.pixels)
        }

        val shouldPostAgain = synchronized(frameLock) {
            if (pendingFrame != null && !renderPosted) {
                renderPosted = true
                true
            } else {
                false
            }
        }
        if (shouldPostAgain) handler?.post(renderRunnable)
    }

    private fun queueRgbaFrameDirectUpload(
        rgbaBuffer: ByteBuffer,
        bufferCapacity: Int,
        rowStride: Int,
        pixelStride: Int,
        left: Int,
        top: Int,
        width: Int,
        height: Int,
        frameProducedNs: Long,
        queueStartNs: Long,
    ): Boolean {
        if (closed || pixelStride != 4 || rowStride <= 0 || bufferCapacity <= 0) return false
        val h = handler ?: return false
        val eglLocal = egl ?: return false
        val rendererLocal = renderer ?: return false
        if (width != safeCodecWidth || height != safeCodecHeight) return false
        if (rowStride % 4 != 0) return false
        val offset = top * rowStride + left * pixelStride
        val lastByte = (top + height - 1) * rowStride + left * pixelStride + width * pixelStride
        if (offset < 0 || lastByte > bufferCapacity || lastByte > rgbaBuffer.capacity()) return false

        val done = CountDownLatch(1)
        var ok = false
        val frameBuffer = rgbaBuffer.duplicate()
        val task = Runnable {
            try {
                if (closed) return@Runnable
                val taskStartNs = System.nanoTime()
                eglLocal.makeCurrent()
                val drawStartNs = System.nanoTime()
                val uploaded = rendererLocal.drawFromSourceRegion(
                    source = frameBuffer,
                    rowStride = rowStride,
                    left = left,
                    top = top,
                    width = width,
                    height = height,
                )
                val drawEndNs = System.nanoTime()
                if (!uploaded) return@Runnable
                val ptsNs = nextPresentationTimeNs(frameProducedNs)
                eglLocal.setPresentationTime(ptsNs)
                val swapStartNs = System.nanoTime()
                eglLocal.swapBuffers()
                val swapEndNs = System.nanoTime()
                rememberTiming(
                    ptsNs,
                    H264Timing(
                        frameProducedNs = frameProducedNs,
                        queueStartNs = queueStartNs,
                        drawStartNs = drawStartNs,
                        uploadDoneNs = drawEndNs,
                        swapStartNs = swapStartNs,
                        swapDoneNs = swapEndNs,
                    )
                )
                lastQueuedVideoFrameWallMs = SystemClock.elapsedRealtime()
                lastRenderedVideoFrameWallMs = lastQueuedVideoFrameWallMs
                ++renderedFrames
                ++directUploadFrames
                val drainStartNs = System.nanoTime()
                drainEncoder(false)
                val drainEndNs = System.nanoTime()
                directDrawNs += (drawEndNs - drawStartNs).coerceAtLeast(0L)
                directSwapNs += (swapEndNs - swapStartNs).coerceAtLeast(0L)
                directDrainNs += (drainEndNs - drainStartNs).coerceAtLeast(0L)
                directTaskNs += (drainEndNs - taskStartNs).coerceAtLeast(0L)
                ok = true
            } catch (t: Throwable) {
                if (!closed) Log.w(TAG, "direct center ROI GL upload failed; fallback to compact copy", t)
            } finally {
                done.countDown()
            }
        }
        if (Thread.currentThread() === thread) {
            task.run()
        } else {
            if (!h.post(task)) return false
            val waitStartNs = System.nanoTime()
            done.await()
            directCallerWaitNs += (System.nanoTime() - waitStartNs).coerceAtLeast(0L)
        }
        return ok
    }

    private fun recycle(buffer: ByteBuffer) {
        synchronized(frameLock) {
            if (pool.size < 3) pool.addLast(buffer)
        }
    }

    private fun nextPresentationTimeNs(sourceNs: Long): Long {
        val now = System.nanoTime()
        val candidate = when {
            sourceNs > 0L -> sourceNs
            else -> now
        }
        val next = if (candidate > lastPtsNs) candidate else lastPtsNs + 1_000_000L
        lastPtsNs = next
        return next
    }

    private fun drainEncoder(endOfStream: Boolean) {
        val enc = codec ?: return
        if (endOfStream) runCatching { enc.signalEndOfInputStream() }

        val info = MediaCodec.BufferInfo()
        while (!closed) {
            val dequeueStartNs = System.nanoTime()
            val index = enc.dequeueOutputBuffer(info, 0)
            codecDequeueNs += (System.nanoTime() - dequeueStartNs).coerceAtLeast(0L)
            codecDequeueCalls += 1
            if (index == MediaCodec.INFO_TRY_AGAIN_LATER) break
            if (index == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                val actualFormat = enc.outputFormat
                handleOutputFormatChanged(actualFormat)
                Log.w(TAG, "encoder actual outputFormat=$actualFormat")
                continue
            }
            if (index < 0) continue

            val buffer = enc.getOutputBuffer(index)
            if (buffer != null && info.size > 0) {
                codecOutputBuffers += 1
                buffer.position(info.offset)
                buffer.limit(info.offset + info.size)
                val rawPayload = ByteArray(info.size)
                buffer.get(rawPayload)
                val payload = toAnnexB(rawPayload)

                var flags = 0
                if ((info.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG) != 0) flags = flags or FLAG_CODEC_CONFIG
                if ((info.flags and MediaCodec.BUFFER_FLAG_KEY_FRAME) != 0) flags = flags or FLAG_KEY_FRAME
                if ((flags and FLAG_CODEC_CONFIG) != 0) latestCodecConfig = payload
                if ((flags and FLAG_CODEC_CONFIG) == 0) ++encodedFrames

                val out = synchronized(outputLock) { output }
                if (out != null) {
                    try {
                        val outputNs = System.nanoTime()
                        val timing = if ((flags and FLAG_CODEC_CONFIG) == 0) {
                            timingByPtsUs.remove(info.presentationTimeUs)
                        } else {
                            null
                        }

                        timing?.let { t ->
                            fun ms(ns: Long): Double = ns.coerceAtLeast(0L) / 1_000_000.0

                            val queueMs = ms(t.queueStartNs - t.frameProducedNs)
                            val waitGlThreadMs = ms(t.drawStartNs - t.queueStartNs)
                            val drawMs = ms(t.uploadDoneNs - t.drawStartNs)
                            val swapMs = ms(t.swapDoneNs - t.swapStartNs)
                            val encodeInternalMs = ms(outputNs - t.swapDoneNs)
                            val totalToOutputMs = ms(outputNs - t.frameProducedNs)

                            val nowMs = SystemClock.elapsedRealtime()
                            if (nowMs - lastSegmentLogMs >= 500L) {
                                lastSegmentLogMs = nowMs
                                Log.w(
                                    TAG,
                                    "H264SEG " +
                                            "queue=${"%.2f".format(java.util.Locale.US, queueMs)}ms " +
                                            "waitGL=${"%.2f".format(java.util.Locale.US, waitGlThreadMs)}ms " +
                                            "draw=${"%.2f".format(java.util.Locale.US, drawMs)}ms " +
                                            "swap=${"%.2f".format(java.util.Locale.US, swapMs)}ms " +
                                            "encodeInternal=${"%.2f".format(java.util.Locale.US, encodeInternalMs)}ms " +
                                            "totalToOutput=${"%.2f".format(java.util.Locale.US, totalToOutputMs)}ms " +
                                            "size=${payload.size / 1024}KB " +
                                            "ptsUs=${info.presentationTimeUs}"
                                )
                            }
                        }

                        val writeStartNs = System.nanoTime()
                        writePacket(
                            out = out,
                            flags = flags,
                            ptsUs = info.presentationTimeUs,
                            payload = payload,
                            frameProducedNs = timing?.frameProducedNs ?: 0L,
                            queueStartNs = timing?.queueStartNs ?: 0L,
                            uploadDoneNs = timing?.uploadDoneNs ?: 0L,
                            swapDoneNs = timing?.swapDoneNs ?: 0L,
                            outputNs = outputNs,
                            writeStartNs = writeStartNs,
                        )
                        val writeEndNs = System.nanoTime()
                        socketWriteNs += (writeEndNs - writeStartNs).coerceAtLeast(0L)
                        socketWritePackets += 1
                        sentBytes += payload.size.toLong()
                        sentPackets += 1
                        if ((flags and FLAG_CODEC_CONFIG) == 0) {
                            sentVideoFrames += 1
                            lastSentVideoFrameWallMs = SystemClock.elapsedRealtime()
                        }
                    } catch (t: Throwable) {
                        if (!closed) Log.e(TAG, "write center ROI video packet failed; drop client", t)
                        closeCurrentClient()
                    }
                }
            }
            enc.releaseOutputBuffer(index, false)
            if ((info.flags and MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0) break
        }
        logStatsMaybe()
    }

    private fun handleOutputFormatChanged(format: MediaFormat) {
        val parts = ArrayList<ByteArray>(2)
        format.getByteBuffer("csd-0")?.let { parts += toAnnexB(copyRemaining(it)) }
        format.getByteBuffer("csd-1")?.let { parts += toAnnexB(copyRemaining(it)) }
        if (parts.isEmpty()) return
        val total = parts.sumOf { it.size }
        val config = ByteArray(total)
        var offset = 0
        for (p in parts) {
            System.arraycopy(p, 0, config, offset, p.size)
            offset += p.size
        }
        latestCodecConfig = config
        synchronized(outputLock) { output }?.let { out ->
            runCatching {
                writePacket(out, FLAG_CODEC_CONFIG, 0L, config)
                sentBytes += config.size.toLong()
                sentPackets += 1
            }.onFailure { t ->
                if (!closed) Log.e(TAG, "write center ROI codec config failed; drop client", t)
                closeCurrentClient()
            }
        }
    }

    private fun requestSyncFrame() {
        val enc = codec ?: return
        runCatching {
            enc.setParameters(Bundle().apply {
                putInt(MediaCodec.PARAMETER_KEY_REQUEST_SYNC_FRAME, 0)
            })
        }
    }

    private fun copyRemaining(buffer: ByteBuffer): ByteArray {
        val dup = buffer.duplicate()
        val out = ByteArray(dup.remaining())
        dup.get(out)
        return out
    }

    private fun hasAnnexBStartCode(data: ByteArray): Boolean {
        if (data.size >= 4 && data[0] == 0.toByte() && data[1] == 0.toByte() && data[2] == 0.toByte() && data[3] == 1.toByte()) return true
        return data.size >= 3 && data[0] == 0.toByte() && data[1] == 0.toByte() && data[2] == 1.toByte()
    }

    private fun toAnnexB(data: ByteArray): ByteArray {
        if (data.isEmpty() || hasAnnexBStartCode(data)) return data
        val out = ByteArray(data.size)
        var src = 0
        var dst = 0
        while (src + 4 <= data.size) {
            val n = ((data[src].toInt() and 0xff) shl 24) or
                    ((data[src + 1].toInt() and 0xff) shl 16) or
                    ((data[src + 2].toInt() and 0xff) shl 8) or
                    (data[src + 3].toInt() and 0xff)
            if (n <= 0 || src + 4 + n > data.size || dst + 4 + n > out.size) {
                return data
            }
            out[dst++] = 0
            out[dst++] = 0
            out[dst++] = 0
            out[dst++] = 1
            System.arraycopy(data, src + 4, out, dst, n)
            dst += n
            src += 4 + n
        }
        return if (src == data.size && dst == out.size) out else data
    }

    private fun writePacket(
        out: BufferedOutputStream,
        flags: Int,
        ptsUs: Long,
        payload: ByteArray,
        frameProducedNs: Long = 0L,
        queueStartNs: Long = 0L,
        uploadDoneNs: Long = 0L,
        swapDoneNs: Long = 0L,
        outputNs: Long = 0L,
        writeStartNs: Long = 0L,
    ) {
        val header = ByteBuffer.allocate(112).order(ByteOrder.BIG_ENDIAN)
        header.putInt(PACKET_MAGIC)
        header.putInt(PACKET_VERSION)
        header.putInt(flags)
        header.putInt(safeSourceWidth)
        header.putInt(safeSourceHeight)
        header.putInt(roiLeft)
        header.putInt(roiTop)
        header.putInt(roiWidth)
        header.putInt(roiHeight)
        header.putInt(safeCodecWidth)
        header.putInt(safeCodecHeight)
        header.putInt(payload.size)
        header.putLong(ptsUs)
        header.putLong(frameProducedNs)
        header.putLong(queueStartNs)
        header.putLong(uploadDoneNs)
        header.putLong(swapDoneNs)
        header.putLong(outputNs)
        header.putLong(writeStartNs)
        // writeEndNs is not known until after the payload write completes; use writeStartNs
        // here so PC can still separate encoder pipeline from pre-write waiting. Exact socket
        // write cost remains available in Android logcat's socketAvgMs(write=...).
        header.putLong(writeStartNs)
        out.write(header.array())
        out.write(payload)
        out.flush()
    }

    private fun logStatsMaybe() {
        val now = android.os.SystemClock.elapsedRealtime()
        if (lastLogMs == 0L) {
            lastLogMs = now
            return
        }
        val elapsed = now - lastLogMs
        if (elapsed < 1000L) return
        val mbps = sentBytes * 8.0 / elapsed.toDouble() / 1000.0
        val avgKb = if (sentPackets > 0) sentBytes / 1024.0 / sentPackets.toDouble() else 0.0
        fun avgMs(ns: Long, n: Int): Double = if (n > 0) ns / 1_000_000.0 / n.toDouble() else 0.0
        val directCount = directUploadFrames.coerceAtLeast(0)
        val copyCount = copyUploadFrames.coerceAtLeast(0)
        val codecCallCount = codecDequeueCalls.coerceAtLeast(0)
        val writeCount = socketWritePackets.coerceAtLeast(0)
        Log.w(
            TAG,
            "center ROI video renderFps=${renderedFrames * 1000.0 / elapsed} " +
                    "encodedFps=${encodedFrames * 1000.0 / elapsed} sentFps=${sentVideoFrames * 1000.0 / elapsed} " +
                    "dropFps=${droppedFrames * 1000.0 / elapsed} " +
                    "avgKB=${"%.1f".format(avgKb)} mbps=${"%.2f".format(mbps)} bytes=$sentBytes packets=$sentPackets " +
                    "queueCalls=$queuedCalls upload=direct:$directUploadFrames copy:$copyUploadFrames fallback:$directUploadFallbackFrames " +
                    "roi=$roiLeft,$roiTop ${roiWidth}x${roiHeight} codec=${safeCodecWidth}x$safeCodecHeight"
        )
        Log.w(
            TAG,
            "center ROI video timing " +
                    "directAvgMs(waitCaller=${"%.2f".format(avgMs(directCallerWaitNs, directCount))} " +
                    "draw=${"%.2f".format(avgMs(directDrawNs, directCount))} " +
                    "swap=${"%.2f".format(avgMs(directSwapNs, directCount))} " +
                    "drain=${"%.2f".format(avgMs(directDrainNs, directCount))} " +
                    "task=${"%.2f".format(avgMs(directTaskNs, directCount))}) " +
                    "copyAvgMs(copy=${"%.2f".format(avgMs(compactCopyNs, copyCount))} " +
                    "draw=${"%.2f".format(avgMs(renderDrawNs, copyCount))} " +
                    "swap=${"%.2f".format(avgMs(renderSwapNs, copyCount))} " +
                    "drain=${"%.2f".format(avgMs(renderDrainNs, copyCount))} " +
                    "task=${"%.2f".format(avgMs(renderTaskNs, copyCount))}) " +
                    "codecAvgMs(dequeue=${"%.3f".format(avgMs(codecDequeueNs, codecCallCount))} calls=$codecDequeueCalls outputs=$codecOutputBuffers) " +
                    "socketAvgMs(write=${"%.2f".format(avgMs(socketWriteNs, writeCount))} packets=$socketWritePackets)"
        )
        encodedFrames = 0
        renderedFrames = 0
        droppedFrames = 0
        sentBytes = 0L
        sentPackets = 0
        sentVideoFrames = 0
        directUploadFrames = 0
        copyUploadFrames = 0
        directUploadFallbackFrames = 0
        queuedCalls = 0
        directCallerWaitNs = 0L
        directDrawNs = 0L
        directSwapNs = 0L
        directDrainNs = 0L
        directTaskNs = 0L
        compactCopyNs = 0L
        renderDrawNs = 0L
        renderSwapNs = 0L
        renderDrainNs = 0L
        renderTaskNs = 0L
        codecDequeueNs = 0L
        codecDequeueCalls = 0
        codecOutputBuffers = 0
        socketWriteNs = 0L
        socketWritePackets = 0
        lastLogMs = now
    }

    private fun closeCurrentClient() {
        synchronized(outputLock) {
            runCatching { output?.close() }
            runCatching { client?.close() }
            output = null
            client = null
            isConnected = false
            lastQueuedVideoFrameWallMs = 0L
            lastRenderedVideoFrameWallMs = 0L
            lastSentVideoFrameWallMs = 0L
        }
    }

    private fun releasePipeline() {
        runCatching { drainEncoder(true) }
        runCatching { codec?.stop() }
        runCatching { codec?.release() }
        codec = null
        runCatching { inputSurface?.release() }
        inputSurface = null
        runCatching { renderer?.release() }
        renderer = null
        runCatching { egl?.release() }
        egl = null
        latestCodecConfig = null
        encodedFrames = 0
        renderedFrames = 0
        droppedFrames = 0
        sentBytes = 0L
        sentPackets = 0
        sentVideoFrames = 0
        directUploadFrames = 0
        copyUploadFrames = 0
        directUploadFallbackFrames = 0
        queuedCalls = 0
        directCallerWaitNs = 0L
        directDrawNs = 0L
        directSwapNs = 0L
        directDrainNs = 0L
        directTaskNs = 0L
        compactCopyNs = 0L
        renderDrawNs = 0L
        renderSwapNs = 0L
        renderDrainNs = 0L
        renderTaskNs = 0L
        codecDequeueNs = 0L
        codecDequeueCalls = 0
        codecOutputBuffers = 0
        socketWriteNs = 0L
        socketWritePackets = 0
        lastLogMs = 0L
        lastPtsNs = 0L
        lastQueuedVideoFrameWallMs = 0L
        lastRenderedVideoFrameWallMs = 0L
        lastSentVideoFrameWallMs = 0L
    }

    private fun even(v: Int): Int {
        val x = v.coerceAtLeast(2)
        return if (x and 1 == 0) x else x - 1
    }

    private class EncoderEgl(surface: Surface) {
        private var display: EGLDisplay = EGL14.EGL_NO_DISPLAY
        private var context: EGLContext = EGL14.EGL_NO_CONTEXT
        private var eglSurface: EGLSurface = EGL14.EGL_NO_SURFACE

        init {
            display = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY)
            val version = IntArray(2)
            check(EGL14.eglInitialize(display, version, 0, version, 1)) { "eglInitialize failed" }

            val attribList = intArrayOf(
                EGL14.EGL_RED_SIZE, 8,
                EGL14.EGL_GREEN_SIZE, 8,
                EGL14.EGL_BLUE_SIZE, 8,
                EGL14.EGL_ALPHA_SIZE, 8,
                EGL14.EGL_RENDERABLE_TYPE, EGL14.EGL_OPENGL_ES2_BIT,
                EGL_RECORDABLE_ANDROID, 1,
                EGL14.EGL_NONE,
            )
            val configs = arrayOfNulls<EGLConfig>(1)
            val numConfigs = IntArray(1)
            check(EGL14.eglChooseConfig(display, attribList, 0, configs, 0, 1, numConfigs, 0)) { "eglChooseConfig failed" }
            val config = configs[0] ?: error("missing EGLConfig")

            // Prefer ES3 so GL_UNPACK_ROW_LENGTH can upload a cropped ROI from a pitched
            // ImageReader row without first compact-copying it.  Fall back to ES2 if a device
            // refuses ES3; the sender then uses the old compact-copy path.
            context = EGL14.eglCreateContext(
                display,
                config,
                EGL14.EGL_NO_CONTEXT,
                intArrayOf(EGL14.EGL_CONTEXT_CLIENT_VERSION, 3, EGL14.EGL_NONE),
                0,
            )
            if (context == EGL14.EGL_NO_CONTEXT) {
                context = EGL14.eglCreateContext(
                    display,
                    config,
                    EGL14.EGL_NO_CONTEXT,
                    intArrayOf(EGL14.EGL_CONTEXT_CLIENT_VERSION, 2, EGL14.EGL_NONE),
                    0,
                )
            }
            check(context != EGL14.EGL_NO_CONTEXT) { "eglCreateContext failed" }
            eglSurface = EGL14.eglCreateWindowSurface(display, config, surface, intArrayOf(EGL14.EGL_NONE), 0)
            check(eglSurface != EGL14.EGL_NO_SURFACE) { "eglCreateWindowSurface failed" }
            makeCurrent()
        }

        fun makeCurrent() {
            check(EGL14.eglMakeCurrent(display, eglSurface, eglSurface, context)) { "eglMakeCurrent failed" }
        }

        fun setPresentationTime(ns: Long) {
            EGLExt.eglPresentationTimeANDROID(display, eglSurface, ns)
        }

        fun swapBuffers() {
            EGL14.eglSwapBuffers(display, eglSurface)
        }

        fun release() {
            if (display != EGL14.EGL_NO_DISPLAY) {
                EGL14.eglMakeCurrent(display, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_CONTEXT)
                if (eglSurface != EGL14.EGL_NO_SURFACE) EGL14.eglDestroySurface(display, eglSurface)
                if (context != EGL14.EGL_NO_CONTEXT) EGL14.eglDestroyContext(display, context)
                EGL14.eglReleaseThread()
                EGL14.eglTerminate(display)
            }
            display = EGL14.EGL_NO_DISPLAY
            context = EGL14.EGL_NO_CONTEXT
            eglSurface = EGL14.EGL_NO_SURFACE
        }
    }

    private class RgbaTextureRenderer(
        private val width: Int,
        private val height: Int,
    ) {
        private var textureId = 0
        private var program = 0
        private var aPos = 0
        private var aTex = 0
        private var uTexture = 0
        private val vertices = floatArrayOf(
            -1f, -1f,  1f, -1f, -1f, 1f,
            1f, -1f,  1f,  1f, -1f, 1f,
        )
        // RGBA buffer rows come from ImageReader top-to-bottom.  Flip V so PC display is upright.
        private val texCoords = floatArrayOf(
            0f, 1f, 1f, 1f, 0f, 0f,
            1f, 1f, 1f, 0f, 0f, 0f,
        )

        fun init() {
            textureId = createTexture()
            program = createProgram(VERTEX_SHADER, FRAGMENT_SHADER)
            aPos = GLES20.glGetAttribLocation(program, "aPosition")
            aTex = GLES20.glGetAttribLocation(program, "aTexCoord")
            uTexture = GLES20.glGetUniformLocation(program, "sTexture")
        }

        fun drawFromSourceRegion(
            source: ByteBuffer,
            rowStride: Int,
            left: Int,
            top: Int,
            width: Int,
            height: Int,
        ): Boolean {
            if (width != this.width || height != this.height || rowStride <= 0 || rowStride % 4 != 0) return false
            val offset = top * rowStride + left * 4
            if (offset < 0 || offset >= source.capacity()) return false

            drawBegin()
            val dup = source.duplicate()
            dup.position(offset)
            GLES20.glPixelStorei(GLES20.GL_UNPACK_ALIGNMENT, 1)
            GLES30.glPixelStorei(GLES30.GL_UNPACK_ROW_LENGTH, rowStride / 4)
            GLES20.glTexSubImage2D(
                GLES20.GL_TEXTURE_2D,
                0,
                0,
                0,
                width,
                height,
                GLES20.GL_RGBA,
                GLES20.GL_UNSIGNED_BYTE,
                dup,
            )
            GLES30.glPixelStorei(GLES30.GL_UNPACK_ROW_LENGTH, 0)
            val error = GLES20.glGetError()
            if (error != GLES20.GL_NO_ERROR) return false
            drawQuad()
            return true
        }

        fun draw(pixels: ByteBuffer) {
            drawBegin()
            pixels.position(0)
            GLES20.glTexSubImage2D(
                GLES20.GL_TEXTURE_2D,
                0,
                0,
                0,
                width,
                height,
                GLES20.GL_RGBA,
                GLES20.GL_UNSIGNED_BYTE,
                pixels,
            )
            drawQuad()
        }

        private fun drawBegin() {
            GLES20.glViewport(0, 0, width, height)
            GLES20.glDisable(GLES20.GL_BLEND)
            GLES20.glColorMask(true, true, true, true)
            GLES20.glClearColor(0f, 0f, 0f, 1f)
            GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT)
            GLES20.glPixelStorei(GLES20.GL_UNPACK_ALIGNMENT, 1)
            GLES20.glUseProgram(program)
            GLES20.glActiveTexture(GLES20.GL_TEXTURE0)
            GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, textureId)
            GLES20.glUniform1i(uTexture, 0)
        }

        private fun drawQuad() {
            val vb = ByteBuffer.allocateDirect(vertices.size * 4).order(ByteOrder.nativeOrder()).asFloatBuffer()
            vb.put(vertices).position(0)
            val tb = ByteBuffer.allocateDirect(texCoords.size * 4).order(ByteOrder.nativeOrder()).asFloatBuffer()
            tb.put(texCoords).position(0)

            GLES20.glEnableVertexAttribArray(aPos)
            GLES20.glVertexAttribPointer(aPos, 2, GLES20.GL_FLOAT, false, 0, vb)
            GLES20.glEnableVertexAttribArray(aTex)
            GLES20.glVertexAttribPointer(aTex, 2, GLES20.GL_FLOAT, false, 0, tb)
            GLES20.glDrawArrays(GLES20.GL_TRIANGLES, 0, 6)
            GLES20.glDisableVertexAttribArray(aPos)
            GLES20.glDisableVertexAttribArray(aTex)
        }

        fun release() {
            if (program != 0) GLES20.glDeleteProgram(program)
            if (textureId != 0) GLES20.glDeleteTextures(1, intArrayOf(textureId), 0)
            program = 0
            textureId = 0
        }

        private fun createTexture(): Int {
            val ids = IntArray(1)
            GLES20.glGenTextures(1, ids, 0)
            GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, ids[0])
            GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_MIN_FILTER, GLES20.GL_LINEAR)
            GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_MAG_FILTER, GLES20.GL_LINEAR)
            GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_WRAP_S, GLES20.GL_CLAMP_TO_EDGE)
            GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_WRAP_T, GLES20.GL_CLAMP_TO_EDGE)
            GLES20.glTexImage2D(
                GLES20.GL_TEXTURE_2D,
                0,
                GLES20.GL_RGBA,
                width,
                height,
                0,
                GLES20.GL_RGBA,
                GLES20.GL_UNSIGNED_BYTE,
                null,
            )
            return ids[0]
        }

        private fun createProgram(vertex: String, fragment: String): Int {
            val vs = loadShader(GLES20.GL_VERTEX_SHADER, vertex)
            val fs = loadShader(GLES20.GL_FRAGMENT_SHADER, fragment)
            val p = GLES20.glCreateProgram()
            GLES20.glAttachShader(p, vs)
            GLES20.glAttachShader(p, fs)
            GLES20.glLinkProgram(p)
            val ok = IntArray(1)
            GLES20.glGetProgramiv(p, GLES20.GL_LINK_STATUS, ok, 0)
            if (ok[0] == 0) error("program link failed: ${GLES20.glGetProgramInfoLog(p)}")
            GLES20.glDeleteShader(vs)
            GLES20.glDeleteShader(fs)
            return p
        }

        private fun loadShader(type: Int, source: String): Int {
            val shader = GLES20.glCreateShader(type)
            GLES20.glShaderSource(shader, source)
            GLES20.glCompileShader(shader)
            val ok = IntArray(1)
            GLES20.glGetShaderiv(shader, GLES20.GL_COMPILE_STATUS, ok, 0)
            if (ok[0] == 0) error("shader compile failed: ${GLES20.glGetShaderInfoLog(shader)}")
            return shader
        }

        companion object {
            private const val VERTEX_SHADER = """
                attribute vec4 aPosition;
                attribute vec2 aTexCoord;
                varying vec2 vTexCoord;
                void main() {
                    gl_Position = aPosition;
                    vTexCoord = aTexCoord;
                }
            """

            private const val FRAGMENT_SHADER = """
                precision mediump float;
                varying vec2 vTexCoord;
                uniform sampler2D sTexture;
                void main() {
                    vec4 c = texture2D(sTexture, vTexCoord);
                    // ImageReader RGBA buffers from some devices carry alpha=0 even when RGB is valid.
                    // A MediaCodec input Surface may treat that as premultiplied transparent black.
                    // Force an opaque alpha channel so the H.264 encoder receives the real RGB pixels.
                    gl_FragColor = vec4(c.rgb, 1.0);
                }
            """
        }
    }
}
