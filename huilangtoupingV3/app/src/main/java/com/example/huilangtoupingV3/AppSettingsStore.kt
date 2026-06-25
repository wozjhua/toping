package com.example.huilangtoupingV3

import android.content.Context
import android.content.SharedPreferences
import androidx.core.content.edit

data class AppSettingsSnapshot(
    val screenMirrorEnabled: Boolean = AppSettingsStore.DEFAULT_SCREEN_MIRROR_ENABLED,
    val screenMirrorSummary: String = AppSettingsStore.DEFAULT_SCREEN_MIRROR_SUMMARY,
    val screenMirrorCoreStatus: String = AppSettingsStore.DEFAULT_SCREEN_MIRROR_CORE_STATUS,
    val screenMirrorCaptureMode: Int = AppSettingsStore.DEFAULT_SCREEN_MIRROR_CAPTURE_MODE,
    val screenMirrorCropSize: Int = AppSettingsStore.DEFAULT_SCREEN_MIRROR_CROP_SIZE,
    val screenMirrorFullscreenPreset: Int = AppSettingsStore.DEFAULT_SCREEN_MIRROR_FULLSCREEN_PRESET,
    val screenMirrorJpegQuality: Int = AppSettingsStore.DEFAULT_SCREEN_MIRROR_JPEG_QUALITY,
    val screenMirrorKeepFrameRate: Boolean = AppSettingsStore.DEFAULT_SCREEN_MIRROR_KEEP_FRAME_RATE,
    val screenMirrorJpegSubsamplingMode: Int = AppSettingsStore.DEFAULT_SCREEN_MIRROR_JPEG_SUBSAMPLING_MODE,
    val screenMirrorFullscreenSplitParts: Int = AppSettingsStore.DEFAULT_SCREEN_MIRROR_FULLSCREEN_SPLIT_PARTS,
    val screenMirrorCropSplitParts: Int = AppSettingsStore.DEFAULT_SCREEN_MIRROR_CROP_SPLIT_PARTS,
    val screenMirrorFps: Int = AppSettingsStore.DEFAULT_SCREEN_MIRROR_FPS,
    val screenMirrorPort: Int = AppSettingsStore.DEFAULT_SCREEN_MIRROR_PORT,
)

class AppSettingsStore(context: Context) {
    private val prefs = context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)

    fun read(): AppSettingsSnapshot {
        return AppSettingsSnapshot(
            screenMirrorEnabled = prefs.getBoolean(KEY_SCREEN_MIRROR_ENABLED, DEFAULT_SCREEN_MIRROR_ENABLED),
            screenMirrorSummary = prefs.getString(KEY_SCREEN_MIRROR_SUMMARY, DEFAULT_SCREEN_MIRROR_SUMMARY)
                .orEmpty()
                .ifBlank { DEFAULT_SCREEN_MIRROR_SUMMARY },
            screenMirrorCoreStatus = prefs.getString(KEY_SCREEN_MIRROR_CORE_STATUS, DEFAULT_SCREEN_MIRROR_CORE_STATUS)
                .orEmpty()
                .ifBlank { DEFAULT_SCREEN_MIRROR_CORE_STATUS },
            screenMirrorCaptureMode = prefs.getInt(KEY_SCREEN_MIRROR_CAPTURE_MODE, DEFAULT_SCREEN_MIRROR_CAPTURE_MODE)
                .coerceIn(MIN_SCREEN_MIRROR_CAPTURE_MODE, MAX_SCREEN_MIRROR_CAPTURE_MODE),
            screenMirrorCropSize = prefs.getInt(KEY_SCREEN_MIRROR_CROP_SIZE, DEFAULT_SCREEN_MIRROR_CROP_SIZE)
                .coerceIn(MIN_SCREEN_MIRROR_CROP_SIZE, MAX_SCREEN_MIRROR_CROP_SIZE),
            screenMirrorFullscreenPreset = prefs.getInt(KEY_SCREEN_MIRROR_FULLSCREEN_PRESET, DEFAULT_SCREEN_MIRROR_FULLSCREEN_PRESET)
                .coerceIn(MIN_SCREEN_MIRROR_FULLSCREEN_PRESET, MAX_SCREEN_MIRROR_FULLSCREEN_PRESET),
            screenMirrorJpegQuality = prefs.getInt(KEY_SCREEN_MIRROR_JPEG_QUALITY, DEFAULT_SCREEN_MIRROR_JPEG_QUALITY)
                .coerceIn(MIN_SCREEN_MIRROR_JPEG_QUALITY, MAX_SCREEN_MIRROR_JPEG_QUALITY),
            screenMirrorKeepFrameRate = prefs.getBoolean(KEY_SCREEN_MIRROR_KEEP_FRAME_RATE, DEFAULT_SCREEN_MIRROR_KEEP_FRAME_RATE),
            screenMirrorJpegSubsamplingMode = normalizeJpegSubsamplingMode(
                prefs.getInt(KEY_SCREEN_MIRROR_JPEG_SUBSAMPLING_MODE, DEFAULT_SCREEN_MIRROR_JPEG_SUBSAMPLING_MODE)
            ),
            screenMirrorFullscreenSplitParts = prefs.getInt(KEY_SCREEN_MIRROR_FULLSCREEN_SPLIT_PARTS, DEFAULT_SCREEN_MIRROR_FULLSCREEN_SPLIT_PARTS)
                .coerceIn(MIN_SCREEN_MIRROR_FULLSCREEN_SPLIT_PARTS, MAX_SCREEN_MIRROR_FULLSCREEN_SPLIT_PARTS),
            screenMirrorCropSplitParts = prefs.getInt(KEY_SCREEN_MIRROR_CROP_SPLIT_PARTS, DEFAULT_SCREEN_MIRROR_CROP_SPLIT_PARTS)
                .coerceIn(MIN_SCREEN_MIRROR_CROP_SPLIT_PARTS, MAX_SCREEN_MIRROR_CROP_SPLIT_PARTS),
            screenMirrorFps = normalizeFpsPreset(
                prefs.getInt(KEY_SCREEN_MIRROR_FPS, DEFAULT_SCREEN_MIRROR_FPS)
            ),
            screenMirrorPort = prefs.getInt(KEY_SCREEN_MIRROR_PORT, DEFAULT_SCREEN_MIRROR_PORT)
                .coerceIn(MIN_SCREEN_MIRROR_PORT, MAX_SCREEN_MIRROR_PORT),
        )
    }

    fun registerListener(listener: SharedPreferences.OnSharedPreferenceChangeListener) {
        prefs.registerOnSharedPreferenceChangeListener(listener)
    }

    fun unregisterListener(listener: SharedPreferences.OnSharedPreferenceChangeListener) {
        prefs.unregisterOnSharedPreferenceChangeListener(listener)
    }

    fun setScreenMirrorEnabled(enabled: Boolean): AppSettingsSnapshot =
        persist(read().copy(screenMirrorEnabled = enabled))

    fun setScreenMirrorSummary(summary: String): AppSettingsSnapshot =
        persist(read().copy(screenMirrorSummary = summary.ifBlank { DEFAULT_SCREEN_MIRROR_SUMMARY }))

    fun setScreenMirrorCoreStatus(coreStatus: String): AppSettingsSnapshot =
        persist(read().copy(screenMirrorCoreStatus = coreStatus.ifBlank { DEFAULT_SCREEN_MIRROR_CORE_STATUS }))

    fun setScreenMirrorConfig(
        captureMode: Int = read().screenMirrorCaptureMode,
        cropSize: Int = read().screenMirrorCropSize,
        fullscreenPreset: Int = read().screenMirrorFullscreenPreset,
        jpegQuality: Int = read().screenMirrorJpegQuality,
        keepFrameRate: Boolean = read().screenMirrorKeepFrameRate,
        jpegSubsamplingMode: Int = read().screenMirrorJpegSubsamplingMode,
        fullscreenSplitParts: Int = read().screenMirrorFullscreenSplitParts,
        cropSplitParts: Int = read().screenMirrorCropSplitParts,
        fps: Int = read().screenMirrorFps,
        port: Int = read().screenMirrorPort,
    ): AppSettingsSnapshot =
        persist(
            read().copy(
                screenMirrorCaptureMode = captureMode.coerceIn(MIN_SCREEN_MIRROR_CAPTURE_MODE, MAX_SCREEN_MIRROR_CAPTURE_MODE),
                screenMirrorCropSize = cropSize.coerceIn(MIN_SCREEN_MIRROR_CROP_SIZE, MAX_SCREEN_MIRROR_CROP_SIZE),
                screenMirrorFullscreenPreset = fullscreenPreset.coerceIn(MIN_SCREEN_MIRROR_FULLSCREEN_PRESET, MAX_SCREEN_MIRROR_FULLSCREEN_PRESET),
                screenMirrorJpegQuality = jpegQuality.coerceIn(MIN_SCREEN_MIRROR_JPEG_QUALITY, MAX_SCREEN_MIRROR_JPEG_QUALITY),
                screenMirrorKeepFrameRate = keepFrameRate,
                screenMirrorJpegSubsamplingMode = normalizeJpegSubsamplingMode(jpegSubsamplingMode),
                screenMirrorFullscreenSplitParts = normalizeFullscreenSplitParts(fullscreenSplitParts),
                screenMirrorCropSplitParts = normalizeCropSplitParts(cropSplitParts),
                screenMirrorFps = normalizeFpsPreset(fps),
                screenMirrorPort = port.coerceIn(MIN_SCREEN_MIRROR_PORT, MAX_SCREEN_MIRROR_PORT),
            )
        )

    private fun persist(snapshot: AppSettingsSnapshot): AppSettingsSnapshot {
        prefs.edit {
            putBoolean(KEY_SCREEN_MIRROR_ENABLED, snapshot.screenMirrorEnabled)
            putString(KEY_SCREEN_MIRROR_SUMMARY, snapshot.screenMirrorSummary)
            putString(KEY_SCREEN_MIRROR_CORE_STATUS, snapshot.screenMirrorCoreStatus)
            putInt(KEY_SCREEN_MIRROR_CAPTURE_MODE, snapshot.screenMirrorCaptureMode)
            putInt(KEY_SCREEN_MIRROR_CROP_SIZE, snapshot.screenMirrorCropSize)
            putInt(KEY_SCREEN_MIRROR_FULLSCREEN_PRESET, snapshot.screenMirrorFullscreenPreset)
            putInt(KEY_SCREEN_MIRROR_JPEG_QUALITY, snapshot.screenMirrorJpegQuality)
            putBoolean(KEY_SCREEN_MIRROR_KEEP_FRAME_RATE, snapshot.screenMirrorKeepFrameRate)
            putInt(KEY_SCREEN_MIRROR_JPEG_SUBSAMPLING_MODE, snapshot.screenMirrorJpegSubsamplingMode)
            putInt(KEY_SCREEN_MIRROR_FULLSCREEN_SPLIT_PARTS, snapshot.screenMirrorFullscreenSplitParts)
            putInt(KEY_SCREEN_MIRROR_CROP_SPLIT_PARTS, snapshot.screenMirrorCropSplitParts)
            putInt(KEY_SCREEN_MIRROR_FPS, snapshot.screenMirrorFps)
            putInt(KEY_SCREEN_MIRROR_PORT, snapshot.screenMirrorPort)
        }
        return snapshot
    }

    companion object {
        private const val PREFS_NAME = "screen_mirror_settings"
        private const val KEY_SCREEN_MIRROR_ENABLED = "screen_mirror_enabled"
        private const val KEY_SCREEN_MIRROR_SUMMARY = "screen_mirror_summary"
        private const val KEY_SCREEN_MIRROR_CORE_STATUS = "screen_mirror_core_status"
        private const val KEY_SCREEN_MIRROR_CAPTURE_MODE = "screen_mirror_capture_mode"
        private const val KEY_SCREEN_MIRROR_CROP_SIZE = "screen_mirror_crop_size"
        private const val KEY_SCREEN_MIRROR_FULLSCREEN_PRESET = "screen_mirror_fullscreen_preset"
        private const val KEY_SCREEN_MIRROR_JPEG_QUALITY = "screen_mirror_jpeg_quality"
        private const val KEY_SCREEN_MIRROR_KEEP_FRAME_RATE = "screen_mirror_keep_frame_rate"
        private const val KEY_SCREEN_MIRROR_JPEG_SUBSAMPLING_MODE = "screen_mirror_jpeg_subsampling_mode"
        private const val KEY_SCREEN_MIRROR_FULLSCREEN_SPLIT_PARTS = "screen_mirror_fullscreen_split_parts"
        private const val KEY_SCREEN_MIRROR_CROP_SPLIT_PARTS = "screen_mirror_crop_split_parts"
        private const val KEY_SCREEN_MIRROR_FPS = "screen_mirror_fps"
        private const val KEY_SCREEN_MIRROR_PORT = "screen_mirror_port"

        const val SCREEN_MIRROR_CAPTURE_MODE_CENTER_CROP = 0
        const val SCREEN_MIRROR_CAPTURE_MODE_FULLSCREEN_SCALED = 1

        const val SCREEN_MIRROR_FULLSCREEN_PRESET_720P = 0
        const val SCREEN_MIRROR_FULLSCREEN_PRESET_900P = 1
        const val SCREEN_MIRROR_FULLSCREEN_PRESET_1080P = 2
        const val SCREEN_MIRROR_FULLSCREEN_PRESET_2K = 3
        const val SCREEN_MIRROR_FULLSCREEN_PRESET_3K = 4
        const val SCREEN_MIRROR_FULLSCREEN_PRESET_DEVICE_MAX = 5

        const val SCREEN_MIRROR_JPEG_SUBSAMPLING_420 = 420
        const val SCREEN_MIRROR_JPEG_SUBSAMPLING_444 = 444

        const val DEFAULT_SCREEN_MIRROR_ENABLED = false
        const val DEFAULT_SCREEN_MIRROR_SUMMARY = "画面传输：未启动"
        const val DEFAULT_SCREEN_MIRROR_CORE_STATUS = "核心状态：未启动"
        const val DEFAULT_SCREEN_MIRROR_CAPTURE_MODE = SCREEN_MIRROR_CAPTURE_MODE_FULLSCREEN_SCALED
        const val DEFAULT_SCREEN_MIRROR_CROP_SIZE = 640
        const val DEFAULT_SCREEN_MIRROR_FULLSCREEN_PRESET = SCREEN_MIRROR_FULLSCREEN_PRESET_1080P
        const val DEFAULT_SCREEN_MIRROR_JPEG_QUALITY = 100
        const val DEFAULT_SCREEN_MIRROR_KEEP_FRAME_RATE = true
        const val DEFAULT_SCREEN_MIRROR_JPEG_SUBSAMPLING_MODE = SCREEN_MIRROR_JPEG_SUBSAMPLING_420
        const val DEFAULT_SCREEN_MIRROR_FULLSCREEN_SPLIT_PARTS = 7
        const val DEFAULT_SCREEN_MIRROR_CROP_SPLIT_PARTS = 2
        const val DEFAULT_SCREEN_MIRROR_FPS = 144
        const val DEFAULT_SCREEN_MIRROR_PORT = 27183

        val SCREEN_MIRROR_FPS_PRESETS = intArrayOf(30, 60, 90, 120, 144, 165)

        const val MIN_SCREEN_MIRROR_CAPTURE_MODE = SCREEN_MIRROR_CAPTURE_MODE_CENTER_CROP
        const val MAX_SCREEN_MIRROR_CAPTURE_MODE = SCREEN_MIRROR_CAPTURE_MODE_FULLSCREEN_SCALED
        const val MIN_SCREEN_MIRROR_FULLSCREEN_PRESET = SCREEN_MIRROR_FULLSCREEN_PRESET_720P
        const val MAX_SCREEN_MIRROR_FULLSCREEN_PRESET = SCREEN_MIRROR_FULLSCREEN_PRESET_DEVICE_MAX
        const val MIN_SCREEN_MIRROR_CROP_SIZE = 320
        const val MAX_SCREEN_MIRROR_CROP_SIZE = 1080
        const val MIN_SCREEN_MIRROR_JPEG_QUALITY = 30
        const val MAX_SCREEN_MIRROR_JPEG_QUALITY = 100
        const val MIN_SCREEN_MIRROR_FULLSCREEN_SPLIT_PARTS = 2
        const val MAX_SCREEN_MIRROR_FULLSCREEN_SPLIT_PARTS = 16
        const val MIN_SCREEN_MIRROR_CROP_SPLIT_PARTS = 1
        const val MAX_SCREEN_MIRROR_CROP_SPLIT_PARTS = 4
        const val MIN_SCREEN_MIRROR_FPS = 30
        const val MAX_SCREEN_MIRROR_FPS = 165
        const val MIN_SCREEN_MIRROR_PORT = 1024
        const val MAX_SCREEN_MIRROR_PORT = 65535

        fun normalizeJpegSubsamplingMode(mode: Int): Int {
            return if (mode == SCREEN_MIRROR_JPEG_SUBSAMPLING_444) {
                SCREEN_MIRROR_JPEG_SUBSAMPLING_444
            } else {
                SCREEN_MIRROR_JPEG_SUBSAMPLING_420
            }
        }

        fun jpegSubsamplingLabel(mode: Int): String {
            return if (normalizeJpegSubsamplingMode(mode) == SCREEN_MIRROR_JPEG_SUBSAMPLING_444) {
                "4:4:4极限"
            } else {
                "4:2:0高质量"
            }
        }

        fun normalizeFullscreenSplitParts(parts: Int): Int {
            return parts.coerceIn(MIN_SCREEN_MIRROR_FULLSCREEN_SPLIT_PARTS, MAX_SCREEN_MIRROR_FULLSCREEN_SPLIT_PARTS)
        }

        fun normalizeCropSplitParts(parts: Int): Int {
            return parts.coerceIn(MIN_SCREEN_MIRROR_CROP_SPLIT_PARTS, MAX_SCREEN_MIRROR_CROP_SPLIT_PARTS)
        }

        fun progressForFullscreenSplitParts(parts: Int): Int {
            return normalizeFullscreenSplitParts(parts) - MIN_SCREEN_MIRROR_FULLSCREEN_SPLIT_PARTS
        }

        fun fullscreenSplitPartsForProgress(progress: Int): Int {
            return MIN_SCREEN_MIRROR_FULLSCREEN_SPLIT_PARTS + progress.coerceIn(0, MAX_SCREEN_MIRROR_FULLSCREEN_SPLIT_PARTS - MIN_SCREEN_MIRROR_FULLSCREEN_SPLIT_PARTS)
        }

        fun progressForCropSplitParts(parts: Int): Int {
            return normalizeCropSplitParts(parts) - MIN_SCREEN_MIRROR_CROP_SPLIT_PARTS
        }

        fun cropSplitPartsForProgress(progress: Int): Int {
            return MIN_SCREEN_MIRROR_CROP_SPLIT_PARTS + progress.coerceIn(0, MAX_SCREEN_MIRROR_CROP_SPLIT_PARTS - MIN_SCREEN_MIRROR_CROP_SPLIT_PARTS)
        }

        fun normalizeFpsPreset(fps: Int): Int {
            return SCREEN_MIRROR_FPS_PRESETS.minByOrNull { kotlin.math.abs(it - fps) }
                ?: DEFAULT_SCREEN_MIRROR_FPS
        }

        fun fpsForProgress(progress: Int): Int {
            return SCREEN_MIRROR_FPS_PRESETS[
                progress.coerceIn(0, SCREEN_MIRROR_FPS_PRESETS.lastIndex)
            ]
        }

        fun progressForFps(fps: Int): Int {
            val normalized = normalizeFpsPreset(fps)
            return SCREEN_MIRROR_FPS_PRESETS.indexOf(normalized).coerceAtLeast(0)
        }

        fun fullscreenPresetLabel(preset: Int): String {
            return when (preset) {
                SCREEN_MIRROR_FULLSCREEN_PRESET_900P -> "1600×900"
                SCREEN_MIRROR_FULLSCREEN_PRESET_1080P -> "1920×1080"
                SCREEN_MIRROR_FULLSCREEN_PRESET_2K -> "长边2560"
                SCREEN_MIRROR_FULLSCREEN_PRESET_3K -> "长边3200(3K)"
                SCREEN_MIRROR_FULLSCREEN_PRESET_DEVICE_MAX -> "设备最大"
                else -> "1280×720"
            }
        }
    }
}
