// pc_uinputd.cpp
// Minimal Android native daemon for PC-driven touch injection.
// Build as PIE executable, push to /data/local/tmp, run from adb shell.

#include "pc_uinput_protocol.h"

#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <signal.h>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>
#include <vector>
#include <atomic>
#include <mutex>
#include <thread>
#include <poll.h>

#ifndef UI_ABS_SETUP
#error "linux/uinput.h is too old: UI_ABS_SETUP missing"
#endif

namespace {

// Android MotionEvent supports more than 10 pointers; keep PC menu slots 10/11
// distinct from view-lock slots 2/9 instead of letting them be clamped to 9.
constexpr int kPcMaxSlots = 16;
constexpr int kMirrorSlotBase = 16;
constexpr int kMirrorMaxSlots = 16;
constexpr int kMaxSlots = kPcMaxSlots + kMirrorMaxSlots;
constexpr int kScanMaxEvent = 127;
constexpr const char* kDefaultSocketName = "huilang_pc_uinput";

volatile sig_atomic_t g_stop = 0;
int g_uinputFd = -1;
int g_nextTrackingId = 100;
int g_rotation = -1; // -1 = auto-detect; 0/1/2/3 = explicit Surface rotation

struct AxisRange {
    int minX = 0;
    int minY = 0;
    // Old uinput_touchd2 defaults.  Many Android input stacks normalize virtual
    // direct-touch devices using the virtual device's declared ABS range; this
    // range is known-good for the original path and remains the fallback when
    // real-touch probing is unavailable.
    int maxX = 19040;
    int maxY = 30400;
};

struct DisplayConfig {
    int uiW = 0;
    int uiH = 0;
    int physW = 0;
    int physH = 0;
    int rotation = -1;
    std::string rotationSource = "none";
};

struct TouchSlot {
    bool active = false;
    int trackingId = -1;
    int rawX = 0;
    int rawY = 0;
};

TouchSlot g_slots[kMaxSlots];
AxisRange g_range;
DisplayConfig g_display;
std::string g_selectedTouchPath;
std::string g_selectedTouchName;

std::mutex g_touchMutex;
// 默认关闭真实触摸镜像，避免启动后 EVIOCGRAB 物理触摸屏导致部分设备真实手指失效。
// 需要镜像真实手指时仍可通过启动参数 --mirror 1 显式开启。
std::atomic<bool> g_mirrorEnabled{false};
std::atomic<bool> g_mirrorRunning{false};
std::thread g_mirrorThread;
int g_mirrorInputFd = -1;

struct MirrorFingerSlot {
    bool active = false;
    bool downSent = false;
    bool haveX = false;
    bool haveY = false;
    int trackingId = -1;
    int rawX = 0;
    int rawY = 0;
};

MirrorFingerSlot g_mirrorFingers[kMirrorMaxSlots];

static void on_signal(int) {
    g_stop = 1;
}

static int clamp_int(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static bool bit_is_set(const unsigned long* bits, int bit) {
    const int bitsPerLong = static_cast<int>(sizeof(unsigned long) * 8);
    return ((bits[bit / bitsPerLong] >> (bit % bitsPerLong)) & 1UL) != 0;
}

static std::string lower_copy(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

static bool contains_any(const std::string& s, std::initializer_list<const char*> keys) {
    for (const char* k : keys) {
        if (s.find(k) != std::string::npos) return true;
    }
    return false;
}


static std::string run_shell_capture(const char* cmd) {
    std::string out;
    FILE* fp = popen(cmd, "r");
    if (!fp) return out;
    char buf[256];
    while (fgets(buf, sizeof(buf), fp)) out += buf;
    pclose(fp);
    return out;
}

static bool parse_first_size_token(const std::string& text, int& outW, int& outH) {
    bool found = false;
    int bestW = 0, bestH = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(text[i]))) continue;
        size_t j = i;
        long long w = 0;
        while (j < text.size() && std::isdigit(static_cast<unsigned char>(text[j]))) {
            w = w * 10 + (text[j] - '0');
            ++j;
        }
        if (j >= text.size() || (text[j] != 'x' && text[j] != 'X')) { i = j; continue; }
        ++j;
        if (j >= text.size() || !std::isdigit(static_cast<unsigned char>(text[j]))) { i = j; continue; }
        long long h = 0;
        while (j < text.size() && std::isdigit(static_cast<unsigned char>(text[j]))) {
            h = h * 10 + (text[j] - '0');
            ++j;
        }
        if (w > 0 && h > 0 && w <= 100000 && h <= 100000) {
            // Use the last WxH token.  `wm size` may print Physical and Override;
            // the later token is normally the effective one.
            bestW = static_cast<int>(w);
            bestH = static_cast<int>(h);
            found = true;
        }
        i = j;
    }
    if (found) { outW = bestW; outH = bestH; }
    return found;
}

static bool parse_rotation_value_at(const std::string& text, size_t pos, int& outRot) {
    while (pos < text.size() &&
           (text[pos] == ':' || text[pos] == '=' || text[pos] == ',' || text[pos] == ' ' ||
            text[pos] == '\t' || text[pos] == '\r' || text[pos] == '\n')) {
        ++pos;
    }
    if (pos >= text.size()) return false;

    // Common Android dumpsys forms:
    //   SurfaceOrientation: 1
    //   mRotation=ROTATION_90
    //   rotation 3
    //   Surface.ROTATION_270
    if (text.compare(pos, 9, "ROTATION_") == 0) {
        pos += 9;
        if (text.compare(pos, 3, "270") == 0) { outRot = 3; return true; }
        if (text.compare(pos, 3, "180") == 0) { outRot = 2; return true; }
        if (text.compare(pos, 2, "90") == 0)  { outRot = 1; return true; }
        if (text.compare(pos, 1, "0") == 0)   { outRot = 0; return true; }
    }
    const char* surf = "Surface.ROTATION_";
    const size_t surfLen = std::strlen(surf);
    if (text.compare(pos, surfLen, surf) == 0) {
        pos += surfLen;
        if (text.compare(pos, 3, "270") == 0) { outRot = 3; return true; }
        if (text.compare(pos, 3, "180") == 0) { outRot = 2; return true; }
        if (text.compare(pos, 2, "90") == 0)  { outRot = 1; return true; }
        if (text.compare(pos, 1, "0") == 0)   { outRot = 0; return true; }
    }
    if (std::isdigit(static_cast<unsigned char>(text[pos]))) {
        int r = text[pos] - '0';
        if (r >= 0 && r <= 3) { outRot = r; return true; }
    }
    return false;
}

static bool parse_rotation_after_key(const std::string& text, const char* key, int& outRot) {
    size_t pos = 0;
    while ((pos = text.find(key, pos)) != std::string::npos) {
        size_t p = pos + std::strlen(key);
        if (parse_rotation_value_at(text, p, outRot)) return true;
        pos = p;
    }
    return false;
}

static bool parse_first_rotation_value(const std::string& text, int& outRot) {
    for (size_t i = 0; i < text.size(); ++i) {
        if (parse_rotation_value_at(text, i, outRot)) return true;
    }
    return false;
}

static bool parse_surface_orientation(const std::string& text, int& outRot) {
    const char* keys[] = {
        "SurfaceOrientation",
        "surfaceOrientation",
        "mSurfaceOrientation",
        "mCurrentOrientation",
        "mCurrentRotation",
        "mDisplayRotation",
        "mRotation",
        "DisplayRotation",
        "rotation",
        "Rotation",
        "orientation",
        "Orientation",
    };
    for (const char* key : keys) {
        if (parse_rotation_after_key(text, key, outRot)) return true;
    }
    return false;
}

static bool detect_rotation_from_command(const char* cmd, const char* source, int& outRot, std::string& outSource) {
    const std::string text = run_shell_capture(cmd);
    if (text.empty()) return false;
    int r = -1;
    // dumpsys 输出里数字很多，必须优先按明确 key 解析；
    // 只有 settings/cmd 这类短输出才允许宽松解析第一个 0..3。
    bool ok = parse_surface_orientation(text, r);
    if (!ok) {
        const bool shortValue = text.size() < 32 || std::strstr(source, "settings") || std::strstr(source, "cmd-display");
        if (shortValue) ok = parse_first_rotation_value(text, r);
    }
    if (ok && r >= 0 && r <= 3) {
        outRot = r;
        outSource = source;
        return true;
    }
    return false;
}

static bool detect_current_rotation_best_effort(int& outRot, std::string& outSource) {
    // Try cheap/direct APIs first.  Different Android versions expose rotation
    // in different services, so keep several fallbacks.
    const struct { const char* cmd; const char* source; } probes[] = {
        // 先读当前显示/窗口实际旋转；settings user_rotation 在部分设备横屏时仍为 0，不能优先信任。
        {"dumpsys input 2>/dev/null", "dumpsys-input"},
        {"dumpsys display 2>/dev/null", "dumpsys-display"},
        {"dumpsys window displays 2>/dev/null", "dumpsys-window-displays"},
        {"dumpsys window 2>/dev/null", "dumpsys-window"},
        {"cmd display get-user-rotation 2>/dev/null", "cmd-display-user-rotation"},
        {"settings get system user_rotation 2>/dev/null", "settings-user_rotation"},
    };
    for (const auto& p : probes) {
        if (detect_rotation_from_command(p.cmd, p.source, outRot, outSource)) return true;
    }
    return false;
}

static void detect_display_config_best_effort() {
    int w = 0, h = 0;
    std::string wm = run_shell_capture("wm size 2>/dev/null");
    if (parse_first_size_token(wm, w, h)) {
        g_display.uiW = w;
        g_display.uiH = h;
        g_display.physW = w;
        g_display.physH = h;
    }

    int rot = -1;
    std::string rotSource;
    if (detect_current_rotation_best_effort(rot, rotSource)) {
        g_display.rotation = rot;
        g_display.rotationSource = rotSource;
        if (g_rotation < 0) g_rotation = rot;
    } else {
        g_display.rotation = -1;
        g_display.rotationSource = "undetected";
    }

    if (g_rotation < 0) g_rotation = 0;
}

static int normalized_rotation() {
    int r = g_rotation;
    if (r < 0) r = g_display.rotation;
    if (r < 0) r = 0;
    return r & 3;
}

static std::string input_device_name(int fd) {
    char name[256]{};
    if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) < 0) return "";
    return std::string(name);
}

static bool fd_has_abs_code(int fd, int code) {
    unsigned long bits[(ABS_MAX / static_cast<int>(sizeof(unsigned long) * 8)) + 2]{};
    if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(bits)), bits) < 0) return false;
    return bit_is_set(bits, code);
}

static bool fd_has_key_code(int fd, int code) {
    unsigned long bits[(KEY_MAX / static_cast<int>(sizeof(unsigned long) * 8)) + 2]{};
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(bits)), bits) < 0) return false;
    return bit_is_set(bits, code);
}

static bool fd_has_prop_direct(int fd) {
#ifdef EVIOCGPROP
    unsigned long bits[(INPUT_PROP_MAX / static_cast<int>(sizeof(unsigned long) * 8)) + 2]{};
    if (ioctl(fd, EVIOCGPROP(sizeof(bits)), bits) < 0) return false;
    return bit_is_set(bits, INPUT_PROP_DIRECT);
#else
    (void)fd;
    return false;
#endif
}

static int fd_abs_min(int fd, int code) {
    input_absinfo info{};
    if (ioctl(fd, EVIOCGABS(code), &info) < 0) return 0;
    return info.minimum;
}

static int fd_abs_max(int fd, int code) {
    input_absinfo info{};
    if (ioctl(fd, EVIOCGABS(code), &info) < 0) return -1;
    return info.maximum;
}

struct TouchCandidate {
    std::string path;
    std::string name;
    bool direct = false;
    bool hasMtX = false;
    bool hasMtY = false;
    bool hasAbsX = false;
    bool hasAbsY = false;
    bool hasSlot = false;
    bool hasTrackingId = false;
    bool hasBtnTouch = false;
    AxisRange range;
    int score = -100000;
};

static long long area_score(const AxisRange& r) {
    long long w = static_cast<long long>(r.maxX) - r.minX;
    long long h = static_cast<long long>(r.maxY) - r.minY;
    return (w > 0 && h > 0) ? w * h : 0;
}

static bool build_touch_candidate(int fd, const std::string& path, const std::string& name, TouchCandidate& out) {
    out = TouchCandidate{};
    out.path = path;
    out.name = name;
    const std::string lower = lower_copy(name);

    out.direct = fd_has_prop_direct(fd);
    out.hasMtX = fd_has_abs_code(fd, ABS_MT_POSITION_X);
    out.hasMtY = fd_has_abs_code(fd, ABS_MT_POSITION_Y);
    out.hasAbsX = fd_has_abs_code(fd, ABS_X);
    out.hasAbsY = fd_has_abs_code(fd, ABS_Y);
    out.hasSlot = fd_has_abs_code(fd, ABS_MT_SLOT);
    out.hasTrackingId = fd_has_abs_code(fd, ABS_MT_TRACKING_ID);
    out.hasBtnTouch = fd_has_key_code(fd, BTN_TOUCH);

    const bool hasMtCoords = out.hasMtX && out.hasMtY;
    const bool hasAbsCoords = out.hasAbsX && out.hasAbsY;
    if (!out.direct || (!hasMtCoords && !hasAbsCoords)) return false;

    // Avoid selecting our own previous virtual device or non-touch peripherals.
    if (contains_any(lower, {"virtual", "uinput", "pc-uinput", "oai-virtual"})) return false;
    if (contains_any(lower, {"mouse", "trackpad", "touchpad", "keyboard", "consumer control", "system control"})) return false;
    if (contains_any(lower, {"fingerprint", "pwrkey", "power", "haptic", "vibrator", "headset", "jack"})) return false;

    if (hasAbsCoords) {
        out.range.minX = fd_abs_min(fd, ABS_X);
        out.range.maxX = fd_abs_max(fd, ABS_X);
        out.range.minY = fd_abs_min(fd, ABS_Y);
        out.range.maxY = fd_abs_max(fd, ABS_Y);
    }
    if ((out.range.maxX <= out.range.minX || out.range.maxY <= out.range.minY) && hasMtCoords) {
        out.range.minX = fd_abs_min(fd, ABS_MT_POSITION_X);
        out.range.maxX = fd_abs_max(fd, ABS_MT_POSITION_X);
        out.range.minY = fd_abs_min(fd, ABS_MT_POSITION_Y);
        out.range.maxY = fd_abs_max(fd, ABS_MT_POSITION_Y);
    }
    if (out.range.maxX <= out.range.minX || out.range.maxY <= out.range.minY) return false;

    int score = 100;
    if (hasMtCoords) score += 24;
    if (hasAbsCoords) score += 12;
    if (out.hasTrackingId) score += 12;
    if (out.hasSlot) score += 8;
    if (out.hasBtnTouch) score += 8;
    score += 10;
    out.score = score;
    return true;
}

static bool detect_touch_range(AxisRange& outRange, std::string& outPath, std::string& outName) {
    std::vector<TouchCandidate> candidates;
    for (int i = 0; i <= kScanMaxEvent; ++i) {
        char path[64]{};
        std::snprintf(path, sizeof(path), "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;
        std::string name = input_device_name(fd);
        TouchCandidate c;
        if (build_touch_candidate(fd, path, name, c)) candidates.push_back(c);
        close(fd);
    }
    if (candidates.empty()) return false;
    std::sort(candidates.begin(), candidates.end(), [](const TouchCandidate& a, const TouchCandidate& b) {
        if (a.score != b.score) return a.score > b.score;
        const long long aa = area_score(a.range);
        const long long ba = area_score(b.range);
        if (aa != ba) return aa > ba;
        return a.path < b.path;
    });
    const TouchCandidate& best = candidates.front();
    outRange = best.range;
    outPath = best.path;
    outName = best.name;
    return true;
}

static bool emit_ev(int fd, int type, int code, int value) {
    input_event ev{};
    ev.type = type;
    ev.code = code;
    ev.value = value;
    return write(fd, &ev, sizeof(ev)) == static_cast<ssize_t>(sizeof(ev));
}

static int active_count() {
    int c = 0;
    for (const auto& s : g_slots) if (s.active) ++c;
    return c;
}

static bool emit_tool_state() {
    const int count = active_count();
    bool ok = true;
    ok = ok && emit_ev(g_uinputFd, EV_KEY, BTN_TOUCH, count > 0 ? 1 : 0);
    ok = ok && emit_ev(g_uinputFd, EV_KEY, BTN_TOOL_FINGER, count == 1 ? 1 : 0);
    ok = ok && emit_ev(g_uinputFd, EV_KEY, BTN_TOOL_DOUBLETAP, count == 2 ? 1 : 0);
    ok = ok && emit_ev(g_uinputFd, EV_KEY, BTN_TOOL_TRIPLETAP, count == 3 ? 1 : 0);
    ok = ok && emit_ev(g_uinputFd, EV_KEY, BTN_TOOL_QUADTAP, count >= 4 ? 1 : 0);
#ifdef BTN_TOOL_QUINTTAP
    ok = ok && emit_ev(g_uinputFd, EV_KEY, BTN_TOOL_QUINTTAP, count >= 5 ? 1 : 0);
#endif
    return ok;
}

static bool syn_report() {
    return emit_ev(g_uinputFd, EV_SYN, SYN_REPORT, 0);
}

static bool emit_slot_coords(int slot, int rawX, int rawY) {
    return emit_ev(g_uinputFd, EV_ABS, ABS_MT_SLOT, slot) &&
           emit_ev(g_uinputFd, EV_ABS, ABS_MT_POSITION_X, rawX) &&
           emit_ev(g_uinputFd, EV_ABS, ABS_MT_POSITION_Y, rawY) &&
           emit_ev(g_uinputFd, EV_ABS, ABS_X, rawX) &&
           emit_ev(g_uinputFd, EV_ABS, ABS_Y, rawY) &&
           emit_ev(g_uinputFd, EV_ABS, ABS_MT_TOUCH_MAJOR, 80) &&
           emit_ev(g_uinputFd, EV_ABS, ABS_MT_PRESSURE, 1);
}

static bool flush_touch() {
    return emit_tool_state() && syn_report();
}

static bool touch_down_nolock(int slot, int rawX, int rawY) {
    if (slot < 0 || slot >= kMaxSlots) return false;
    TouchSlot& s = g_slots[slot];
    if (!s.active) {
        s.active = true;
        s.trackingId = g_nextTrackingId++;
    }
    s.rawX = rawX;
    s.rawY = rawY;
    return emit_ev(g_uinputFd, EV_ABS, ABS_MT_SLOT, slot) &&
           emit_ev(g_uinputFd, EV_ABS, ABS_MT_TRACKING_ID, s.trackingId) &&
           emit_slot_coords(slot, rawX, rawY);
}

static bool touch_move_nolock(int slot, int rawX, int rawY) {
    if (slot < 0 || slot >= kMaxSlots) return false;
    TouchSlot& s = g_slots[slot];
    if (!s.active) {
        // For robustness, treat MOVE without DOWN as DOWN.
        return touch_down_nolock(slot, rawX, rawY);
    }
    s.rawX = rawX;
    s.rawY = rawY;
    return emit_ev(g_uinputFd, EV_ABS, ABS_MT_SLOT, slot) &&
           emit_ev(g_uinputFd, EV_ABS, ABS_MT_TRACKING_ID, s.trackingId) &&
           emit_slot_coords(slot, rawX, rawY);
}

static bool touch_up_nolock(int slot) {
    if (slot < 0 || slot >= kMaxSlots) return false;
    TouchSlot& s = g_slots[slot];
    if (!s.active) return true;
    s.active = false;
    s.trackingId = -1;
    return emit_ev(g_uinputFd, EV_ABS, ABS_MT_SLOT, slot) &&
           emit_ev(g_uinputFd, EV_ABS, ABS_MT_TRACKING_ID, -1);
}

static void reset_slot_range_nolock(int firstSlot, int lastExclusive) {
    firstSlot = clamp_int(firstSlot, 0, kMaxSlots);
    lastExclusive = clamp_int(lastExclusive, 0, kMaxSlots);
    for (int i = firstSlot; i < lastExclusive; ++i) {
        if (g_slots[i].active) touch_up_nolock(i);
    }
    flush_touch();
}

static void reset_pc_touches_safe() {
    std::lock_guard<std::mutex> lk(g_touchMutex);
    reset_slot_range_nolock(0, kPcMaxSlots);
}

static void reset_all_touches_safe() {
    std::lock_guard<std::mutex> lk(g_touchMutex);
    reset_slot_range_nolock(0, kMaxSlots);
}

static void reset_all_touches() {
    reset_all_touches_safe();
}

static float clamp_float(float v, float lo, float hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static void norm_to_raw_pair(int xNorm, int yNorm, int& rawX, int& rawY) {
    xNorm = clamp_int(xNorm, 0, PCU_NORM_MAX);
    yNorm = clamp_int(yNorm, 0, PCU_NORM_MAX);

    // Match the old display_transform.cpp path used by uinput_touchd2:
    //   norm -> UI px -> physical px -> rotation transform -> raw ABS range.
    // If display size detection fails, the math intentionally falls back to the
    // pure normalized transform, so old/current packets still work.
    const float uiW = static_cast<float>(std::max(1, g_display.uiW));
    const float uiH = static_cast<float>(std::max(1, g_display.uiH));
    const float physW = static_cast<float>(std::max(1, g_display.physW > 0 ? g_display.physW : g_display.uiW));
    const float physH = static_cast<float>(std::max(1, g_display.physH > 0 ? g_display.physH : g_display.uiH));

    const float xPx = (static_cast<float>(xNorm) / static_cast<float>(PCU_NORM_MAX)) * std::max(0.0f, uiW - 1.0f);
    const float yPx = (static_cast<float>(yNorm) / static_cast<float>(PCU_NORM_MAX)) * std::max(0.0f, uiH - 1.0f);

    const float scaledX = clamp_float(xPx, 0.0f, std::max(0.0f, uiW - 1.0f)) * physW / uiW;
    const float scaledY = clamp_float(yPx, 0.0f, std::max(0.0f, uiH - 1.0f)) * physH / uiH;

    float tx = scaledX;
    float ty = scaledY;
    float tw = physW;
    float th = physH;

    switch (normalized_rotation()) {
        case 1:
            tx = physH - scaledY;
            ty = scaledX;
            tw = physH;
            th = physW;
            break;
        case 2:
            tx = physW - scaledX;
            ty = physH - scaledY;
            tw = physW;
            th = physH;
            break;
        case 3:
            tx = scaledY;
            ty = physW - scaledX;
            tw = physH;
            th = physW;
            break;
        case 0:
        default:
            tx = scaledX;
            ty = scaledY;
            tw = physW;
            th = physH;
            break;
    }

    const int spanX = std::max(1, g_range.maxX - g_range.minX);
    const int spanY = std::max(1, g_range.maxY - g_range.minY);
    rawX = static_cast<int>(std::lround(static_cast<float>(g_range.minX) + tx * static_cast<float>(spanX) / std::max(1.0f, tw)));
    rawY = static_cast<int>(std::lround(static_cast<float>(g_range.minY) + ty * static_cast<float>(spanY) / std::max(1.0f, th)));
    rawX = clamp_int(rawX, g_range.minX, g_range.maxX);
    rawY = clamp_int(rawY, g_range.minY, g_range.maxY);
}


static void clear_mirror_finger(MirrorFingerSlot& s) {
    s.active = false;
    s.downSent = false;
    s.haveX = false;
    s.haveY = false;
    s.trackingId = -1;
    s.rawX = 0;
    s.rawY = 0;
}

static bool mirror_emit_down_or_move_nolock(int realSlot) {
    if (realSlot < 0 || realSlot >= kMirrorMaxSlots) return false;
    MirrorFingerSlot& f = g_mirrorFingers[realSlot];
    if (!f.active || !f.haveX || !f.haveY) return false;

    const int outSlot = kMirrorSlotBase + realSlot;
    if (!f.downSent) {
        f.downSent = true;
        return touch_down_nolock(outSlot, f.rawX, f.rawY);
    }
    return touch_move_nolock(outSlot, f.rawX, f.rawY);
}

static void mirror_release_nolock(int realSlot) {
    if (realSlot < 0 || realSlot >= kMirrorMaxSlots) return;
    MirrorFingerSlot& f = g_mirrorFingers[realSlot];
    if (f.downSent) {
        touch_up_nolock(kMirrorSlotBase + realSlot);
    }
    clear_mirror_finger(f);
}

static void mirror_release_all_nolock() {
    for (int i = 0; i < kMirrorMaxSlots; ++i) {
        mirror_release_nolock(i);
    }
    flush_touch();
}

static bool has_abs_slot(const std::string& path) {
    int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return false;
    const bool ok = fd_has_abs_code(fd, ABS_MT_SLOT);
    close(fd);
    return ok;
}

static void mirror_reader_loop(int fd, bool typeB) {
    std::fprintf(stderr, "pc_uinputd: MIRROR reader started input=%s name=%s type=%s slots=%d..%d\n",
                 g_selectedTouchPath.c_str(), g_selectedTouchName.c_str(), typeB ? "TypeB" : "TypeA-basic",
                 kMirrorSlotBase, kMaxSlots - 1);
    std::fflush(stderr);

    int currentSlot = 0;
    bool frameDirty = false;

    while (!g_stop && g_mirrorRunning.load()) {
        pollfd pfd{};
        pfd.fd = fd;
        pfd.events = POLLIN;
        const int pr = poll(&pfd, 1, 100);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pr == 0) continue;
        if (!(pfd.revents & POLLIN)) continue;

        input_event ev{};
        while (read(fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
            if (ev.type == EV_ABS) {
                if (typeB && ev.code == ABS_MT_SLOT) {
                    currentSlot = clamp_int(ev.value, 0, kMirrorMaxSlots - 1);
                } else if (ev.code == ABS_MT_TRACKING_ID) {
                    std::lock_guard<std::mutex> lk(g_touchMutex);
                    if (ev.value < 0) {
                        mirror_release_nolock(currentSlot);
                    } else {
                        MirrorFingerSlot& f = g_mirrorFingers[currentSlot];
                        if (f.downSent) mirror_release_nolock(currentSlot);
                        f.active = true;
                        f.trackingId = ev.value;
                    }
                    frameDirty = true;
                } else if (ev.code == ABS_MT_POSITION_X || (!typeB && ev.code == ABS_X)) {
                    std::lock_guard<std::mutex> lk(g_touchMutex);
                    MirrorFingerSlot& f = g_mirrorFingers[currentSlot];
                    f.rawX = clamp_int(ev.value, g_range.minX, g_range.maxX);
                    f.haveX = true;
                    if (!typeB) f.active = true;
                    if (mirror_emit_down_or_move_nolock(currentSlot)) frameDirty = true;
                } else if (ev.code == ABS_MT_POSITION_Y || (!typeB && ev.code == ABS_Y)) {
                    std::lock_guard<std::mutex> lk(g_touchMutex);
                    MirrorFingerSlot& f = g_mirrorFingers[currentSlot];
                    f.rawY = clamp_int(ev.value, g_range.minY, g_range.maxY);
                    f.haveY = true;
                    if (!typeB) f.active = true;
                    if (mirror_emit_down_or_move_nolock(currentSlot)) frameDirty = true;
                }
            } else if (ev.type == EV_KEY && ev.code == BTN_TOUCH && ev.value == 0 && !typeB) {
                std::lock_guard<std::mutex> lk(g_touchMutex);
                mirror_release_nolock(0);
                frameDirty = true;
            } else if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
                if (frameDirty) {
                    std::lock_guard<std::mutex> lk(g_touchMutex);
                    flush_touch();
                    frameDirty = false;
                }
            }
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            break;
        }
    }

    {
        std::lock_guard<std::mutex> lk(g_touchMutex);
        mirror_release_all_nolock();
    }
    std::fprintf(stderr, "pc_uinputd: MIRROR reader stopped\n");
    std::fflush(stderr);
}

static bool start_touch_mirror() {
    if (!g_mirrorEnabled.load()) {
        std::fprintf(stderr, "pc_uinputd: MIRROR disabled; physical touch input is not grabbed\n");
        std::fflush(stderr);
        return false;
    }
    if (g_selectedTouchPath.empty()) {
        std::fprintf(stderr, "pc_uinputd: MIRROR no selected physical touch input\n");
        std::fflush(stderr);
        return false;
    }
    if (g_mirrorRunning.load()) return true;

    int fd = open(g_selectedTouchPath.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        std::fprintf(stderr, "pc_uinputd: MIRROR open %s failed errno=%d\n", g_selectedTouchPath.c_str(), errno);
        std::fflush(stderr);
        return false;
    }

    if (ioctl(fd, EVIOCGRAB, 1) != 0) {
        std::fprintf(stderr, "pc_uinputd: MIRROR EVIOCGRAB failed input=%s errno=%d; real finger may still break PC mapping\n",
                     g_selectedTouchPath.c_str(), errno);
        std::fflush(stderr);
        close(fd);
        return false;
    }

    g_mirrorInputFd = fd;
    g_mirrorRunning = true;
    const bool typeB = has_abs_slot(g_selectedTouchPath);
    g_mirrorThread = std::thread(mirror_reader_loop, fd, typeB);
    std::fprintf(stderr, "pc_uinputd: MIRROR enabled input=%s[%s] grab=1 pcSlots=0..%d mirrorSlots=%d..%d\n",
                 g_selectedTouchPath.c_str(), g_selectedTouchName.c_str(), kPcMaxSlots - 1, kMirrorSlotBase, kMaxSlots - 1);
    std::fflush(stderr);
    return true;
}

static void stop_touch_mirror() {
    if (!g_mirrorRunning.exchange(false)) return;
    if (g_mirrorThread.joinable()) g_mirrorThread.join();
    if (g_mirrorInputFd >= 0) {
        ioctl(g_mirrorInputFd, EVIOCGRAB, 0);
        close(g_mirrorInputFd);
        g_mirrorInputFd = -1;
    }
    std::fprintf(stderr, "pc_uinputd: MIRROR disabled grab=0\n");
    std::fflush(stderr);
}

static bool setup_abs(int fd, int code, int minValue, int maxValue) {
    uinput_abs_setup abs{};
    abs.code = code;
    abs.absinfo.minimum = minValue;
    abs.absinfo.maximum = maxValue;
    return ioctl(fd, UI_ABS_SETUP, &abs) >= 0;
}

static int setup_uinput_device() {
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        std::perror("open /dev/uinput");
        return -1;
    }

    auto req = [&](unsigned long request, int value, const char* label) -> bool {
        if (ioctl(fd, request, value) < 0) {
            std::fprintf(stderr, "pc_uinputd: ioctl %s failed errno=%d\n", label, errno);
            return false;
        }
        return true;
    };

    if (!req(UI_SET_EVBIT, EV_SYN, "EV_SYN") ||
        !req(UI_SET_EVBIT, EV_KEY, "EV_KEY") ||
        !req(UI_SET_EVBIT, EV_ABS, "EV_ABS") ||
        !req(UI_SET_EVBIT, EV_REL, "EV_REL")) {
        close(fd);
        return -1;
    }

    const int keyBits[] = {
        BTN_TOUCH, BTN_TOOL_FINGER, BTN_TOOL_DOUBLETAP, BTN_TOOL_TRIPLETAP, BTN_TOOL_QUADTAP,
#ifdef BTN_TOOL_QUINTTAP
        BTN_TOOL_QUINTTAP,
#endif
        BTN_LEFT, BTN_RIGHT, BTN_MIDDLE, BTN_SIDE, BTN_EXTRA,
        KEY_ESC, KEY_TAB, KEY_BACKSPACE, KEY_ENTER, KEY_SPACE,
        KEY_LEFTSHIFT, KEY_RIGHTSHIFT, KEY_LEFTCTRL, KEY_RIGHTCTRL, KEY_LEFTALT, KEY_RIGHTALT,
        KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_HOME, KEY_END, KEY_PAGEUP, KEY_PAGEDOWN,
        KEY_0, KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9,
        KEY_A, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G, KEY_H, KEY_I, KEY_J, KEY_K, KEY_L, KEY_M,
        KEY_N, KEY_O, KEY_P, KEY_Q, KEY_R, KEY_S, KEY_T, KEY_U, KEY_V, KEY_W, KEY_X, KEY_Y, KEY_Z
    };
    for (int code : keyBits) {
        (void)ioctl(fd, UI_SET_KEYBIT, code);
    }

    (void)ioctl(fd, UI_SET_RELBIT, REL_WHEEL);
#ifdef REL_WHEEL_HI_RES
    (void)ioctl(fd, UI_SET_RELBIT, REL_WHEEL_HI_RES);
#endif

    if (ioctl(fd, UI_SET_PROPBIT, INPUT_PROP_DIRECT) < 0) {
        std::fprintf(stderr, "pc_uinputd: INPUT_PROP_DIRECT failed errno=%d\n", errno);
    }

    const int absBits[] = {ABS_X, ABS_Y, ABS_MT_SLOT, ABS_MT_TRACKING_ID, ABS_MT_POSITION_X, ABS_MT_POSITION_Y, ABS_MT_TOUCH_MAJOR, ABS_MT_PRESSURE};
    for (int code : absBits) {
        if (ioctl(fd, UI_SET_ABSBIT, code) < 0) {
            std::fprintf(stderr, "pc_uinputd: UI_SET_ABSBIT %d failed errno=%d\n", code, errno);
            close(fd);
            return -1;
        }
    }

    uinput_setup us{};
    std::snprintf(us.name, UINPUT_MAX_NAME_SIZE, "pc-uinput-touch");
    us.id.bustype = BUS_VIRTUAL;
    us.id.vendor = 0x484c;
    us.id.product = 0x0002;
    us.id.version = 1;
    if (ioctl(fd, UI_DEV_SETUP, &us) < 0) {
        std::perror("UI_DEV_SETUP");
        close(fd);
        return -1;
    }

    if (!setup_abs(fd, ABS_X, g_range.minX, g_range.maxX) ||
        !setup_abs(fd, ABS_Y, g_range.minY, g_range.maxY) ||
        !setup_abs(fd, ABS_MT_POSITION_X, g_range.minX, g_range.maxX) ||
        !setup_abs(fd, ABS_MT_POSITION_Y, g_range.minY, g_range.maxY) ||
        !setup_abs(fd, ABS_MT_SLOT, 0, kMaxSlots - 1) ||
        !setup_abs(fd, ABS_MT_TRACKING_ID, 0, 65535) ||
        !setup_abs(fd, ABS_MT_TOUCH_MAJOR, 0, 255) ||
        !setup_abs(fd, ABS_MT_PRESSURE, 0, 1)) {
        std::perror("UI_ABS_SETUP");
        close(fd);
        return -1;
    }

    if (ioctl(fd, UI_DEV_CREATE) < 0) {
        std::perror("UI_DEV_CREATE");
        close(fd);
        return -1;
    }
    usleep(250000);
    return fd;
}

static void destroy_uinput_device() {
    if (g_uinputFd >= 0) {
        reset_all_touches();
        ioctl(g_uinputFd, UI_DEV_DESTROY);
        close(g_uinputFd);
        g_uinputFd = -1;
    }
}

static bool read_all(int fd, void* dst, size_t size) {
    char* p = static_cast<char*>(dst);
    size_t got = 0;
    while (got < size) {
        ssize_t n = read(fd, p + got, size - got);
        if (n == 0) return false;
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        got += static_cast<size_t>(n);
    }
    return true;
}

static void handle_packet(const PcUinputPacket& p) {
    switch (p.type) {
        case PCU_HELLO:
        case PCU_PING:
            break;
        case PCU_CONFIG: {
            // V2：完整迁移旧 Android DISPLAYCFG 语义：
            // uiW/uiH 是当前 UI/投屏坐标系；physW/physH 是真实物理面板坐标系；rotation 是 Surface rotation。
            // 旧代码的最终路径就是：norm -> UI px -> ui_px_to_touch_px -> px_to_raw -> touch_*。
            if (p.b > 0 && p.c > 0) {
                g_display.uiW = p.b;
                g_display.uiH = p.c;
            }
            if (p.d > 0 && p.e > 0) {
                g_display.physW = p.d;
                g_display.physH = p.e;
            }

            if (p.a >= 0 && p.a <= 3) {
                g_rotation = p.a;
                g_display.rotation = p.a;
                g_display.rotationSource = "pc-forced";
            } else {
                int rot = -1;
                std::string src;
                if (detect_current_rotation_best_effort(rot, src)) {
                    g_rotation = rot;
                    g_display.rotation = rot;
                    g_display.rotationSource = src;
                }
            }

            std::fprintf(stderr,
                         "pc_uinputd: CONFIG ui=%dx%d phys=%dx%d raw=(%d..%d,%d..%d) rotation=%d source=%s transform=old-display_transform\n",
                         g_display.uiW, g_display.uiH, g_display.physW, g_display.physH,
                         g_range.minX, g_range.maxX, g_range.minY, g_range.maxY,
                         normalized_rotation(), g_display.rotationSource.c_str());
            std::fflush(stderr);
            break;
        }
        case PCU_TOUCH_DOWN: {
            const int slot = p.a;
            if (slot < 0 || slot >= kPcMaxSlots) break;
            int rawX = 0, rawY = 0;
            norm_to_raw_pair(p.b, p.c, rawX, rawY);
            {
                std::lock_guard<std::mutex> lk(g_touchMutex);
                touch_down_nolock(slot, rawX, rawY);
                flush_touch();
            }
            break;
        }
        case PCU_TOUCH_MOVE: {
            const int slot = p.a;
            if (slot < 0 || slot >= kPcMaxSlots) break;
            int rawX = 0, rawY = 0;
            norm_to_raw_pair(p.b, p.c, rawX, rawY);
            {
                std::lock_guard<std::mutex> lk(g_touchMutex);
                touch_move_nolock(slot, rawX, rawY);
                flush_touch();
            }
            break;
        }
        case PCU_TOUCH_UP: {
            const int slot = p.a;
            if (slot < 0 || slot >= kPcMaxSlots) break;
            {
                std::lock_guard<std::mutex> lk(g_touchMutex);
                touch_up_nolock(slot);
                flush_touch();
            }
            break;
        }
        case PCU_KEY: {
            if (p.a > 0 && p.a < KEY_MAX) {
                std::lock_guard<std::mutex> lk(g_touchMutex);
                emit_ev(g_uinputFd, EV_KEY, p.a, p.b ? 1 : 0);
                syn_report();
            }
            break;
        }
        case PCU_WHEEL: {
            if (p.a != 0) {
                std::lock_guard<std::mutex> lk(g_touchMutex);
                emit_ev(g_uinputFd, EV_REL, REL_WHEEL, p.a);
#ifdef REL_WHEEL_HI_RES
                emit_ev(g_uinputFd, EV_REL, REL_WHEEL_HI_RES, p.a * 120);
#endif
                syn_report();
            }
            break;
        }
        case PCU_RESET:
            reset_pc_touches_safe();
            break;
        case PCU_EXIT:
            reset_all_touches_safe();
            g_stop = 1;
            break;
        default:
            break;
    }
}

static int create_abstract_server(const std::string& name) {
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s < 0) {
        std::perror("socket");
        return -1;
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    addr.sun_path[0] = '\0';
    const size_t maxName = sizeof(addr.sun_path) - 2;
    const size_t n = std::min(maxName, name.size());
    std::memcpy(addr.sun_path + 1, name.data(), n);
    const socklen_t len = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + 1 + n);

    if (bind(s, reinterpret_cast<sockaddr*>(&addr), len) < 0) {
        std::perror("bind abstract socket");
        close(s);
        return -1;
    }
    if (listen(s, 1) < 0) {
        std::perror("listen");
        close(s);
        return -1;
    }
    return s;
}

static void serve_loop(const std::string& socketName) {
    int server = create_abstract_server(socketName);
    if (server < 0) return;

    std::fprintf(stderr, "pc_uinputd: READY socket=%s display=%dx%d phys=%dx%d raw=(%d..%d,%d..%d) touch=%s[%s] rotation=%d detectedRotation=%d rotationSource=%s mirror=%d pcSlots=0..%d mirrorSlots=%d..%d\n",
                 socketName.c_str(), g_display.uiW, g_display.uiH, g_display.physW, g_display.physH,
                 g_range.minX, g_range.maxX, g_range.minY, g_range.maxY,
                 g_selectedTouchPath.c_str(), g_selectedTouchName.c_str(), normalized_rotation(), g_display.rotation,
                 g_display.rotationSource.c_str(), g_mirrorRunning.load() ? 1 : 0, kPcMaxSlots - 1, kMirrorSlotBase, kMaxSlots - 1);
    std::fflush(stderr);

    while (!g_stop) {
        int client = accept(server, nullptr, nullptr);
        if (client < 0) {
            if (errno == EINTR) continue;
            break;
        }

        while (!g_stop) {
            PcUinputPacket p{};
            if (!read_all(client, &p, sizeof(p))) break;
            if (p.magic[0] != PCU_MAGIC0 || p.magic[1] != PCU_MAGIC1 || p.version != PCU_VERSION) {
                break;
            }
            handle_packet(p);
        }
        reset_pc_touches_safe();
        close(client);
    }
    close(server);
}

} // namespace

int main(int argc, char** argv) {
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
#ifdef SIGPIPE
    signal(SIGPIPE, SIG_IGN);
#endif

    std::string socketName = kDefaultSocketName;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i] ? argv[i] : "";
        if ((a == "--socket" || a == "-s") && i + 1 < argc) {
            socketName = argv[++i];
        } else if ((a == "--rotation" || a == "-r") && i + 1 < argc) {
            int r = std::atoi(argv[++i]);
            g_rotation = (r >= 0 && r <= 3) ? r : -1;
        } else if (a == "--no-mirror") {
            g_mirrorEnabled = false;
        } else if (a == "--mirror" && i + 1 < argc) {
            g_mirrorEnabled = std::atoi(argv[++i]) != 0;
        }
    }

    detect_display_config_best_effort();

    if (!detect_touch_range(g_range, g_selectedTouchPath, g_selectedTouchName)) {
        std::fprintf(stderr, "pc_uinputd: touch probe failed, fallback raw old-default 0..19040,0..30400\n");
        g_range = AxisRange{};
    }

    g_uinputFd = setup_uinput_device();
    if (g_uinputFd < 0) return 1;

    start_touch_mirror();

    serve_loop(socketName);
    stop_touch_mirror();
    destroy_uinput_device();
    return 0;
}
