#include <jni.h>
#include <android/log.h>
#include <cstdint>
#include <vector>
#include <array>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <cstring>
#include <sys/uio.h>
#include <sched.h>
#include <sys/syscall.h>
#include <sys/resource.h>
#include <sys/prctl.h>
#include <cstdio>
#include <thread>
#include <condition_variable>
#include <cmath>
#include "native_center_crop_jpeg.h"
#include <turbojpeg.h>

#define LOG_TAG "NativeCenterCropJpeg"
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// 正式低延迟模式默认关闭 native 详细日志，避免 logcat 写入造成偶发调度尖刺。
// 调试裁剪路径时可临时改成 true。
static constexpr bool ENABLE_NATIVE_DEBUG_LOGS = false;

static constexpr int32_t FRAME_MAGIC = 0x484C4D32; // HLM2
static constexpr int32_t HEADER_VERSION = 2;
static constexpr int32_t HEADER_VERSION_SPLIT2 = 3;
static constexpr int32_t HEADER_VERSION_SPLIT4 = 4;
static constexpr int32_t HEADER_VERSION_SPLIT5 = 5;
static constexpr int32_t HEADER_VERSION_SPLIT6 = 6; // legacy v5 extension; current Android sender always sends JPEG payload
static constexpr int32_t HEADER_VERSION_SPLIT7 = 7; // v6 + rectangular ROI split (partLeft/partWidth)
static constexpr int32_t HEADER_VERSION_SPLIT8 = 8; // v7 + edge ROI encoded at lower horizontal resolution
static constexpr int32_t HEADER_VERSION_SPLIT9 = 9; // v8 + per-part Android dispatch/write timing diagnostics
static constexpr size_t HEADER_SIZE = 68;
static constexpr size_t HEADER_V3_SIZE = 100;
static constexpr size_t HEADER_V4_SIZE = 108;
static constexpr size_t HEADER_V5_SIZE = 112;
static constexpr size_t HEADER_V7_SIZE = 120;
static constexpr size_t HEADER_V8_SIZE = 120; // Same 68+52 layout as v7; base width/height are encoded size, extra width/height are display size.
static constexpr size_t HEADER_V9_SIZE = 156; // v8 + 36 bytes diagnostics: worker/order/dispatch/write timing.
// 编译上限只是安全边界；实际可用分块数启动时按 CPU 核心数量动态计算：
// 可用编码核心 = 在线 CPU 数 - 回调线程保留核心。
static constexpr int MAX_SPLIT_PARTS = 24;

struct ThreadLocalJpegState {
    tjhandle compressor = nullptr;
    std::vector<uint8_t> rgbBuffer;
    std::vector<uint8_t> yuv420Buffer;
    std::vector<int> scaleXOffsets;
    int scaleXSourceWidth = 0;
    int scaleXEncodedWidth = 0;
    int scaleXPixelSize = 0;
    unsigned char* jpegBuffer = nullptr;
    unsigned long jpegCapacity = 0;

    ~ThreadLocalJpegState() {
        if (compressor != nullptr) {
            tjDestroy(compressor);
            compressor = nullptr;
        }
        if (jpegBuffer != nullptr) {
            tjFree(jpegBuffer);
            jpegBuffer = nullptr;
            jpegCapacity = 0;
        }
    }
};

static thread_local ThreadLocalJpegState g_tls;

static std::mutex g_send_mutex;
static int g_output_fd = -1;
static std::atomic<bool> g_output_fd_available{false};
static std::atomic<int64_t> g_last_sent_frame_produced_ns{0};

// Last successfully sent frame statistics for Kotlin ScreenPerf.
// Values are per complete frame. For split frames, part0/part1 are upper-half/lower-half aggregates.
static std::atomic<int64_t> g_last_sent_total_bytes{0};
static std::atomic<int64_t> g_last_sent_part0_bytes{0};
static std::atomic<int64_t> g_last_sent_part1_bytes{0};
static std::atomic<int64_t> g_last_socket_write_ns{0};
static std::atomic<int64_t> g_last_part0_encode_ns{0};
static std::atomic<int64_t> g_last_part1_encode_ns{0};
static std::atomic<int64_t> g_last_split_parts{1};

// 真实分块统计：每个 part 的大小、编码耗时、实际运行 CPU。
// Kotlin 侧用它输出中文日志，避免把 p0/p1 误解成两个 native 编码线程。
static std::atomic<int64_t> g_last_split_part_bytes[MAX_SPLIT_PARTS]{};
static std::atomic<int64_t> g_last_split_part_encode_ns[MAX_SPLIT_PARTS]{};
static std::atomic<int64_t> g_last_split_part_cpu[MAX_SPLIT_PARTS]{};

// 中心裁剪路径标记：用于 logcat 快速确认当前是否真正走零拷贝裁剪。
// 0=unknown, 1=zero-copy, 2=copy fallback。
static int64_t wall_now_ms();
static std::atomic<int> g_last_center_crop_path{0};
static std::atomic<int64_t> g_center_crop_zero_copy_count{0};
static std::atomic<int64_t> g_center_crop_copy_count{0};
static std::atomic<int64_t> g_last_center_crop_path_log_ms{0};
static std::atomic<int> g_last_logged_center_crop_path{0};

static const char* center_crop_path_name(int path) {
    switch (path) {
        case 1: return "zero-copy";
        case 2: return "copy";
        default: return "unknown";
    }
}

static void mark_center_crop_path(bool zeroCopy, const char* reason) {
    const int path = zeroCopy ? 1 : 2;
    g_last_center_crop_path.store(path, std::memory_order_release);
    const int64_t zc = zeroCopy
            ? g_center_crop_zero_copy_count.fetch_add(1, std::memory_order_acq_rel) + 1
            : g_center_crop_zero_copy_count.load(std::memory_order_acquire);
    const int64_t cp = zeroCopy
            ? g_center_crop_copy_count.load(std::memory_order_acquire)
            : g_center_crop_copy_count.fetch_add(1, std::memory_order_acq_rel) + 1;

    // 不每帧刷屏：路径切换时立即输出；否则最多 1 秒输出一次。
    const int64_t nowMs = wall_now_ms();
    const int64_t lastMs = g_last_center_crop_path_log_ms.load(std::memory_order_acquire);
    const int lastPath = g_last_logged_center_crop_path.load(std::memory_order_acquire);
    if (ENABLE_NATIVE_DEBUG_LOGS && (path != lastPath || nowMs - lastMs >= 1000)) {
        g_last_center_crop_path_log_ms.store(nowMs, std::memory_order_release);
        g_last_logged_center_crop_path.store(path, std::memory_order_release);
        ALOGE("裁剪路径=%s reason=%s zeroCopy=%lld fallbackCopy=%lld",
              center_crop_path_name(path),
              reason != nullptr ? reason : "-",
              static_cast<long long>(zc),
              static_cast<long long>(cp));
    }
}

// 回调线程保留核心：native 分块 worker 不再占用这个 CPU。
// Kotlin 会在绑定 ImageReader 回调线程前后写入这个值；常见为 CPU0。
static std::atomic<int> g_reserved_callback_cpu{-1};

// PC 端 F2 窗口下发的手动分块权重。
// 权重按当前分块顺序生效：块0->第一个编码核心，块1->第二个编码核心...
// Android 侧再次做限制，避免 PC 误发导致某块过小/过大。
static std::mutex g_manual_split_weight_mutex;
static bool g_manual_split_weight_enabled = false;
static int g_manual_split_weight_count = 0;
static double g_manual_split_weights[MAX_SPLIT_PARTS]{};

// PC 端新版 F2 面板按 CPU 调权重，而不是按块号。
// 这样块0当前跑在 CPU7 时，调的就是 CPU7；下次如果块序或 worker 调度变化，也不会错绑。
static bool g_manual_cpu_weight_enabled = false;
static int g_manual_cpu_weight_count = 0;
static int g_manual_cpu_ids[MAX_SPLIT_PARTS]{};
static double g_manual_cpu_weights[MAX_SPLIT_PARTS]{};

static constexpr double MANUAL_SPLIT_WEIGHT_MIN = 0.70;
static constexpr double MANUAL_SPLIT_WEIGHT_MAX = 1.30;

// JPEG 色度采样模式：
// 420 = 高质量模式，体积和性能更稳；444 = 极限保真模式，颜色不降采样，体积/编码/解码压力更高。
// 当前主要用于 RGBA -> JPEG 路径；YUV420 输入路径仍然按 420 发送。
static std::atomic<int> g_jpeg_subsampling_mode{420};

static int current_jpeg_subsamp() {
    const int mode = g_jpeg_subsampling_mode.load(std::memory_order_acquire);
    return mode == 444 ? TJSAMP_444 : TJSAMP_420;
}


static double clamp_manual_split_weight(double v) {
    if (!(v > 0.0) || v != v) return 1.0;
    return std::max(MANUAL_SPLIT_WEIGHT_MIN, std::min(MANUAL_SPLIT_WEIGHT_MAX, v));
}

static bool copy_manual_split_weights_if_match(int partCount, const std::vector<int>& cpus, double outWeights[]) {
    if (partCount <= 1 || partCount > MAX_SPLIT_PARTS || outWeights == nullptr) return false;
    std::lock_guard<std::mutex> lk(g_manual_split_weight_mutex);

    if (g_manual_cpu_weight_enabled && g_manual_cpu_weight_count > 0 &&
        static_cast<int>(cpus.size()) >= partCount) {
        double sum = 0.0;
        for (int i = 0; i < partCount; ++i) {
            const int cpuId = cpus[static_cast<size_t>(i)];
            double w = 0.0;
            for (int j = 0; j < g_manual_cpu_weight_count; ++j) {
                if (g_manual_cpu_ids[j] == cpuId) {
                    w = clamp_manual_split_weight(g_manual_cpu_weights[j]);
                    break;
                }
            }
            if (!(w > 0.0)) return false;
            outWeights[i] = w;
            sum += w;
        }
        return sum > 0.0;
    }

    if (!g_manual_split_weight_enabled || g_manual_split_weight_count != partCount) return false;
    double sum = 0.0;
    for (int i = 0; i < partCount; ++i) {
        outWeights[i] = clamp_manual_split_weight(g_manual_split_weights[i]);
        sum += outWeights[i];
    }
    return sum > 0.0;
}

static int64_t mono_now_ns() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000000LL + static_cast<int64_t>(ts.tv_nsec);
}

static int64_t wall_now_ms() {
    timespec ts{};
    clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000LL + static_cast<int64_t>(ts.tv_nsec) / 1000000LL;
}

static tjhandle get_compressor() {
    if (g_tls.compressor == nullptr) {
        g_tls.compressor = tjInitCompress();
    }
    return g_tls.compressor;
}

static bool ensure_jpeg_buffer(unsigned long requiredSize) {
    if (requiredSize == 0) return false;
    if (g_tls.jpegBuffer != nullptr && g_tls.jpegCapacity >= requiredSize) {
        return true;
    }

    auto* newBuffer = static_cast<unsigned char*>(tjAlloc(static_cast<int>(requiredSize)));
    if (newBuffer == nullptr) {
        ALOGE("tjAlloc jpeg buffer failed size=%lu", requiredSize);
        return false;
    }

    if (g_tls.jpegBuffer != nullptr) {
        tjFree(g_tls.jpegBuffer);
    }
    g_tls.jpegBuffer = newBuffer;
    g_tls.jpegCapacity = requiredSize;
    return true;
}

static inline int tj_pixel_size_for_format(int pixelFormat) {
    switch (pixelFormat) {
        case TJPF_RGB:
        case TJPF_BGR:
            return 3;
        case TJPF_RGBX:
        case TJPF_BGRX:
        case TJPF_XBGR:
        case TJPF_XRGB:
        case TJPF_RGBA:
        case TJPF_BGRA:
        case TJPF_ABGR:
        case TJPF_ARGB:
            return 4;
        case TJPF_GRAY:
            return 1;
        default:
            return 0;
    }
}

static inline int normalize_pitch_for_tj(int width, int pitch, int pixelFormat) {
    const int pixelSize = tj_pixel_size_for_format(pixelFormat);
    if (pixelSize > 0 && pitch == width * pixelSize) {
        return 0;
    }
    return pitch;
}

static constexpr int64_t FRAME_TIMESTAMP_REWIND_RESET_NS = 500000000LL;

static inline bool is_stale_for_encode(int64_t frameProducedNs) {
    if (frameProducedNs <= 0) return false;
    const int64_t last = g_last_sent_frame_produced_ns.load(std::memory_order_acquire);
    if (last <= 0 || frameProducedNs > last) return false;

    // ImageReader / VirtualDisplay reconfiguration may restart or rewind Image.timestamp.
    // Treat a large rewind as a new capture timeline instead of dropping every new frame as stale.
    if (last - frameProducedNs > FRAME_TIMESTAMP_REWIND_RESET_NS) {
        g_last_sent_frame_produced_ns.store(0, std::memory_order_release);
        return false;
    }
    return true;
}

struct CpuCandidate {
    int id = -1;
    int64_t maxFreqKhz = 0;
};

static bool read_i64_file(const char* path, int64_t& outValue) {
    FILE* fp = fopen(path, "r");
    if (fp == nullptr) return false;
    long long value = 0;
    const int ok = fscanf(fp, "%lld", &value);
    fclose(fp);
    if (ok != 1) return false;
    outValue = static_cast<int64_t>(value);
    return true;
}

static bool cpu_is_online(int cpuId) {
    if (cpuId < 0) return false;
    if (cpuId == 0) return true;
    char path[128]{};
    snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/online", cpuId);
    int64_t online = 1;
    if (!read_i64_file(path, online)) {
        // Some kernels do not expose cpu0/online or restrict this file. Treat as usable.
        return true;
    }
    return online != 0;
}

static int64_t cpu_max_freq_khz(int cpuId) {
    if (cpuId < 0) return 0;
    char path[160]{};
    int64_t value = 0;

    snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", cpuId);
    if (read_i64_file(path, value) && value > 0) return value;

    snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_max_freq", cpuId);
    if (read_i64_file(path, value) && value > 0) return value;

    // Fallback: big cores are usually higher CPU ids on Android big.LITTLE devices.
    return static_cast<int64_t>(cpuId + 1);
}

static int current_cpu_id() {
    unsigned cpu = 0;
#if defined(SYS_getcpu)
    if (syscall(SYS_getcpu, &cpu, nullptr, nullptr) == 0) {
        return static_cast<int>(cpu);
    }
#elif defined(__NR_getcpu)
    if (syscall(__NR_getcpu, &cpu, nullptr, nullptr) == 0) {
        return static_cast<int>(cpu);
    }
#endif
    return -1;
}

static inline int make_even_floor(int value);

static const std::vector<CpuCandidate>& sorted_cpu_candidates_cached() {
    static std::once_flag once;
    static std::vector<CpuCandidate> candidates;
    std::call_once(once, [] {
        long cpuCount = sysconf(_SC_NPROCESSORS_CONF);
        if (cpuCount <= 0) cpuCount = sysconf(_SC_NPROCESSORS_ONLN);
        if (cpuCount <= 0) cpuCount = 8;
        if (cpuCount > CPU_SETSIZE) cpuCount = CPU_SETSIZE;

        candidates.reserve(static_cast<size_t>(cpuCount));
        for (int cpu = 0; cpu < static_cast<int>(cpuCount); ++cpu) {
            if (!cpu_is_online(cpu)) continue;
            candidates.push_back(CpuCandidate{cpu, cpu_max_freq_khz(cpu)});
        }

        std::sort(candidates.begin(), candidates.end(), [](const CpuCandidate& a, const CpuCandidate& b) {
            if (a.maxFreqKhz != b.maxFreqKhz) return a.maxFreqKhz > b.maxFreqKhz;
            return a.id > b.id;
        });
    });
    return candidates;
}

static int64_t cached_cpu_max_freq_khz(int cpuId) {
    for (const auto& cpu : sorted_cpu_candidates_cached()) {
        if (cpu.id == cpuId) return cpu.maxFreqKhz;
    }
    return cpu_max_freq_khz(cpuId);
}

static std::vector<int> choose_encode_cpu_ids(int requestedCount, int reservedCpu = -1) {
    std::vector<int> result;
    if (requestedCount <= 0) return result;

    for (const auto& cpu : sorted_cpu_candidates_cached()) {
        if (cpu.id == reservedCpu) continue;
        if (static_cast<int>(result.size()) >= requestedCount) break;
        result.push_back(cpu.id);
    }
    return result;
}

// 每帧热路径用固定数组填充，避免反复构造 std::vector。
static int fill_encode_cpu_ids(int requestedCount, int reservedCpu, int outIds[]) {
    if (outIds == nullptr || requestedCount <= 0) return 0;
    int n = 0;
    for (const auto& cpu : sorted_cpu_candidates_cached()) {
        if (cpu.id == reservedCpu) continue;
        if (n >= requestedCount || n >= MAX_SPLIT_PARTS) break;
        outIds[n++] = cpu.id;
    }
    return n;
}

static int available_encode_cpu_count() {
    const int reservedCpu = g_reserved_callback_cpu.load(std::memory_order_acquire);
    int cpus[MAX_SPLIT_PARTS]{};
    const int n = fill_encode_cpu_ids(MAX_SPLIT_PARTS, reservedCpu, cpus);
    return std::max(1, std::min(MAX_SPLIT_PARTS, n));
}

// 低延迟正式模式不再使用 2/3 块：只要设备有至少 4 个可用编码核心，
// split 请求都会提升到至少 4 块。这样顶部/底部质量下调策略总能稳定生效。
static constexpr int SPLIT_MIN_ACTIVE_PARTS = 4;

static int effective_split_part_count(int requestedCount) {
    if (requestedCount <= 1) return 1;
    int cpus[MAX_SPLIT_PARTS]{};
    const int reservedCpu = g_reserved_callback_cpu.load(std::memory_order_acquire);
    const int requested = std::min(requestedCount, MAX_SPLIT_PARTS);
    const int n = fill_encode_cpu_ids(MAX_SPLIT_PARTS, reservedCpu, cpus);
    if (n < 2) return 1;

    int effective = std::min(requested, n);
    if (n >= SPLIT_MIN_ACTIVE_PARTS && effective < SPLIT_MIN_ACTIVE_PARTS) {
        effective = SPLIT_MIN_ACTIVE_PARTS;
    }
    return std::min(effective, MAX_SPLIT_PARTS);
}

// 分块画质策略：改为按宽度 ROI。
// 左右边缘区域降 10 个质量点；中心区域保持原始质量。
// partCount>=4 时启用。partIndex 必须按从左到右的屏幕顺序排列。
static constexpr int DEFAULT_SPLIT_EDGE_QUALITY_REDUCTION = 10;
static constexpr int MIN_SPLIT_EDGE_QUALITY_REDUCTION = 0;
static constexpr int MAX_SPLIT_EDGE_QUALITY_REDUCTION = 40;
static std::atomic<int> g_split_edge_quality_reduction{DEFAULT_SPLIT_EDGE_QUALITY_REDUCTION};
static constexpr int SPLIT_EDGE_QUALITY_MIN_PARTS = 4;
static constexpr int SPLIT_CENTER_RATIO_NUM = 1;  // center = 50%
static constexpr int SPLIT_CENTER_RATIO_DEN = 2;
// Low-value ROI downscale: Q-10 regions are encoded at slightly lower horizontal
// resolution, then upscaled by the PC.  This reduces fixed pixel scan / JPEG work
// for top/edge areas without touching the high-quality center/bottom paths.
static constexpr int DEFAULT_SPLIT_EDGE_ENCODE_SCALE_PERCENT = 75;
static constexpr int MIN_SPLIT_EDGE_ENCODE_SCALE_PERCENT = 50;
static constexpr int MAX_SPLIT_EDGE_ENCODE_SCALE_PERCENT = 100;
static std::atomic<int> g_split_edge_encode_scale_percent{DEFAULT_SPLIT_EDGE_ENCODE_SCALE_PERCENT};


// JPEG 中心矩形 ROI 可调参数。
// 纯 JPEG / 底部 H.264 模式使用同一套逻辑：
//   - 中心矩形为高质量 JPEG，优先由高频核心处理；
//   - 中心外侧四周为低质量 JPEG，默认至少保留 2 个 worker 处理；
//   - 中心 H.264 模式下 JPEG 只负责外圈，不使用这里的中心高质量矩形。
static constexpr int DEFAULT_SPLIT_CENTER_HEIGHT_PERCENT = 30;
static constexpr int MIN_SPLIT_CENTER_HEIGHT_PERCENT = 10;
static constexpr int MAX_SPLIT_CENTER_HEIGHT_PERCENT = 80;
// 兼容旧命名：HLROI 第 7 个参数以前叫 topLowPercent，现在作为 JPEG 中心高度百分比。
static constexpr int DEFAULT_SPLIT_TOP_LOW_PERCENT = DEFAULT_SPLIT_CENTER_HEIGHT_PERCENT;
static constexpr int MIN_SPLIT_TOP_LOW_PERCENT = MIN_SPLIT_CENTER_HEIGHT_PERCENT;
static constexpr int MAX_SPLIT_TOP_LOW_PERCENT = MAX_SPLIT_CENTER_HEIGHT_PERCENT;
static std::atomic<int> g_split_top_low_percent{DEFAULT_SPLIT_TOP_LOW_PERCENT};

static constexpr int DEFAULT_SPLIT_CENTER_WIDTH_PERCENT = 30;
static constexpr int MIN_SPLIT_CENTER_WIDTH_PERCENT = 10;
static constexpr int MAX_SPLIT_CENTER_WIDTH_PERCENT = 80;
static std::atomic<int> g_split_center_width_percent{DEFAULT_SPLIT_CENTER_WIDTH_PERCENT};

// 继续复用旧协议第三个字段，但新版语义是“JPEG 中心高质量区域核心数”。
// 正常模式会给外围低质量 JPEG 至少保留 2 个 worker；只投中心区域时可使用全部 worker。
static constexpr int DEFAULT_SPLIT_CENTER_CORE_COUNT = 4;
static constexpr int MIN_SPLIT_CENTER_CORE_COUNT = 1;
static constexpr int MAX_SPLIT_CENTER_CORE_COUNT = MAX_SPLIT_PARTS;
static std::atomic<int> g_split_center_core_count{DEFAULT_SPLIT_CENTER_CORE_COUNT};

// 大核心权重百分比：100=平均切块，>100=最高频核心块更大，<100=最高频核心块更小。
static constexpr int DEFAULT_SPLIT_BIG_CORE_WEIGHT_PERCENT = 120;
static constexpr int MIN_SPLIT_BIG_CORE_WEIGHT_PERCENT = 50;
static constexpr int MAX_SPLIT_BIG_CORE_WEIGHT_PERCENT = 200;
static std::atomic<int> g_split_big_core_weight_percent{DEFAULT_SPLIT_BIG_CORE_WEIGHT_PERCENT};

// 只投中心区域：Android/native 不再生成外围低质量 JPEG 任务。
// 中心 H.264 模式下由 Kotlin 直接跳过外圈 JPEG 编码。
static std::atomic<bool> g_jpeg_center_only_enabled{false};

static inline int current_split_edge_quality_reduction() {
    return std::max(MIN_SPLIT_EDGE_QUALITY_REDUCTION,
                    std::min(MAX_SPLIT_EDGE_QUALITY_REDUCTION,
                             g_split_edge_quality_reduction.load(std::memory_order_acquire)));
}

static inline int current_split_edge_encode_scale_percent() {
    return std::max(MIN_SPLIT_EDGE_ENCODE_SCALE_PERCENT,
                    std::min(MAX_SPLIT_EDGE_ENCODE_SCALE_PERCENT,
                             g_split_edge_encode_scale_percent.load(std::memory_order_acquire)));
}

static inline int current_split_top_low_percent() {
    return std::max(MIN_SPLIT_TOP_LOW_PERCENT,
                    std::min(MAX_SPLIT_TOP_LOW_PERCENT,
                             g_split_top_low_percent.load(std::memory_order_acquire)));
}

static inline int current_split_center_height_percent() {
    return current_split_top_low_percent();
}

static inline int current_split_center_width_percent() {
    return std::max(MIN_SPLIT_CENTER_WIDTH_PERCENT,
                    std::min(MAX_SPLIT_CENTER_WIDTH_PERCENT,
                             g_split_center_width_percent.load(std::memory_order_acquire)));
}

static inline int current_split_center_core_count() {
    const int v = g_split_center_core_count.load(std::memory_order_acquire);
    return std::max(MIN_SPLIT_CENTER_CORE_COUNT,
                    std::min(MAX_SPLIT_CENTER_CORE_COUNT, v));
}

static inline bool current_jpeg_center_only_enabled() {
    return g_jpeg_center_only_enabled.load(std::memory_order_acquire);
}

static inline int current_split_big_core_weight_percent() {
    const int v = g_split_big_core_weight_percent.load(std::memory_order_acquire);
    return std::max(MIN_SPLIT_BIG_CORE_WEIGHT_PERCENT,
                    std::min(MAX_SPLIT_BIG_CORE_WEIGHT_PERCENT, v));
}

static inline int current_split_center_cpu_weight_percent() {
    // 旧 weighted-center-column 布局保留函数需要这个符号；
    // 新中心矩形 ROI 不再使用中心 CPU 权重。
    return 100;
}

static inline int split_edge_parts_each(int partCount);

static inline bool split_part_is_edge_region(int partIndex, int partCount) {
    if (partIndex < 0 || partIndex >= partCount || partCount < SPLIT_EDGE_QUALITY_MIN_PARTS) return false;
    const int edgeParts = split_edge_parts_each(partCount);
    return partIndex < edgeParts || partIndex >= partCount - edgeParts;
}

static inline int encoded_width_for_low_value_roi(int displayWidth) {
    if (displayWidth <= 0) return 0;
    int w = (displayWidth * current_split_edge_encode_scale_percent()) / 100;
    if (w < 2) w = 2;
    if ((w & 1) != 0) ++w;
    if (w > displayWidth) w = displayWidth;
    return w;
}

static inline int encoded_width_for_split_part(int displayWidth, int partIndex, int partCount) {
    if (displayWidth <= 0) return 0;
    if (!split_part_is_edge_region(partIndex, partCount)) return displayWidth;
    return encoded_width_for_low_value_roi(displayWidth);
}

static inline int split_edge_parts_each(int partCount) {
    if (partCount < SPLIT_EDGE_QUALITY_MIN_PARTS) return 0;

    // Balanced column ROI layout.
    // 4 -> left1 | center2 | right1
    // 5 -> left1 | center3 | right1
    // 6 -> left2 | center2 | right2
    // 7 -> left2 | center3 | right2
    // 8 -> left2 | center4 | right2
    // This avoids giving a whole 25% edge region to one low-frequency CPU.
    int edge = (partCount + 2) / 4;
    if (edge < 1) edge = 1;
    const int maxEdge = (partCount - 2) / 2;
    if (edge > maxEdge) edge = maxEdge;
    return edge;
}


// 将“可用编码核心数”和“ROI 逻辑块数”拆开。
// 7 个 worker 固定绑定 7 个 CPU，但画面可以切成更多小任务；worker 完成后继续抢下一个任务。
// 这样 3K/2K 下中心高质量区域不会被某一个 CPU 独占 16%~17% 宽度而拖慢整帧尾部。
static int roi_task_count_for_worker_count(int workerCount) {
    if (workerCount <= 1) return 1;
    if (workerCount < SPLIT_EDGE_QUALITY_MIN_PARTS) return workerCount;

    // Layered center ROI layout:
    // edge columns: split 50/50 vertically, all Q-10 => edgeCols * 2 * 2 tasks
    // center middle 60%: split by center column, original Q => centerCols tasks
    // center top 20%: merged into one Q-10 task across the whole center width
    // center bottom 20%: merged into one Q-10 task across the whole center width
    // This removes unnecessary top/bottom fragmentation while keeping the high-Q middle
    // area distributed across the center columns.
    const int edgeParts = split_edge_parts_each(workerCount);
    const int centerCols = workerCount - edgeParts * 2;
    int tasks = edgeParts * 2 * 2 + centerCols + 2;
    if (tasks <= 0) tasks = workerCount;
    return std::min(tasks, MAX_SPLIT_PARTS);
}

static int split_worker_target_cpu(int workerIndex) {
    if (workerIndex < 0 || workerIndex >= MAX_SPLIT_PARTS) return -1;
    int cpus[MAX_SPLIT_PARTS]{};
    const int reservedCpu = g_reserved_callback_cpu.load(std::memory_order_acquire);
    const int n = fill_encode_cpu_ids(MAX_SPLIT_PARTS, reservedCpu, cpus);
    if (workerIndex >= n) return -1;
    return cpus[workerIndex];
}


// ROI 中心列宽度历史：记录每个稳定 partIndex 上一段时间实际跑到的 CPU 频率。
// 目的：中心高 Q 区不再按几何等宽，而是让高频核心对应列稍宽、低频核心对应列稍窄，
// 使 part4/part7/part10 这类中心主块的 encode 完成时间更接近。
static std::mutex g_split_cpu_width_history_mutex;
static int g_split_cpu_width_history_part_count = 0;
static double g_split_cpu_width_freq_ema_khz[MAX_SPLIT_PARTS]{};
static constexpr double SPLIT_CPU_WIDTH_FREQ_EMA_ALPHA = 0.45;

static void reset_split_cpu_width_history_locked(int partCount) {
    g_split_cpu_width_history_part_count = partCount;
    for (int i = 0; i < MAX_SPLIT_PARTS; ++i) {
        g_split_cpu_width_freq_ema_khz[i] = 0.0;
    }
}

static void update_split_cpu_width_history_fixed(int partCount, const int partCpuIds[]) {
    if (partCount <= 0 || partCount > MAX_SPLIT_PARTS || partCpuIds == nullptr) return;

    std::lock_guard<std::mutex> lk(g_split_cpu_width_history_mutex);
    if (g_split_cpu_width_history_part_count != partCount) {
        reset_split_cpu_width_history_locked(partCount);
    }

    for (int i = 0; i < partCount; ++i) {
        const int cpuId = partCpuIds[i];
        if (cpuId < 0) continue;
        const int64_t freq = cached_cpu_max_freq_khz(cpuId);
        if (freq <= 100000) continue;

        double& ema = g_split_cpu_width_freq_ema_khz[i];
        const double cur = static_cast<double>(freq);
        if (!(ema > 0.0)) ema = cur;
        else ema = ema * (1.0 - SPLIT_CPU_WIDTH_FREQ_EMA_ALPHA) + cur * SPLIT_CPU_WIDTH_FREQ_EMA_ALPHA;
    }
}

static double split_part_cpu_width_freq_ema(int partCount, int partIndex) {
    if (partCount <= 0 || partCount > MAX_SPLIT_PARTS || partIndex < 0 || partIndex >= partCount) return 0.0;
    std::lock_guard<std::mutex> lk(g_split_cpu_width_history_mutex);
    if (g_split_cpu_width_history_part_count != partCount) return 0.0;
    return g_split_cpu_width_freq_ema_khz[partIndex];
}

static double split_weight_for_cpu(int cpuId, int rank, int64_t baselineFreqKhz) {
    if (baselineFreqKhz > 100000) {
        const int64_t freq = cached_cpu_max_freq_khz(cpuId);
        if (freq > 100000) {
            const double ratio = static_cast<double>(freq) / static_cast<double>(baselineFreqKhz);
            // 保守加权：只按频率线性分配，不再使用频率平方。
            // 之前 8 Elite 上 Prime 核会被压到约 1.28~1.33，块过大后 PC 解码/组帧更不稳。
            // 这里把单核心原始权重限制到 1.20，最终归一后大核心大约 1.13~1.15，性能核约 0.94。
            return std::max(1.0, std::min(1.20, ratio));
        }
    }
    // 读不到频率时也只给前两个高性能核心轻微加权，避免大核心块过重。
    return rank < 2 ? 1.16 : 1.0;
}

// 按 CPU 频率生成“worker 槽位高度”。
// worker0/1/... 对应 split_worker_target_cpu() 的高频 -> 低频 CPU 顺序。
// 这里不再接受 PC/F2 手动权重：打包时可以删除 PC 侧权重 UI，native 分块只看 CPU 频率和近期块耗时。
static bool build_cpu_slot_heights_by_frequency(
        int totalHeight,
        int partCount,
        int slotHeights[]) {
    if (totalHeight <= 0 || partCount <= 0 || partCount > MAX_SPLIT_PARTS || slotHeights == nullptr) return false;
    if (totalHeight < partCount * 2) return false;

    const int reservedCpu = g_reserved_callback_cpu.load(std::memory_order_acquire);
    int cpus[MAX_SPLIT_PARTS]{};
    if (fill_encode_cpu_ids(partCount, reservedCpu, cpus) < partCount) return false;

    int64_t maxFreq = 0;
    int64_t minFreq = INT64_MAX;
    for (int i = 0; i < partCount; ++i) {
        const int64_t f = cached_cpu_max_freq_khz(cpus[i]);
        if (f > maxFreq) maxFreq = f;
        if (f > 0 && f < minFreq) minFreq = f;
    }
    if (minFreq == INT64_MAX) minFreq = 0;

    // 只有存在明显高频核心时才加 5 个百分点。
    // 例：7 块时平均 14.3%，CPU7/CPU6 约 19.3%，其余约 12.3%。
    // 例：5 块时平均 20%，高频核心约 25%，其余自动均分。
    const double avgPermille = 1000.0 / static_cast<double>(partCount);
    bool isHigh[MAX_SPLIT_PARTS]{};
    int highCount = 0;
    if (maxFreq > 100000 && minFreq > 100000 && maxFreq > minFreq * 105 / 100) {
        for (int i = 0; i < partCount; ++i) {
            const int64_t f = cached_cpu_max_freq_khz(cpus[i]);
            // “先不写死”：按频率自动判断最高频簇，而不是写死 CPU7/CPU6。
            if (f >= maxFreq * 98 / 100) {
                isHigh[i] = true;
                ++highCount;
            }
        }
    }

    double sharePermille[MAX_SPLIT_PARTS]{};
    if (highCount <= 0 || highCount >= partCount) {
        for (int i = 0; i < partCount; ++i) sharePermille[i] = avgPermille;
    } else {
        const int lowCount = partCount - highCount;
        double highShare = avgPermille + 40.0; // +4 个百分点
        double lowShare = (1000.0 - highShare * highCount) / static_cast<double>(lowCount);

        // 防止极端设备/极端分块数把低频核心压得过小。
        const double minLowShare = avgPermille * 0.65;
        if (lowShare < minLowShare) {
            lowShare = minLowShare;
            highShare = (1000.0 - lowShare * lowCount) / static_cast<double>(highCount);
        }

        for (int i = 0; i < partCount; ++i) {
            sharePermille[i] = isHigh[i] ? highShare : lowShare;
        }
    }

    double quota[MAX_SPLIT_PARTS]{};
    int sum = 0;
    for (int i = 0; i < partCount; ++i) {
        quota[i] = static_cast<double>(totalHeight) * sharePermille[i] / 1000.0;
        int h = make_even_floor(static_cast<int>(quota[i]));
        if (h < 2) h = 2;
        slotHeights[i] = h;
        sum += h;
    }

    while (sum > totalHeight) {
        int best = -1;
        for (int i = partCount - 1; i >= 0; --i) {
            if (slotHeights[i] > 2 && (best < 0 || slotHeights[i] > slotHeights[best])) best = i;
        }
        if (best < 0) return false;
        slotHeights[best] -= 2;
        sum -= 2;
    }

    while (sum + 2 <= totalHeight) {
        int best = 0;
        double bestNeed = -1e30;
        for (int i = 0; i < partCount; ++i) {
            const double need = quota[i] - static_cast<double>(slotHeights[i]);
            if (need > bestNeed) {
                bestNeed = need;
                best = i;
            }
        }
        slotHeights[best] += 2;
        sum += 2;
    }

    if (sum < totalHeight) {
        slotHeights[partCount - 1] += (totalHeight - sum);
        sum = totalHeight;
    }
    return sum == totalHeight;
}

static bool build_part_layout_for_worker_assignment(
        int totalHeight,
        int partCount,
        const int workerPartMap[],
        int partTops[],
        int partHeights[]) {
    if (totalHeight <= 0 || partCount <= 0 || partCount > MAX_SPLIT_PARTS) return false;
    if (workerPartMap == nullptr || partTops == nullptr || partHeights == nullptr) return false;

    // 先按 CPU 频率权重生成“屏幕固定分块”。
    // partIndex 必须代表稳定的屏幕区域，不能因为 workerPartMap 每帧变化而改变高度/边界。
    // 否则近期 JPEG/耗时历史会对应错区域，排序会抖动甚至反向。
    int slotHeights[MAX_SPLIT_PARTS]{};
    if (!build_cpu_slot_heights_by_frequency(totalHeight, partCount, slotHeights)) return false;

    bool usedParts[MAX_SPLIT_PARTS]{};
    for (int worker = 0; worker < partCount; ++worker) {
        const int partIndex = workerPartMap[worker];
        if (partIndex < 0 || partIndex >= partCount || usedParts[partIndex]) return false;
        usedParts[partIndex] = true;
    }

    int top = 0;
    for (int i = 0; i < partCount; ++i) {
        if (!usedParts[i]) return false;
        partHeights[i] = slotHeights[i];
        if (partHeights[i] <= 0) return false;
        partTops[i] = top;
        top += partHeights[i];
    }
    return top == totalHeight;
}

// 旧函数名保留给未改到的调用兜底；真正的 split 路径会使用 build_part_layout_for_worker_assignment()。
static bool build_weighted_part_layout(
        int totalHeight,
        int partCount,
        int partTops[],
        int partHeights[]) {
    int identity[MAX_SPLIT_PARTS]{};
    for (int i = 0; i < MAX_SPLIT_PARTS; ++i) identity[i] = i;
    return build_part_layout_for_worker_assignment(totalHeight, partCount, identity, partTops, partHeights);
}


static bool build_span_layout_even(int total, int count, int offsets[], int sizes[]) {
    if (total <= 0 || count <= 0 || offsets == nullptr || sizes == nullptr) return false;
    int base = total / count;
    int rem = total % count;
    int pos = 0;
    for (int i = 0; i < count; ++i) {
        int span = base + (i < rem ? 1 : 0);
        if (i + 1 < count && span > 2) span &= ~1;
        if (span <= 0) span = 1;
        if (i + 1 == count) span = total - pos;
        offsets[i] = pos;
        sizes[i] = span;
        pos += span;
    }
    if (pos != total) {
        sizes[count - 1] += (total - pos);
        pos = total;
    }
    return pos == total;
}

static bool build_center_span_layout_by_big_core_weight(int total, int count, int offsets[], int sizes[]) {
    if (total <= 0 || count <= 0 || count > MAX_SPLIT_PARTS || offsets == nullptr || sizes == nullptr) return false;
    if (count == 1) {
        offsets[0] = 0;
        sizes[0] = total;
        return true;
    }

    const int reservedCpu = g_reserved_callback_cpu.load(std::memory_order_acquire);
    int cpus[MAX_SPLIT_PARTS]{};
    if (fill_encode_cpu_ids(count, reservedCpu, cpus) < count) {
        return build_span_layout_even(total, count, offsets, sizes);
    }

    int64_t maxFreq = 0;
    int64_t minFreq = INT64_MAX;
    for (int i = 0; i < count; ++i) {
        const int64_t f = cached_cpu_max_freq_khz(cpus[i]);
        if (f > maxFreq) maxFreq = f;
        if (f > 100000 && f < minFreq) minFreq = f;
    }
    if (minFreq == INT64_MAX || maxFreq <= 100000 || maxFreq <= minFreq * 105 / 100) {
        return build_span_layout_even(total, count, offsets, sizes);
    }

    bool isBig[MAX_SPLIT_PARTS]{};
    int bigCount = 0;
    for (int i = 0; i < count; ++i) {
        const int64_t f = cached_cpu_max_freq_khz(cpus[i]);
        if (f >= maxFreq * 98 / 100) {
            isBig[i] = true;
            ++bigCount;
        }
    }
    if (bigCount <= 0 || bigCount >= count) {
        return build_span_layout_even(total, count, offsets, sizes);
    }

    const double bigMultiplier = static_cast<double>(current_split_big_core_weight_percent()) / 100.0;
    double weights[MAX_SPLIT_PARTS]{};
    double weightSum = 0.0;
    for (int i = 0; i < count; ++i) {
        double w = isBig[i] ? bigMultiplier : 1.0;
        if (!(w > 0.0)) w = 1.0;
        weights[i] = w;
        weightSum += w;
    }
    if (!(weightSum > 0.0)) return build_span_layout_even(total, count, offsets, sizes);

    int pos = 0;
    int sum = 0;
    double quota[MAX_SPLIT_PARTS]{};
    for (int i = 0; i < count; ++i) {
        quota[i] = static_cast<double>(total) * weights[i] / weightSum;
        int w = make_even_floor(static_cast<int>(quota[i] + 0.5));
        if (w < 2) w = 2;
        sizes[i] = w;
        sum += w;
    }

    while (sum > total) {
        int best = -1;
        double bestOver = -1e30;
        for (int i = 0; i < count; ++i) {
            if (sizes[i] <= 2) continue;
            const double over = static_cast<double>(sizes[i]) - quota[i];
            if (over > bestOver) { bestOver = over; best = i; }
        }
        if (best < 0) break;
        sizes[best] -= 2;
        sum -= 2;
    }
    while (sum < total) {
        int best = -1;
        double bestUnder = -1e30;
        for (int i = 0; i < count; ++i) {
            const double under = quota[i] - static_cast<double>(sizes[i]);
            if (under > bestUnder) { bestUnder = under; best = i; }
        }
        if (best < 0) best = count - 1;
        sizes[best] += 2;
        sum += 2;
    }
    if (sum != total) {
        sizes[count - 1] += total - sum;
    }

    pos = 0;
    for (int i = 0; i < count; ++i) {
        if (sizes[i] <= 0) return false;
        offsets[i] = pos;
        pos += sizes[i];
    }
    return pos == total;
}


static bool build_weighted_center_column_layout(
        int centerWidth,
        int centerCols,
        int edgeParts,
        int roiTaskCount,
        int outOffsets[],
        int outWidths[]) {
    if (centerWidth <= 0 || centerCols <= 0 || centerCols > MAX_SPLIT_PARTS ||
        outOffsets == nullptr || outWidths == nullptr) {
        return false;
    }
    if (centerCols == 1) {
        outOffsets[0] = 0;
        outWidths[0] = centerWidth;
        return true;
    }

    double freqKhz[MAX_SPLIT_PARTS]{};
    bool hasAnyFreq = false;

    // 对分层 ROI：左侧 edge 任务之后，连续 centerCols 个 part 就是
    // center middle/high-Q 主块。它们的历史实际 CPU 频率最能代表该列当前
    // 会被哪个性能档位的 worker 处理。
    for (int ci = 0; ci < centerCols; ++ci) {
        const int middlePartIndex = edgeParts * 2 + ci;
        double f = split_part_cpu_width_freq_ema(roiTaskCount, middlePartIndex);
        if (!(f > 100000.0)) {
            // 首几帧没有历史时，用中心主块首批派发 worker 的目标 CPU 作为初值。
            // 这里不写死 CPU 编号，只按当前设备检测到的 CPU 频率排序。
            const int cpuId = split_worker_target_cpu(ci);
            const int64_t cf = cached_cpu_max_freq_khz(cpuId);
            if (cf > 100000) f = static_cast<double>(cf);
        }
        if (f > 100000.0) {
            freqKhz[ci] = f;
            hasAnyFreq = true;
        }
    }

    if (!hasAnyFreq) {
        return build_span_layout_even(centerWidth, centerCols, outOffsets, outWidths);
    }

    double fallback = 0.0;
    int fallbackCount = 0;
    for (int ci = 0; ci < centerCols; ++ci) {
        if (freqKhz[ci] > 100000.0) {
            fallback += freqKhz[ci];
            ++fallbackCount;
        }
    }
    fallback = fallbackCount > 0 ? fallback / static_cast<double>(fallbackCount) : 1.0;
    for (int ci = 0; ci < centerCols; ++ci) {
        if (!(freqKhz[ci] > 100000.0)) freqKhz[ci] = fallback;
    }

    double avgFreq = 0.0;
    for (int ci = 0; ci < centerCols; ++ci) avgFreq += freqKhz[ci];
    avgFreq /= static_cast<double>(centerCols);
    if (!(avgFreq > 100000.0)) {
        return build_span_layout_even(centerWidth, centerCols, outOffsets, outWidths);
    }

    const double strength = static_cast<double>(current_split_center_cpu_weight_percent()) / 100.0;
    if (strength <= 0.001) {
        return build_span_layout_even(centerWidth, centerCols, outOffsets, outWidths);
    }

    double weights[MAX_SPLIT_PARTS]{};
    double weightSum = 0.0;
    for (int ci = 0; ci < centerCols; ++ci) {
        // cpuWeight=100 为默认强度；0 为平均切；200 更偏向高频核心。
        double ratio = freqKhz[ci] / avgFreq;
        if (!(ratio > 0.0)) ratio = 1.0;
        double raw = std::pow(ratio, 0.85);
        raw = std::max(0.80, std::min(1.25, raw));
        double w = 1.0 + (raw - 1.0) * strength;
        const double minW = std::max(0.70, 1.0 - 0.12 * strength);
        const double maxW = std::min(1.35, 1.0 + 0.12 * strength);
        w = std::max(minW, std::min(maxW, w));
        weights[ci] = w;
        weightSum += w;
    }
    if (!(weightSum > 0.0)) {
        return build_span_layout_even(centerWidth, centerCols, outOffsets, outWidths);
    }

    const double evenWidth = static_cast<double>(centerWidth) / static_cast<double>(centerCols);
    const double minWidth = evenWidth * 0.82;
    const double maxWidth = evenWidth * 1.15;

    double quota[MAX_SPLIT_PARTS]{};
    int sum = 0;
    for (int ci = 0; ci < centerCols; ++ci) {
        double q = static_cast<double>(centerWidth) * weights[ci] / weightSum;
        q = std::max(minWidth, std::min(maxWidth, q));
        quota[ci] = q;
        int w = make_even_floor(static_cast<int>(q));
        if (w < 2) w = 2;
        outWidths[ci] = w;
        sum += w;
    }

    while (sum > centerWidth) {
        int best = -1;
        for (int ci = 0; ci < centerCols; ++ci) {
            if (outWidths[ci] <= 2) continue;
            if (best < 0 || outWidths[ci] > outWidths[best]) best = ci;
        }
        if (best < 0) return false;
        outWidths[best] -= 2;
        sum -= 2;
    }
    while (sum + 2 <= centerWidth) {
        int best = 0;
        double bestNeed = -1e30;
        for (int ci = 0; ci < centerCols; ++ci) {
            const double need = quota[ci] - static_cast<double>(outWidths[ci]);
            if (need > bestNeed) {
                bestNeed = need;
                best = ci;
            }
        }
        outWidths[best] += 2;
        sum += 2;
    }
    if (sum < centerWidth) {
        outWidths[centerCols - 1] += centerWidth - sum;
        sum = centerWidth;
    }

    int pos = 0;
    for (int ci = 0; ci < centerCols; ++ci) {
        if (outWidths[ci] <= 0) return false;
        outOffsets[ci] = pos;
        pos += outWidths[ci];
    }
    return pos == centerWidth;
}

static bool build_column_roi_layout(int totalWidth, int partCount, int partLefts[], int partWidths[]) {
    if (totalWidth <= 0 || partCount <= 0 || partCount > MAX_SPLIT_PARTS || partLefts == nullptr || partWidths == nullptr) return false;
    if (partCount < SPLIT_EDGE_QUALITY_MIN_PARTS) {
        return build_span_layout_even(totalWidth, partCount, partLefts, partWidths);
    }

    const int edgeParts = split_edge_parts_each(partCount);
    const int centerParts = partCount - edgeParts * 2;
    if (centerParts <= 0) return false;

    const int minCenterWidth = std::max(2, 2 * centerParts);
    const int minEdgeWidthEach = std::max(1, edgeParts);

    if (totalWidth < minCenterWidth + minEdgeWidthEach * 2) {
        return false;
    }

    // JPEG中宽：由 PC/F2 的 IDC_SLIDER_ROI_JPEG_CENTER_WIDTH_PERCENT 控制。
    // 纯 JPEG 和 bottom-video JPEG 都使用同一个 current_split_center_width_percent()。
    int centerWidth = make_even_floor((totalWidth * current_split_center_width_percent()) / 100);
    centerWidth = std::max(minCenterWidth, std::min(centerWidth, totalWidth - minEdgeWidthEach * 2));

    int edgeTotal = totalWidth - centerWidth;
    int leftWidth = edgeTotal / 2;
    int rightWidth = edgeTotal - leftWidth;

    if (leftWidth < minEdgeWidthEach) {
        leftWidth = minEdgeWidthEach;
        rightWidth = edgeTotal - leftWidth;
    }
    if (rightWidth < minEdgeWidthEach) {
        rightWidth = minEdgeWidthEach;
        leftWidth = edgeTotal - rightWidth;
    }

    int tmpOff[MAX_SPLIT_PARTS]{};
    int tmpSize[MAX_SPLIT_PARTS]{};
    if (edgeParts > 0) {
        if (!build_span_layout_even(leftWidth, edgeParts, tmpOff, tmpSize)) return false;
        for (int i = 0; i < edgeParts; ++i) {
            partLefts[i] = tmpOff[i];
            partWidths[i] = tmpSize[i];
        }
    }
    const int roiTaskCount = edgeParts * 2 * 2 + centerParts + 2;
    if (!build_weighted_center_column_layout(centerWidth, centerParts, edgeParts, roiTaskCount, tmpOff, tmpSize)) return false;
    for (int i = 0; i < centerParts; ++i) {
        partLefts[edgeParts + i] = leftWidth + tmpOff[i];
        partWidths[edgeParts + i] = tmpSize[i];
    }
    if (edgeParts > 0) {
        if (!build_span_layout_even(rightWidth, edgeParts, tmpOff, tmpSize)) return false;
        for (int i = 0; i < edgeParts; ++i) {
            partLefts[edgeParts + centerParts + i] = leftWidth + centerWidth + tmpOff[i];
            partWidths[edgeParts + centerParts + i] = tmpSize[i];
        }
    }
    int covered = 0;
    for (int i = 0; i < partCount; ++i) {
        if (partWidths[i] <= 0 || partLefts[i] < 0) return false;
        if (i > 0 && partLefts[i] != partLefts[i - 1] + partWidths[i - 1]) return false;
        covered += partWidths[i];
    }
    return covered == totalWidth;
}


static inline int clamp_jpeg_quality(jint jpegQuality) {
    return std::max(20, std::min(100, static_cast<int>(jpegQuality)));
}

// 旧常量保留说明：默认 2/6/2；实际比例由 g_split_top_low_percent 动态控制。


struct JpegRoiSourceRect {
    int left = 0;
    int top = 0;
    int width = 0;
    int height = 0;
};

static bool build_center_rect_jpeg_roi_layout(
        int totalWidth,
        int totalHeight,
        int workerCount,
        int& outPartCount,
        int partLefts[],
        int partTops[],
        int partWidths[],
        int partHeights[],
        int partEncodedWidths[],
        int partJpegQualities[],
        int baseJpegQuality,
        bool anchorCenterToBottom = false) {
    outPartCount = 0;
    if (totalWidth <= 0 || totalHeight <= 0 || workerCount <= 0 || workerCount > MAX_SPLIT_PARTS) return false;
    if (partLefts == nullptr || partTops == nullptr || partWidths == nullptr || partHeights == nullptr ||
        partEncodedWidths == nullptr || partJpegQualities == nullptr) return false;

    const int highQ = std::max(20, std::min(100, baseJpegQuality));
    const int lowQ = std::max(20, std::min(100, baseJpegQuality - current_split_edge_quality_reduction()));


    int centerWidth = make_even_floor((totalWidth * current_split_center_width_percent()) / 100);
    int centerHeight = make_even_floor((totalHeight * current_split_center_height_percent()) / 100);
    centerWidth = std::max(2, std::min(centerWidth, totalWidth - 2));
    centerHeight = std::max(2, std::min(centerHeight, totalHeight - 2));

    int centerLeft = make_even_floor((totalWidth - centerWidth) / 2);
    // 纯 JPEG：中心高质量矩形居中。
    // 底部 H.264：中心高质量 JPEG 贴住 H.264 上边缘，避免两者之间插入低质量 JPEG 条带。
    int centerTop = anchorCenterToBottom
        ? make_even_floor(totalHeight - centerHeight)
        : make_even_floor((totalHeight - centerHeight) / 2);
    if (centerLeft < 0) centerLeft = 0;
    if (centerTop < 0) centerTop = 0;
    if (centerLeft + centerWidth > totalWidth) centerWidth = make_even_floor(totalWidth - centerLeft);
    if (centerTop + centerHeight > totalHeight) centerHeight = make_even_floor(totalHeight - centerTop);
    if (centerWidth <= 0 || centerHeight <= 0) return false;

    // 中心矩形核心数由 PC/F2 手动控制。
    // 规则：
    // - 宽度/高度只决定中心矩形大小；
    // - 中心核心数只决定中心矩形切成几块；
    // - worker 足够时，外围低质量 JPEG 至少保留 2 个 worker；
    // - worker 不足时自动降级，但不改变中心矩形宽高。
    const bool centerOnly = current_jpeg_center_only_enabled();
    const int lowWorkerReserve = centerOnly ? 0 : ((workerCount >= 4) ? 2 : std::max(1, workerCount - 1));
    const int maxCenterWorkers = centerOnly ? workerCount : std::max(1, workerCount - lowWorkerReserve);
    int centerWorkers = current_split_center_core_count();
    centerWorkers = std::max(1, std::min(centerWorkers, maxCenterWorkers));
    centerWorkers = std::min(centerWorkers, MAX_SPLIT_PARTS);
    centerWorkers = std::min(centerWorkers, std::max(1, centerWidth / 2));
    const int lowWorkerCount = centerOnly ? 0 : std::max(0, workerCount - centerWorkers);

    int n = 0;
    auto addPart = [&](int left, int top, int width, int height, int q, bool lowValue) -> bool {
        if (n >= MAX_SPLIT_PARTS) return false;
        if (width <= 0 || height <= 0) return true;
        if (left < 0 || top < 0 || left + width > totalWidth || top + height > totalHeight) return false;
        partLefts[n] = left;
        partTops[n] = top;
        partWidths[n] = width;
        partHeights[n] = height;
        partEncodedWidths[n] = lowValue ? encoded_width_for_low_value_roi(width) : width;
        partJpegQualities[n] = q;
        ++n;
        return true;
    };

    // 1) 中心高质量区域：按中心 worker 数纵向切成若干列。
    int centerOffs[MAX_SPLIT_PARTS]{};
    int centerSizes[MAX_SPLIT_PARTS]{};
    if (!build_center_span_layout_by_big_core_weight(centerWidth, centerWorkers, centerOffs, centerSizes)) return false;
    for (int i = 0; i < centerWorkers; ++i) {
        if (!addPart(centerLeft + centerOffs[i], centerTop, centerSizes[i], centerHeight, highQ, false)) return false;
    }

    if (centerOnly) {
        outPartCount = n;
        return outPartCount > 0 && outPartCount <= MAX_SPLIT_PARTS;
    }

    // 2) 外围低质量区域：中心矩形外侧四周按面积自动拆分。
    std::vector<JpegRoiSourceRect> lowRects;
    auto addLowRect = [&](int left, int top, int width, int height) {
        if (width > 0 && height > 0) {
            lowRects.push_back(JpegRoiSourceRect{left, top, width, height});
        }
    };

    const int centerRight = centerLeft + centerWidth;
    const int centerBottom = centerTop + centerHeight;
    addLowRect(0, 0, totalWidth, centerTop);                                  // top
    addLowRect(0, centerBottom, totalWidth, totalHeight - centerBottom);       // bottom
    addLowRect(0, centerTop, centerLeft, centerHeight);                        // left
    addLowRect(centerRight, centerTop, totalWidth - centerRight, centerHeight); // right

    int lowTaskTarget = std::min(MAX_SPLIT_PARTS - n, std::max(lowWorkerCount, lowWorkerCount * 2));
    lowTaskTarget = std::max(0, lowTaskTarget);
    if (!lowRects.empty() && lowTaskTarget <= 0) return false;

    auto rectArea = [](const JpegRoiSourceRect& r) -> int64_t {
        return static_cast<int64_t>(std::max(0, r.width)) * static_cast<int64_t>(std::max(0, r.height));
    };

    while (static_cast<int>(lowRects.size()) < lowTaskTarget) {
        int best = -1;
        int64_t bestArea = 0;
        for (int i = 0; i < static_cast<int>(lowRects.size()); ++i) {
            const auto& r = lowRects[static_cast<size_t>(i)];
            const bool canSplitW = r.width >= 4;
            const bool canSplitH = r.height >= 4;
            if (!canSplitW && !canSplitH) continue;
            const int64_t area = rectArea(r);
            if (area > bestArea) {
                bestArea = area;
                best = i;
            }
        }
        if (best < 0) break;

        JpegRoiSourceRect r = lowRects[static_cast<size_t>(best)];
        lowRects.erase(lowRects.begin() + best);

        const bool splitW = r.width >= r.height ? (r.width >= 4) : !(r.height >= 4);
        if (splitW) {
            int w1 = make_even_floor(r.width / 2);
            if (w1 <= 0 || w1 >= r.width) {
                lowRects.push_back(r);
                break;
            }
            lowRects.push_back(JpegRoiSourceRect{r.left, r.top, w1, r.height});
            lowRects.push_back(JpegRoiSourceRect{r.left + w1, r.top, r.width - w1, r.height});
        } else {
            int h1 = make_even_floor(r.height / 2);
            if (h1 <= 0 || h1 >= r.height) {
                lowRects.push_back(r);
                break;
            }
            lowRects.push_back(JpegRoiSourceRect{r.left, r.top, r.width, h1});
            lowRects.push_back(JpegRoiSourceRect{r.left, r.top + h1, r.width, r.height - h1});
        }
    }

    std::stable_sort(lowRects.begin(), lowRects.end(), [&](const JpegRoiSourceRect& a, const JpegRoiSourceRect& b) {
        const int64_t aa = rectArea(a);
        const int64_t ba = rectArea(b);
        if (aa != ba) return aa > ba;
        if (a.top != b.top) return a.top < b.top;
        return a.left < b.left;
    });

    for (const auto& r : lowRects) {
        if (!addPart(r.left, r.top, r.width, r.height, lowQ, true)) return false;
    }

    outPartCount = n;
    return outPartCount > 0 && outPartCount <= MAX_SPLIT_PARTS;
}

// 旧函数名保留，内部已替换为“中心高质量矩形 + 外围低质量任务池”。
static bool build_column_roi_2_6_2_layout(
        int totalWidth,
        int totalHeight,
        int workerCount,
        int& outPartCount,
        int partLefts[],
        int partTops[],
        int partWidths[],
        int partHeights[],
        int partEncodedWidths[],
        int partJpegQualities[],
        int baseJpegQuality) {
    return build_center_rect_jpeg_roi_layout(totalWidth, totalHeight, workerCount, outPartCount,
                                             partLefts, partTops, partWidths, partHeights,
                                             partEncodedWidths, partJpegQualities, baseJpegQuality);
}

struct OutsideRoiSourceRect {
    int left = 0;
    int top = 0;
    int width = 0;
    int height = 0;
    bool splitHorizontal = true;
};

static bool build_outside_center_roi_layout(
        int totalWidth,
        int totalHeight,
        int centerLeft,
        int centerTop,
        int centerWidth,
        int centerHeight,
        int workerCount,
        int& outPartCount,
        int partLefts[],
        int partTops[],
        int partWidths[],
        int partHeights[],
        int partEncodedWidths[],
        int partJpegQualities[],
        int baseJpegQuality) {
    outPartCount = 0;
    if (totalWidth <= 0 || totalHeight <= 0 || workerCount <= 0 || workerCount > MAX_SPLIT_PARTS) return false;
    if (partLefts == nullptr || partTops == nullptr || partWidths == nullptr || partHeights == nullptr ||
        partEncodedWidths == nullptr || partJpegQualities == nullptr) return false;

    centerLeft = make_even_floor(std::max(0, centerLeft));
    centerTop = make_even_floor(std::max(0, centerTop));
    centerWidth = make_even_floor(std::max(2, centerWidth));
    centerHeight = make_even_floor(std::max(2, centerHeight));
    if (centerLeft + centerWidth > totalWidth) centerWidth = make_even_floor(totalWidth - centerLeft);
    if (centerTop + centerHeight > totalHeight) centerHeight = make_even_floor(totalHeight - centerTop);
    if (centerWidth <= 0 || centerHeight <= 0 || centerLeft < 0 || centerTop < 0 ||
        centerLeft + centerWidth > totalWidth || centerTop + centerHeight > totalHeight) {
        return false;
    }

    OutsideRoiSourceRect rects[4]{};
    int rectCount = 0;
    auto addRect = [&](int left, int top, int width, int height, bool splitHorizontal) {
        if (width <= 0 || height <= 0 || rectCount >= 4) return;
        rects[rectCount++] = OutsideRoiSourceRect{left, top, width, height, splitHorizontal};
    };

    const int centerRight = centerLeft + centerWidth;
    const int centerBottom = centerTop + centerHeight;
    addRect(0, 0, totalWidth, centerTop, true);                                      // top strip
    addRect(0, centerBottom, totalWidth, totalHeight - centerBottom, true);          // bottom strip
    addRect(0, centerTop, centerLeft, centerHeight, false);                          // left side
    addRect(centerRight, centerTop, totalWidth - centerRight, centerHeight, false);   // right side
    if (rectCount <= 0) return false;

    const int targetParts = std::min(MAX_SPLIT_PARTS, std::max(workerCount + 2, rectCount));
    int rectParts[4]{};
    int64_t totalArea = 0;
    for (int i = 0; i < rectCount; ++i) {
        totalArea += static_cast<int64_t>(rects[i].width) * static_cast<int64_t>(rects[i].height);
        rectParts[i] = 1;
    }
    int assigned = rectCount;
    while (assigned < targetParts) {
        int best = 0;
        double bestScore = -1.0;
        for (int i = 0; i < rectCount; ++i) {
            const double area = static_cast<double>(rects[i].width) * static_cast<double>(rects[i].height);
            const double score = area / static_cast<double>(rectParts[i] + 1);
            if (score > bestScore) {
                bestScore = score;
                best = i;
            }
        }
        rectParts[best] += 1;
        assigned += 1;
    }

    const int lowQ = clamp_jpeg_quality(baseJpegQuality - current_split_edge_quality_reduction());
    int n = 0;
    auto addPart = [&](int left, int top, int width, int height) -> bool {
        if (n >= MAX_SPLIT_PARTS) return false;
        if (left < 0 || top < 0 || width <= 0 || height <= 0) return false;
        if (left + width > totalWidth || top + height > totalHeight) return false;
        partLefts[n] = left;
        partTops[n] = top;
        partWidths[n] = width;
        partHeights[n] = height;
        partEncodedWidths[n] = encoded_width_for_low_value_roi(width);
        partJpegQualities[n] = lowQ;
        ++n;
        return true;
    };

    for (int ri = 0; ri < rectCount; ++ri) {
        const OutsideRoiSourceRect& r = rects[ri];
        const int count = std::max(1, rectParts[ri]);
        int offs[MAX_SPLIT_PARTS]{};
        int sizes[MAX_SPLIT_PARTS]{};
        if (r.splitHorizontal) {
            if (!build_span_layout_even(r.width, count, offs, sizes)) return false;
            for (int i = 0; i < count; ++i) {
                if (!addPart(r.left + offs[i], r.top, sizes[i], r.height)) return false;
            }
        } else {
            if (!build_span_layout_even(r.height, count, offs, sizes)) return false;
            for (int i = 0; i < count; ++i) {
                if (!addPart(r.left, r.top + offs[i], r.width, sizes[i])) return false;
            }
        }
    }

    outPartCount = n;
    return outPartCount > 0 && outPartCount <= MAX_SPLIT_PARTS;
}

// Perceptual hybrid layout for the new bottom-video mode.
// The bottom strip is owned by H.264.  The upper JPEG area is split independently from
// the older pure-JPEG 2/6/2 function so future tuning of this mode does not regress the
// legacy full-frame JPEG path.
//
// Layout in the JPEG area above the video strip:
//   - top strip: low value, split into 1-2 horizontal tasks, Q-drop + optional X downscale
//   - left/right side: low value, one task each, Q-drop + optional X downscale
//   - middle visual field: high quality, split into multiple columns, original Q

static bool build_upper_jpeg_bottom_video_layout(
        int totalWidth,
        int totalHeight,
        int videoTop,
        int videoHeight,
        int workerCount,
        int& outPartCount,
        int partLefts[],
        int partTops[],
        int partWidths[],
        int partHeights[],
        int partEncodedWidths[],
        int partJpegQualities[],
        int baseJpegQuality) {
    outPartCount = 0;
    if (totalWidth <= 0 || totalHeight <= 0 || workerCount <= 0 || workerCount > MAX_SPLIT_PARTS) return false;
    if (partLefts == nullptr || partTops == nullptr || partWidths == nullptr || partHeights == nullptr ||
        partEncodedWidths == nullptr || partJpegQualities == nullptr) return false;

    videoTop = make_even_floor(std::max(2, videoTop));
    videoHeight = make_even_floor(std::max(2, videoHeight));
    if (videoTop > totalHeight - 2) videoTop = make_even_floor(totalHeight - 2);
    if (videoTop + videoHeight > totalHeight) videoHeight = make_even_floor(totalHeight - videoTop);

    // 底部 H.264 模式：JPEG 只负责视频区域上方的画面。
    // 这块 JPEG 区域也使用同一套“中心高质量矩形 + 外围低质量任务池”逻辑。
    const int jpegHeight = videoTop;
    if (jpegHeight < 4) return false;

    return build_center_rect_jpeg_roi_layout(totalWidth, jpegHeight, workerCount, outPartCount,
                                             partLefts, partTops, partWidths, partHeights,
                                             partEncodedWidths, partJpegQualities, baseJpegQuality,
                                             true);
}

static int get_fd_from_file_descriptor(JNIEnv* env, jobject fileDescriptor) {
    if (fileDescriptor == nullptr) return -1;

    jclass cls = env->FindClass("java/io/FileDescriptor");
    if (cls == nullptr) {
        env->ExceptionClear();
        return -1;
    }

    jfieldID fid = env->GetFieldID(cls, "descriptor", "I");
    if (fid == nullptr) {
        env->ExceptionClear();
        fid = env->GetFieldID(cls, "fd", "I");
        if (fid == nullptr) {
            env->ExceptionClear();
            return -1;
        }
    }

    return env->GetIntField(fileDescriptor, fid);
}

static void put_i32_be(uint8_t* p, int32_t v) {
    uint32_t u = static_cast<uint32_t>(v);
    p[0] = static_cast<uint8_t>((u >> 24) & 0xFF);
    p[1] = static_cast<uint8_t>((u >> 16) & 0xFF);
    p[2] = static_cast<uint8_t>((u >> 8) & 0xFF);
    p[3] = static_cast<uint8_t>(u & 0xFF);
}

static void put_i64_be(uint8_t* p, int64_t v) {
    uint64_t u = static_cast<uint64_t>(v);
    p[0] = static_cast<uint8_t>((u >> 56) & 0xFF);
    p[1] = static_cast<uint8_t>((u >> 48) & 0xFF);
    p[2] = static_cast<uint8_t>((u >> 40) & 0xFF);
    p[3] = static_cast<uint8_t>((u >> 32) & 0xFF);
    p[4] = static_cast<uint8_t>((u >> 24) & 0xFF);
    p[5] = static_cast<uint8_t>((u >> 16) & 0xFF);
    p[6] = static_cast<uint8_t>((u >> 8) & 0xFF);
    p[7] = static_cast<uint8_t>(u & 0xFF);
}

// 分块画质策略：改为按宽度 ROI。常量和 split_edge_parts_each() 已提前声明，供布局函数使用。
static inline int jpeg_quality_for_split_part(int baseQuality, int partIndex, int partCount) {
    int q = clamp_jpeg_quality(baseQuality);
    if (partCount < SPLIT_EDGE_QUALITY_MIN_PARTS) return q;
    const int edgeParts = split_edge_parts_each(partCount);
    const int centerStart = edgeParts;
    const int centerEnd = partCount - edgeParts;
    if (partIndex < centerStart || partIndex >= centerEnd) {
        q = clamp_jpeg_quality(q - current_split_edge_quality_reduction());
    }
    return q;
}

static inline int make_even_floor(int value) {
    return value & ~1;
}

static bool has_output_fd() {
    return g_output_fd_available.load(std::memory_order_acquire);
}

static void clear_last_split_part_stats(int fromIndex = 0) {
    const int start = std::max(0, std::min(fromIndex, MAX_SPLIT_PARTS));
    for (int i = start; i < MAX_SPLIT_PARTS; ++i) {
        g_last_split_part_bytes[i].store(0, std::memory_order_release);
        g_last_split_part_encode_ns[i].store(0, std::memory_order_release);
        g_last_split_part_cpu[i].store(-1, std::memory_order_release);
    }
}

static void close_output_fd_locked() {
    g_output_fd_available.store(false, std::memory_order_release);
    if (g_output_fd >= 0) {
        close(g_output_fd);
        g_output_fd = -1;
    }
}

static bool writev_all_two(
        int fd,
        const void* first,
        size_t firstSize,
        const void* second,
        size_t secondSize) {
    iovec iov[2]{};
    iov[0].iov_base = const_cast<void*>(first);
    iov[0].iov_len = firstSize;
    iov[1].iov_base = const_cast<void*>(second);
    iov[1].iov_len = secondSize;
    int iovCount = 2;

    while (iovCount > 0) {
        ssize_t n = writev(fd, iov, iovCount);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false;

        size_t written = static_cast<size_t>(n);
        while (iovCount > 0 && written >= iov[0].iov_len) {
            written -= iov[0].iov_len;
            if (iovCount == 2) {
                iov[0] = iov[1];
            }
            --iovCount;
        }
        if (iovCount > 0 && written > 0) {
            iov[0].iov_base = static_cast<uint8_t*>(iov[0].iov_base) + written;
            iov[0].iov_len -= written;
        }
    }
    return true;
}

static bool writev_all_iovs(int fd, iovec* iov, int iovCount) {
    while (iovCount > 0) {
        ssize_t n = writev(fd, iov, iovCount);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false;

        size_t written = static_cast<size_t>(n);
        while (iovCount > 0 && written >= iov[0].iov_len) {
            written -= iov[0].iov_len;
            for (int i = 1; i < iovCount; ++i) {
                iov[i - 1] = iov[i];
            }
            --iovCount;
        }
        if (iovCount > 0 && written > 0) {
            iov[0].iov_base = static_cast<uint8_t*>(iov[0].iov_base) + written;
            iov[0].iov_len -= written;
        }
    }
    return true;
}

static bool send_encoded_jpeg_short_lock(
        const unsigned char* jpegBuf,
        unsigned long jpegSize,
        int width,
        int height,
        int64_t frameProducedNs,
        int64_t callbackStartNs,
        int64_t encodeStartNs,
        int64_t encodeEndNs) {
    if (jpegBuf == nullptr || jpegSize == 0 || width <= 0 || height <= 0) return false;

    // Two encoder workers may finish out of order. Drop old frames before waiting on send lock.
    int64_t last = g_last_sent_frame_produced_ns.load(std::memory_order_acquire);
    if (frameProducedNs > 0 && last > 0 && frameProducedNs <= last) {
        return true;
    }

    uint8_t header[HEADER_SIZE]{};
    uint8_t* p = header;
    put_i32_be(p, FRAME_MAGIC); p += 4;
    put_i32_be(p, HEADER_VERSION); p += 4;
    put_i32_be(p, width); p += 4;
    put_i32_be(p, height); p += 4;
    put_i32_be(p, static_cast<int32_t>(jpegSize)); p += 4;
    put_i64_be(p, frameProducedNs); p += 8;
    put_i64_be(p, callbackStartNs); p += 8;
    put_i64_be(p, encodeStartNs); p += 8;
    put_i64_be(p, encodeEndNs); p += 8;
    put_i64_be(p, mono_now_ns()); p += 8;
    put_i64_be(p, wall_now_ms()); p += 8;

    // Only serialize the socket write. Do not hold this mutex while tjCompress2/tjCompressFromYUV runs.
    std::lock_guard<std::mutex> lk(g_send_mutex);

    last = g_last_sent_frame_produced_ns.load(std::memory_order_acquire);
    if (frameProducedNs > 0 && last > 0 && frameProducedNs <= last) {
        return true;
    }

    const int fd = g_output_fd;
    if (fd < 0) return false;

    const int64_t writeStartNs = mono_now_ns();
    const bool writeOk = writev_all_two(fd, header, sizeof(header), jpegBuf, static_cast<size_t>(jpegSize));
    const int64_t writeEndNs = mono_now_ns();
    if (!writeOk) {
        if (g_output_fd == fd) {
            close_output_fd_locked();
        }
        return false;
    }

    g_last_sent_total_bytes.store(static_cast<int64_t>(jpegSize), std::memory_order_release);
    g_last_sent_part0_bytes.store(static_cast<int64_t>(jpegSize), std::memory_order_release);
    g_last_sent_part1_bytes.store(0, std::memory_order_release);
    g_last_socket_write_ns.store((writeEndNs - writeStartNs), std::memory_order_release);
    g_last_part0_encode_ns.store((encodeEndNs - encodeStartNs), std::memory_order_release);
    g_last_part1_encode_ns.store(0, std::memory_order_release);
    g_last_split_parts.store(1, std::memory_order_release);
    g_last_split_part_bytes[0].store(static_cast<int64_t>(jpegSize), std::memory_order_release);
    g_last_split_part_encode_ns[0].store((encodeEndNs - encodeStartNs), std::memory_order_release);
    g_last_split_part_cpu[0].store(current_cpu_id(), std::memory_order_release);
    clear_last_split_part_stats(1);

    if (frameProducedNs > 0) {
        g_last_sent_frame_produced_ns.store(frameProducedNs, std::memory_order_release);
    }
    return true;
}

static bool is_region_in_bounds(
        jint bufferCapacity,
        jint rowStride,
        jint pixelStride,
        jint left,
        jint top,
        jint width,
        jint height) {
    if (bufferCapacity <= 0 || rowStride <= 0 || pixelStride <= 0 || width <= 0 || height <= 0) return false;
    if (left < 0 || top < 0) return false;

    const int64_t start = static_cast<int64_t>(top) * rowStride + static_cast<int64_t>(left) * pixelStride;
    const int64_t end = start
            + static_cast<int64_t>(height - 1) * rowStride
            + static_cast<int64_t>(width) * pixelStride;
    return start >= 0 && end <= static_cast<int64_t>(bufferCapacity);
}

static bool normalize_crop_420(
        jint srcWidth,
        jint srcHeight,
        jint cropSize,
        int& cropLeft,
        int& cropTop,
        int& cropWidth,
        int& cropHeight) {
    if (srcWidth <= 1 || srcHeight <= 1 || cropSize <= 1) return false;

    cropWidth = make_even_floor(std::min(static_cast<int>(cropSize), static_cast<int>(srcWidth)));
    cropHeight = make_even_floor(std::min(static_cast<int>(cropSize), static_cast<int>(srcHeight)));
    if (cropWidth <= 1 || cropHeight <= 1) return false;

    cropLeft = make_even_floor((static_cast<int>(srcWidth) - cropWidth) / 2);
    cropTop = make_even_floor((static_cast<int>(srcHeight) - cropHeight) / 2);
    if (cropLeft < 0 || cropTop < 0) return false;
    if (cropLeft + cropWidth > srcWidth || cropTop + cropHeight > srcHeight) return false;
    return true;
}

static bool normalize_full_frame_420(jint srcWidth, jint srcHeight, int& width, int& height) {
    width = make_even_floor(static_cast<int>(srcWidth));
    height = make_even_floor(static_cast<int>(srcHeight));
    return width > 1 && height > 1;
}

static bool copy_yuv420888_region_to_i420(
        const uint8_t* yBase,
        jint yCapacity,
        const uint8_t* uBase,
        jint uCapacity,
        const uint8_t* vBase,
        jint vCapacity,
        jint srcWidth,
        jint srcHeight,
        jint yRowStride,
        jint uRowStride,
        jint vRowStride,
        jint uPixelStride,
        jint vPixelStride,
        jint cropLeft,
        jint cropTop,
        jint cropWidth,
        jint cropHeight) {
    if (yBase == nullptr || uBase == nullptr || vBase == nullptr) return false;
    if (srcWidth <= 0 || srcHeight <= 0) return false;
    if (yRowStride <= 0 || uRowStride <= 0 || vRowStride <= 0) return false;
    if (uPixelStride <= 0 || vPixelStride <= 0) return false;
    if (cropLeft < 0 || cropTop < 0 || cropWidth <= 0 || cropHeight <= 0) return false;
    if ((cropLeft & 1) != 0 || (cropTop & 1) != 0 || (cropWidth & 1) != 0 || (cropHeight & 1) != 0) return false;
    if (cropLeft + cropWidth > srcWidth || cropTop + cropHeight > srcHeight) return false;

    const int chromaLeft = cropLeft / 2;
    const int chromaTop = cropTop / 2;
    const int chromaWidth = cropWidth / 2;
    const int chromaHeight = cropHeight / 2;

    if (!is_region_in_bounds(yCapacity, yRowStride, 1, cropLeft, cropTop, cropWidth, cropHeight)) {
        ALOGE("Y plane bounds invalid");
        return false;
    }
    if (!is_region_in_bounds(uCapacity, uRowStride, uPixelStride, chromaLeft, chromaTop, chromaWidth, chromaHeight)) {
        ALOGE("U plane bounds invalid");
        return false;
    }
    if (!is_region_in_bounds(vCapacity, vRowStride, vPixelStride, chromaLeft, chromaTop, chromaWidth, chromaHeight)) {
        ALOGE("V plane bounds invalid");
        return false;
    }

    const size_t yPlaneSize = static_cast<size_t>(cropWidth) * static_cast<size_t>(cropHeight);
    const size_t chromaPlaneSize = static_cast<size_t>(chromaWidth) * static_cast<size_t>(chromaHeight);
    const size_t totalSize = yPlaneSize + chromaPlaneSize * 2u;
    if (g_tls.yuv420Buffer.size() < totalSize) {
        g_tls.yuv420Buffer.resize(totalSize);
    }

    uint8_t* yDst = g_tls.yuv420Buffer.data();
    uint8_t* uDst = yDst + yPlaneSize;
    uint8_t* vDst = uDst + chromaPlaneSize;

    for (int y = 0; y < cropHeight; ++y) {
        const uint8_t* src = yBase + static_cast<int64_t>(cropTop + y) * yRowStride + cropLeft;
        std::memcpy(yDst + static_cast<size_t>(y) * cropWidth, src, static_cast<size_t>(cropWidth));
    }

    for (int y = 0; y < chromaHeight; ++y) {
        const uint8_t* uSrcRow = uBase + static_cast<int64_t>(chromaTop + y) * uRowStride + static_cast<int64_t>(chromaLeft) * uPixelStride;
        const uint8_t* vSrcRow = vBase + static_cast<int64_t>(chromaTop + y) * vRowStride + static_cast<int64_t>(chromaLeft) * vPixelStride;
        uint8_t* uDstRow = uDst + static_cast<size_t>(y) * chromaWidth;
        uint8_t* vDstRow = vDst + static_cast<size_t>(y) * chromaWidth;

        if (uPixelStride == 1) {
            std::memcpy(uDstRow, uSrcRow, static_cast<size_t>(chromaWidth));
        } else {
            for (int x = 0; x < chromaWidth; ++x) {
                uDstRow[x] = uSrcRow[static_cast<size_t>(x) * uPixelStride];
            }
        }

        if (vPixelStride == 1) {
            std::memcpy(vDstRow, vSrcRow, static_cast<size_t>(chromaWidth));
        } else {
            for (int x = 0; x < chromaWidth; ++x) {
                vDstRow[x] = vSrcRow[static_cast<size_t>(x) * vPixelStride];
            }
        }
    }

    return true;
}

static jbyteArray jpeg_to_byte_array(JNIEnv* env, const unsigned char* jpegBuf, unsigned long jpegSize) {
    if (jpegBuf == nullptr || jpegSize == 0) return nullptr;
    jbyteArray out = env->NewByteArray(static_cast<jsize>(jpegSize));
    if (out == nullptr) return nullptr;
    env->SetByteArrayRegion(out, 0, static_cast<jsize>(jpegSize), reinterpret_cast<const jbyte*>(jpegBuf));
    return out;
}

static bool compress_pixels_raw(
        unsigned char* pixels,
        int width,
        int height,
        int pitch,
        int pixelFormat,
        int jpegQuality,
        unsigned char*& jpegBuf,
        unsigned long& jpegSize) {
    if (pixels == nullptr || width <= 0 || height <= 0) return false;

    tjhandle compressor = get_compressor();
    if (compressor == nullptr) {
        ALOGE("tjInitCompress failed");
        return false;
    }

    const int quality = clamp_jpeg_quality(jpegQuality);
    const int subsamp = current_jpeg_subsamp();
    const unsigned long requiredSize = tjBufSize(width, height, subsamp);
    if (requiredSize == 0) {
        ALOGE("tjBufSize failed width=%d height=%d", width, height);
        return false;
    }
    if (!ensure_jpeg_buffer(requiredSize)) {
        return false;
    }

    jpegBuf = g_tls.jpegBuffer;
    jpegSize = g_tls.jpegCapacity;
    const int flags = TJFLAG_FASTDCT | TJFLAG_NOREALLOC;
    const int tjPitch = normalize_pitch_for_tj(width, pitch, pixelFormat);

    const int ret = tjCompress2(
            compressor,
            pixels,
            width,
            tjPitch,
            height,
            pixelFormat,
            &jpegBuf,
            &jpegSize,
            subsamp,
            quality,
            flags);

    if (ret != 0 || jpegBuf == nullptr || jpegSize == 0) {
        ALOGE("tjCompress2 failed: %s", tjGetErrorStr());
        return false;
    }
    return true;
}

static bool compress_pixels_scaled_x_raw(
        unsigned char* pixels,
        int sourceWidth,
        int encodedWidth,
        int height,
        int pitch,
        int pixelFormat,
        int jpegQuality,
        unsigned char*& jpegBuf,
        unsigned long& jpegSize) {
    if (pixels == nullptr || sourceWidth <= 0 || encodedWidth <= 0 || height <= 0 || pitch <= 0) return false;
    if (encodedWidth >= sourceWidth) {
        return compress_pixels_raw(pixels, sourceWidth, height, pitch, pixelFormat, jpegQuality, jpegBuf, jpegSize);
    }

    const int pixelSize = tj_pixel_size_for_format(pixelFormat);
    if (pixelSize != 3 && pixelSize != 4) return false;

    const size_t outBytes = static_cast<size_t>(encodedWidth) * static_cast<size_t>(height) * static_cast<size_t>(pixelSize);
    if (g_tls.rgbBuffer.size() < outBytes) {
        g_tls.rgbBuffer.resize(outBytes);
    }

    if (g_tls.scaleXSourceWidth != sourceWidth ||
        g_tls.scaleXEncodedWidth != encodedWidth ||
        g_tls.scaleXPixelSize != pixelSize ||
        static_cast<int>(g_tls.scaleXOffsets.size()) != encodedWidth) {
        g_tls.scaleXSourceWidth = sourceWidth;
        g_tls.scaleXEncodedWidth = encodedWidth;
        g_tls.scaleXPixelSize = pixelSize;
        g_tls.scaleXOffsets.resize(static_cast<size_t>(encodedWidth));
        for (int x = 0; x < encodedWidth; ++x) {
            int sx = static_cast<int>((static_cast<int64_t>(x) * sourceWidth + encodedWidth / 2) / encodedWidth);
            if (sx >= sourceWidth) sx = sourceWidth - 1;
            g_tls.scaleXOffsets[static_cast<size_t>(x)] = sx * pixelSize;
        }
    }

    uint8_t* dstBase = g_tls.rgbBuffer.data();
    const int dstPitch = encodedWidth * pixelSize;
    const int* xOffsets = g_tls.scaleXOffsets.data();
    for (int y = 0; y < height; ++y) {
        const uint8_t* srcRow = pixels + static_cast<int64_t>(y) * pitch;
        uint8_t* dstRow = dstBase + static_cast<size_t>(y) * static_cast<size_t>(dstPitch);
        if (pixelSize == 4) {
            for (int x = 0; x < encodedWidth; ++x) {
                std::memcpy(dstRow + static_cast<size_t>(x) * 4u, srcRow + xOffsets[x], 4u);
            }
        } else {
            for (int x = 0; x < encodedWidth; ++x) {
                const uint8_t* src = srcRow + xOffsets[x];
                uint8_t* dst = dstRow + static_cast<size_t>(x) * 3u;
                dst[0] = src[0];
                dst[1] = src[1];
                dst[2] = src[2];
            }
        }
    }

    return compress_pixels_raw(reinterpret_cast<unsigned char*>(dstBase),
                               encodedWidth,
                               height,
                               dstPitch,
                               pixelFormat,
                               jpegQuality,
                               jpegBuf,
                               jpegSize);
}


struct SplitEncodeJob {
    unsigned char* pixels = nullptr;
    // width/height are JPEG encoded dimensions. sourceWidth is the original/display width
    // sampled from the ImageReader buffer before optional horizontal downscale.
    int width = 0;
    int height = 0;
    int sourceWidth = 0;
    int pitch = 0;
    int pixelFormat = TJPF_RGBA;
    int jpegQuality = 80;
    // 分块结果先从 worker 的 TurboJPEG TLS buffer 复制到 ownedJpegBuffer。
    // 这样 caller 可以在 socket writev 之前立刻给同一个 worker 派下一个 part，
    // 避免 ADB/TCP 写入反向卡住 JPEG 编码流水线。
    std::vector<uint8_t> ownedJpegBuffer;
    const unsigned char* jpegPtr = nullptr;
    size_t jpegSize = 0;
    bool hasJob = false;
    bool done = false;
    bool ok = false;
    bool skipped = false;
    int64_t dispatchNs = 0;
    int64_t encodeStartNs = 0;
    int64_t encodeEndNs = 0;
    int cpuId = -1;
    int partIndex = -1;
    int partLeft = 0;
};

struct EncodedJpegRef {
    const unsigned char* data = nullptr;
    size_t size = 0;
    bool skipped = false;
};

// 高速模式明确禁用上一帧复用。
// 不做像素对比，不做 sample hash，不做 full hash，不产生空 payload part；
// 每个 part 每帧都真实 JPEG 编码并发送。
static void reset_split_no_prev_reuse_state() {
    // Intentionally empty. Kept only for stale/error cleanup call sites.
}

// 分块真实编码耗时 EMA：用于“耗时最长的图像区域 -> 高频 CPU”。
// ROI 模式下 JPEG KB 会被低质量/低分辨率策略扭曲，不能代表编码工作量。
// 这里只记录耗时趋势用于负载均衡，不做像素对比，也不复用上一帧画面。
static std::mutex g_split_complexity_mutex;
static int g_split_complexity_part_count = 0;
static double g_split_complexity_ema_bytes[MAX_SPLIT_PARTS]{};
static constexpr double SPLIT_COMPLEXITY_EMA_ALPHA = 0.35;

static void reset_split_complexity_locked(int partCount) {
    g_split_complexity_part_count = partCount;
    for (int i = 0; i < MAX_SPLIT_PARTS; ++i) {
        g_split_complexity_ema_bytes[i] = 0.0;
    }
}

static void update_split_complexity_history(
        int partCount,
        const std::vector<EncodedJpegRef>& partJpegs,
        const int partHeights[]) {
    if (partCount <= 0 || partCount > MAX_SPLIT_PARTS) return;
    if (static_cast<int>(partJpegs.size()) < partCount) return;
    if (partHeights == nullptr) return;

    std::lock_guard<std::mutex> lk(g_split_complexity_mutex);
    if (g_split_complexity_part_count != partCount) {
        reset_split_complexity_locked(partCount);
    }

    for (int i = 0; i < partCount; ++i) {
        // partIndex 对应稳定屏幕区域，直接用 JPEG 总 bytes 作为工作量近似。
        // 这里不除以高度：高频 CPU 本来就应该优先吃“总工作量更大”的块。
        if (partHeights[i] <= 0) continue;
        const double cur = static_cast<double>(partJpegs[static_cast<size_t>(i)].size);
        if (!(cur > 0.0)) continue;
        double& ema = g_split_complexity_ema_bytes[i];
        if (!(ema > 0.0)) ema = cur;
        else ema = ema * (1.0 - SPLIT_COMPLEXITY_EMA_ALPHA) + cur * SPLIT_COMPLEXITY_EMA_ALPHA;
    }
}


static void update_split_complexity_history_fixed(
        int partCount,
        const int64_t partEncodeNs[],
        const int partWidths[],
        const int partHeights[]) {
    if (partCount <= 0 || partCount > MAX_SPLIT_PARTS) return;
    if (partEncodeNs == nullptr || partWidths == nullptr || partHeights == nullptr) return;

    std::lock_guard<std::mutex> lk(g_split_complexity_mutex);
    if (g_split_complexity_part_count != partCount) {
        reset_split_complexity_locked(partCount);
    }

    for (int i = 0; i < partCount; ++i) {
        if (partWidths[i] <= 0 || partHeights[i] <= 0) continue;

        // ROI mode makes JPEG bytes misleading:
        // low-quality edge parts may be tiny in KB, but still scan a large display area.
        // Use the recent real measured encode time instead.
        const double cur = static_cast<double>(partEncodeNs[i]);
        if (!(cur > 0.0)) continue;

        double& ema = g_split_complexity_ema_bytes[i];
        if (!(ema > 0.0)) ema = cur;
        else ema = ema * (1.0 - SPLIT_COMPLEXITY_EMA_ALPHA) + cur * SPLIT_COMPLEXITY_EMA_ALPHA;
    }
}

static bool build_worker_part_assignment(int partCount, int outPartIndexForWorker[]) {
    // 这里的 outPartIndexForWorker 实际上是“任务优先级顺序”。
    // 固定 7 worker + 12 task 时，worker 会按这个顺序抢任务：重任务先发，轻任务后发。
    if (partCount <= 0 || partCount > MAX_SPLIT_PARTS || outPartIndexForWorker == nullptr) return false;

    for (int i = 0; i < partCount; ++i) outPartIndexForWorker[i] = i;

    double score[MAX_SPLIT_PARTS]{};
    bool hasHistory = false;
    {
        std::lock_guard<std::mutex> lk(g_split_complexity_mutex);
        if (g_split_complexity_part_count == partCount) {
            for (int i = 0; i < partCount; ++i) {
                score[i] = g_split_complexity_ema_bytes[i];
                if (score[i] > 0.0) hasHistory = true;
            }
        }
    }

    struct PartScore {
        int partIndex;
        double scoreNs;
    };

    PartScore parts[MAX_SPLIT_PARTS]{};
    for (int i = 0; i < partCount; ++i) {
        // 首帧没有历史时也不要从左到右排：中心 Q 区优先，边缘 Q-10 区靠后。
        double s = hasHistory ? score[i] : 0.0;
        if (!hasHistory) {
            s = split_part_is_edge_region(i, partCount) ? 1.0 : 2.0;
        }
        parts[i] = PartScore{i, s};
    }

    std::stable_sort(parts, parts + partCount, [](const PartScore& a, const PartScore& b) {
        if (a.scoreNs != b.scoreNs) return a.scoreNs > b.scoreNs;
        return a.partIndex < b.partIndex;
    });

    for (int order = 0; order < partCount; ++order) {
        outPartIndexForWorker[order] = parts[order].partIndex;
    }
    return true;
}

static bool build_worker_task_assignment_by_geometry(
        int partCount,
        const int partWidths[],
        const int partHeights[],
        const int partJpegQualities[],
        int baseJpegQuality,
        int outPartIndexForWorker[]) {
    if (partCount <= 0 || partCount > MAX_SPLIT_PARTS || outPartIndexForWorker == nullptr) return false;
    if (partWidths == nullptr || partHeights == nullptr || partJpegQualities == nullptr) return false;

    struct PartScore { int partIndex; double score; };
    PartScore parts[MAX_SPLIT_PARTS]{};
    for (int i = 0; i < partCount; ++i) {
        const double area = static_cast<double>(std::max(1, partWidths[i])) * static_cast<double>(std::max(1, partHeights[i]));
        const bool highQ = partJpegQualities[i] >= baseJpegQuality;
        // No previous-frame or CPU-time prediction here: next frame and CPU availability are unstable.
        // Use only static risk known before encoding: pixel area and quality.
        const double qualityWeight = highQ ? 1.45 : 0.80;
        parts[i] = PartScore{i, area * qualityWeight};
    }

    std::stable_sort(parts, parts + partCount, [](const PartScore& a, const PartScore& b) {
        if (a.score != b.score) return a.score > b.score;
        return a.partIndex < b.partIndex;
    });
    for (int i = 0; i < partCount; ++i) outPartIndexForWorker[i] = parts[i].partIndex;
    return true;
}


static std::mutex g_split_call_mutex;
static std::mutex g_split_encode_mutex;
static std::condition_variable g_split_worker_cvs[MAX_SPLIT_PARTS];
static std::condition_variable g_split_encode_done_cv;
static std::once_flag g_split_encode_once;
static SplitEncodeJob g_split_encode_jobs[MAX_SPLIT_PARTS];

static void boost_current_split_worker_thread_once(int index) {
    static thread_local bool boosted = false;
    if (boosted) return;
    boosted = true;

    char name[16]{};
    snprintf(name, sizeof(name), "HLJpeg%d", index);
    prctl(PR_SET_NAME, name, 0, 0, 0);

    const int tid = static_cast<int>(syscall(SYS_gettid));
    errno = 0;
    if (setpriority(PRIO_PROCESS, tid, -8) != 0 && ENABLE_NATIVE_DEBUG_LOGS) {
        ALOGE("split setpriority worker=%d tid=%d failed errno=%d %s", index, tid, errno, strerror(errno));
    }
}

static void warmup_current_thread_jpeg_once() {
    static thread_local bool warmed420 = false;
    static thread_local bool warmed444 = false;
    const int mode = g_jpeg_subsampling_mode.load(std::memory_order_acquire);
    bool& warmed = (mode == 444) ? warmed444 : warmed420;
    if (warmed) return;

    constexpr int kWarmW = 64;
    constexpr int kWarmH = 64;
    static thread_local std::vector<uint8_t> dummy;
    dummy.assign(static_cast<size_t>(kWarmW) * kWarmH * 4u, 0);

    unsigned char* jpegBuf = nullptr;
    unsigned long jpegSize = 0;
    (void)compress_pixels_raw(dummy.data(), kWarmW, kWarmH, kWarmW * 4, TJPF_RGBA, 80, jpegBuf, jpegSize);
    warmed = true;
}

static void split_encode_worker_loop(int index) {
    int lastBoundCpu = -2;
    boost_current_split_worker_thread_once(index);

    for (;;) {
        const int targetCpu = split_worker_target_cpu(index);
        if (targetCpu != lastBoundCpu && targetCpu >= 0 && targetCpu < CPU_SETSIZE && cpu_is_online(targetCpu)) {
            cpu_set_t set;
            CPU_ZERO(&set);
            CPU_SET(targetCpu, &set);
            if (sched_setaffinity(0, sizeof(set), &set) != 0) {
                ALOGE("split sched_setaffinity worker=%d cpu=%d failed: %s", index, targetCpu, strerror(errno));
            } else {
                lastBoundCpu = targetCpu;
            }
        }

        warmup_current_thread_jpeg_once();

        unsigned char* pixels = nullptr;
        int width = 0;
        int height = 0;
        int sourceWidth = 0;
        int pitch = 0;
        int pixelFormat = TJPF_RGBA;
        int jpegQuality = 80;
        {
            std::unique_lock<std::mutex> lk(g_split_encode_mutex);
            g_split_worker_cvs[index].wait(lk, [index] { return g_split_encode_jobs[index].hasJob; });
            auto& job = g_split_encode_jobs[index];
            pixels = job.pixels;
            width = job.width;
            height = job.height;
            sourceWidth = job.sourceWidth > 0 ? job.sourceWidth : job.width;
            pitch = job.pitch;
            pixelFormat = job.pixelFormat;
            jpegQuality = job.jpegQuality;
            job.hasJob = false;
        }

        unsigned char* jpegBuf = nullptr;
        unsigned long jpegSize = 0;
        const int64_t startNs = mono_now_ns();
        const int cpuAtEncode = current_cpu_id();
        bool ok = true;
        if (sourceWidth > width) {
            ok = compress_pixels_scaled_x_raw(pixels, sourceWidth, width, height, pitch, pixelFormat, jpegQuality, jpegBuf, jpegSize);
        } else {
            ok = compress_pixels_raw(pixels, width, height, pitch, pixelFormat, jpegQuality, jpegBuf, jpegSize);
        }
        bool hasPayload = ok && jpegBuf != nullptr && jpegSize > 0;


        {
            std::lock_guard<std::mutex> lk(g_split_encode_mutex);
            auto& job = g_split_encode_jobs[index];
            if (hasPayload) {
                job.ownedJpegBuffer.resize(static_cast<size_t>(jpegSize));
                std::memcpy(job.ownedJpegBuffer.data(), jpegBuf, static_cast<size_t>(jpegSize));
                job.jpegPtr = job.ownedJpegBuffer.data();
                job.jpegSize = job.ownedJpegBuffer.size();
            } else {
                job.ownedJpegBuffer.clear();
                job.jpegPtr = nullptr;
                job.jpegSize = 0;
            }
            const int64_t endNs = mono_now_ns();
            job.skipped = false;
            job.ok = ok && job.jpegPtr != nullptr && job.jpegSize > 0;
            job.encodeStartNs = startNs;
            job.encodeEndNs = endNs;
            job.cpuId = cpuAtEncode;
            job.done = true;
        }
        g_split_encode_done_cv.notify_one();
    }
}

static void ensure_split_encode_workers_started() {
    std::call_once(g_split_encode_once, [] {
        for (int i = 0; i < MAX_SPLIT_PARTS; ++i) std::thread(split_encode_worker_loop, i).detach();
    });
}

static bool encode_split_parallel(
        int partCount,
        unsigned char* partPixels[],
        const int partHeights[],
        const int workerPartMap[],
        int width,
        int pitch,
        int pixelFormat,
        int jpegQuality,
        std::vector<EncodedJpegRef>& partJpegs,
        int64_t& encodeStartNs,
        int64_t& encodeEndNs,
        std::vector<int64_t>& partEncodeNs,
        std::vector<int>& partCpuIds) {
    if (partCount <= 0 || partCount > MAX_SPLIT_PARTS) return false;
    ensure_split_encode_workers_started();

    bool usedParts[MAX_SPLIT_PARTS]{};

    std::unique_lock<std::mutex> lk(g_split_encode_mutex);
    for (int worker = 0; worker < partCount; ++worker) {
        const int partIndex = workerPartMap != nullptr ? workerPartMap[worker] : worker;
        if (partIndex < 0 || partIndex >= partCount || usedParts[partIndex]) return false;
        usedParts[partIndex] = true;

        auto& job = g_split_encode_jobs[worker];
        job.pixels = partPixels[partIndex];
        job.width = width;
        job.height = partHeights[partIndex];
        job.sourceWidth = width;
        job.pitch = pitch;
        job.pixelFormat = pixelFormat;
        job.jpegQuality = jpeg_quality_for_split_part(jpegQuality, partIndex, partCount);
        job.jpegPtr = nullptr;
        job.jpegSize = 0;
        job.ok = false;
        job.skipped = false;
        job.done = false;
        job.hasJob = true;
        job.dispatchNs = mono_now_ns();
        job.encodeStartNs = 0;
        job.encodeEndNs = 0;
        job.cpuId = -1;
        job.partIndex = partIndex;
    }
    for (int worker = 0; worker < partCount; ++worker) {
        g_split_worker_cvs[worker].notify_one();
    }
    g_split_encode_done_cv.wait(lk, [partCount] {
        for (int i = 0; i < partCount; ++i) if (!g_split_encode_jobs[i].done) return false;
        return true;
    });

    bool ok = true;
    partJpegs.assign(static_cast<size_t>(partCount), {});
    partEncodeNs.assign(static_cast<size_t>(partCount), 0);
    partCpuIds.assign(static_cast<size_t>(partCount), -1);
    encodeStartNs = 0;
    encodeEndNs = 0;
    for (int worker = 0; worker < partCount; ++worker) {
        auto& job = g_split_encode_jobs[worker];
        const int partIndex = job.partIndex;
        ok = ok && job.ok && partIndex >= 0 && partIndex < partCount;
        if (worker == 0 || job.encodeStartNs < encodeStartNs) encodeStartNs = job.encodeStartNs;
        if (job.encodeEndNs > encodeEndNs) encodeEndNs = job.encodeEndNs;
        if (partIndex >= 0 && partIndex < partCount) {
            partEncodeNs[static_cast<size_t>(partIndex)] = job.encodeEndNs - job.encodeStartNs;
            partCpuIds[static_cast<size_t>(partIndex)] = job.cpuId;
            partJpegs[static_cast<size_t>(partIndex)] = EncodedJpegRef{job.jpegPtr, job.jpegSize, job.skipped};
        }
    }
    return ok;
}


// 固定数组版本：正式热路径使用，避免每帧 std::vector assign/扩容。
static bool encode_split_parallel_fixed(
        int partCount,
        unsigned char* partPixels[],
        const int partHeights[],
        const int workerPartMap[],
        int width,
        int pitch,
        int pixelFormat,
        int jpegQuality,
        EncodedJpegRef partJpegs[],
        int64_t& encodeStartNs,
        int64_t& encodeEndNs,
        int64_t partEncodeNs[],
        int partCpuIds[]) {
    if (partCount <= 0 || partCount > MAX_SPLIT_PARTS) return false;
    if (partJpegs == nullptr || partEncodeNs == nullptr || partCpuIds == nullptr) return false;
    ensure_split_encode_workers_started();

    bool usedParts[MAX_SPLIT_PARTS]{};

    std::unique_lock<std::mutex> lk(g_split_encode_mutex);
    for (int worker = 0; worker < partCount; ++worker) {
        const int partIndex = workerPartMap != nullptr ? workerPartMap[worker] : worker;
        if (partIndex < 0 || partIndex >= partCount || usedParts[partIndex]) return false;
        usedParts[partIndex] = true;

        auto& job = g_split_encode_jobs[worker];
        job.pixels = partPixels[partIndex];
        job.width = width;
        job.height = partHeights[partIndex];
        job.sourceWidth = width;
        job.pitch = pitch;
        job.pixelFormat = pixelFormat;
        job.jpegQuality = jpeg_quality_for_split_part(jpegQuality, partIndex, partCount);
        job.jpegPtr = nullptr;
        job.jpegSize = 0;
        job.ok = false;
        job.skipped = false;
        job.done = false;
        job.hasJob = true;
        job.dispatchNs = mono_now_ns();
        job.encodeStartNs = 0;
        job.encodeEndNs = 0;
        job.cpuId = -1;
        job.partIndex = partIndex;
    }
    for (int worker = 0; worker < partCount; ++worker) {
        g_split_worker_cvs[worker].notify_one();
    }
    g_split_encode_done_cv.wait(lk, [partCount] {
        for (int i = 0; i < partCount; ++i) if (!g_split_encode_jobs[i].done) return false;
        return true;
    });

    bool ok = true;
    for (int i = 0; i < MAX_SPLIT_PARTS; ++i) {
        partJpegs[i] = EncodedJpegRef{};
        partEncodeNs[i] = 0;
        partCpuIds[i] = -1;
    }
    encodeStartNs = 0;
    encodeEndNs = 0;
    for (int worker = 0; worker < partCount; ++worker) {
        auto& job = g_split_encode_jobs[worker];
        const int partIndex = job.partIndex;
        ok = ok && job.ok && partIndex >= 0 && partIndex < partCount;
        if (worker == 0 || job.encodeStartNs < encodeStartNs) encodeStartNs = job.encodeStartNs;
        if (job.encodeEndNs > encodeEndNs) encodeEndNs = job.encodeEndNs;
        if (partIndex >= 0 && partIndex < partCount) {
            partEncodeNs[partIndex] = job.encodeEndNs - job.encodeStartNs;
            partCpuIds[partIndex] = job.cpuId;
            partJpegs[partIndex] = EncodedJpegRef{job.jpegPtr, job.jpegSize, job.skipped};
        }
    }
    return ok;
}

static void put_v3_header(
        uint8_t* header,
        int partWidth,
        int partHeight,
        int jpegSize,
        int64_t frameProducedNs,
        int64_t callbackStartNs,
        int64_t encodeStartNs,
        int64_t encodeEndNs,
        int partIndex,
        int partCount,
        int fullWidth,
        int fullHeight,
        int partTop,
        int partEncodeUs,
        int partCpuId) {
    uint8_t* p = header;
    put_i32_be(p, FRAME_MAGIC); p += 4;
    put_i32_be(p, HEADER_VERSION_SPLIT2); p += 4;
    put_i32_be(p, partWidth); p += 4;
    put_i32_be(p, partHeight); p += 4;
    put_i32_be(p, jpegSize); p += 4;
    put_i64_be(p, frameProducedNs); p += 8;
    put_i64_be(p, callbackStartNs); p += 8;
    put_i64_be(p, encodeStartNs); p += 8;
    put_i64_be(p, encodeEndNs); p += 8;
    put_i64_be(p, mono_now_ns()); p += 8;
    put_i64_be(p, wall_now_ms()); p += 8;
    put_i32_be(p, partIndex); p += 4;
    put_i32_be(p, partCount); p += 4;
    put_i32_be(p, fullWidth); p += 4;
    put_i32_be(p, fullHeight); p += 4;
    put_i32_be(p, partTop); p += 4;
    put_i32_be(p, partHeight); p += 4;
    put_i32_be(p, partEncodeUs); p += 4;
    put_i32_be(p, partCpuId); p += 4;
}


static void put_v5_header(
        uint8_t* header,
        int partWidth,
        int partHeight,
        int jpegSize,
        int64_t frameProducedNs,
        int64_t callbackStartNs,
        int64_t encodeStartNs,
        int64_t encodeEndNs,
        int partIndex,
        int partCount,
        int fullWidth,
        int fullHeight,
        int partTop,
        int partEncodeUs,
        int partCpuId,
        int partCpuMaxFreqKhz,
        int partSharePermille,
        int availableEncodeCpuCount,
        int headerVersion = HEADER_VERSION_SPLIT5) {
    uint8_t* p = header;
    put_i32_be(p, FRAME_MAGIC); p += 4;
    put_i32_be(p, headerVersion); p += 4;
    put_i32_be(p, partWidth); p += 4;
    put_i32_be(p, partHeight); p += 4;
    put_i32_be(p, jpegSize); p += 4;
    put_i64_be(p, frameProducedNs); p += 8;
    put_i64_be(p, callbackStartNs); p += 8;
    put_i64_be(p, encodeStartNs); p += 8;
    put_i64_be(p, encodeEndNs); p += 8;
    put_i64_be(p, mono_now_ns()); p += 8;
    put_i64_be(p, wall_now_ms()); p += 8;
    put_i32_be(p, partIndex); p += 4;
    put_i32_be(p, partCount); p += 4;
    put_i32_be(p, fullWidth); p += 4;
    put_i32_be(p, fullHeight); p += 4;
    put_i32_be(p, partTop); p += 4;
    put_i32_be(p, partHeight); p += 4;
    put_i32_be(p, partEncodeUs); p += 4;
    put_i32_be(p, partCpuId); p += 4;
    put_i32_be(p, partCpuMaxFreqKhz); p += 4;
    put_i32_be(p, partSharePermille); p += 4;
    put_i32_be(p, availableEncodeCpuCount); p += 4;
}


static void put_v7_header(
        uint8_t header[HEADER_V7_SIZE],
        int partWidth,
        int partHeight,
        int jpegSize,
        int64_t frameProducedNs,
        int64_t callbackStartNs,
        int64_t encodeStartNs,
        int64_t encodeEndNs,
        int partIndex,
        int partCount,
        int fullWidth,
        int fullHeight,
        int partLeft,
        int partTop,
        int partEncodeUs,
        int partCpuId,
        int partCpuMaxFreqKhz,
        int partSharePermille,
        int availableEncodeCpuCount,
        int headerVersion = HEADER_VERSION_SPLIT7) {
    uint8_t* p = header;
    put_i32_be(p, FRAME_MAGIC); p += 4;
    put_i32_be(p, headerVersion); p += 4;
    put_i32_be(p, partWidth); p += 4;
    put_i32_be(p, partHeight); p += 4;
    put_i32_be(p, jpegSize); p += 4;
    put_i64_be(p, frameProducedNs); p += 8;
    put_i64_be(p, callbackStartNs); p += 8;
    put_i64_be(p, encodeStartNs); p += 8;
    put_i64_be(p, encodeEndNs); p += 8;
    put_i64_be(p, mono_now_ns()); p += 8;
    put_i64_be(p, wall_now_ms()); p += 8;
    put_i32_be(p, partIndex); p += 4;
    put_i32_be(p, partCount); p += 4;
    put_i32_be(p, fullWidth); p += 4;
    put_i32_be(p, fullHeight); p += 4;
    put_i32_be(p, partLeft); p += 4;
    put_i32_be(p, partTop); p += 4;
    put_i32_be(p, partWidth); p += 4;
    put_i32_be(p, partHeight); p += 4;
    put_i32_be(p, partEncodeUs); p += 4;
    put_i32_be(p, partCpuId); p += 4;
    put_i32_be(p, partCpuMaxFreqKhz); p += 4;
    put_i32_be(p, partSharePermille); p += 4;
    put_i32_be(p, availableEncodeCpuCount); p += 4;
}

static void put_v8_header(
        uint8_t header[HEADER_V8_SIZE],
        int encodedWidth,
        int encodedHeight,
        int jpegSize,
        int64_t frameProducedNs,
        int64_t callbackStartNs,
        int64_t encodeStartNs,
        int64_t encodeEndNs,
        int partIndex,
        int partCount,
        int fullWidth,
        int fullHeight,
        int displayLeft,
        int displayTop,
        int displayWidth,
        int displayHeight,
        int partEncodeUs,
        int partCpuId,
        int partCpuMaxFreqKhz,
        int displaySharePermille,
        int availableEncodeCpuCount) {
    uint8_t* p = header;
    put_i32_be(p, FRAME_MAGIC); p += 4;
    put_i32_be(p, HEADER_VERSION_SPLIT8); p += 4;
    put_i32_be(p, encodedWidth); p += 4;
    put_i32_be(p, encodedHeight); p += 4;
    put_i32_be(p, jpegSize); p += 4;
    put_i64_be(p, frameProducedNs); p += 8;
    put_i64_be(p, callbackStartNs); p += 8;
    put_i64_be(p, encodeStartNs); p += 8;
    put_i64_be(p, encodeEndNs); p += 8;
    put_i64_be(p, mono_now_ns()); p += 8;
    put_i64_be(p, wall_now_ms()); p += 8;
    put_i32_be(p, partIndex); p += 4;
    put_i32_be(p, partCount); p += 4;
    put_i32_be(p, fullWidth); p += 4;
    put_i32_be(p, fullHeight); p += 4;
    put_i32_be(p, displayLeft); p += 4;
    put_i32_be(p, displayTop); p += 4;
    put_i32_be(p, displayWidth); p += 4;
    put_i32_be(p, displayHeight); p += 4;
    put_i32_be(p, partEncodeUs); p += 4;
    put_i32_be(p, partCpuId); p += 4;
    put_i32_be(p, partCpuMaxFreqKhz); p += 4;
    put_i32_be(p, displaySharePermille); p += 4;
    put_i32_be(p, availableEncodeCpuCount); p += 4;
}

static void put_v9_header(
        uint8_t header[HEADER_V9_SIZE],
        int encodedWidth,
        int encodedHeight,
        int jpegSize,
        int64_t frameProducedNs,
        int64_t callbackStartNs,
        int64_t encodeStartNs,
        int64_t encodeEndNs,
        int64_t writeStartNs,
        int partIndex,
        int partCount,
        int fullWidth,
        int fullHeight,
        int displayLeft,
        int displayTop,
        int displayWidth,
        int displayHeight,
        int partEncodeUs,
        int partCpuId,
        int partCpuMaxFreqKhz,
        int displaySharePermille,
        int availableEncodeCpuCount,
        int workerId,
        int sendOrder,
        int dispatchDelayUs,
        int waitBeforeWriteUs,
        int writeBeforeUs,
        int previousWriteUs,
        int writerCpuId,
        int senderState,
        int flags) {
    uint8_t* p = header;
    put_i32_be(p, FRAME_MAGIC); p += 4;
    put_i32_be(p, HEADER_VERSION_SPLIT9); p += 4;
    put_i32_be(p, encodedWidth); p += 4;
    put_i32_be(p, encodedHeight); p += 4;
    put_i32_be(p, jpegSize); p += 4;
    put_i64_be(p, frameProducedNs); p += 8;
    put_i64_be(p, callbackStartNs); p += 8;
    put_i64_be(p, encodeStartNs); p += 8;
    put_i64_be(p, encodeEndNs); p += 8;
    put_i64_be(p, writeStartNs); p += 8;
    put_i64_be(p, wall_now_ms()); p += 8;
    put_i32_be(p, partIndex); p += 4;
    put_i32_be(p, partCount); p += 4;
    put_i32_be(p, fullWidth); p += 4;
    put_i32_be(p, fullHeight); p += 4;
    put_i32_be(p, displayLeft); p += 4;
    put_i32_be(p, displayTop); p += 4;
    put_i32_be(p, displayWidth); p += 4;
    put_i32_be(p, displayHeight); p += 4;
    put_i32_be(p, partEncodeUs); p += 4;
    put_i32_be(p, partCpuId); p += 4;
    put_i32_be(p, partCpuMaxFreqKhz); p += 4;
    put_i32_be(p, displaySharePermille); p += 4;
    put_i32_be(p, availableEncodeCpuCount); p += 4;
    put_i32_be(p, workerId); p += 4;
    put_i32_be(p, sendOrder); p += 4;
    put_i32_be(p, dispatchDelayUs); p += 4;
    put_i32_be(p, waitBeforeWriteUs); p += 4;
    put_i32_be(p, writeBeforeUs); p += 4;
    put_i32_be(p, previousWriteUs); p += 4;
    put_i32_be(p, writerCpuId); p += 4;
    put_i32_be(p, senderState); p += 4;
    put_i32_be(p, flags); p += 4;
}

static bool send_encoded_jpeg_split_locked(
        const std::vector<EncodedJpegRef>& partJpegs,
        int partCount,
        int fullWidth,
        int fullHeight,
        const int partTops[],
        const int partHeights[],
        int64_t frameProducedNs,
        int64_t callbackStartNs,
        int64_t encodeStartNs,
        int64_t encodeEndNs,
        const std::vector<int64_t>& partEncodeNs,
        const std::vector<int>& partCpuIds) {
    if (partCount <= 0 || partCount > MAX_SPLIT_PARTS || fullWidth <= 0 || fullHeight <= 0) return false;
    for (int i = 0; i < partCount; ++i) {
        const auto& jpeg = partJpegs[static_cast<size_t>(i)];
        if (jpeg.data == nullptr || jpeg.size == 0 || partHeights[i] <= 0) return false;
    }

    std::lock_guard<std::mutex> lk(g_send_mutex);
    int64_t last = g_last_sent_frame_produced_ns.load(std::memory_order_acquire);
    if (frameProducedNs > 0 && last > 0 && frameProducedNs <= last) return true;

    const int fd = g_output_fd;
    if (fd < 0) return false;

    uint8_t headers[MAX_SPLIT_PARTS][HEADER_V5_SIZE]{};
    iovec iov[MAX_SPLIT_PARTS * 2]{};
    const int availCpuCount = available_encode_cpu_count();
    for (int i = 0; i < partCount; ++i) {
        const auto& jpeg = partJpegs[static_cast<size_t>(i)];
        const int partEncodeUs = (i < static_cast<int>(partEncodeNs.size()))
                ? static_cast<int>((partEncodeNs[static_cast<size_t>(i)] + 500) / 1000)
                : 0;
        const int partCpuIdForHeader = (i < static_cast<int>(partCpuIds.size())) ? partCpuIds[static_cast<size_t>(i)] : -1;
        const int partCpuMaxFreqKhz = partCpuIdForHeader >= 0
                ? static_cast<int>(std::min<int64_t>(2147483647LL, cached_cpu_max_freq_khz(partCpuIdForHeader)))
                : 0;
        const int partSharePermille = fullHeight > 0 ? static_cast<int>((static_cast<int64_t>(partHeights[i]) * 1000LL + fullHeight / 2) / fullHeight) : 0;
        put_v5_header(headers[i], fullWidth, partHeights[i], static_cast<int>(jpeg.size),
                      frameProducedNs, callbackStartNs, encodeStartNs, encodeEndNs,
                      i, partCount, fullWidth, fullHeight, partTops[i], partEncodeUs,
                      partCpuIdForHeader, partCpuMaxFreqKhz, partSharePermille, availCpuCount);
        iov[i * 2].iov_base = headers[i];
        iov[i * 2].iov_len = HEADER_V5_SIZE;
        iov[i * 2 + 1].iov_base = const_cast<unsigned char*>(jpeg.data);
        iov[i * 2 + 1].iov_len = jpeg.size;
    }

    const int64_t writeStartNs = mono_now_ns();
    const bool writeOk = writev_all_iovs(fd, iov, partCount * 2);
    const int64_t writeEndNs = mono_now_ns();
    if (!writeOk) {
        if (g_output_fd == fd) close_output_fd_locked();
        return false;
    }

    int64_t totalBytes = 0;
    int64_t firstHalfBytes = 0;
    int64_t secondHalfBytes = 0;
    int64_t firstHalfEncodeNs = 0;
    int64_t secondHalfEncodeNs = 0;
    const int splitAt = (partCount + 1) / 2;
    for (int i = 0; i < partCount; ++i) {
        const int64_t bytes = static_cast<int64_t>(partJpegs[static_cast<size_t>(i)].size);
        const int64_t encNs = (i < static_cast<int>(partEncodeNs.size())) ? partEncodeNs[static_cast<size_t>(i)] : 0;
        const int cpuId = (i < static_cast<int>(partCpuIds.size())) ? partCpuIds[static_cast<size_t>(i)] : -1;
        g_last_split_part_bytes[i].store(bytes, std::memory_order_release);
        g_last_split_part_encode_ns[i].store(encNs, std::memory_order_release);
        g_last_split_part_cpu[i].store(cpuId, std::memory_order_release);
        totalBytes += bytes;
        if (i < splitAt) {
            firstHalfBytes += bytes;
            if (encNs > firstHalfEncodeNs) firstHalfEncodeNs = encNs;
        } else {
            secondHalfBytes += bytes;
            if (encNs > secondHalfEncodeNs) secondHalfEncodeNs = encNs;
        }
    }

    clear_last_split_part_stats(partCount);

    g_last_sent_total_bytes.store(totalBytes, std::memory_order_release);
    g_last_sent_part0_bytes.store(firstHalfBytes, std::memory_order_release);
    g_last_sent_part1_bytes.store(secondHalfBytes, std::memory_order_release);
    g_last_socket_write_ns.store((writeEndNs - writeStartNs), std::memory_order_release);
    g_last_part0_encode_ns.store(firstHalfEncodeNs, std::memory_order_release);
    g_last_part1_encode_ns.store(secondHalfEncodeNs, std::memory_order_release);
    g_last_split_parts.store(partCount, std::memory_order_release);

    if (frameProducedNs > 0) g_last_sent_frame_produced_ns.store(frameProducedNs, std::memory_order_release);
    return true;
}


// 固定数组版本：正式热路径使用，避免 per-frame vector 参数。
static bool send_encoded_jpeg_split_locked_fixed(
        const EncodedJpegRef partJpegs[],
        int partCount,
        int fullWidth,
        int fullHeight,
        const int partTops[],
        const int partHeights[],
        int64_t frameProducedNs,
        int64_t callbackStartNs,
        int64_t encodeStartNs,
        int64_t encodeEndNs,
        const int64_t partEncodeNs[],
        const int partCpuIds[]) {
    if (partJpegs == nullptr || partEncodeNs == nullptr || partCpuIds == nullptr) return false;
    if (partCount <= 0 || partCount > MAX_SPLIT_PARTS || fullWidth <= 0 || fullHeight <= 0) return false;
    for (int i = 0; i < partCount; ++i) {
        const auto& jpeg = partJpegs[i];
        if (jpeg.data == nullptr || jpeg.size == 0 || partHeights[i] <= 0) return false;
    }

    std::lock_guard<std::mutex> lk(g_send_mutex);
    int64_t last = g_last_sent_frame_produced_ns.load(std::memory_order_acquire);
    if (frameProducedNs > 0 && last > 0 && frameProducedNs <= last) return true;

    const int fd = g_output_fd;
    if (fd < 0) return false;

    uint8_t headers[MAX_SPLIT_PARTS][HEADER_V5_SIZE]{};
    iovec iov[MAX_SPLIT_PARTS * 2]{};
    const int availCpuCount = available_encode_cpu_count();
    for (int i = 0; i < partCount; ++i) {
        const auto& jpeg = partJpegs[i];
        const int partEncodeUs = static_cast<int>((partEncodeNs[i] + 500) / 1000);
        const int partCpuIdForHeader = partCpuIds[i];
        const int partCpuMaxFreqKhz = partCpuIdForHeader >= 0
                ? static_cast<int>(std::min<int64_t>(2147483647LL, cached_cpu_max_freq_khz(partCpuIdForHeader)))
                : 0;
        const int partSharePermille = fullHeight > 0 ? static_cast<int>((static_cast<int64_t>(partHeights[i]) * 1000LL + fullHeight / 2) / fullHeight) : 0;
        put_v5_header(headers[i], fullWidth, partHeights[i], static_cast<int>(jpeg.size),
                      frameProducedNs, callbackStartNs, encodeStartNs, encodeEndNs,
                      i, partCount, fullWidth, fullHeight, partTops[i], partEncodeUs,
                      partCpuIdForHeader, partCpuMaxFreqKhz, partSharePermille, availCpuCount);
        iov[i * 2].iov_base = headers[i];
        iov[i * 2].iov_len = HEADER_V5_SIZE;
        iov[i * 2 + 1].iov_base = const_cast<unsigned char*>(jpeg.data);
        iov[i * 2 + 1].iov_len = jpeg.size;
    }

    const int64_t writeStartNs = mono_now_ns();
    const bool writeOk = writev_all_iovs(fd, iov, partCount * 2);
    const int64_t writeEndNs = mono_now_ns();
    if (!writeOk) {
        if (g_output_fd == fd) close_output_fd_locked();
        return false;
    }

    int64_t totalBytes = 0;
    int64_t firstHalfBytes = 0;
    int64_t secondHalfBytes = 0;
    int64_t firstHalfEncodeNs = 0;
    int64_t secondHalfEncodeNs = 0;
    const int splitAt = (partCount + 1) / 2;
    for (int i = 0; i < partCount; ++i) {
        const int64_t bytes = static_cast<int64_t>(partJpegs[i].size);
        const int64_t encNs = partEncodeNs[i];
        const int cpuId = partCpuIds[i];
        g_last_split_part_bytes[i].store(bytes, std::memory_order_release);
        g_last_split_part_encode_ns[i].store(encNs, std::memory_order_release);
        g_last_split_part_cpu[i].store(cpuId, std::memory_order_release);
        totalBytes += bytes;
        if (i < splitAt) {
            firstHalfBytes += bytes;
            if (encNs > firstHalfEncodeNs) firstHalfEncodeNs = encNs;
        } else {
            secondHalfBytes += bytes;
            if (encNs > secondHalfEncodeNs) secondHalfEncodeNs = encNs;
        }
    }

    clear_last_split_part_stats(partCount);

    g_last_sent_total_bytes.store(totalBytes, std::memory_order_release);
    g_last_sent_part0_bytes.store(firstHalfBytes, std::memory_order_release);
    g_last_sent_part1_bytes.store(secondHalfBytes, std::memory_order_release);
    g_last_socket_write_ns.store((writeEndNs - writeStartNs), std::memory_order_release);
    g_last_part0_encode_ns.store(firstHalfEncodeNs, std::memory_order_release);
    g_last_part1_encode_ns.store(secondHalfEncodeNs, std::memory_order_release);
    g_last_split_parts.store(partCount, std::memory_order_release);

    if (frameProducedNs > 0) g_last_sent_frame_produced_ns.store(frameProducedNs, std::memory_order_release);
    return true;
}


struct ReadyEncodedSplitPart {
    int worker = -1;
    int partIndex = -1;
    std::vector<uint8_t> payload;
    const unsigned char* jpegPtr = nullptr;
    size_t jpegSize = 0;
    bool skipped = false;
    bool ok = false;
    int64_t dispatchNs = 0;
    int64_t encodeStartNs = 0;
    int64_t encodeEndNs = 0;
    int cpuId = -1;
};

// v6 fast path: workers encode independently; completed payloads are detached from the
// worker TLS buffer, then the worker is re-dispatched before the caller writes the ready
// batch to the socket.  This overlaps Android JPEG encode, USB/ADB write, and PC decode
// without changing JPEG quality.
static bool encode_split_parallel_send_ready_fixed(
        int partCount,
        int workerCount,
        unsigned char* partPixels[],
        const int partLefts[],
        const int partTops[],
        const int partWidths[],
        const int partEncodedWidths[],
        const int partHeights[],
        const int partJpegQualities[],
        const int workerPartMap[],
        int pitch,
        int pixelFormat,
        int jpegQuality,
        int fullWidth,
        int fullHeight,
        int64_t frameProducedNs,
        int64_t callbackStartNs,
        EncodedJpegRef partJpegs[],
        int64_t& encodeStartNs,
        int64_t& encodeEndNs,
        int64_t partEncodeNs[],
        int partCpuIds[]) {
    if (partCount <= 0 || partCount > MAX_SPLIT_PARTS) return false;
    if (workerCount <= 0 || workerCount > MAX_SPLIT_PARTS) return false;
    if (workerCount > partCount) workerCount = partCount;
    if (partJpegs == nullptr || partEncodeNs == nullptr || partCpuIds == nullptr) return false;
    if (partPixels == nullptr || partLefts == nullptr || partTops == nullptr || partWidths == nullptr ||
        partEncodedWidths == nullptr || partHeights == nullptr || partJpegQualities == nullptr || fullWidth <= 0 || fullHeight <= 0) return false;

    ensure_split_encode_workers_started();
    bool queuedPart[MAX_SPLIT_PARTS]{};
    bool workerActive[MAX_SPLIT_PARTS]{};
    for (int i = 0; i < MAX_SPLIT_PARTS; ++i) {
        partJpegs[i] = EncodedJpegRef{};
        partEncodeNs[i] = 0;
        partCpuIds[i] = -1;
    }
    encodeStartNs = 0;
    encodeEndNs = 0;

    const int availCpuCount = available_encode_cpu_count();
    workerCount = std::max(1, std::min(workerCount, availCpuCount));
    workerCount = std::min(workerCount, partCount);

    auto assignJobLocked = [&](int worker, int partIndex) -> bool {
        if (worker < 0 || worker >= workerCount) return false;
        if (partIndex < 0 || partIndex >= partCount) return false;
        if (partHeights[partIndex] <= 0 || partWidths[partIndex] <= 0 ||
            partEncodedWidths[partIndex] <= 0 || partPixels[partIndex] == nullptr) return false;

        auto& job = g_split_encode_jobs[worker];
        job.pixels = partPixels[partIndex];
        job.width = partEncodedWidths[partIndex];
        job.height = partHeights[partIndex];
        job.sourceWidth = partWidths[partIndex];
        job.pitch = pitch;
        job.pixelFormat = pixelFormat;
        job.jpegQuality = clamp_jpeg_quality(partJpegQualities[partIndex]);
        job.jpegPtr = nullptr;
        job.jpegSize = 0;
        job.ok = false;
        job.skipped = false;
        // 高速模式：不做上一帧复用/skip，每个 part 每帧都真实编码发送。
        job.done = false;
        job.hasJob = true;
        job.dispatchNs = mono_now_ns();
        job.encodeStartNs = 0;
        job.encodeEndNs = 0;
        job.cpuId = -1;
        job.partIndex = partIndex;
        job.partLeft = partLefts[partIndex];
        workerActive[worker] = true;
        queuedPart[partIndex] = true;
        return true;
    };

    int nextTask = 0;
    {
        std::unique_lock<std::mutex> lk(g_split_encode_mutex);
        for (int worker = 0; worker < workerCount && nextTask < partCount; ++worker) {
            const int partIndex = workerPartMap != nullptr ? workerPartMap[nextTask] : nextTask;
            if (!assignJobLocked(worker, partIndex)) return false;
            ++nextTask;
        }
    }
    for (int worker = 0; worker < workerCount; ++worker) {
        if (workerActive[worker]) g_split_worker_cvs[worker].notify_one();
    }

    std::lock_guard<std::mutex> sendLock(g_send_mutex);
    int64_t last = g_last_sent_frame_produced_ns.load(std::memory_order_acquire);
    const bool becameStaleBeforeFirstSend = frameProducedNs > 0 && last > 0 && frameProducedNs <= last;
    const int fd = g_output_fd;

    bool allOk = true;
    bool writeOk = !becameStaleBeforeFirstSend && fd >= 0;
    int completedParts = 0;
    int64_t accumulatedSocketWriteNs = 0;
    int64_t previousPartWriteNs = 0;

    while (completedParts < partCount) {
        std::vector<ReadyEncodedSplitPart> readyBatch;
        std::vector<int> selectedWorkers;
        {
            std::unique_lock<std::mutex> lk(g_split_encode_mutex);
            g_split_encode_done_cv.wait(lk, [&] {
                for (int worker = 0; worker < workerCount; ++worker) {
                    if (workerActive[worker] && g_split_encode_jobs[worker].done) return true;
                }
                return false;
            });

            // Drain every worker that is already done and send those parts with one writev().
            // This preserves "send as soon as ready" latency, but removes a burst of small
            // header/payload writes when several workers finish in the same scheduler slice.
            for (int worker = 0; worker < workerCount; ++worker) {
                if (!(workerActive[worker] && g_split_encode_jobs[worker].done)) continue;
                auto& job = g_split_encode_jobs[worker];
                ReadyEncodedSplitPart ready{};
                ready.worker = worker;
                ready.partIndex = job.partIndex;
                ready.payload = std::move(job.ownedJpegBuffer);
                ready.jpegPtr = (!ready.payload.empty()) ? ready.payload.data() : nullptr;
                ready.jpegSize = ready.payload.size();
                ready.skipped = job.skipped;
                job.jpegPtr = nullptr;
                job.jpegSize = 0;
                ready.ok = job.ok;
                ready.dispatchNs = job.dispatchNs;
                ready.encodeStartNs = job.encodeStartNs;
                ready.encodeEndNs = job.encodeEndNs;
                ready.cpuId = job.cpuId;
                workerActive[worker] = false;
                readyBatch.push_back(std::move(ready));
                selectedWorkers.push_back(worker);
                ++completedParts;
            }
        }

        if (readyBatch.empty()) continue;

        // readyBatch now owns the compressed payload. Re-dispatch these workers before socket
        // writev so USB/ADB backpressure can overlap with the next JPEG tasks.
        for (int selectedWorker : selectedWorkers) {
            if (selectedWorker < 0 || nextTask >= partCount) continue;
            const int partIndex = workerPartMap != nullptr ? workerPartMap[nextTask] : nextTask;
            bool assigned = false;
            {
                std::unique_lock<std::mutex> lk(g_split_encode_mutex);
                assigned = assignJobLocked(selectedWorker, partIndex);
            }
            if (!assigned) {
                allOk = false;
                writeOk = false;
            } else {
                ++nextTask;
                g_split_worker_cvs[selectedWorker].notify_one();
            }
        }

        if (writeOk) {
            const int64_t batchWriteStartNs = mono_now_ns();
            std::vector<std::array<uint8_t, HEADER_V9_SIZE>> headers(readyBatch.size());
            std::vector<iovec> iovs;
            iovs.reserve(readyBatch.size() * 2);
            const int sendOrderBase = completedParts - static_cast<int>(readyBatch.size());

            for (size_t bi = 0; bi < readyBatch.size(); ++bi) {
                const ReadyEncodedSplitPart& ready = readyBatch[bi];
                const bool partIndexOk = ready.partIndex >= 0 && ready.partIndex < partCount;
                const bool jpegOk = ready.jpegPtr != nullptr && ready.jpegSize > 0;
                if (!ready.ok || !partIndexOk || !jpegOk) {
                    allOk = false;
                    writeOk = false;
                    break;
                }

                partEncodeNs[ready.partIndex] = ready.encodeEndNs - ready.encodeStartNs;
                partCpuIds[ready.partIndex] = ready.cpuId;
                partJpegs[ready.partIndex] = EncodedJpegRef{ready.jpegPtr, ready.jpegSize, ready.skipped};
                if (encodeStartNs == 0 || ready.encodeStartNs < encodeStartNs) encodeStartNs = ready.encodeStartNs;
                if (ready.encodeEndNs > encodeEndNs) encodeEndNs = ready.encodeEndNs;

                const int64_t dispatchDelayNs = (ready.dispatchNs > 0 && ready.encodeStartNs >= ready.dispatchNs)
                        ? (ready.encodeStartNs - ready.dispatchNs)
                        : 0;
                const int64_t waitBeforeWriteNs = (ready.encodeEndNs > 0 && batchWriteStartNs >= ready.encodeEndNs)
                        ? (batchWriteStartNs - ready.encodeEndNs)
                        : 0;
                const int partEncodeUs = static_cast<int>((partEncodeNs[ready.partIndex] + 500) / 1000);
                const int partCpuMaxFreqKhz = ready.cpuId >= 0
                        ? static_cast<int>(std::min<int64_t>(2147483647LL, cached_cpu_max_freq_khz(ready.cpuId)))
                        : 0;
                const int64_t fullArea = static_cast<int64_t>(fullWidth) * static_cast<int64_t>(fullHeight);
                const int64_t partArea = static_cast<int64_t>(partWidths[ready.partIndex]) * static_cast<int64_t>(partHeights[ready.partIndex]);
                const int partSharePermille = fullArea > 0
                        ? static_cast<int>((partArea * 1000LL + fullArea / 2) / fullArea)
                        : 0;
                const int sendOrder = sendOrderBase + static_cast<int>(bi);
                const int writeBeforeUs = static_cast<int>(std::min<int64_t>(2147483647LL, (accumulatedSocketWriteNs + 500) / 1000));
                const int previousWriteUs = static_cast<int>(std::min<int64_t>(2147483647LL, (previousPartWriteNs + 500) / 1000));
                const int dispatchDelayUs = static_cast<int>(std::min<int64_t>(2147483647LL, (dispatchDelayNs + 500) / 1000));
                const int waitBeforeWriteUs = static_cast<int>(std::min<int64_t>(2147483647LL, (waitBeforeWriteNs + 500) / 1000));
                const int writerCpuId = current_cpu_id();
                const int senderState = nextTask; // how many tasks had already been dispatched when this batch write started
                const int flags = 0;

                put_v9_header(headers[bi].data(),
                              partEncodedWidths[ready.partIndex],
                              partHeights[ready.partIndex],
                              static_cast<int>(ready.jpegSize),
                              frameProducedNs,
                              callbackStartNs,
                              ready.encodeStartNs,
                              ready.encodeEndNs,
                              batchWriteStartNs,
                              ready.partIndex,
                              partCount,
                              fullWidth,
                              fullHeight,
                              partLefts[ready.partIndex],
                              partTops[ready.partIndex],
                              partWidths[ready.partIndex],
                              partHeights[ready.partIndex],
                              partEncodeUs,
                              ready.cpuId,
                              partCpuMaxFreqKhz,
                              partSharePermille,
                              availCpuCount,
                              ready.worker,
                              sendOrder,
                              dispatchDelayUs,
                              waitBeforeWriteUs,
                              writeBeforeUs,
                              previousWriteUs,
                              writerCpuId,
                              senderState,
                              flags);

                iovec headerIov{};
                headerIov.iov_base = headers[bi].data();
                headerIov.iov_len = HEADER_V9_SIZE;
                iovs.push_back(headerIov);
                iovec payloadIov{};
                payloadIov.iov_base = const_cast<unsigned char*>(ready.jpegPtr);
                payloadIov.iov_len = ready.jpegSize;
                iovs.push_back(payloadIov);
            }

            if (writeOk && !iovs.empty()) {
                const bool thisWriteOk = writev_all_iovs(fd, iovs.data(), static_cast<int>(iovs.size()));
                const int64_t writeEndNs = mono_now_ns();
                const int64_t thisWriteNs = writeEndNs - batchWriteStartNs;
                accumulatedSocketWriteNs += thisWriteNs;
                previousPartWriteNs = thisWriteNs;
                if (!thisWriteOk) {
                    if (g_output_fd == fd) close_output_fd_locked();
                    writeOk = false;
                    allOk = false;
                }
            }
        } else {
            for (const ReadyEncodedSplitPart& ready : readyBatch) {
                const bool partIndexOk = ready.partIndex >= 0 && ready.partIndex < partCount;
                const bool jpegOk = ready.jpegPtr != nullptr && ready.jpegSize > 0;
                if (!ready.ok || !partIndexOk || !jpegOk) {
                    allOk = false;
                    continue;
                }
                partEncodeNs[ready.partIndex] = ready.encodeEndNs - ready.encodeStartNs;
                partCpuIds[ready.partIndex] = ready.cpuId;
                partJpegs[ready.partIndex] = EncodedJpegRef{ready.jpegPtr, ready.jpegSize, ready.skipped};
                if (encodeStartNs == 0 || ready.encodeStartNs < encodeStartNs) encodeStartNs = ready.encodeStartNs;
                if (ready.encodeEndNs > encodeEndNs) encodeEndNs = ready.encodeEndNs;
            }
        }

    }

    // 如果 part order 里有重复/漏项，直接失败，避免 PC 一直等缺失 part。
    for (int i = 0; i < partCount; ++i) {
        if (!queuedPart[i]) {
            allOk = false;
            writeOk = false;
        }
    }

    if (becameStaleBeforeFirstSend) {
        reset_split_no_prev_reuse_state();
        return true;
    }
    if (!allOk || !writeOk) {
        reset_split_no_prev_reuse_state();
        return false;
    }

    int64_t totalBytes = 0;
    int64_t firstHalfBytes = 0;
    int64_t secondHalfBytes = 0;
    int64_t firstHalfEncodeNs = 0;
    int64_t secondHalfEncodeNs = 0;
    const int splitAt = (partCount + 1) / 2;
    for (int i = 0; i < partCount; ++i) {
        const int64_t bytes = static_cast<int64_t>(partJpegs[i].size);
        const int64_t encNs = partEncodeNs[i];
        const int cpuId = partCpuIds[i];
        g_last_split_part_bytes[i].store(bytes, std::memory_order_release);
        g_last_split_part_encode_ns[i].store(encNs, std::memory_order_release);
        g_last_split_part_cpu[i].store(cpuId, std::memory_order_release);
        totalBytes += bytes;
        if (i < splitAt) {
            firstHalfBytes += bytes;
            if (encNs > firstHalfEncodeNs) firstHalfEncodeNs = encNs;
        } else {
            secondHalfBytes += bytes;
            if (encNs > secondHalfEncodeNs) secondHalfEncodeNs = encNs;
        }
    }
    clear_last_split_part_stats(partCount);

    g_last_sent_total_bytes.store(totalBytes, std::memory_order_release);
    g_last_sent_part0_bytes.store(firstHalfBytes, std::memory_order_release);
    g_last_sent_part1_bytes.store(secondHalfBytes, std::memory_order_release);
    g_last_socket_write_ns.store(accumulatedSocketWriteNs, std::memory_order_release);
    g_last_part0_encode_ns.store(firstHalfEncodeNs, std::memory_order_release);
    g_last_part1_encode_ns.store(secondHalfEncodeNs, std::memory_order_release);
    g_last_split_parts.store(partCount, std::memory_order_release);

    if (frameProducedNs > 0) g_last_sent_frame_produced_ns.store(frameProducedNs, std::memory_order_release);
    return true;
}

static bool compress_i420_raw(
        int width,
        int height,
        int jpegQuality,
        unsigned char*& jpegBuf,
        unsigned long& jpegSize) {
    if (width <= 0 || height <= 0) return false;

    tjhandle compressor = get_compressor();
    if (compressor == nullptr) {
        ALOGE("tjInitCompress failed");
        return false;
    }

    const unsigned long requiredSize = tjBufSize(width, height, TJSAMP_420);
    if (requiredSize == 0) {
        ALOGE("tjBufSize failed width=%d height=%d", width, height);
        return false;
    }
    if (!ensure_jpeg_buffer(requiredSize)) {
        return false;
    }

    jpegBuf = g_tls.jpegBuffer;
    jpegSize = g_tls.jpegCapacity;

    const int ret = tjCompressFromYUV(
            compressor,
            g_tls.yuv420Buffer.data(),
            width,
            1,
            height,
            TJSAMP_420,
            &jpegBuf,
            &jpegSize,
            clamp_jpeg_quality(jpegQuality),
            TJFLAG_FASTDCT | TJFLAG_NOREALLOC);

    if (ret != 0 || jpegBuf == nullptr || jpegSize == 0) {
        ALOGE("tjCompressFromYUV failed: %s", tjGetErrorStr());
        return false;
    }
    return true;
}

static jbyteArray compress_pixels_to_byte_array(
        JNIEnv* env,
        unsigned char* pixels,
        int width,
        int height,
        int pitch,
        int pixelFormat,
        int jpegQuality) {
    unsigned char* jpegBuf = nullptr;
    unsigned long jpegSize = 0;
    if (!compress_pixels_raw(pixels, width, height, pitch, pixelFormat, jpegQuality, jpegBuf, jpegSize)) return nullptr;
    return jpeg_to_byte_array(env, jpegBuf, jpegSize);
}

static bool compress_pixels_and_send(
        unsigned char* pixels,
        int width,
        int height,
        int pitch,
        int pixelFormat,
        int jpegQuality,
        int64_t frameProducedNs,
        int64_t callbackStartNs) {
    if (!has_output_fd()) return false;
    if (is_stale_for_encode(frameProducedNs)) return true;

    unsigned char* jpegBuf = nullptr;
    unsigned long jpegSize = 0;
    const int64_t encodeStartNs = mono_now_ns();
    const bool ok = compress_pixels_raw(pixels, width, height, pitch, pixelFormat, jpegQuality, jpegBuf, jpegSize);
    const int64_t encodeEndNs = mono_now_ns();
    if (!ok) return false;

    return send_encoded_jpeg_short_lock(jpegBuf, jpegSize, width, height,
                                        frameProducedNs, callbackStartNs, encodeStartNs, encodeEndNs);
}

static jbyteArray compress_i420_to_byte_array(JNIEnv* env, int width, int height, jint jpegQuality) {
    unsigned char* jpegBuf = nullptr;
    unsigned long jpegSize = 0;
    if (!compress_i420_raw(width, height, jpegQuality, jpegBuf, jpegSize)) return nullptr;
    return jpeg_to_byte_array(env, jpegBuf, jpegSize);
}

static bool compress_i420_and_send(
        int width,
        int height,
        jint jpegQuality,
        int64_t frameProducedNs,
        int64_t callbackStartNs) {
    if (!has_output_fd()) return false;
    if (is_stale_for_encode(frameProducedNs)) return true;

    unsigned char* jpegBuf = nullptr;
    unsigned long jpegSize = 0;
    const int64_t encodeStartNs = mono_now_ns();
    const bool ok = compress_i420_raw(width, height, jpegQuality, jpegBuf, jpegSize);
    const int64_t encodeEndNs = mono_now_ns();
    if (!ok) return false;

    return send_encoded_jpeg_short_lock(jpegBuf, jpegSize, width, height,
                                        frameProducedNs, callbackStartNs, encodeStartNs, encodeEndNs);
}

static bool copy_region_to_rgb_buffer(
        const uint8_t* base,
        jint bufferCapacity,
        jint rowStride,
        jint pixelStride,
        jint left,
        jint top,
        jint width,
        jint height) {
    if (!is_region_in_bounds(bufferCapacity, rowStride, pixelStride, left, top, width, height)) {
        ALOGE("buffer overrun left=%d top=%d width=%d height=%d rowStride=%d pixelStride=%d cap=%d",
              left, top, width, height, rowStride, pixelStride, bufferCapacity);
        return false;
    }

    const size_t rgbBytes = static_cast<size_t>(width) * static_cast<size_t>(height) * 3u;
    if (g_tls.rgbBuffer.size() < rgbBytes) {
        g_tls.rgbBuffer.resize(rgbBytes);
    }

    uint8_t* dst = g_tls.rgbBuffer.data();
    size_t dstIndex = 0;
    for (int y = 0; y < height; ++y) {
        const int64_t rowBase = static_cast<int64_t>(top + y) * rowStride + static_cast<int64_t>(left) * pixelStride;
        const uint8_t* src = base + rowBase;
        for (int x = 0; x < width; ++x) {
            dst[dstIndex++] = src[0];
            dst[dstIndex++] = src[1];
            dst[dstIndex++] = src[2];
            src += pixelStride;
        }
    }
    return true;
}

static jbyteArray copy_region_and_compress(
        JNIEnv* env,
        const uint8_t* base,
        jint bufferCapacity,
        jint rowStride,
        jint pixelStride,
        jint left,
        jint top,
        jint width,
        jint height,
        jint jpegQuality) {
    if (!copy_region_to_rgb_buffer(base, bufferCapacity, rowStride, pixelStride, left, top, width, height)) return nullptr;
    return compress_pixels_to_byte_array(env,
                                         reinterpret_cast<unsigned char*>(g_tls.rgbBuffer.data()),
                                         width,
                                         height,
                                         0,
                                         TJPF_RGB,
                                         jpegQuality);
}

static bool copy_region_and_send(
        const uint8_t* base,
        jint bufferCapacity,
        jint rowStride,
        jint pixelStride,
        jint left,
        jint top,
        jint width,
        jint height,
        jint jpegQuality,
        int64_t frameProducedNs,
        int64_t callbackStartNs) {
    if (!copy_region_to_rgb_buffer(base, bufferCapacity, rowStride, pixelStride, left, top, width, height)) return false;
    return compress_pixels_and_send(reinterpret_cast<unsigned char*>(g_tls.rgbBuffer.data()),
                                    width,
                                    height,
                                    0,
                                    TJPF_RGB,
                                    jpegQuality,
                                    frameProducedNs,
                                    callbackStartNs);
}


// 中心裁剪/任意矩形区域零拷贝发送：
// RGBA/RGB 且区域合法时，直接让 TurboJPEG 读取 ImageReader plane 的原始 buffer + offset。
// 只有非常规 pixelStride 或越界兜底时，才退回 copy_region_and_send()。
static bool encode_region_and_send_zero_copy_or_copy(
        const uint8_t* base,
        jint bufferCapacity,
        jint rowStride,
        jint pixelStride,
        jint left,
        jint top,
        jint width,
        jint height,
        jint jpegQuality,
        int64_t frameProducedNs,
        int64_t callbackStartNs) {
    if (base == nullptr || width <= 0 || height <= 0 || rowStride <= 0 || pixelStride <= 0) {
        return false;
    }

    if ((pixelStride == 4 || pixelStride == 3) &&
        is_region_in_bounds(bufferCapacity, rowStride, pixelStride, left, top, width, height)) {
        mark_center_crop_path(true, "region_zero_copy");
        auto* regionPtr = const_cast<unsigned char*>(
                base + static_cast<int64_t>(top) * rowStride + static_cast<int64_t>(left) * pixelStride);
        return compress_pixels_and_send(regionPtr,
                                        width,
                                        height,
                                        rowStride,
                                        pixelStride == 4 ? TJPF_RGBA : TJPF_RGB,
                                        jpegQuality,
                                        frameProducedNs,
                                        callbackStartNs);
    }

    mark_center_crop_path(false, "fallback_copy");
    return copy_region_and_send(base,
                                bufferCapacity,
                                rowStride,
                                pixelStride,
                                left,
                                top,
                                width,
                                height,
                                jpegQuality,
                                frameProducedNs,
                                callbackStartNs);
}

extern "C"
JNIEXPORT jboolean JNICALL
Java_com_example_huilangtoupingV3_NativeCenterCropJpegEncoder_nativeSetOutputFileDescriptor(
        JNIEnv* env,
        jobject /*thiz*/,
        jobject fileDescriptor) {
    const int fd = get_fd_from_file_descriptor(env, fileDescriptor);
    if (fd < 0) return JNI_FALSE;

    const int duplicated = dup(fd);
    if (duplicated < 0) {
        ALOGE("dup fd failed: %s", strerror(errno));
        return JNI_FALSE;
    }

    std::lock_guard<std::mutex> lk(g_send_mutex);
    close_output_fd_locked();
    reset_split_no_prev_reuse_state();
    g_output_fd = duplicated;
    g_last_sent_frame_produced_ns.store(0, std::memory_order_release);
    g_output_fd_available.store(true, std::memory_order_release);
    return JNI_TRUE;
}

extern "C"
JNIEXPORT void JNICALL
Java_com_example_huilangtoupingV3_NativeCenterCropJpegEncoder_nativeClearOutputFileDescriptor(
        JNIEnv* /*env*/,
        jobject /*thiz*/) {
    std::lock_guard<std::mutex> lk(g_send_mutex);
    close_output_fd_locked();
    reset_split_no_prev_reuse_state();
    g_last_sent_frame_produced_ns.store(0, std::memory_order_release);
}

extern "C"
JNIEXPORT jboolean JNICALL
Java_com_example_huilangtoupingV3_NativeCenterCropJpegEncoder_nativeHasOutputFileDescriptor(
        JNIEnv* /*env*/,
        jobject /*thiz*/) {
    return has_output_fd() ? JNI_TRUE : JNI_FALSE;
}

extern "C"
JNIEXPORT jboolean JNICALL
Java_com_example_huilangtoupingV3_NativeCenterCropJpegEncoder_nativePrepareCurrentThreadForEncoding(
        JNIEnv* /*env*/,
        jobject /*thiz*/) {
    return get_compressor() != nullptr ? JNI_TRUE : JNI_FALSE;
}

extern "C"
JNIEXPORT jboolean JNICALL
Java_com_example_huilangtoupingV3_NativeCenterCropJpegEncoder_nativeBindCurrentThreadToCpu(
        JNIEnv* /*env*/,
        jobject /*thiz*/,
        jint cpuId) {
    if (cpuId < 0 || cpuId >= CPU_SETSIZE) return JNI_FALSE;
    if (!cpu_is_online(static_cast<int>(cpuId))) return JNI_FALSE;

    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(static_cast<int>(cpuId), &set);
    if (sched_setaffinity(0, sizeof(set), &set) != 0) {
        ALOGE("sched_setaffinity cpu=%d failed: %s", static_cast<int>(cpuId), strerror(errno));
        return JNI_FALSE;
    }
    return JNI_TRUE;
}

extern "C"
JNIEXPORT jint JNICALL
Java_com_example_huilangtoupingV3_NativeCenterCropJpegEncoder_nativeGetCurrentCpu(
        JNIEnv* /*env*/,
        jobject /*thiz*/) {
    return static_cast<jint>(current_cpu_id());
}


extern "C"
JNIEXPORT jint JNICALL
Java_com_example_huilangtoupingV3_NativeCenterCropJpegEncoder_nativeGetLastCenterCropPath(
        JNIEnv* /*env*/,
        jobject /*thiz*/) {
    return static_cast<jint>(g_last_center_crop_path.load(std::memory_order_acquire));
}

extern "C"
JNIEXPORT jstring JNICALL
Java_com_example_huilangtoupingV3_NativeCenterCropJpegEncoder_nativeGetLastCenterCropPathText(
        JNIEnv* env,
        jobject /*thiz*/) {
    const int path = g_last_center_crop_path.load(std::memory_order_acquire);
    return env->NewStringUTF(center_crop_path_name(path));
}

extern "C"
JNIEXPORT void JNICALL
Java_com_example_huilangtoupingV3_NativeCenterCropJpegEncoder_nativeSetReservedCallbackCpu(
        JNIEnv* /*env*/,
        jobject /*thiz*/,
        jint cpuId) {
    const int value = static_cast<int>(cpuId);
    if (value >= 0 && value < CPU_SETSIZE && cpu_is_online(value)) {
        g_reserved_callback_cpu.store(value, std::memory_order_release);
    } else {
        g_reserved_callback_cpu.store(-1, std::memory_order_release);
    }
}

extern "C"
JNIEXPORT jboolean JNICALL
Java_com_example_huilangtoupingV3_NativeCenterCropJpegEncoder_nativeSetManualSplitWeights(
        JNIEnv* env,
        jobject /*thiz*/,
        jfloatArray weightsArray) {
    if (weightsArray == nullptr) return JNI_FALSE;
    const jsize n = env->GetArrayLength(weightsArray);
    if (n < 2 || n > MAX_SPLIT_PARTS) return JNI_FALSE;

    jfloat tmp[MAX_SPLIT_PARTS]{};
    env->GetFloatArrayRegion(weightsArray, 0, n, tmp);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        return JNI_FALSE;
    }

    double raw[MAX_SPLIT_PARTS]{};
    double sum = 0.0;
    for (int i = 0; i < static_cast<int>(n); ++i) {
        raw[i] = clamp_manual_split_weight(static_cast<double>(tmp[i]));
        sum += raw[i];
    }
    if (sum <= 0.0) return JNI_FALSE;

    // 归一化到平均值为 1，再二次限制，确保每块都在合理范围内。
    const double avg = sum / static_cast<double>(n);
    double normalized[MAX_SPLIT_PARTS]{};
    double normalizedSum = 0.0;
    for (int i = 0; i < static_cast<int>(n); ++i) {
        normalized[i] = clamp_manual_split_weight(raw[i] / avg);
        normalizedSum += normalized[i];
    }
    if (normalizedSum <= 0.0) return JNI_FALSE;

    {
        std::lock_guard<std::mutex> lk(g_manual_split_weight_mutex);
        g_manual_split_weight_enabled = true;
        g_manual_split_weight_count = static_cast<int>(n);
        g_manual_cpu_weight_enabled = false;
        g_manual_cpu_weight_count = 0;
        const double avg2 = normalizedSum / static_cast<double>(n);
        for (int i = 0; i < static_cast<int>(n); ++i) {
            g_manual_split_weights[i] = clamp_manual_split_weight(normalized[i] / avg2);
        }
        for (int i = static_cast<int>(n); i < MAX_SPLIT_PARTS; ++i) {
            g_manual_split_weights[i] = 1.0;
        }
    }
    return JNI_TRUE;
}

extern "C"
JNIEXPORT jboolean JNICALL
Java_com_example_huilangtoupingV3_NativeCenterCropJpegEncoder_nativeSetManualCpuSplitWeights(
        JNIEnv* env,
        jobject /*thiz*/,
        jintArray cpuIdsArray,
        jfloatArray weightsArray) {
    if (cpuIdsArray == nullptr || weightsArray == nullptr) return JNI_FALSE;
    const jsize nCpu = env->GetArrayLength(cpuIdsArray);
    const jsize nWeight = env->GetArrayLength(weightsArray);
    if (nCpu < 2 || nCpu > MAX_SPLIT_PARTS || nCpu != nWeight) return JNI_FALSE;

    jint cpuTmp[MAX_SPLIT_PARTS]{};
    jfloat weightTmp[MAX_SPLIT_PARTS]{};
    env->GetIntArrayRegion(cpuIdsArray, 0, nCpu, cpuTmp);
    env->GetFloatArrayRegion(weightsArray, 0, nWeight, weightTmp);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        return JNI_FALSE;
    }

    double sum = 0.0;
    for (int i = 0; i < static_cast<int>(nCpu); ++i) {
        if (cpuTmp[i] < 0 || cpuTmp[i] >= CPU_SETSIZE) return JNI_FALSE;
        sum += clamp_manual_split_weight(static_cast<double>(weightTmp[i]));
    }
    if (sum <= 0.0) return JNI_FALSE;

    const double avg = sum / static_cast<double>(nCpu);
    {
        std::lock_guard<std::mutex> lk(g_manual_split_weight_mutex);
        g_manual_cpu_weight_enabled = true;
        g_manual_cpu_weight_count = static_cast<int>(nCpu);
        g_manual_split_weight_enabled = false;
        g_manual_split_weight_count = 0;
        for (int i = 0; i < static_cast<int>(nCpu); ++i) {
            g_manual_cpu_ids[i] = static_cast<int>(cpuTmp[i]);
            g_manual_cpu_weights[i] = clamp_manual_split_weight(static_cast<double>(weightTmp[i]) / avg);
        }
        for (int i = static_cast<int>(nCpu); i < MAX_SPLIT_PARTS; ++i) {
            g_manual_cpu_ids[i] = -1;
            g_manual_cpu_weights[i] = 1.0;
        }
    }
    return JNI_TRUE;
}

extern "C"
JNIEXPORT void JNICALL
Java_com_example_huilangtoupingV3_NativeCenterCropJpegEncoder_nativeClearManualSplitWeights(
        JNIEnv* /*env*/,
        jobject /*thiz*/) {
    std::lock_guard<std::mutex> lk(g_manual_split_weight_mutex);
    g_manual_split_weight_enabled = false;
    g_manual_split_weight_count = 0;
    g_manual_cpu_weight_enabled = false;
    g_manual_cpu_weight_count = 0;
    for (double& w : g_manual_split_weights) w = 1.0;
    for (int& id : g_manual_cpu_ids) id = -1;
    for (double& w : g_manual_cpu_weights) w = 1.0;
}


extern "C"
JNIEXPORT jboolean JNICALL
Java_com_example_huilangtoupingV3_NativeCenterCropJpegEncoder_nativeSetSplitRoiQualityParams(
        JNIEnv* /*env*/,
        jobject /*thiz*/,
        jint edgeQualityReduction,
        jint edgeEncodeScalePercent) {
    const int qDrop = std::max(MIN_SPLIT_EDGE_QUALITY_REDUCTION,
                               std::min(MAX_SPLIT_EDGE_QUALITY_REDUCTION,
                                        static_cast<int>(edgeQualityReduction)));
    const int scale = std::max(MIN_SPLIT_EDGE_ENCODE_SCALE_PERCENT,
                               std::min(MAX_SPLIT_EDGE_ENCODE_SCALE_PERCENT,
                                        static_cast<int>(edgeEncodeScalePercent)));
    g_split_edge_quality_reduction.store(qDrop, std::memory_order_release);
    g_split_edge_encode_scale_percent.store(scale, std::memory_order_release);
    if (ENABLE_NATIVE_DEBUG_LOGS) {
        ALOGE("split ROI quality params edgeQDrop=%d edgeScale=%d%%", qDrop, scale);
    }
    return JNI_TRUE;
}


extern "C"
JNIEXPORT jboolean JNICALL
Java_com_example_huilangtoupingV3_NativeCenterCropJpegEncoder_nativeSetSplitLayoutParams(
        JNIEnv* /*env*/,
        jobject /*thiz*/,
        jint centerHeightPercent,
        jint centerWidthPercent,
        jint centerCoreCount,
        jboolean centerOnly,
        jint bigCoreWeightPercent) {
    const int centerHeight = std::max(MIN_SPLIT_CENTER_HEIGHT_PERCENT,
                                      std::min(MAX_SPLIT_CENTER_HEIGHT_PERCENT, static_cast<int>(centerHeightPercent)));
    const int centerWidth = std::max(MIN_SPLIT_CENTER_WIDTH_PERCENT,
                                     std::min(MAX_SPLIT_CENTER_WIDTH_PERCENT, static_cast<int>(centerWidthPercent)));
    const int centerCores = std::max(MIN_SPLIT_CENTER_CORE_COUNT,
                                     std::min(MAX_SPLIT_CENTER_CORE_COUNT, static_cast<int>(centerCoreCount)));
    const int bigWeight = std::max(MIN_SPLIT_BIG_CORE_WEIGHT_PERCENT,
                                   std::min(MAX_SPLIT_BIG_CORE_WEIGHT_PERCENT, static_cast<int>(bigCoreWeightPercent)));
    g_split_top_low_percent.store(centerHeight, std::memory_order_release);
    g_split_center_width_percent.store(centerWidth, std::memory_order_release);
    g_split_center_core_count.store(centerCores, std::memory_order_release);
    g_jpeg_center_only_enabled.store(centerOnly == JNI_TRUE, std::memory_order_release);
    g_split_big_core_weight_percent.store(bigWeight, std::memory_order_release);
    if (ENABLE_NATIVE_DEBUG_LOGS) {
        ALOGE("split layout params centerHeight=%d%% centerWidth=%d%% centerCores=%d centerOnly=%d bigWeight=%d%%",
              centerHeight, centerWidth, centerCores, centerOnly == JNI_TRUE ? 1 : 0, bigWeight);
    }
    return JNI_TRUE;
}

extern "C"
JNIEXPORT jboolean JNICALL
Java_com_example_huilangtoupingV3_NativeCenterCropJpegEncoder_nativeSetJpegSubsamplingMode(
        JNIEnv* /*env*/,
        jobject /*thiz*/,
        jint mode) {
    if (mode == 444) {
        g_jpeg_subsampling_mode.store(444, std::memory_order_release);
        return JNI_TRUE;
    }
    if (mode == 420) {
        g_jpeg_subsampling_mode.store(420, std::memory_order_release);
        return JNI_TRUE;
    }
    return JNI_FALSE;
}



extern "C"
JNIEXPORT void JNICALL
Java_com_example_huilangtoupingV3_NativeCenterCropJpegEncoder_nativeResetFrameTimeline(
        JNIEnv* /*env*/,
        jobject /*thiz*/) {
    // ImageReader / VirtualDisplay 重建或 resize 后，Image.timestamp 可能重启/回退。
    // 清掉旧发送时间线，避免 stale-frame 保护把新尺寸帧当旧帧丢掉。
    g_last_sent_frame_produced_ns.store(0, std::memory_order_release);
}

extern "C"
JNIEXPORT jint JNICALL
Java_com_example_huilangtoupingV3_NativeCenterCropJpegEncoder_nativeGetAvailableEncodeCpuCount(
        JNIEnv* /*env*/,
        jobject /*thiz*/) {
    return static_cast<jint>(available_encode_cpu_count());
}

extern "C"
JNIEXPORT jintArray JNICALL
Java_com_example_huilangtoupingV3_NativeCenterCropJpegEncoder_nativeGetSuggestedEncodeCpuIds(
        JNIEnv* env,
        jobject /*thiz*/,
        jint count) {
    const int requested = std::max(0, std::min(static_cast<int>(count), MAX_SPLIT_PARTS));
    jintArray array = env->NewIntArray(requested);
    if (array == nullptr) return nullptr;
    if (requested <= 0) return array;

    jint out[MAX_SPLIT_PARTS]{};
    for (int i = 0; i < requested; ++i) out[i] = -1;
    int chosen[MAX_SPLIT_PARTS]{};
    const int n = fill_encode_cpu_ids(requested, -1, chosen);
    for (int i = 0; i < n && i < requested; ++i) {
        out[i] = static_cast<jint>(chosen[i]);
    }
    env->SetIntArrayRegion(array, 0, requested, out);
    return array;
}


extern "C"
JNIEXPORT jboolean JNICALL
Java_com_example_huilangtoupingV3_NativeCenterCropJpegEncoder_nativeWarmupSplitEncoder(
        JNIEnv* /*env*/,
        jobject /*thiz*/,
        jint width,
        jint height,
        jint splitParts,
        jint jpegSubsamplingMode) {
    const int safeWidth = std::max(16, static_cast<int>(width));
    const int safeHeight = std::max(16, static_cast<int>(height));
    int partCount = effective_split_part_count(static_cast<int>(splitParts));
    if (partCount <= 0) partCount = 1;
    if (partCount > MAX_SPLIT_PARTS) partCount = MAX_SPLIT_PARTS;

    g_jpeg_subsampling_mode.store(jpegSubsamplingMode == 444 ? 444 : 420, std::memory_order_release);
    ensure_split_encode_workers_started();
    std::lock_guard<std::mutex> callLock(g_split_call_mutex);

    if (partCount <= 1) {
        static thread_local std::vector<uint8_t> dummyOne;
        const int h = std::min(safeHeight, std::max(16, make_even_floor((safeHeight + 3) / 4)));
        dummyOne.assign(static_cast<size_t>(safeWidth) * static_cast<size_t>(h) * 4u, 0);
        unsigned char* jpegBuf = nullptr;
        unsigned long jpegSize = 0;
        return compress_pixels_raw(dummyOne.data(), safeWidth, h, safeWidth * 4, TJPF_RGBA, 90, jpegBuf, jpegSize)
                ? JNI_TRUE : JNI_FALSE;
    }

    // 用内存换时间：预热每个 split worker 的 tjhandle、JPEG 输出 buffer 和 TurboJPEG 内部路径。
    // 预热高度按 3 倍平均块高估算，避免复杂场景/权重变化时再触发 tjAlloc 扩容。
    const int warmHeight = std::min(
            safeHeight,
            std::max(16, make_even_floor((safeHeight * 3 + partCount - 1) / partCount)));
    static std::mutex warmupMutex;
    static std::vector<uint8_t> warmupRgba;
    {
        std::lock_guard<std::mutex> lk(warmupMutex);
        warmupRgba.assign(static_cast<size_t>(safeWidth) * static_cast<size_t>(warmHeight) * 4u, 0);

        unsigned char* partPixels[MAX_SPLIT_PARTS]{};
        int partHeights[MAX_SPLIT_PARTS]{};
        int workerPartMap[MAX_SPLIT_PARTS]{};
        for (int i = 0; i < partCount; ++i) {
            partPixels[i] = warmupRgba.data();
            partHeights[i] = warmHeight;
            workerPartMap[i] = i;
        }

        EncodedJpegRef partJpegs[MAX_SPLIT_PARTS]{};
        int64_t partEncodeNs[MAX_SPLIT_PARTS]{};
        int partCpuIds[MAX_SPLIT_PARTS]{};
        int64_t encodeStartNs = 0;
        int64_t encodeEndNs = 0;
        const bool ok = encode_split_parallel_fixed(partCount, partPixels, partHeights, workerPartMap,
                                                    safeWidth, safeWidth * 4, TJPF_RGBA, 90,
                                                    partJpegs, encodeStartNs, encodeEndNs,
                                                    partEncodeNs, partCpuIds);
        return ok ? JNI_TRUE : JNI_FALSE;
    }
}

extern "C"
JNIEXPORT jboolean JNICALL
Java_com_example_huilangtoupingV3_NativeCenterCropJpegEncoder_nativeFillLastSendStats(
        JNIEnv* env,
        jobject /*thiz*/,
        jlongArray outArray) {
    if (outArray == nullptr || env->GetArrayLength(outArray) < 7) return JNI_FALSE;
    jlong values[7]{};
    values[0] = static_cast<jlong>(g_last_sent_total_bytes.load(std::memory_order_acquire));
    values[1] = static_cast<jlong>(g_last_sent_part0_bytes.load(std::memory_order_acquire));
    values[2] = static_cast<jlong>(g_last_sent_part1_bytes.load(std::memory_order_acquire));
    values[3] = static_cast<jlong>(g_last_socket_write_ns.load(std::memory_order_acquire));
    values[4] = static_cast<jlong>(g_last_part0_encode_ns.load(std::memory_order_acquire));
    values[5] = static_cast<jlong>(g_last_part1_encode_ns.load(std::memory_order_acquire));
    values[6] = static_cast<jlong>(g_last_split_parts.load(std::memory_order_acquire));
    env->SetLongArrayRegion(outArray, 0, 7, values);
    return env->ExceptionCheck() ? JNI_FALSE : JNI_TRUE;
}

extern "C"
JNIEXPORT jboolean JNICALL
Java_com_example_huilangtoupingV3_NativeCenterCropJpegEncoder_nativeFillLastSplitPartStats(
        JNIEnv* env,
        jobject /*thiz*/,
        jlongArray outArray) {
    // values[0] = reported partCount; each part uses 3 slots: bytes, encodeNs, cpuId.
    // Some Kotlin builds still allocate space for 16 parts.  This native build may send more
    // ROI tasks (for example 17 with 2/6/2), so fill as many as the caller can hold instead of failing.
    if (outArray == nullptr) return JNI_FALSE;
    const int arrayLen = env->GetArrayLength(outArray);
    if (arrayLen < 4) return JNI_FALSE;
    const int maxReportParts = std::min<int>(MAX_SPLIT_PARTS, (arrayLen - 1) / 3);
    const int actualPartCount = std::max<int>(1, std::min<int>(MAX_SPLIT_PARTS, static_cast<int>(
            g_last_split_parts.load(std::memory_order_acquire))));
    const int reportPartCount = std::min(actualPartCount, maxReportParts);
    const int kValues = 1 + reportPartCount * 3;
    std::vector<jlong> values(static_cast<size_t>(kValues), 0);
    values[0] = static_cast<jlong>(reportPartCount);
    for (int i = 0; i < reportPartCount; ++i) {
        const int base = 1 + i * 3;
        values[base] = static_cast<jlong>(g_last_split_part_bytes[i].load(std::memory_order_acquire));
        values[base + 1] = static_cast<jlong>(g_last_split_part_encode_ns[i].load(std::memory_order_acquire));
        values[base + 2] = static_cast<jlong>(g_last_split_part_cpu[i].load(std::memory_order_acquire));
    }
    env->SetLongArrayRegion(outArray, 0, kValues, values.data());
    return env->ExceptionCheck() ? JNI_FALSE : JNI_TRUE;
}

extern "C"
JNIEXPORT jlongArray JNICALL
Java_com_example_huilangtoupingV3_NativeCenterCropJpegEncoder_nativeGetLastSendStats(
        JNIEnv* env,
        jobject /*thiz*/) {
    jlong values[7]{};
    values[0] = static_cast<jlong>(g_last_sent_total_bytes.load(std::memory_order_acquire));
    values[1] = static_cast<jlong>(g_last_sent_part0_bytes.load(std::memory_order_acquire));
    values[2] = static_cast<jlong>(g_last_sent_part1_bytes.load(std::memory_order_acquire));
    values[3] = static_cast<jlong>(g_last_socket_write_ns.load(std::memory_order_acquire));
    values[4] = static_cast<jlong>(g_last_part0_encode_ns.load(std::memory_order_acquire));
    values[5] = static_cast<jlong>(g_last_part1_encode_ns.load(std::memory_order_acquire));
    values[6] = static_cast<jlong>(g_last_split_parts.load(std::memory_order_acquire));
    jlongArray arr = env->NewLongArray(7);
    if (arr == nullptr) return nullptr;
    env->SetLongArrayRegion(arr, 0, 7, values);
    return arr;
}

extern "C"
JNIEXPORT jlongArray JNICALL
Java_com_example_huilangtoupingV3_NativeCenterCropJpegEncoder_nativeGetLastSplitPartStats(
        JNIEnv* env,
        jobject /*thiz*/) {
    // values[0] = partCount; then each part uses 3 slots: bytes, encodeNs, cpuId.
    constexpr int kValues = 1 + MAX_SPLIT_PARTS * 3;
    jlong values[kValues]{};
    const int partCount = std::max<int>(1, std::min<int>(MAX_SPLIT_PARTS, static_cast<int>(
            g_last_split_parts.load(std::memory_order_acquire))));
    values[0] = static_cast<jlong>(partCount);
    for (int i = 0; i < MAX_SPLIT_PARTS; ++i) {
        const int base = 1 + i * 3;
        values[base] = static_cast<jlong>(g_last_split_part_bytes[i].load(std::memory_order_acquire));
        values[base + 1] = static_cast<jlong>(g_last_split_part_encode_ns[i].load(std::memory_order_acquire));
        values[base + 2] = static_cast<jlong>(g_last_split_part_cpu[i].load(std::memory_order_acquire));
    }
    jlongArray arr = env->NewLongArray(kValues);
    if (arr == nullptr) return nullptr;
    env->SetLongArrayRegion(arr, 0, kValues, values);
    return arr;
}

extern "C"
JNIEXPORT jbyteArray JNICALL
Java_com_example_huilangtoupingV3_NativeCenterCropJpegEncoder_nativeEncodeCenterCropRgba8888(
        JNIEnv* env,
        jobject /*thiz*/,
        jobject rgbaBuffer,
        jint bufferCapacity,
        jint srcWidth,
        jint srcHeight,
        jint rowStride,
        jint pixelStride,
        jint cropSize,
        jint jpegQuality) {
    if (rgbaBuffer == nullptr || srcWidth <= 0 || srcHeight <= 0 || cropSize <= 0 || rowStride <= 0 || pixelStride <= 0) return nullptr;
    auto* base = static_cast<uint8_t*>(env->GetDirectBufferAddress(rgbaBuffer));
    if (base == nullptr) return nullptr;

    const int size = std::min(static_cast<int>(cropSize), std::min(static_cast<int>(srcWidth), static_cast<int>(srcHeight)));
    const int cropLeft = std::max(0, (static_cast<int>(srcWidth) - size) / 2);
    const int cropTop = std::max(0, (static_cast<int>(srcHeight) - size) / 2);

    if (pixelStride == 4 && is_region_in_bounds(bufferCapacity, rowStride, pixelStride, cropLeft, cropTop, size, size)) {
        unsigned char* cropPtr = base + static_cast<int64_t>(cropTop) * rowStride + static_cast<int64_t>(cropLeft) * pixelStride;
        return compress_pixels_to_byte_array(env, cropPtr, size, size, rowStride, TJPF_RGBA, jpegQuality);
    }
    if (pixelStride == 3 && is_region_in_bounds(bufferCapacity, rowStride, pixelStride, cropLeft, cropTop, size, size)) {
        unsigned char* cropPtr = base + static_cast<int64_t>(cropTop) * rowStride + static_cast<int64_t>(cropLeft) * pixelStride;
        return compress_pixels_to_byte_array(env, cropPtr, size, size, rowStride, TJPF_RGB, jpegQuality);
    }
    return copy_region_and_compress(env, base, bufferCapacity, rowStride, pixelStride, cropLeft, cropTop, size, size, jpegQuality);
}

extern "C"
JNIEXPORT jbyteArray JNICALL
Java_com_example_huilangtoupingV3_NativeCenterCropJpegEncoder_nativeEncodeFullFrameRgba8888(
        JNIEnv* env,
        jobject /*thiz*/,
        jobject rgbaBuffer,
        jint bufferCapacity,
        jint srcWidth,
        jint srcHeight,
        jint rowStride,
        jint pixelStride,
        jint jpegQuality) {
    if (rgbaBuffer == nullptr || srcWidth <= 0 || srcHeight <= 0 || rowStride <= 0 || pixelStride <= 0) return nullptr;
    auto* base = static_cast<uint8_t*>(env->GetDirectBufferAddress(rgbaBuffer));
    if (base == nullptr) return nullptr;

    if (pixelStride == 4 && is_region_in_bounds(bufferCapacity, rowStride, pixelStride, 0, 0, srcWidth, srcHeight)) {
        return compress_pixels_to_byte_array(env, reinterpret_cast<unsigned char*>(base), srcWidth, srcHeight, rowStride, TJPF_RGBA, jpegQuality);
    }
    if (pixelStride == 3 && is_region_in_bounds(bufferCapacity, rowStride, pixelStride, 0, 0, srcWidth, srcHeight)) {
        return compress_pixels_to_byte_array(env, reinterpret_cast<unsigned char*>(base), srcWidth, srcHeight, rowStride, TJPF_RGB, jpegQuality);
    }
    return copy_region_and_compress(env, base, bufferCapacity, rowStride, pixelStride, 0, 0, srcWidth, srcHeight, jpegQuality);
}

extern "C"
JNIEXPORT jboolean JNICALL
Java_com_example_huilangtoupingV3_NativeCenterCropJpegEncoder_nativeEncodeAndSendCenterCropRgba8888(
        JNIEnv* env,
        jobject /*thiz*/,
        jobject rgbaBuffer,
        jint bufferCapacity,
        jint srcWidth,
        jint srcHeight,
        jint rowStride,
        jint pixelStride,
        jint cropSize,
        jint jpegQuality,
        jlong frameProducedNs,
        jlong callbackStartNs) {
    if (rgbaBuffer == nullptr || srcWidth <= 0 || srcHeight <= 0 || cropSize <= 0 || rowStride <= 0 || pixelStride <= 0) return JNI_FALSE;
    if (!has_output_fd()) return JNI_FALSE;
    auto* base = static_cast<uint8_t*>(env->GetDirectBufferAddress(rgbaBuffer));
    if (base == nullptr) return JNI_FALSE;

    const int size = std::min(static_cast<int>(cropSize), std::min(static_cast<int>(srcWidth), static_cast<int>(srcHeight)));
    const int cropLeft = std::max(0, (static_cast<int>(srcWidth) - size) / 2);
    const int cropTop = std::max(0, (static_cast<int>(srcHeight) - size) / 2);

    const bool ok = encode_region_and_send_zero_copy_or_copy(base,
                                                            bufferCapacity,
                                                            rowStride,
                                                            pixelStride,
                                                            cropLeft,
                                                            cropTop,
                                                            size,
                                                            size,
                                                            jpegQuality,
                                                            frameProducedNs,
                                                            callbackStartNs);
    return ok ? JNI_TRUE : JNI_FALSE;
}

extern "C"
JNIEXPORT jboolean JNICALL
Java_com_example_huilangtoupingV3_NativeCenterCropJpegEncoder_nativeEncodeAndSendFullFrameRgba8888(
        JNIEnv* env,
        jobject /*thiz*/,
        jobject rgbaBuffer,
        jint bufferCapacity,
        jint srcWidth,
        jint srcHeight,
        jint rowStride,
        jint pixelStride,
        jint jpegQuality,
        jlong frameProducedNs,
        jlong callbackStartNs) {
    if (rgbaBuffer == nullptr || srcWidth <= 0 || srcHeight <= 0 || rowStride <= 0 || pixelStride <= 0) return JNI_FALSE;
    if (!has_output_fd()) return JNI_FALSE;
    auto* base = static_cast<uint8_t*>(env->GetDirectBufferAddress(rgbaBuffer));
    if (base == nullptr) return JNI_FALSE;

    bool ok = false;
    if (pixelStride == 4 && is_region_in_bounds(bufferCapacity, rowStride, pixelStride, 0, 0, srcWidth, srcHeight)) {
        ok = compress_pixels_and_send(reinterpret_cast<unsigned char*>(base), srcWidth, srcHeight, rowStride, TJPF_RGBA, jpegQuality, frameProducedNs, callbackStartNs);
    } else if (pixelStride == 3 && is_region_in_bounds(bufferCapacity, rowStride, pixelStride, 0, 0, srcWidth, srcHeight)) {
        ok = compress_pixels_and_send(reinterpret_cast<unsigned char*>(base), srcWidth, srcHeight, rowStride, TJPF_RGB, jpegQuality, frameProducedNs, callbackStartNs);
    } else {
        ok = copy_region_and_send(base, bufferCapacity, rowStride, pixelStride, 0, 0, srcWidth, srcHeight, jpegQuality, frameProducedNs, callbackStartNs);
    }
    return ok ? JNI_TRUE : JNI_FALSE;
}


static bool encode_and_send_full_frame_rgba8888_split_n(
        JNIEnv* env,
        jobject rgbaBuffer,
        jint bufferCapacity,
        jint srcWidth,
        jint srcHeight,
        jint rowStride,
        jint pixelStride,
        jint jpegQuality,
        jlong frameProducedNs,
        jlong callbackStartNs,
        int partCount) {
    if (rgbaBuffer == nullptr || srcWidth <= 0 || srcHeight <= 0 || rowStride <= 0 || pixelStride <= 0) return false;
    if (!has_output_fd()) return false;
    if (is_stale_for_encode(frameProducedNs)) return true;
    // 串行保护同一条视频流的 part/frame 顺序；worker TLS payload 已在 send-ready 路径中脱钩。
    std::lock_guard<std::mutex> callLock(g_split_call_mutex);
    const int workerCount = effective_split_part_count(partCount);

    auto* base = static_cast<uint8_t*>(env->GetDirectBufferAddress(rgbaBuffer));
    if (base == nullptr) return false;

    if (partCount <= 1 || partCount > MAX_SPLIT_PARTS ||
        pixelStride != 4 || srcHeight < partCount * 2 || srcWidth < 2 ||
        !is_region_in_bounds(bufferCapacity, rowStride, pixelStride, 0, 0, srcWidth, srcHeight)) {
        return compress_pixels_and_send(reinterpret_cast<unsigned char*>(base), srcWidth, srcHeight,
                                        rowStride, pixelStride == 3 ? TJPF_RGB : TJPF_RGBA,
                                        jpegQuality, frameProducedNs, callbackStartNs);
    }

    int partLefts[MAX_SPLIT_PARTS]{};
    int partWidths[MAX_SPLIT_PARTS]{};
    int partEncodedWidths[MAX_SPLIT_PARTS]{};
    int partTops[MAX_SPLIT_PARTS]{};
    int partHeights[MAX_SPLIT_PARTS]{};
    int partJpegQualities[MAX_SPLIT_PARTS]{};
    unsigned char* partPixels[MAX_SPLIT_PARTS]{};
    int workerPartMap[MAX_SPLIT_PARTS]{};

    int actualPartCount = 0;
    if (!build_column_roi_2_6_2_layout(static_cast<int>(srcWidth), static_cast<int>(srcHeight), workerCount, actualPartCount,
                                     partLefts, partTops, partWidths, partHeights, partEncodedWidths, partJpegQualities, jpegQuality)) {
        return compress_pixels_and_send(reinterpret_cast<unsigned char*>(base), srcWidth, srcHeight,
                                        rowStride, TJPF_RGBA, jpegQuality, frameProducedNs, callbackStartNs);
    }
    partCount = actualPartCount;
    if (!build_worker_task_assignment_by_geometry(partCount, partWidths, partHeights, partJpegQualities, jpegQuality, workerPartMap)) return false;
    for (int i = 0; i < partCount; ++i) {
        if (partWidths[i] <= 0 || partEncodedWidths[i] <= 0 || partHeights[i] <= 0 ||
            !is_region_in_bounds(bufferCapacity, rowStride, pixelStride, partLefts[i], partTops[i], partWidths[i], partHeights[i])) {
            return compress_pixels_and_send(reinterpret_cast<unsigned char*>(base), srcWidth, srcHeight,
                                            rowStride, TJPF_RGBA, jpegQuality, frameProducedNs, callbackStartNs);
        }
        partPixels[i] = reinterpret_cast<unsigned char*>(base + static_cast<int64_t>(partTops[i]) * rowStride + static_cast<int64_t>(partLefts[i]) * pixelStride);
    }

    EncodedJpegRef partJpegs[MAX_SPLIT_PARTS]{};
    int64_t partEncodeNs[MAX_SPLIT_PARTS]{};
    int partCpuIds[MAX_SPLIT_PARTS]{};
    int64_t encodeStartNs = 0;
    int64_t encodeEndNs = 0;

    const bool sent = encode_split_parallel_send_ready_fixed(partCount, workerCount, partPixels, partLefts, partTops, partWidths, partEncodedWidths, partHeights, partJpegQualities, workerPartMap,
                                                            static_cast<int>(rowStride),
                                                            TJPF_RGBA, jpegQuality,
                                                            static_cast<int>(srcWidth), static_cast<int>(srcHeight),
                                                            frameProducedNs, callbackStartNs,
                                                            partJpegs, encodeStartNs, encodeEndNs, partEncodeNs, partCpuIds);
    if (!sent) return false;
    update_split_complexity_history_fixed(partCount, partEncodeNs, partWidths, partHeights);
    update_split_cpu_width_history_fixed(partCount, partCpuIds);
    return true;
}


static bool encode_and_send_frame_except_center_rgba8888_split_n(
        JNIEnv* env,
        jobject rgbaBuffer,
        jint bufferCapacity,
        jint srcWidth,
        jint srcHeight,
        jint rowStride,
        jint pixelStride,
        jint centerLeft,
        jint centerTop,
        jint centerWidth,
        jint centerHeight,
        jint jpegQuality,
        jlong frameProducedNs,
        jlong callbackStartNs,
        int partCount) {
    if (rgbaBuffer == nullptr || srcWidth <= 0 || srcHeight <= 0 || rowStride <= 0 || pixelStride <= 0) return false;
    if (!has_output_fd()) return false;
    if (is_stale_for_encode(frameProducedNs)) return true;
    // Same socket/order protection as the normal split path; H.264 center video uses another socket.
    std::lock_guard<std::mutex> callLock(g_split_call_mutex);
    const int workerCount = effective_split_part_count(partCount);

    auto* base = static_cast<uint8_t*>(env->GetDirectBufferAddress(rgbaBuffer));
    if (base == nullptr) return false;

    if (partCount <= 1 || partCount > MAX_SPLIT_PARTS || pixelStride != 4 || srcWidth < 4 || srcHeight < 4 ||
        !is_region_in_bounds(bufferCapacity, rowStride, pixelStride, 0, 0, srcWidth, srcHeight)) {
        ALOGE("except-center refused invalid input; not falling back to full-frame CPU JPEG parts=%d fmtStride=%d size=%dx%d",
              partCount, static_cast<int>(pixelStride), static_cast<int>(srcWidth), static_cast<int>(srcHeight));
        return false;
    }

    int partLefts[MAX_SPLIT_PARTS]{};
    int partWidths[MAX_SPLIT_PARTS]{};
    int partEncodedWidths[MAX_SPLIT_PARTS]{};
    int partTops[MAX_SPLIT_PARTS]{};
    int partHeights[MAX_SPLIT_PARTS]{};
    int partJpegQualities[MAX_SPLIT_PARTS]{};
    unsigned char* partPixels[MAX_SPLIT_PARTS]{};
    int workerPartMap[MAX_SPLIT_PARTS]{};

    int actualPartCount = 0;
    if (!build_outside_center_roi_layout(static_cast<int>(srcWidth), static_cast<int>(srcHeight),
                                         static_cast<int>(centerLeft), static_cast<int>(centerTop),
                                         static_cast<int>(centerWidth), static_cast<int>(centerHeight),
                                         workerCount, actualPartCount,
                                         partLefts, partTops, partWidths, partHeights,
                                         partEncodedWidths, partJpegQualities, jpegQuality)) {
        ALOGE("except-center layout failed; not falling back to full-frame CPU JPEG center=%d,%d %dx%d full=%dx%d",
              static_cast<int>(centerLeft), static_cast<int>(centerTop),
              static_cast<int>(centerWidth), static_cast<int>(centerHeight),
              static_cast<int>(srcWidth), static_cast<int>(srcHeight));
        return false;
    }
    partCount = actualPartCount;
    if (!build_worker_task_assignment_by_geometry(partCount, partWidths, partHeights, partJpegQualities, jpegQuality, workerPartMap)) return false;

    for (int i = 0; i < partCount; ++i) {
        if (partWidths[i] <= 0 || partEncodedWidths[i] <= 0 || partHeights[i] <= 0 ||
            !is_region_in_bounds(bufferCapacity, rowStride, pixelStride, partLefts[i], partTops[i], partWidths[i], partHeights[i])) {
            ALOGE("except-center part invalid; not falling back to full-frame CPU JPEG i=%d rect=%d,%d %dx%d",
                  i, partLefts[i], partTops[i], partWidths[i], partHeights[i]);
            return false;
        }
        partPixels[i] = reinterpret_cast<unsigned char*>(base + static_cast<int64_t>(partTops[i]) * rowStride + static_cast<int64_t>(partLefts[i]) * pixelStride);
    }

    EncodedJpegRef partJpegs[MAX_SPLIT_PARTS]{};
    int64_t partEncodeNs[MAX_SPLIT_PARTS]{};
    int partCpuIds[MAX_SPLIT_PARTS]{};
    int64_t encodeStartNs = 0;
    int64_t encodeEndNs = 0;

    const bool sent = encode_split_parallel_send_ready_fixed(partCount, workerCount,
                                                            partPixels, partLefts, partTops,
                                                            partWidths, partEncodedWidths, partHeights,
                                                            partJpegQualities, workerPartMap,
                                                            static_cast<int>(rowStride),
                                                            TJPF_RGBA, jpegQuality,
                                                            static_cast<int>(srcWidth), static_cast<int>(srcHeight),
                                                            frameProducedNs, callbackStartNs,
                                                            partJpegs, encodeStartNs, encodeEndNs,
                                                            partEncodeNs, partCpuIds);
    if (!sent) return false;
    update_split_complexity_history_fixed(partCount, partEncodeNs, partWidths, partHeights);
    update_split_cpu_width_history_fixed(partCount, partCpuIds);
    return true;
}

static bool encode_and_send_frame_with_bottom_video_rgba8888_split_n(
        JNIEnv* env,
        jobject rgbaBuffer,
        jint bufferCapacity,
        jint srcWidth,
        jint srcHeight,
        jint rowStride,
        jint pixelStride,
        jint videoTop,
        jint videoHeight,
        jint jpegQuality,
        jlong frameProducedNs,
        jlong callbackStartNs,
        int partCount) {
    if (rgbaBuffer == nullptr || srcWidth <= 0 || srcHeight <= 0 || rowStride <= 0 || pixelStride <= 0) return false;
    if (!has_output_fd()) return false;
    if (is_stale_for_encode(frameProducedNs)) return true;

    // Separate lock/path from the pure JPEG layout.  This mode sends only the upper JPEG
    // visual field; the bottom strip is sent through the H.264 socket.
    std::lock_guard<std::mutex> callLock(g_split_call_mutex);
    const int workerCount = effective_split_part_count(partCount);

    auto* base = static_cast<uint8_t*>(env->GetDirectBufferAddress(rgbaBuffer));
    if (base == nullptr) return false;

    if (partCount <= 1 || partCount > MAX_SPLIT_PARTS || pixelStride != 4 || srcWidth < 4 || srcHeight < 4 ||
        !is_region_in_bounds(bufferCapacity, rowStride, pixelStride, 0, 0, srcWidth, srcHeight)) {
        ALOGE("bottom-video layout refused invalid input; not falling back to full-frame CPU JPEG parts=%d fmtStride=%d size=%dx%d",
              partCount, static_cast<int>(pixelStride), static_cast<int>(srcWidth), static_cast<int>(srcHeight));
        return false;
    }

    int partLefts[MAX_SPLIT_PARTS]{};
    int partWidths[MAX_SPLIT_PARTS]{};
    int partEncodedWidths[MAX_SPLIT_PARTS]{};
    int partTops[MAX_SPLIT_PARTS]{};
    int partHeights[MAX_SPLIT_PARTS]{};
    int partJpegQualities[MAX_SPLIT_PARTS]{};
    unsigned char* partPixels[MAX_SPLIT_PARTS]{};
    int workerPartMap[MAX_SPLIT_PARTS]{};

    int actualPartCount = 0;
    if (!build_upper_jpeg_bottom_video_layout(static_cast<int>(srcWidth), static_cast<int>(srcHeight),
                                             static_cast<int>(videoTop), static_cast<int>(videoHeight),
                                             workerCount, actualPartCount,
                                             partLefts, partTops, partWidths, partHeights,
                                             partEncodedWidths, partJpegQualities, jpegQuality)) {
        ALOGE("bottom-video JPEG layout failed videoTop=%d videoHeight=%d full=%dx%d",
              static_cast<int>(videoTop), static_cast<int>(videoHeight),
              static_cast<int>(srcWidth), static_cast<int>(srcHeight));
        return false;
    }
    partCount = actualPartCount;
    if (!build_worker_task_assignment_by_geometry(partCount, partWidths, partHeights, partJpegQualities, jpegQuality, workerPartMap)) return false;

    for (int i = 0; i < partCount; ++i) {
        if (partWidths[i] <= 0 || partEncodedWidths[i] <= 0 || partHeights[i] <= 0 ||
            !is_region_in_bounds(bufferCapacity, rowStride, pixelStride, partLefts[i], partTops[i], partWidths[i], partHeights[i])) {
            ALOGE("bottom-video part invalid i=%d rect=%d,%d %dx%d", i, partLefts[i], partTops[i], partWidths[i], partHeights[i]);
            return false;
        }
        partPixels[i] = reinterpret_cast<unsigned char*>(base + static_cast<int64_t>(partTops[i]) * rowStride + static_cast<int64_t>(partLefts[i]) * pixelStride);
    }

    EncodedJpegRef partJpegs[MAX_SPLIT_PARTS]{};
    int64_t partEncodeNs[MAX_SPLIT_PARTS]{};
    int partCpuIds[MAX_SPLIT_PARTS]{};
    int64_t encodeStartNs = 0;
    int64_t encodeEndNs = 0;

    const bool sent = encode_split_parallel_send_ready_fixed(partCount, workerCount,
                                                            partPixels, partLefts, partTops,
                                                            partWidths, partEncodedWidths, partHeights,
                                                            partJpegQualities, workerPartMap,
                                                            static_cast<int>(rowStride),
                                                            TJPF_RGBA, jpegQuality,
                                                            static_cast<int>(srcWidth), static_cast<int>(srcHeight),
                                                            frameProducedNs, callbackStartNs,
                                                            partJpegs, encodeStartNs, encodeEndNs,
                                                            partEncodeNs, partCpuIds);
    if (!sent) return false;
    update_split_complexity_history_fixed(partCount, partEncodeNs, partWidths, partHeights);
    update_split_cpu_width_history_fixed(partCount, partCpuIds);
    return true;
}

extern "C"
JNIEXPORT jboolean JNICALL
Java_com_example_huilangtoupingV3_NativeCenterCropJpegEncoder_nativeEncodeAndSendFullFrameRgba8888Split2(
        JNIEnv* env,
        jobject /*thiz*/,
        jobject rgbaBuffer,
        jint bufferCapacity,
        jint srcWidth,
        jint srcHeight,
        jint rowStride,
        jint pixelStride,
        jint jpegQuality,
        jlong frameProducedNs,
        jlong callbackStartNs) {
    const bool ok = encode_and_send_full_frame_rgba8888_split_n(
            env, rgbaBuffer, bufferCapacity, srcWidth, srcHeight, rowStride, pixelStride,
            jpegQuality, frameProducedNs, callbackStartNs, 2);
    return ok ? JNI_TRUE : JNI_FALSE;
}

extern "C"
JNIEXPORT jboolean JNICALL
Java_com_example_huilangtoupingV3_NativeCenterCropJpegEncoder_nativeEncodeAndSendFullFrameRgba8888Split4(
        JNIEnv* env,
        jobject /*thiz*/,
        jobject rgbaBuffer,
        jint bufferCapacity,
        jint srcWidth,
        jint srcHeight,
        jint rowStride,
        jint pixelStride,
        jint jpegQuality,
        jlong frameProducedNs,
        jlong callbackStartNs) {
    const bool ok = encode_and_send_full_frame_rgba8888_split_n(
            env, rgbaBuffer, bufferCapacity, srcWidth, srcHeight, rowStride, pixelStride,
            jpegQuality, frameProducedNs, callbackStartNs, 4);
    return ok ? JNI_TRUE : JNI_FALSE;
}

extern "C"
JNIEXPORT jboolean JNICALL
Java_com_example_huilangtoupingV3_NativeCenterCropJpegEncoder_nativeEncodeAndSendFullFrameRgba8888Split6(
        JNIEnv* env,
        jobject /*thiz*/,
        jobject rgbaBuffer,
        jint bufferCapacity,
        jint srcWidth,
        jint srcHeight,
        jint rowStride,
        jint pixelStride,
        jint jpegQuality,
        jlong frameProducedNs,
        jlong callbackStartNs) {
    const bool ok = encode_and_send_full_frame_rgba8888_split_n(
            env, rgbaBuffer, bufferCapacity, srcWidth, srcHeight, rowStride, pixelStride,
            jpegQuality, frameProducedNs, callbackStartNs, 6);
    return ok ? JNI_TRUE : JNI_FALSE;
}

extern "C"
JNIEXPORT jboolean JNICALL
Java_com_example_huilangtoupingV3_NativeCenterCropJpegEncoder_nativeEncodeAndSendFullFrameRgba8888Split7(
        JNIEnv* env,
        jobject /*thiz*/,
        jobject rgbaBuffer,
        jint bufferCapacity,
        jint srcWidth,
        jint srcHeight,
        jint rowStride,
        jint pixelStride,
        jint jpegQuality,
        jlong frameProducedNs,
        jlong callbackStartNs) {
    const bool ok = encode_and_send_full_frame_rgba8888_split_n(
            env, rgbaBuffer, bufferCapacity, srcWidth, srcHeight, rowStride, pixelStride,
            jpegQuality, frameProducedNs, callbackStartNs, 7);
    return ok ? JNI_TRUE : JNI_FALSE;
}

extern "C"
JNIEXPORT jboolean JNICALL
Java_com_example_huilangtoupingV3_NativeCenterCropJpegEncoder_nativeEncodeAndSendFullFrameRgba8888Split8(
        JNIEnv* env,
        jobject /*thiz*/,
        jobject rgbaBuffer,
        jint bufferCapacity,
        jint srcWidth,
        jint srcHeight,
        jint rowStride,
        jint pixelStride,
        jint jpegQuality,
        jlong frameProducedNs,
        jlong callbackStartNs) {
    const bool ok = encode_and_send_full_frame_rgba8888_split_n(
            env, rgbaBuffer, bufferCapacity, srcWidth, srcHeight, rowStride, pixelStride,
            jpegQuality, frameProducedNs, callbackStartNs, 8);
    return ok ? JNI_TRUE : JNI_FALSE;
}

extern "C"
JNIEXPORT jboolean JNICALL
Java_com_example_huilangtoupingV3_NativeCenterCropJpegEncoder_nativeEncodeAndSendFullFrameRgba8888SplitN(
        JNIEnv* env,
        jobject /*thiz*/,
        jobject rgbaBuffer,
        jint bufferCapacity,
        jint srcWidth,
        jint srcHeight,
        jint rowStride,
        jint pixelStride,
        jint jpegQuality,
        jlong frameProducedNs,
        jlong callbackStartNs,
        jint splitParts) {
    const bool ok = encode_and_send_full_frame_rgba8888_split_n(
            env, rgbaBuffer, bufferCapacity, srcWidth, srcHeight, rowStride, pixelStride,
            jpegQuality, frameProducedNs, callbackStartNs, static_cast<int>(splitParts));
    return ok ? JNI_TRUE : JNI_FALSE;
}


extern "C"
JNIEXPORT jboolean JNICALL
Java_com_example_huilangtoupingV3_NativeCenterCropJpegEncoder_nativeEncodeAndSendFrameExceptCenterRgba8888SplitN(
        JNIEnv* env,
        jobject /*thiz*/,
        jobject rgbaBuffer,
        jint bufferCapacity,
        jint srcWidth,
        jint srcHeight,
        jint rowStride,
        jint pixelStride,
        jint centerLeft,
        jint centerTop,
        jint centerWidth,
        jint centerHeight,
        jint jpegQuality,
        jlong frameProducedNs,
        jlong callbackStartNs,
        jint splitParts) {
    const bool ok = encode_and_send_frame_except_center_rgba8888_split_n(
            env, rgbaBuffer, bufferCapacity, srcWidth, srcHeight, rowStride, pixelStride,
            centerLeft, centerTop, centerWidth, centerHeight,
            jpegQuality, frameProducedNs, callbackStartNs, static_cast<int>(splitParts));
    return ok ? JNI_TRUE : JNI_FALSE;
}

extern "C"
JNIEXPORT jboolean JNICALL
Java_com_example_huilangtoupingV3_NativeCenterCropJpegEncoder_nativeEncodeAndSendFrameWithBottomVideoRgba8888SplitN(
        JNIEnv* env,
        jobject /*thiz*/,
        jobject rgbaBuffer,
        jint bufferCapacity,
        jint srcWidth,
        jint srcHeight,
        jint rowStride,
        jint pixelStride,
        jint videoTop,
        jint videoHeight,
        jint jpegQuality,
        jlong frameProducedNs,
        jlong callbackStartNs,
        jint splitParts) {
    const bool ok = encode_and_send_frame_with_bottom_video_rgba8888_split_n(
            env, rgbaBuffer, bufferCapacity, srcWidth, srcHeight, rowStride, pixelStride,
            videoTop, videoHeight,
            jpegQuality, frameProducedNs, callbackStartNs, static_cast<int>(splitParts));
    return ok ? JNI_TRUE : JNI_FALSE;
}


static bool encode_and_send_center_crop_rgba8888_split_n(
        JNIEnv* env,
        jobject rgbaBuffer,
        jint bufferCapacity,
        jint srcWidth,
        jint srcHeight,
        jint rowStride,
        jint pixelStride,
        jint cropSize,
        jint jpegQuality,
        jlong frameProducedNs,
        jlong callbackStartNs,
        int partCount) {
    if (rgbaBuffer == nullptr || srcWidth <= 0 || srcHeight <= 0 || cropSize <= 0 || rowStride <= 0 || pixelStride <= 0) return false;
    if (!has_output_fd()) return false;
    if (is_stale_for_encode(frameProducedNs)) return true;
    // 串行保护同一条视频流的 part/frame 顺序；worker TLS payload 已在 send-ready 路径中脱钩。
    std::lock_guard<std::mutex> callLock(g_split_call_mutex);
    const int workerCount = effective_split_part_count(partCount);

    auto* base = static_cast<uint8_t*>(env->GetDirectBufferAddress(rgbaBuffer));
    if (base == nullptr) return false;

    const int size = std::min(static_cast<int>(cropSize), std::min(static_cast<int>(srcWidth), static_cast<int>(srcHeight)));
    const int cropLeft = std::max(0, (static_cast<int>(srcWidth) - size) / 2);
    const int cropTop = std::max(0, (static_cast<int>(srcHeight) - size) / 2);

    if (partCount <= 1 || partCount > MAX_SPLIT_PARTS ||
        pixelStride != 4 || size < partCount * 2 ||
        !is_region_in_bounds(bufferCapacity, rowStride, pixelStride, cropLeft, cropTop, size, size)) {
        return encode_region_and_send_zero_copy_or_copy(base, bufferCapacity, rowStride, pixelStride,
                                                        cropLeft, cropTop, size, size,
                                                        jpegQuality, frameProducedNs, callbackStartNs);
    }

    int partLefts[MAX_SPLIT_PARTS]{};
    int partWidths[MAX_SPLIT_PARTS]{};
    int partEncodedWidths[MAX_SPLIT_PARTS]{};
    int partTops[MAX_SPLIT_PARTS]{};
    int partHeights[MAX_SPLIT_PARTS]{};
    int partJpegQualities[MAX_SPLIT_PARTS]{};
    unsigned char* partPixels[MAX_SPLIT_PARTS]{};
    int workerPartMap[MAX_SPLIT_PARTS]{};

    int actualPartCount = 0;
    if (!build_column_roi_2_6_2_layout(size, size, workerCount, actualPartCount,
                                     partLefts, partTops, partWidths, partHeights, partEncodedWidths, partJpegQualities, jpegQuality)) {
        return encode_region_and_send_zero_copy_or_copy(base, bufferCapacity, rowStride, pixelStride,
                                                        cropLeft, cropTop, size, size,
                                                        jpegQuality, frameProducedNs, callbackStartNs);
    }
    partCount = actualPartCount;
    if (!build_worker_task_assignment_by_geometry(partCount, partWidths, partHeights, partJpegQualities, jpegQuality, workerPartMap)) return false;

    for (int i = 0; i < partCount; ++i) {
        if (partWidths[i] <= 0 || partEncodedWidths[i] <= 0 || partHeights[i] <= 0) {
            return encode_region_and_send_zero_copy_or_copy(base, bufferCapacity, rowStride, pixelStride,
                                                            cropLeft, cropTop, size, size,
                                                            jpegQuality, frameProducedNs, callbackStartNs);
        }
        partPixels[i] = reinterpret_cast<unsigned char*>(
                base + static_cast<int64_t>(cropTop + partTops[i]) * rowStride
                     + static_cast<int64_t>(cropLeft + partLefts[i]) * pixelStride);
    }

    mark_center_crop_path(true, "split_zero_copy");

    EncodedJpegRef partJpegs[MAX_SPLIT_PARTS]{};
    int64_t partEncodeNs[MAX_SPLIT_PARTS]{};
    int partCpuIds[MAX_SPLIT_PARTS]{};
    int64_t encodeStartNs = 0;
    int64_t encodeEndNs = 0;

    const bool sent = encode_split_parallel_send_ready_fixed(partCount, workerCount, partPixels, partLefts, partTops, partWidths, partEncodedWidths, partHeights, partJpegQualities, workerPartMap,
                                                            static_cast<int>(rowStride),
                                                            TJPF_RGBA, jpegQuality,
                                                            size, size,
                                                            frameProducedNs, callbackStartNs,
                                                            partJpegs, encodeStartNs, encodeEndNs, partEncodeNs, partCpuIds);
    if (!sent) return false;
    update_split_complexity_history_fixed(partCount, partEncodeNs, partWidths, partHeights);
    update_split_cpu_width_history_fixed(partCount, partCpuIds);
    return true;
}

extern "C"
JNIEXPORT jboolean JNICALL
Java_com_example_huilangtoupingV3_NativeCenterCropJpegEncoder_nativeEncodeAndSendCenterCropRgba8888Split2(
        JNIEnv* env,
        jobject /*thiz*/,
        jobject rgbaBuffer,
        jint bufferCapacity,
        jint srcWidth,
        jint srcHeight,
        jint rowStride,
        jint pixelStride,
        jint cropSize,
        jint jpegQuality,
        jlong frameProducedNs,
        jlong callbackStartNs) {
    const bool ok = encode_and_send_center_crop_rgba8888_split_n(
            env, rgbaBuffer, bufferCapacity, srcWidth, srcHeight, rowStride, pixelStride,
            cropSize, jpegQuality, frameProducedNs, callbackStartNs, 2);
    return ok ? JNI_TRUE : JNI_FALSE;
}

extern "C"
JNIEXPORT jboolean JNICALL
Java_com_example_huilangtoupingV3_NativeCenterCropJpegEncoder_nativeEncodeAndSendCenterCropRgba8888Split4(
        JNIEnv* env,
        jobject /*thiz*/,
        jobject rgbaBuffer,
        jint bufferCapacity,
        jint srcWidth,
        jint srcHeight,
        jint rowStride,
        jint pixelStride,
        jint cropSize,
        jint jpegQuality,
        jlong frameProducedNs,
        jlong callbackStartNs) {
    const bool ok = encode_and_send_center_crop_rgba8888_split_n(
            env, rgbaBuffer, bufferCapacity, srcWidth, srcHeight, rowStride, pixelStride,
            cropSize, jpegQuality, frameProducedNs, callbackStartNs, 4);
    return ok ? JNI_TRUE : JNI_FALSE;
}


extern "C"
JNIEXPORT jboolean JNICALL
Java_com_example_huilangtoupingV3_NativeCenterCropJpegEncoder_nativeEncodeAndSendCenterCropRgba8888SplitN(
        JNIEnv* env,
        jobject /*thiz*/,
        jobject rgbaBuffer,
        jint bufferCapacity,
        jint srcWidth,
        jint srcHeight,
        jint rowStride,
        jint pixelStride,
        jint cropSize,
        jint jpegQuality,
        jlong frameProducedNs,
        jlong callbackStartNs,
        jint splitParts) {
    const bool ok = encode_and_send_center_crop_rgba8888_split_n(
            env, rgbaBuffer, bufferCapacity, srcWidth, srcHeight, rowStride, pixelStride,
            cropSize, jpegQuality, frameProducedNs, callbackStartNs, static_cast<int>(splitParts));
    return ok ? JNI_TRUE : JNI_FALSE;
}

extern "C"
JNIEXPORT jbyteArray JNICALL
Java_com_example_huilangtoupingV3_NativeCenterCropJpegEncoder_nativeEncodeCenterCropYuv420888(
        JNIEnv* env,
        jobject /*thiz*/,
        jobject yBuffer,
        jint yCapacity,
        jobject uBuffer,
        jint uCapacity,
        jobject vBuffer,
        jint vCapacity,
        jint srcWidth,
        jint srcHeight,
        jint yRowStride,
        jint uRowStride,
        jint vRowStride,
        jint uPixelStride,
        jint vPixelStride,
        jint cropSize,
        jint jpegQuality) {
    if (yBuffer == nullptr || uBuffer == nullptr || vBuffer == nullptr) return nullptr;
    auto* yBase = static_cast<uint8_t*>(env->GetDirectBufferAddress(yBuffer));
    auto* uBase = static_cast<uint8_t*>(env->GetDirectBufferAddress(uBuffer));
    auto* vBase = static_cast<uint8_t*>(env->GetDirectBufferAddress(vBuffer));
    if (yBase == nullptr || uBase == nullptr || vBase == nullptr) return nullptr;

    int cropLeft = 0, cropTop = 0, cropWidth = 0, cropHeight = 0;
    if (!normalize_crop_420(srcWidth, srcHeight, cropSize, cropLeft, cropTop, cropWidth, cropHeight)) return nullptr;
    if (!copy_yuv420888_region_to_i420(yBase, yCapacity, uBase, uCapacity, vBase, vCapacity,
                                       srcWidth, srcHeight, yRowStride, uRowStride, vRowStride,
                                       uPixelStride, vPixelStride, cropLeft, cropTop, cropWidth, cropHeight)) {
        return nullptr;
    }
    return compress_i420_to_byte_array(env, cropWidth, cropHeight, jpegQuality);
}

extern "C"
JNIEXPORT jbyteArray JNICALL
Java_com_example_huilangtoupingV3_NativeCenterCropJpegEncoder_nativeEncodeFullFrameYuv420888(
        JNIEnv* env,
        jobject /*thiz*/,
        jobject yBuffer,
        jint yCapacity,
        jobject uBuffer,
        jint uCapacity,
        jobject vBuffer,
        jint vCapacity,
        jint srcWidth,
        jint srcHeight,
        jint yRowStride,
        jint uRowStride,
        jint vRowStride,
        jint uPixelStride,
        jint vPixelStride,
        jint jpegQuality) {
    if (yBuffer == nullptr || uBuffer == nullptr || vBuffer == nullptr) return nullptr;
    auto* yBase = static_cast<uint8_t*>(env->GetDirectBufferAddress(yBuffer));
    auto* uBase = static_cast<uint8_t*>(env->GetDirectBufferAddress(uBuffer));
    auto* vBase = static_cast<uint8_t*>(env->GetDirectBufferAddress(vBuffer));
    if (yBase == nullptr || uBase == nullptr || vBase == nullptr) return nullptr;

    int width = 0, height = 0;
    if (!normalize_full_frame_420(srcWidth, srcHeight, width, height)) return nullptr;
    if (!copy_yuv420888_region_to_i420(yBase, yCapacity, uBase, uCapacity, vBase, vCapacity,
                                       srcWidth, srcHeight, yRowStride, uRowStride, vRowStride,
                                       uPixelStride, vPixelStride, 0, 0, width, height)) {
        return nullptr;
    }
    return compress_i420_to_byte_array(env, width, height, jpegQuality);
}

extern "C"
JNIEXPORT jboolean JNICALL
Java_com_example_huilangtoupingV3_NativeCenterCropJpegEncoder_nativeEncodeAndSendCenterCropYuv420888(
        JNIEnv* env,
        jobject /*thiz*/,
        jobject yBuffer,
        jint yCapacity,
        jobject uBuffer,
        jint uCapacity,
        jobject vBuffer,
        jint vCapacity,
        jint srcWidth,
        jint srcHeight,
        jint yRowStride,
        jint uRowStride,
        jint vRowStride,
        jint uPixelStride,
        jint vPixelStride,
        jint cropSize,
        jint jpegQuality,
        jlong frameProducedNs,
        jlong callbackStartNs) {
    if (!has_output_fd()) return JNI_FALSE;
    if (yBuffer == nullptr || uBuffer == nullptr || vBuffer == nullptr) return JNI_FALSE;

    auto* yBase = static_cast<uint8_t*>(env->GetDirectBufferAddress(yBuffer));
    auto* uBase = static_cast<uint8_t*>(env->GetDirectBufferAddress(uBuffer));
    auto* vBase = static_cast<uint8_t*>(env->GetDirectBufferAddress(vBuffer));
    if (yBase == nullptr || uBase == nullptr || vBase == nullptr) return JNI_FALSE;

    int cropLeft = 0, cropTop = 0, cropWidth = 0, cropHeight = 0;
    if (!normalize_crop_420(srcWidth, srcHeight, cropSize, cropLeft, cropTop, cropWidth, cropHeight)) return JNI_FALSE;
    if (!copy_yuv420888_region_to_i420(yBase, yCapacity, uBase, uCapacity, vBase, vCapacity,
                                       srcWidth, srcHeight, yRowStride, uRowStride, vRowStride,
                                       uPixelStride, vPixelStride, cropLeft, cropTop, cropWidth, cropHeight)) {
        return JNI_FALSE;
    }
    const bool ok = compress_i420_and_send(cropWidth, cropHeight, jpegQuality, frameProducedNs, callbackStartNs);
    return ok ? JNI_TRUE : JNI_FALSE;
}

extern "C"
JNIEXPORT jboolean JNICALL
Java_com_example_huilangtoupingV3_NativeCenterCropJpegEncoder_nativeEncodeAndSendFullFrameYuv420888(
        JNIEnv* env,
        jobject /*thiz*/,
        jobject yBuffer,
        jint yCapacity,
        jobject uBuffer,
        jint uCapacity,
        jobject vBuffer,
        jint vCapacity,
        jint srcWidth,
        jint srcHeight,
        jint yRowStride,
        jint uRowStride,
        jint vRowStride,
        jint uPixelStride,
        jint vPixelStride,
        jint jpegQuality,
        jlong frameProducedNs,
        jlong callbackStartNs) {
    if (!has_output_fd()) return JNI_FALSE;
    if (yBuffer == nullptr || uBuffer == nullptr || vBuffer == nullptr) return JNI_FALSE;

    auto* yBase = static_cast<uint8_t*>(env->GetDirectBufferAddress(yBuffer));
    auto* uBase = static_cast<uint8_t*>(env->GetDirectBufferAddress(uBuffer));
    auto* vBase = static_cast<uint8_t*>(env->GetDirectBufferAddress(vBuffer));
    if (yBase == nullptr || uBase == nullptr || vBase == nullptr) return JNI_FALSE;

    int width = 0, height = 0;
    if (!normalize_full_frame_420(srcWidth, srcHeight, width, height)) return JNI_FALSE;
    if (!copy_yuv420888_region_to_i420(yBase, yCapacity, uBase, uCapacity, vBase, vCapacity,
                                       srcWidth, srcHeight, yRowStride, uRowStride, vRowStride,
                                       uPixelStride, vPixelStride, 0, 0, width, height)) {
        return JNI_FALSE;
    }
    const bool ok = compress_i420_and_send(width, height, jpegQuality, frameProducedNs, callbackStartNs);
    return ok ? JNI_TRUE : JNI_FALSE;
}
