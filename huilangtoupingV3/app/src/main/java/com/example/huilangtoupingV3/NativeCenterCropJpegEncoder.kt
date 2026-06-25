package com.example.huilangtoupingV3

import java.io.FileDescriptor
import java.nio.ByteBuffer

object NativeCenterCropJpegEncoder {
    init {
        System.loadLibrary("turbojpeg")
        System.loadLibrary("huilang_native_encoder")
    }

    external fun nativeSetOutputFileDescriptor(fileDescriptor: FileDescriptor?): Boolean
    external fun nativeClearOutputFileDescriptor()
    external fun nativeHasOutputFileDescriptor(): Boolean
    external fun nativePrepareCurrentThreadForEncoding(): Boolean
    external fun nativeBindCurrentThreadToCpu(cpuId: Int): Boolean
    external fun nativeGetCurrentCpu(): Int
    external fun nativeSetReservedCallbackCpu(cpuId: Int)
    external fun nativeSetManualSplitWeights(weights: FloatArray): Boolean
    external fun nativeSetManualCpuSplitWeights(cpuIds: IntArray, weights: FloatArray): Boolean
    external fun nativeClearManualSplitWeights()
    external fun nativeSetJpegSubsamplingMode(mode: Int): Boolean
    external fun nativeSetSplitRoiQualityParams(edgeQualityReduction: Int, edgeEncodeScalePercent: Int): Boolean
    external fun nativeSetSplitLayoutParams(centerHeightPercent: Int, centerWidthPercent: Int, centerCoreCount: Int, centerOnly: Boolean, bigCoreWeightPercent: Int): Boolean
    external fun nativeResetFrameTimeline()
    external fun nativeWarmupSplitEncoder(width: Int, height: Int, splitParts: Int, jpegSubsamplingMode: Int): Boolean
    external fun nativeGetAvailableEncodeCpuCount(): Int
    external fun nativeGetSuggestedEncodeCpuIds(count: Int): IntArray
    external fun nativeGetLastSendStats(): LongArray
    external fun nativeGetLastSplitPartStats(): LongArray
    external fun nativeFillLastSendStats(out: LongArray): Boolean
    external fun nativeFillLastSplitPartStats(out: LongArray): Boolean

    external fun nativeEncodeCenterCropRgba8888(
        rgbaBuffer: ByteBuffer,
        bufferCapacity: Int,
        srcWidth: Int,
        srcHeight: Int,
        rowStride: Int,
        pixelStride: Int,
        cropSize: Int,
        jpegQuality: Int,
    ): ByteArray?

    external fun nativeEncodeFullFrameRgba8888(
        rgbaBuffer: ByteBuffer,
        bufferCapacity: Int,
        srcWidth: Int,
        srcHeight: Int,
        rowStride: Int,
        pixelStride: Int,
        jpegQuality: Int,
    ): ByteArray?

    external fun nativeEncodeAndSendCenterCropRgba8888(
        rgbaBuffer: ByteBuffer,
        bufferCapacity: Int,
        srcWidth: Int,
        srcHeight: Int,
        rowStride: Int,
        pixelStride: Int,
        cropSize: Int,
        jpegQuality: Int,
        frameProducedNs: Long,
        callbackStartNs: Long,
    ): Boolean

    external fun nativeEncodeAndSendCenterCropRgba8888Split2(
        rgbaBuffer: ByteBuffer,
        bufferCapacity: Int,
        srcWidth: Int,
        srcHeight: Int,
        rowStride: Int,
        pixelStride: Int,
        cropSize: Int,
        jpegQuality: Int,
        frameProducedNs: Long,
        callbackStartNs: Long,
    ): Boolean

    external fun nativeEncodeAndSendCenterCropRgba8888Split4(
        rgbaBuffer: ByteBuffer,
        bufferCapacity: Int,
        srcWidth: Int,
        srcHeight: Int,
        rowStride: Int,
        pixelStride: Int,
        cropSize: Int,
        jpegQuality: Int,
        frameProducedNs: Long,
        callbackStartNs: Long,
    ): Boolean

    external fun nativeEncodeAndSendCenterCropRgba8888SplitN(
        rgbaBuffer: ByteBuffer,
        bufferCapacity: Int,
        srcWidth: Int,
        srcHeight: Int,
        rowStride: Int,
        pixelStride: Int,
        cropSize: Int,
        jpegQuality: Int,
        frameProducedNs: Long,
        callbackStartNs: Long,
        splitParts: Int,
    ): Boolean

    external fun nativeEncodeAndSendFullFrameRgba8888(
        rgbaBuffer: ByteBuffer,
        bufferCapacity: Int,
        srcWidth: Int,
        srcHeight: Int,
        rowStride: Int,
        pixelStride: Int,
        jpegQuality: Int,
        frameProducedNs: Long,
        callbackStartNs: Long,
    ): Boolean

    external fun nativeEncodeAndSendFullFrameRgba8888Split2(
        rgbaBuffer: ByteBuffer,
        bufferCapacity: Int,
        srcWidth: Int,
        srcHeight: Int,
        rowStride: Int,
        pixelStride: Int,
        jpegQuality: Int,
        frameProducedNs: Long,
        callbackStartNs: Long,
    ): Boolean

    external fun nativeEncodeAndSendFullFrameRgba8888Split4(
        rgbaBuffer: ByteBuffer,
        bufferCapacity: Int,
        srcWidth: Int,
        srcHeight: Int,
        rowStride: Int,
        pixelStride: Int,
        jpegQuality: Int,
        frameProducedNs: Long,
        callbackStartNs: Long,
    ): Boolean

    external fun nativeEncodeAndSendFullFrameRgba8888Split6(
        rgbaBuffer: ByteBuffer,
        bufferCapacity: Int,
        srcWidth: Int,
        srcHeight: Int,
        rowStride: Int,
        pixelStride: Int,
        jpegQuality: Int,
        frameProducedNs: Long,
        callbackStartNs: Long,
    ): Boolean

    external fun nativeEncodeAndSendFullFrameRgba8888Split7(
        rgbaBuffer: ByteBuffer,
        bufferCapacity: Int,
        srcWidth: Int,
        srcHeight: Int,
        rowStride: Int,
        pixelStride: Int,
        jpegQuality: Int,
        frameProducedNs: Long,
        callbackStartNs: Long,
    ): Boolean

    external fun nativeEncodeAndSendFullFrameRgba8888Split8(
        rgbaBuffer: ByteBuffer,
        bufferCapacity: Int,
        srcWidth: Int,
        srcHeight: Int,
        rowStride: Int,
        pixelStride: Int,
        jpegQuality: Int,
        frameProducedNs: Long,
        callbackStartNs: Long,
    ): Boolean

    external fun nativeEncodeAndSendFullFrameRgba8888SplitN(
        rgbaBuffer: ByteBuffer,
        bufferCapacity: Int,
        srcWidth: Int,
        srcHeight: Int,
        rowStride: Int,
        pixelStride: Int,
        jpegQuality: Int,
        frameProducedNs: Long,
        callbackStartNs: Long,
        splitParts: Int,
    ): Boolean

    external fun nativeEncodeAndSendFrameExceptCenterRgba8888SplitN(
        rgbaBuffer: ByteBuffer,
        bufferCapacity: Int,
        srcWidth: Int,
        srcHeight: Int,
        rowStride: Int,
        pixelStride: Int,
        centerLeft: Int,
        centerTop: Int,
        centerWidth: Int,
        centerHeight: Int,
        jpegQuality: Int,
        frameProducedNs: Long,
        callbackStartNs: Long,
        splitParts: Int,
    ): Boolean

    external fun nativeEncodeAndSendFrameWithBottomVideoRgba8888SplitN(
        rgbaBuffer: ByteBuffer,
        bufferCapacity: Int,
        srcWidth: Int,
        srcHeight: Int,
        rowStride: Int,
        pixelStride: Int,
        videoTop: Int,
        videoHeight: Int,
        jpegQuality: Int,
        frameProducedNs: Long,
        callbackStartNs: Long,
        splitParts: Int,
    ): Boolean

    external fun nativeEncodeAndSendCenterCpuPriorityRgba8888SplitN(
        rgbaBuffer: ByteBuffer,
        bufferCapacity: Int,
        srcWidth: Int,
        srcHeight: Int,
        rowStride: Int,
        pixelStride: Int,
        centerLeft: Int,
        centerTop: Int,
        centerWidth: Int,
        centerHeight: Int,
        jpegQuality: Int,
        frameProducedNs: Long,
        callbackStartNs: Long,
        splitParts: Int,
    ): Boolean

    external fun nativeEncodeCenterCropYuv420888(
        yBuffer: ByteBuffer,
        yCapacity: Int,
        uBuffer: ByteBuffer,
        uCapacity: Int,
        vBuffer: ByteBuffer,
        vCapacity: Int,
        srcWidth: Int,
        srcHeight: Int,
        yRowStride: Int,
        uRowStride: Int,
        vRowStride: Int,
        uPixelStride: Int,
        vPixelStride: Int,
        cropSize: Int,
        jpegQuality: Int,
    ): ByteArray?

    external fun nativeEncodeFullFrameYuv420888(
        yBuffer: ByteBuffer,
        yCapacity: Int,
        uBuffer: ByteBuffer,
        uCapacity: Int,
        vBuffer: ByteBuffer,
        vCapacity: Int,
        srcWidth: Int,
        srcHeight: Int,
        yRowStride: Int,
        uRowStride: Int,
        vRowStride: Int,
        uPixelStride: Int,
        vPixelStride: Int,
        jpegQuality: Int,
    ): ByteArray?

    external fun nativeEncodeAndSendCenterCropYuv420888(
        yBuffer: ByteBuffer,
        yCapacity: Int,
        uBuffer: ByteBuffer,
        uCapacity: Int,
        vBuffer: ByteBuffer,
        vCapacity: Int,
        srcWidth: Int,
        srcHeight: Int,
        yRowStride: Int,
        uRowStride: Int,
        vRowStride: Int,
        uPixelStride: Int,
        vPixelStride: Int,
        cropSize: Int,
        jpegQuality: Int,
        frameProducedNs: Long,
        callbackStartNs: Long,
    ): Boolean

    external fun nativeEncodeAndSendFullFrameYuv420888(
        yBuffer: ByteBuffer,
        yCapacity: Int,
        uBuffer: ByteBuffer,
        uCapacity: Int,
        vBuffer: ByteBuffer,
        vCapacity: Int,
        srcWidth: Int,
        srcHeight: Int,
        yRowStride: Int,
        uRowStride: Int,
        vRowStride: Int,
        uPixelStride: Int,
        vPixelStride: Int,
        jpegQuality: Int,
        frameProducedNs: Long,
        callbackStartNs: Long,
    ): Boolean

    fun setOutputFileDescriptor(fileDescriptor: FileDescriptor?): Boolean {
        return nativeSetOutputFileDescriptor(fileDescriptor)
    }

    fun clearOutputFileDescriptor() {
        nativeClearOutputFileDescriptor()
    }

    fun hasOutputFileDescriptor(): Boolean {
        return nativeHasOutputFileDescriptor()
    }

    fun prepareCurrentThreadForEncoding(): Boolean {
        return nativePrepareCurrentThreadForEncoding()
    }

    fun bindCurrentThreadToCpu(cpuId: Int): Boolean {
        return nativeBindCurrentThreadToCpu(cpuId)
    }

    fun setReservedCallbackCpu(cpuId: Int) {
        runCatching { nativeSetReservedCallbackCpu(cpuId) }
    }

    fun setManualSplitWeights(weights: FloatArray): Boolean {
        if (weights.size !in 2..16) return false
        val safe = FloatArray(weights.size) { i ->
            val v = weights[i]
            if (v.isFinite() && v > 0f) v.coerceIn(0.70f, 1.30f) else 1f
        }
        val avg = safe.average().toFloat().takeIf { it.isFinite() && it > 0f } ?: 1f
        for (i in safe.indices) {
            safe[i] = (safe[i] / avg).coerceIn(0.70f, 1.30f)
        }
        return runCatching { nativeSetManualSplitWeights(safe) }.getOrDefault(false)
    }

    fun clearManualSplitWeights() {
        runCatching { nativeClearManualSplitWeights() }
    }

    fun setManualCpuSplitWeights(cpuIds: IntArray, weights: FloatArray): Boolean {
        if (cpuIds.size !in 2..16 || cpuIds.size != weights.size) return false
        val safeCpuIds = IntArray(cpuIds.size) { i -> cpuIds[i] }
        val safeWeights = FloatArray(weights.size) { i ->
            val v = weights[i]
            if (v.isFinite() && v > 0f) v.coerceIn(0.70f, 1.30f) else 1f
        }
        val avg = safeWeights.average().toFloat().takeIf { it.isFinite() && it > 0f } ?: 1f
        for (i in safeWeights.indices) {
            safeWeights[i] = (safeWeights[i] / avg).coerceIn(0.70f, 1.30f)
        }
        return runCatching { nativeSetManualCpuSplitWeights(safeCpuIds, safeWeights) }.getOrDefault(false)
    }


    fun setJpegSubsamplingMode(mode: Int): Boolean {
        val safe = if (mode == 444) 444 else 420
        return runCatching { nativeSetJpegSubsamplingMode(safe) }.getOrDefault(false)
    }

    fun setSplitRoiQualityParams(edgeQualityReduction: Int, edgeEncodeScalePercent: Int): Boolean {
        val qDrop = edgeQualityReduction.coerceIn(0, 40)
        val scale = edgeEncodeScalePercent.coerceIn(50, 100)
        return runCatching { nativeSetSplitRoiQualityParams(qDrop, scale) }.getOrDefault(false)
    }

    fun setSplitLayoutParams(
        topLowPercent: Int,
        centerWidthPercent: Int,
        centerCoreCount: Int,
        centerOnly: Boolean = false,
        bigCoreWeightPercent: Int = 120,
    ): Boolean {
        // 新语义：
        // topLowPercent 继续复用旧协议字段，但含义是 JPEG 中心高质量区域“高度百分比”。
        // 第三个参数继续复用旧协议字段，但含义从“中心权重”改为“中心核心数”。
        // bigCoreWeightPercent：100=平均切块，>100=大核心块更大，<100=大核心块更小。
        val centerHeight = topLowPercent.coerceIn(10, 80)
        val centerWidth = centerWidthPercent.coerceIn(10, 80)
        val centerCores = centerCoreCount.coerceIn(1, 24)
        val bigWeight = bigCoreWeightPercent.coerceIn(50, 200)

        return runCatching {
            nativeSetSplitLayoutParams(centerHeight, centerWidth, centerCores, centerOnly, bigWeight)
        }.getOrDefault(false)
    }

    fun resetFrameTimeline() {
        runCatching { nativeResetFrameTimeline() }
    }

    fun warmupSplitEncoder(width: Int, height: Int, splitParts: Int, jpegSubsamplingMode: Int): Boolean {
        val safeMode = if (jpegSubsamplingMode == 444) 444 else 420
        return runCatching {
            nativeWarmupSplitEncoder(
                width.coerceAtLeast(16),
                height.coerceAtLeast(16),
                splitParts.coerceIn(1, 16),
                safeMode,
            )
        }.getOrDefault(false)
    }

    fun encodeAndSendCenterCpuPriorityRgba8888SplitN(
        buffer: ByteBuffer,
        srcWidth: Int,
        srcHeight: Int,
        rowStride: Int,
        pixelStride: Int,
        centerLeft: Int,
        centerTop: Int,
        centerWidth: Int,
        centerHeight: Int,
        jpegQuality: Int,
        frameProducedNs: Long,
        callbackStartNs: Long,
        splitParts: Int,
    ): Boolean {
        return runCatching {
            nativeEncodeAndSendCenterCpuPriorityRgba8888SplitN(
                rgbaBuffer = buffer,
                bufferCapacity = buffer.capacity(),
                srcWidth = srcWidth,
                srcHeight = srcHeight,
                rowStride = rowStride,
                pixelStride = pixelStride,
                centerLeft = centerLeft,
                centerTop = centerTop,
                centerWidth = centerWidth,
                centerHeight = centerHeight,
                jpegQuality = jpegQuality,
                frameProducedNs = frameProducedNs,
                callbackStartNs = callbackStartNs,
                splitParts = splitParts,
            )
        }.getOrDefault(false)
    }

    fun fillLastSendStats(out: LongArray): Boolean {
        if (out.size < 7) return false
        return runCatching { nativeFillLastSendStats(out) }.getOrDefault(false)
    }

    fun fillLastSplitPartStats(out: LongArray): Boolean {
        if (out.size < 1 + 16 * 3) return false
        return runCatching { nativeFillLastSplitPartStats(out) }.getOrDefault(false)
    }

    fun getCurrentCpu(): Int {
        return nativeGetCurrentCpu()
    }

    fun getSuggestedEncodeCpuIds(count: Int): IntArray {
        return nativeGetSuggestedEncodeCpuIds(count.coerceAtLeast(0))
    }

    data class LastSendStats(
        val totalBytes: Long,
        val part0Bytes: Long,
        val part1Bytes: Long,
        val socketWriteNs: Long,
        val part0EncodeNs: Long,
        val part1EncodeNs: Long,
        val splitParts: Int,
    )

    data class SplitPartStat(
        val index: Int,
        val bytes: Long,
        val encodeNs: Long,
        val cpuId: Int,
    )

    data class LastSplitPartStats(
        val partCount: Int,
        val parts: List<SplitPartStat>,
    )

    fun getLastSendStats(): LastSendStats {
        val v = try {
            nativeGetLastSendStats()
        } catch (_: Throwable) {
            LongArray(0)
        }
        return LastSendStats(
            totalBytes = v.getOrNull(0) ?: 0L,
            part0Bytes = v.getOrNull(1) ?: 0L,
            part1Bytes = v.getOrNull(2) ?: 0L,
            socketWriteNs = v.getOrNull(3) ?: 0L,
            part0EncodeNs = v.getOrNull(4) ?: 0L,
            part1EncodeNs = v.getOrNull(5) ?: 0L,
            splitParts = ((v.getOrNull(6) ?: 1L).toInt()).coerceAtLeast(1),
        )
    }

    fun getLastSplitPartStats(): LastSplitPartStats {
        val v = try {
            nativeGetLastSplitPartStats()
        } catch (_: Throwable) {
            LongArray(0)
        }
        if (v.isEmpty()) {
            return LastSplitPartStats(partCount = 0, parts = emptyList())
        }
        val partCount = (v.getOrNull(0) ?: 0L).toInt().coerceIn(0, 16)
        val parts = ArrayList<SplitPartStat>(partCount)
        for (index in 0 until partCount) {
            val base = 1 + index * 3
            parts += SplitPartStat(
                index = index,
                bytes = v.getOrNull(base) ?: 0L,
                encodeNs = v.getOrNull(base + 1) ?: 0L,
                cpuId = (v.getOrNull(base + 2) ?: -1L).toInt(),
            )
        }
        return LastSplitPartStats(partCount = partCount, parts = parts)
    }

    fun encodeCenterCropRgba8888(
        buffer: ByteBuffer,
        srcWidth: Int,
        srcHeight: Int,
        rowStride: Int,
        pixelStride: Int,
        cropSize: Int,
        jpegQuality: Int,
    ): ByteArray? {
        return nativeEncodeCenterCropRgba8888(
            rgbaBuffer = buffer,
            bufferCapacity = buffer.capacity(),
            srcWidth = srcWidth,
            srcHeight = srcHeight,
            rowStride = rowStride,
            pixelStride = pixelStride,
            cropSize = cropSize,
            jpegQuality = jpegQuality,
        )
    }

    fun encodeFullFrameRgba8888(
        buffer: ByteBuffer,
        srcWidth: Int,
        srcHeight: Int,
        rowStride: Int,
        pixelStride: Int,
        jpegQuality: Int,
    ): ByteArray? {
        return nativeEncodeFullFrameRgba8888(
            rgbaBuffer = buffer,
            bufferCapacity = buffer.capacity(),
            srcWidth = srcWidth,
            srcHeight = srcHeight,
            rowStride = rowStride,
            pixelStride = pixelStride,
            jpegQuality = jpegQuality,
        )
    }

    fun encodeAndSendCenterCropRgba8888(
        buffer: ByteBuffer,
        srcWidth: Int,
        srcHeight: Int,
        rowStride: Int,
        pixelStride: Int,
        cropSize: Int,
        jpegQuality: Int,
        frameProducedNs: Long,
        callbackStartNs: Long,
    ): Boolean {
        return nativeEncodeAndSendCenterCropRgba8888(
            rgbaBuffer = buffer,
            bufferCapacity = buffer.capacity(),
            srcWidth = srcWidth,
            srcHeight = srcHeight,
            rowStride = rowStride,
            pixelStride = pixelStride,
            cropSize = cropSize,
            jpegQuality = jpegQuality,
            frameProducedNs = frameProducedNs,
            callbackStartNs = callbackStartNs,
        )
    }

    fun encodeAndSendFullFrameRgba8888(
        buffer: ByteBuffer,
        srcWidth: Int,
        srcHeight: Int,
        rowStride: Int,
        pixelStride: Int,
        jpegQuality: Int,
        frameProducedNs: Long,
        callbackStartNs: Long,
    ): Boolean {
        return nativeEncodeAndSendFullFrameRgba8888(
            rgbaBuffer = buffer,
            bufferCapacity = buffer.capacity(),
            srcWidth = srcWidth,
            srcHeight = srcHeight,
            rowStride = rowStride,
            pixelStride = pixelStride,
            jpegQuality = jpegQuality,
            frameProducedNs = frameProducedNs,
            callbackStartNs = callbackStartNs,
        )
    }

    fun availableEncodeCpuCount(): Int {
        return try {
            nativeGetAvailableEncodeCpuCount().coerceAtLeast(1)
        } catch (_: UnsatisfiedLinkError) {
            Runtime.getRuntime().availableProcessors().coerceAtLeast(1)
        } catch (_: Throwable) {
            Runtime.getRuntime().availableProcessors().coerceAtLeast(1)
        }
    }

    fun encodeAndSendFullFrameRgba8888SplitN(
        buffer: ByteBuffer,
        srcWidth: Int,
        srcHeight: Int,
        rowStride: Int,
        pixelStride: Int,
        jpegQuality: Int,
        frameProducedNs: Long,
        callbackStartNs: Long,
        splitParts: Int,
    ): Boolean {
        val parts = splitParts.coerceAtLeast(1)
        if (parts <= 1) {
            return encodeAndSendFullFrameRgba8888(
                buffer = buffer,
                srcWidth = srcWidth,
                srcHeight = srcHeight,
                rowStride = rowStride,
                pixelStride = pixelStride,
                jpegQuality = jpegQuality,
                frameProducedNs = frameProducedNs,
                callbackStartNs = callbackStartNs,
            )
        }
        return try {
            nativeEncodeAndSendFullFrameRgba8888SplitN(
                rgbaBuffer = buffer,
                bufferCapacity = buffer.capacity(),
                srcWidth = srcWidth,
                srcHeight = srcHeight,
                rowStride = rowStride,
                pixelStride = pixelStride,
                jpegQuality = jpegQuality,
                frameProducedNs = frameProducedNs,
                callbackStartNs = callbackStartNs,
                splitParts = parts,
            )
        } catch (_: UnsatisfiedLinkError) {
            when {
                parts >= 8 -> encodeAndSendFullFrameRgba8888Split8(buffer, srcWidth, srcHeight, rowStride, pixelStride, jpegQuality, frameProducedNs, callbackStartNs)
                parts >= 7 -> encodeAndSendFullFrameRgba8888Split7(buffer, srcWidth, srcHeight, rowStride, pixelStride, jpegQuality, frameProducedNs, callbackStartNs)
                parts >= 6 -> encodeAndSendFullFrameRgba8888Split6(buffer, srcWidth, srcHeight, rowStride, pixelStride, jpegQuality, frameProducedNs, callbackStartNs)
                parts >= 4 -> encodeAndSendFullFrameRgba8888Split4(buffer, srcWidth, srcHeight, rowStride, pixelStride, jpegQuality, frameProducedNs, callbackStartNs)
                else -> encodeAndSendFullFrameRgba8888Split2(buffer, srcWidth, srcHeight, rowStride, pixelStride, jpegQuality, frameProducedNs, callbackStartNs)
            }
        }
    }

    fun encodeAndSendFrameExceptCenterRgba8888SplitN(
        buffer: ByteBuffer,
        srcWidth: Int,
        srcHeight: Int,
        rowStride: Int,
        pixelStride: Int,
        centerLeft: Int,
        centerTop: Int,
        centerWidth: Int,
        centerHeight: Int,
        jpegQuality: Int,
        frameProducedNs: Long,
        callbackStartNs: Long,
        splitParts: Int,
    ): Boolean {
        val parts = splitParts.coerceAtLeast(2)
        return try {
            nativeEncodeAndSendFrameExceptCenterRgba8888SplitN(
                rgbaBuffer = buffer,
                bufferCapacity = buffer.capacity(),
                srcWidth = srcWidth,
                srcHeight = srcHeight,
                rowStride = rowStride,
                pixelStride = pixelStride,
                centerLeft = centerLeft,
                centerTop = centerTop,
                centerWidth = centerWidth,
                centerHeight = centerHeight,
                jpegQuality = jpegQuality,
                frameProducedNs = frameProducedNs,
                callbackStartNs = callbackStartNs,
                splitParts = parts,
            )
        } catch (t: UnsatisfiedLinkError) {
            // Hybrid path must never silently fall back to full-frame JPEG, otherwise the
            // center region goes back to CPU.  A missing native symbol is a build error.
            android.util.Log.e("NativeCenterCropJpeg", "native except-center encoder missing; not using full-frame CPU fallback", t)
            false
        }
    }


    fun encodeAndSendFrameWithBottomVideoRgba8888SplitN(
        buffer: ByteBuffer,
        srcWidth: Int,
        srcHeight: Int,
        rowStride: Int,
        pixelStride: Int,
        videoTop: Int,
        videoHeight: Int,
        jpegQuality: Int,
        frameProducedNs: Long,
        callbackStartNs: Long,
        splitParts: Int,
    ): Boolean {
        val parts = splitParts.coerceAtLeast(2)
        return try {
            nativeEncodeAndSendFrameWithBottomVideoRgba8888SplitN(
                rgbaBuffer = buffer,
                bufferCapacity = buffer.capacity(),
                srcWidth = srcWidth,
                srcHeight = srcHeight,
                rowStride = rowStride,
                pixelStride = pixelStride,
                videoTop = videoTop,
                videoHeight = videoHeight,
                jpegQuality = jpegQuality,
                frameProducedNs = frameProducedNs,
                callbackStartNs = callbackStartNs,
                splitParts = parts,
            )
        } catch (t: UnsatisfiedLinkError) {
            // Bottom-video mode requires the new native symbol. Do not silently fall back
            // to full-frame JPEG, otherwise the bottom video strip would be CPU-encoded twice.
            android.util.Log.e("NativeCenterCropJpeg", "native bottom-video encoder missing; not using full-frame CPU fallback", t)
            false
        }
    }

    fun encodeAndSendCenterCropRgba8888SplitN(
        buffer: ByteBuffer,
        srcWidth: Int,
        srcHeight: Int,
        rowStride: Int,
        pixelStride: Int,
        cropSize: Int,
        jpegQuality: Int,
        frameProducedNs: Long,
        callbackStartNs: Long,
        splitParts: Int,
    ): Boolean {
        val parts = splitParts.coerceAtLeast(1)
        if (parts <= 1) {
            return encodeAndSendCenterCropRgba8888(
                buffer = buffer,
                srcWidth = srcWidth,
                srcHeight = srcHeight,
                rowStride = rowStride,
                pixelStride = pixelStride,
                cropSize = cropSize,
                jpegQuality = jpegQuality,
                frameProducedNs = frameProducedNs,
                callbackStartNs = callbackStartNs,
            )
        }
        return try {
            nativeEncodeAndSendCenterCropRgba8888SplitN(
                rgbaBuffer = buffer,
                bufferCapacity = buffer.capacity(),
                srcWidth = srcWidth,
                srcHeight = srcHeight,
                rowStride = rowStride,
                pixelStride = pixelStride,
                cropSize = cropSize,
                jpegQuality = jpegQuality,
                frameProducedNs = frameProducedNs,
                callbackStartNs = callbackStartNs,
                splitParts = parts,
            )
        } catch (_: UnsatisfiedLinkError) {
            if (parts >= 4) {
                encodeAndSendCenterCropRgba8888Split4(buffer, srcWidth, srcHeight, rowStride, pixelStride, cropSize, jpegQuality, frameProducedNs, callbackStartNs)
            } else {
                encodeAndSendCenterCropRgba8888Split2(buffer, srcWidth, srcHeight, rowStride, pixelStride, cropSize, jpegQuality, frameProducedNs, callbackStartNs)
            }
        }
    }

    fun encodeAndSendCenterCropRgba8888Split2(
        buffer: ByteBuffer,
        srcWidth: Int,
        srcHeight: Int,
        rowStride: Int,
        pixelStride: Int,
        cropSize: Int,
        jpegQuality: Int,
        frameProducedNs: Long,
        callbackStartNs: Long,
    ): Boolean {
        return try {
            nativeEncodeAndSendCenterCropRgba8888Split2(
                rgbaBuffer = buffer,
                bufferCapacity = buffer.capacity(),
                srcWidth = srcWidth,
                srcHeight = srcHeight,
                rowStride = rowStride,
                pixelStride = pixelStride,
                cropSize = cropSize,
                jpegQuality = jpegQuality,
                frameProducedNs = frameProducedNs,
                callbackStartNs = callbackStartNs,
            )
        } catch (_: UnsatisfiedLinkError) {
            nativeEncodeAndSendCenterCropRgba8888(
                rgbaBuffer = buffer,
                bufferCapacity = buffer.capacity(),
                srcWidth = srcWidth,
                srcHeight = srcHeight,
                rowStride = rowStride,
                pixelStride = pixelStride,
                cropSize = cropSize,
                jpegQuality = jpegQuality,
                frameProducedNs = frameProducedNs,
                callbackStartNs = callbackStartNs,
            )
        }
    }

    fun encodeAndSendCenterCropRgba8888Split4(
        buffer: ByteBuffer,
        srcWidth: Int,
        srcHeight: Int,
        rowStride: Int,
        pixelStride: Int,
        cropSize: Int,
        jpegQuality: Int,
        frameProducedNs: Long,
        callbackStartNs: Long,
    ): Boolean {
        return try {
            nativeEncodeAndSendCenterCropRgba8888Split4(
                rgbaBuffer = buffer,
                bufferCapacity = buffer.capacity(),
                srcWidth = srcWidth,
                srcHeight = srcHeight,
                rowStride = rowStride,
                pixelStride = pixelStride,
                cropSize = cropSize,
                jpegQuality = jpegQuality,
                frameProducedNs = frameProducedNs,
                callbackStartNs = callbackStartNs,
            )
        } catch (_: UnsatisfiedLinkError) {
            encodeAndSendCenterCropRgba8888Split2(
                buffer = buffer,
                srcWidth = srcWidth,
                srcHeight = srcHeight,
                rowStride = rowStride,
                pixelStride = pixelStride,
                cropSize = cropSize,
                jpegQuality = jpegQuality,
                frameProducedNs = frameProducedNs,
                callbackStartNs = callbackStartNs,
            )
        }
    }

    fun encodeAndSendFullFrameRgba8888Split2(
        buffer: ByteBuffer,
        srcWidth: Int,
        srcHeight: Int,
        rowStride: Int,
        pixelStride: Int,
        jpegQuality: Int,
        frameProducedNs: Long,
        callbackStartNs: Long,
    ): Boolean {
        return nativeEncodeAndSendFullFrameRgba8888Split2(
            rgbaBuffer = buffer,
            bufferCapacity = buffer.capacity(),
            srcWidth = srcWidth,
            srcHeight = srcHeight,
            rowStride = rowStride,
            pixelStride = pixelStride,
            jpegQuality = jpegQuality,
            frameProducedNs = frameProducedNs,
            callbackStartNs = callbackStartNs,
        )
    }

    fun encodeAndSendFullFrameRgba8888Split4(
        buffer: ByteBuffer,
        srcWidth: Int,
        srcHeight: Int,
        rowStride: Int,
        pixelStride: Int,
        jpegQuality: Int,
        frameProducedNs: Long,
        callbackStartNs: Long,
    ): Boolean {
        return try {
            nativeEncodeAndSendFullFrameRgba8888Split4(
                rgbaBuffer = buffer,
                bufferCapacity = buffer.capacity(),
                srcWidth = srcWidth,
                srcHeight = srcHeight,
                rowStride = rowStride,
                pixelStride = pixelStride,
                jpegQuality = jpegQuality,
                frameProducedNs = frameProducedNs,
                callbackStartNs = callbackStartNs,
            )
        } catch (_: UnsatisfiedLinkError) {
            nativeEncodeAndSendFullFrameRgba8888Split2(
                rgbaBuffer = buffer,
                bufferCapacity = buffer.capacity(),
                srcWidth = srcWidth,
                srcHeight = srcHeight,
                rowStride = rowStride,
                pixelStride = pixelStride,
                jpegQuality = jpegQuality,
                frameProducedNs = frameProducedNs,
                callbackStartNs = callbackStartNs,
            )
        }
    }

    fun encodeAndSendFullFrameRgba8888Split6(
        buffer: ByteBuffer,
        srcWidth: Int,
        srcHeight: Int,
        rowStride: Int,
        pixelStride: Int,
        jpegQuality: Int,
        frameProducedNs: Long,
        callbackStartNs: Long,
    ): Boolean {
        return try {
            nativeEncodeAndSendFullFrameRgba8888Split6(
                rgbaBuffer = buffer,
                bufferCapacity = buffer.capacity(),
                srcWidth = srcWidth,
                srcHeight = srcHeight,
                rowStride = rowStride,
                pixelStride = pixelStride,
                jpegQuality = jpegQuality,
                frameProducedNs = frameProducedNs,
                callbackStartNs = callbackStartNs,
            )
        } catch (_: UnsatisfiedLinkError) {
            encodeAndSendFullFrameRgba8888Split4(
                buffer = buffer,
                srcWidth = srcWidth,
                srcHeight = srcHeight,
                rowStride = rowStride,
                pixelStride = pixelStride,
                jpegQuality = jpegQuality,
                frameProducedNs = frameProducedNs,
                callbackStartNs = callbackStartNs,
            )
        }
    }

    fun encodeAndSendFullFrameRgba8888Split7(
        buffer: ByteBuffer,
        srcWidth: Int,
        srcHeight: Int,
        rowStride: Int,
        pixelStride: Int,
        jpegQuality: Int,
        frameProducedNs: Long,
        callbackStartNs: Long,
    ): Boolean {
        return try {
            nativeEncodeAndSendFullFrameRgba8888Split7(
                rgbaBuffer = buffer,
                bufferCapacity = buffer.capacity(),
                srcWidth = srcWidth,
                srcHeight = srcHeight,
                rowStride = rowStride,
                pixelStride = pixelStride,
                jpegQuality = jpegQuality,
                frameProducedNs = frameProducedNs,
                callbackStartNs = callbackStartNs,
            )
        } catch (_: UnsatisfiedLinkError) {
            encodeAndSendFullFrameRgba8888Split6(
                buffer = buffer,
                srcWidth = srcWidth,
                srcHeight = srcHeight,
                rowStride = rowStride,
                pixelStride = pixelStride,
                jpegQuality = jpegQuality,
                frameProducedNs = frameProducedNs,
                callbackStartNs = callbackStartNs,
            )
        }
    }

    fun encodeAndSendFullFrameRgba8888Split8(
        buffer: ByteBuffer,
        srcWidth: Int,
        srcHeight: Int,
        rowStride: Int,
        pixelStride: Int,
        jpegQuality: Int,
        frameProducedNs: Long,
        callbackStartNs: Long,
    ): Boolean {
        return try {
            nativeEncodeAndSendFullFrameRgba8888Split8(
                rgbaBuffer = buffer,
                bufferCapacity = buffer.capacity(),
                srcWidth = srcWidth,
                srcHeight = srcHeight,
                rowStride = rowStride,
                pixelStride = pixelStride,
                jpegQuality = jpegQuality,
                frameProducedNs = frameProducedNs,
                callbackStartNs = callbackStartNs,
            )
        } catch (_: UnsatisfiedLinkError) {
            encodeAndSendFullFrameRgba8888Split6(
                buffer = buffer,
                srcWidth = srcWidth,
                srcHeight = srcHeight,
                rowStride = rowStride,
                pixelStride = pixelStride,
                jpegQuality = jpegQuality,
                frameProducedNs = frameProducedNs,
                callbackStartNs = callbackStartNs,
            )
        }
    }

    fun encodeCenterCropYuv420888(
        yBuffer: ByteBuffer,
        uBuffer: ByteBuffer,
        vBuffer: ByteBuffer,
        srcWidth: Int,
        srcHeight: Int,
        yRowStride: Int,
        uRowStride: Int,
        vRowStride: Int,
        uPixelStride: Int,
        vPixelStride: Int,
        cropSize: Int,
        jpegQuality: Int,
    ): ByteArray? {
        return nativeEncodeCenterCropYuv420888(
            yBuffer = yBuffer,
            yCapacity = yBuffer.capacity(),
            uBuffer = uBuffer,
            uCapacity = uBuffer.capacity(),
            vBuffer = vBuffer,
            vCapacity = vBuffer.capacity(),
            srcWidth = srcWidth,
            srcHeight = srcHeight,
            yRowStride = yRowStride,
            uRowStride = uRowStride,
            vRowStride = vRowStride,
            uPixelStride = uPixelStride,
            vPixelStride = vPixelStride,
            cropSize = cropSize,
            jpegQuality = jpegQuality,
        )
    }

    fun encodeFullFrameYuv420888(
        yBuffer: ByteBuffer,
        uBuffer: ByteBuffer,
        vBuffer: ByteBuffer,
        srcWidth: Int,
        srcHeight: Int,
        yRowStride: Int,
        uRowStride: Int,
        vRowStride: Int,
        uPixelStride: Int,
        vPixelStride: Int,
        jpegQuality: Int,
    ): ByteArray? {
        return nativeEncodeFullFrameYuv420888(
            yBuffer = yBuffer,
            yCapacity = yBuffer.capacity(),
            uBuffer = uBuffer,
            uCapacity = uBuffer.capacity(),
            vBuffer = vBuffer,
            vCapacity = vBuffer.capacity(),
            srcWidth = srcWidth,
            srcHeight = srcHeight,
            yRowStride = yRowStride,
            uRowStride = uRowStride,
            vRowStride = vRowStride,
            uPixelStride = uPixelStride,
            vPixelStride = vPixelStride,
            jpegQuality = jpegQuality,
        )
    }

    fun encodeAndSendCenterCropYuv420888(
        yBuffer: ByteBuffer,
        uBuffer: ByteBuffer,
        vBuffer: ByteBuffer,
        srcWidth: Int,
        srcHeight: Int,
        yRowStride: Int,
        uRowStride: Int,
        vRowStride: Int,
        uPixelStride: Int,
        vPixelStride: Int,
        cropSize: Int,
        jpegQuality: Int,
        frameProducedNs: Long,
        callbackStartNs: Long,
    ): Boolean {
        return nativeEncodeAndSendCenterCropYuv420888(
            yBuffer = yBuffer,
            yCapacity = yBuffer.capacity(),
            uBuffer = uBuffer,
            uCapacity = uBuffer.capacity(),
            vBuffer = vBuffer,
            vCapacity = vBuffer.capacity(),
            srcWidth = srcWidth,
            srcHeight = srcHeight,
            yRowStride = yRowStride,
            uRowStride = uRowStride,
            vRowStride = vRowStride,
            uPixelStride = uPixelStride,
            vPixelStride = vPixelStride,
            cropSize = cropSize,
            jpegQuality = jpegQuality,
            frameProducedNs = frameProducedNs,
            callbackStartNs = callbackStartNs,
        )
    }

    fun encodeAndSendFullFrameYuv420888(
        yBuffer: ByteBuffer,
        uBuffer: ByteBuffer,
        vBuffer: ByteBuffer,
        srcWidth: Int,
        srcHeight: Int,
        yRowStride: Int,
        uRowStride: Int,
        vRowStride: Int,
        uPixelStride: Int,
        vPixelStride: Int,
        jpegQuality: Int,
        frameProducedNs: Long,
        callbackStartNs: Long,
    ): Boolean {
        return nativeEncodeAndSendFullFrameYuv420888(
            yBuffer = yBuffer,
            yCapacity = yBuffer.capacity(),
            uBuffer = uBuffer,
            uCapacity = uBuffer.capacity(),
            vBuffer = vBuffer,
            vCapacity = vBuffer.capacity(),
            srcWidth = srcWidth,
            srcHeight = srcHeight,
            yRowStride = yRowStride,
            uRowStride = uRowStride,
            vRowStride = vRowStride,
            uPixelStride = uPixelStride,
            vPixelStride = vPixelStride,
            jpegQuality = jpegQuality,
            frameProducedNs = frameProducedNs,
            callbackStartNs = callbackStartNs,
        )
    }
}
