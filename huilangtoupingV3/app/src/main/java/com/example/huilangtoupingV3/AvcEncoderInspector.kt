package com.example.huilangtoupingV3

import android.media.MediaCodecInfo
import android.media.MediaCodecList
import android.media.MediaFormat
import android.os.Build

object AvcEncoderInspector {
    private enum class ChipFamily {
        Qualcomm,
        Mtk,
        Samsung,
        Hisilicon,
        Unisoc,
        Unknown,
    }

    data class Candidate(
        val name: String,
        val surfaceInput: Boolean,
        val softwareLikely: Boolean,
        val vendorHardwareLikely: Boolean,
        val priority: Int,
        val role: String,
    ) {
        val selectable: Boolean
            get() = surfaceInput && !softwareLikely && priority < PRIORITY_NOT_SELECTABLE
    }

    data class Report(
        val selectedName: String?,
        val candidates: List<Candidate>,
        val deviceFamily: String,
        val deviceHint: String,
        val error: String? = null,
    ) {
        fun toLogString(maxItems: Int = 16): String {
            if (error != null) return "device=$deviceFamily hint=$deviceHint error=$error"
            if (candidates.isEmpty()) return "device=$deviceFamily hint=$deviceHint none"
            return "device=$deviceFamily hint=$deviceHint " +
                    candidates.take(maxItems).joinToString(" ") { c ->
                        "[${c.name} role=${c.role} surface=${c.surfaceInput} sw=${c.softwareLikely} prio=${c.priority}]"
                    } + if (candidates.size > maxItems) " ... total=${candidates.size}" else ""
        }

        fun toMainUiText(maxItems: Int = 8): String {
            if (error != null) {
                return "H.264编码器：读取失败\n$error"
            }

            val selected = selectedName ?: "未找到可用硬件 AVC Surface 编码器"
            val sb = StringBuilder()
            sb.append("H.264编码器：").append(selected).append('\n')
            sb.append("芯片判断：").append(deviceFamily)
            if (deviceHint.isNotBlank()) sb.append("（").append(deviceHint).append("）")
            sb.append('\n')
            sb.append("选择策略：高通优先 C2/QTI；天玑优先 C2/MTK；否则优先 C2 硬编\n")

            if (candidates.isEmpty()) {
                sb.append("设备未列出 video/avc 编码器")
                return sb.toString()
            }

            sb.append("设备支持：")
            val shown = candidates.take(maxItems)
            for ((index, c) in shown.withIndex()) {
                if (index > 0) sb.append("；")
                sb.append(c.role).append(' ').append(c.name)
                if (!c.surfaceInput) sb.append("(无Surface)")
                if (c.softwareLikely) sb.append("(软件)")
            }
            if (candidates.size > maxItems) {
                sb.append("；…共").append(candidates.size).append("个")
            }
            return sb.toString()
        }
    }

    fun inspect(): Report {
        val chip = detectChipFamily()
        val deviceHint = buildDeviceHint()
        return try {
            val list = MediaCodecList(MediaCodecList.ALL_CODECS)
            val candidates = ArrayList<Candidate>()

            for (info in list.codecInfos) {
                if (!info.isEncoder) continue
                if (!info.supportedTypes.any { it.equals(MediaFormat.MIMETYPE_VIDEO_AVC, ignoreCase = true) }) continue

                val caps = runCatching { info.getCapabilitiesForType(MediaFormat.MIMETYPE_VIDEO_AVC) }.getOrNull()
                val surfaceInput = caps?.colorFormats?.contains(MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface) == true
                val software = isSoftwareEncoderName(info.name)
                val vendor = isVendorHardwareEncoderName(info.name)
                val role = roleForName(info.name, software, vendor)
                candidates += Candidate(
                    name = info.name,
                    surfaceInput = surfaceInput,
                    softwareLikely = software,
                    vendorHardwareLikely = vendor,
                    priority = priorityForName(info.name, software, vendor, surfaceInput, chip),
                    role = role,
                )
            }

            val sorted = candidates.sortedWith(
                compareBy<Candidate> { it.priority }
                    .thenBy { it.softwareLikely }
                    .thenBy { it.name.lowercase() }
            )
            Report(
                selectedName = sorted.firstOrNull { it.selectable }?.name,
                candidates = sorted,
                deviceFamily = chipToUiName(chip),
                deviceHint = deviceHint,
            )
        } catch (t: Throwable) {
            Report(
                selectedName = null,
                candidates = emptyList(),
                deviceFamily = chipToUiName(chip),
                deviceHint = deviceHint,
                error = t.javaClass.simpleName + ": " + (t.message ?: "unknown"),
            )
        }
    }

    fun selectHardwareAvcEncoderName(): String? {
        return inspect().selectedName
    }

    fun isSoftwareEncoderName(name: String): Boolean {
        val n = name.lowercase()
        return n.contains("google") ||
                n.contains("android") ||
                n.contains("software") ||
                n.contains("ffmpeg") ||
                n.contains("sw")
    }

    fun isVendorHardwareEncoderName(name: String): Boolean {
        val n = name.lowercase()
        return n.contains("qcom") ||
                n.contains("qti") ||
                n.contains("qualcomm") ||
                n.contains("mtk") ||
                n.contains("mediatek") ||
                n.contains("exynos") ||
                n.contains("samsung") ||
                n.contains("sec") ||
                n.contains("hisi") ||
                n.contains("kirin") ||
                n.contains("unisoc") ||
                n.contains("sprd") ||
                n.contains("rockchip") ||
                n.contains("rk")
    }

    private fun priorityForName(
        name: String,
        software: Boolean,
        vendor: Boolean,
        surfaceInput: Boolean,
        chip: ChipFamily,
    ): Int {
        if (!surfaceInput) return PRIORITY_NOT_SELECTABLE + 200
        if (software) return PRIORITY_NOT_SELECTABLE + 100

        val n = name.lowercase()
        val isQcom = n.contains("qcom") || n.contains("qti") || n.contains("qualcomm")
        val isMtk = n.contains("mtk") || n.contains("mediatek")
        val isC2 = n.startsWith("c2.") || n.contains(".c2.")
        val isOmx = n.startsWith("omx.") || n.contains(".omx.")

        return when (chip) {
            ChipFamily.Qualcomm -> when {
                n == "c2.qti.avc.encoder" -> 1
                n == "c2.qcom.avc.encoder" -> 2
                isQcom && isC2 -> 3
                n == "omx.qcom.video.encoder.avc" -> 20
                isQcom -> 30
                vendor && isC2 -> 200
                vendor -> 240
                else -> PRIORITY_NOT_SELECTABLE
            }

            ChipFamily.Mtk -> when {
                n == "c2.mtk.avc.encoder" -> 1
                isMtk && isC2 -> 2
                n == "omx.mtk.video.encoder.avc" -> 20
                isMtk -> 30
                vendor && isC2 -> 200
                vendor -> 240
                else -> PRIORITY_NOT_SELECTABLE
            }

            else -> when {
                // 未识别芯片时，也优先 C2 硬编；避免高通设备同时列出 OMX/C2 时错误选择 OMX。
                n == "c2.qti.avc.encoder" -> 10
                n == "c2.qcom.avc.encoder" -> 11
                n == "c2.mtk.avc.encoder" -> 20
                isQcom && isC2 -> 30
                isMtk && isC2 -> 40
                vendor && isC2 -> 50
                n == "omx.qcom.video.encoder.avc" -> 120
                n == "omx.mtk.video.encoder.avc" -> 130
                isQcom -> 140
                isMtk -> 150
                vendor && isOmx -> 180
                vendor -> 220
                else -> PRIORITY_NOT_SELECTABLE
            }
        }
    }

    private fun roleForName(name: String, software: Boolean, vendor: Boolean): String {
        val n = name.lowercase()
        return when {
            n.contains("qcom") || n.contains("qti") || n.contains("qualcomm") -> "高通"
            n.contains("mtk") || n.contains("mediatek") -> "MTK"
            n.contains("exynos") || n.contains("samsung") || n.contains("sec") -> "三星"
            n.contains("hisi") || n.contains("kirin") -> "麒麟"
            n.contains("unisoc") || n.contains("sprd") -> "展锐"
            n.contains("rockchip") || n.contains("rk") -> "瑞芯微"
            software -> "软件"
            vendor -> "硬编"
            else -> "未知"
        }
    }

    private fun detectChipFamily(): ChipFamily {
        val text = buildDeviceHint().lowercase()
        return when {
            text.contains("qcom") ||
                    text.contains("qti") ||
                    text.contains("qualcomm") ||
                    text.contains("snapdragon") ||
                    text.contains("sm") && text.any { it.isDigit() } -> ChipFamily.Qualcomm

            text.contains("mtk") ||
                    text.contains("mediatek") ||
                    text.contains("dimensity") ||
                    text.contains("天玑") ||
                    text.contains("mt") && text.any { it.isDigit() } -> ChipFamily.Mtk

            text.contains("exynos") || text.contains("samsung") -> ChipFamily.Samsung
            text.contains("kirin") || text.contains("hisi") || text.contains("hisilicon") -> ChipFamily.Hisilicon
            text.contains("unisoc") || text.contains("sprd") -> ChipFamily.Unisoc
            else -> ChipFamily.Unknown
        }
    }

    private fun buildDeviceHint(): String {
        val parts = ArrayList<String>()
        fun add(v: String?) {
            val s = v?.trim().orEmpty()
            if (s.isNotEmpty() && s != "unknown" && parts.none { it.equals(s, ignoreCase = true) }) {
                parts += s
            }
        }

        if (Build.VERSION.SDK_INT >= 31) {
            runCatching { add(Build.SOC_MANUFACTURER) }
            runCatching { add(Build.SOC_MODEL) }
        }
        add(Build.HARDWARE)
        add(Build.BOARD)
        add(Build.DEVICE)
        add(Build.PRODUCT)
        add(Build.MANUFACTURER)
        add(Build.BRAND)
        return parts.take(8).joinToString("/")
    }

    private fun chipToUiName(chip: ChipFamily): String {
        return when (chip) {
            ChipFamily.Qualcomm -> "高通/Qualcomm"
            ChipFamily.Mtk -> "天玑/MTK"
            ChipFamily.Samsung -> "三星/Exynos"
            ChipFamily.Hisilicon -> "麒麟/HiSilicon"
            ChipFamily.Unisoc -> "展锐/Unisoc"
            ChipFamily.Unknown -> "未知"
        }
    }

    private const val PRIORITY_NOT_SELECTABLE = 1000
}
