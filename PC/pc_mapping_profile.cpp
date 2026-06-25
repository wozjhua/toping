#include "pc_mapping_profile.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace {
static std::string wideToUtf8Loose(const std::wstring& ws) {
    if (ws.empty()) return std::string();
    int need = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (need <= 0) return std::string();
    std::string out(static_cast<size_t>(need - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, out.data(), need, nullptr, nullptr);
    return out;
}

static std::wstring utf8ToWideLoose(const std::string& s) {
    if (s.empty()) return std::wstring();
    int need = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (need <= 0) return std::wstring();
    std::wstring out(static_cast<size_t>(need - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), need);
    return out;
}

static std::string comboToString(const std::vector<int>& codes) {
    std::string out;
    for (size_t i = 0; i < codes.size(); ++i) {
        if (i) out += ",";
        out += std::to_string(codes[i]);
    }
    return out;
}

static std::vector<int> comboFromString(const std::string& s) {
    std::vector<int> out;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ',')) {
        if (item.empty()) continue;
        int v = std::atoi(item.c_str());
        if (v > 0 && std::find(out.begin(), out.end(), v) == out.end()) out.push_back(v);
    }
    return out;
}

static int clampNormLocal(int v) {
    return (std::max)(0, (std::min)(1000000, v));
}

static double clampRelativeSpeed(double v) {
    return (std::max)(0.1, (std::min)(1.2, v));
}

static int clampItemWheelDistance(int v) { return (std::max)(PC_MENU_ITEM_WHEEL_DISTANCE_MIN, (std::min)(PC_MENU_ITEM_WHEEL_DISTANCE_MAX, v)); }
static int clampItemWheelStep(int v) { return (std::max)(PC_MENU_ITEM_WHEEL_STEP_MIN, (std::min)(PC_MENU_ITEM_WHEEL_STEP_MAX, v)); }
static int clampItemWheelSpeed(int v) { return (std::max)(PC_MENU_ITEM_WHEEL_SPEED_MIN, (std::min)(PC_MENU_ITEM_WHEEL_SPEED_MAX, v)); }
static int clampMenuButtonRadiusForProfile(int v) { return PcMappingProfile::ClampButtonRadiusNorm(v); }
static int clampMenuPlacement(int v) { return (v >= 1 && v <= 4) ? v : static_cast<int>(PcMenuTriggerPlacement::Bottom); }
static int clampItemLandingZone(int v) { return (v >= 1 && v <= 4) ? v : static_cast<int>(PcItemLandingZone::Bottom); }

static double normalizeLegacyRelativeSpeed(double v, int mapVersion) {
    // HLMAP12 及之前的“速度”是 norm/RawInput 像素，典型值 1150/1600。
    // HLMAP13 起统一改为客户端像素倍率 0.4~1.2。旧值过大时回到默认 0.5，避免升级后仍然过快。
    if (mapVersion > 0 && mapVersion < 13 && v > 10.0) return 0.5;
    return clampRelativeSpeed(v);
}

static bool validMenuRange(int l, int t, int r, int b) {
    return l >= 0 && t >= 0 && r >= 0 && b >= 0 && r > l && b > t;
}

static void defaultMenuRange(const PcMenuBinding& m, int& l, int& t, int& r, int& b) {
    if (m.category == PcMenuCategory::MenuRadial) {
        const int cx = clampNormLocal(m.centerXNorm);
        const int cy = clampNormLocal(m.centerYNorm);
        const int radius = clampMenuButtonRadiusForProfile(m.radiusNorm);
        l = clampNormLocal(cx - radius);
        t = clampNormLocal(cy - radius);
        r = clampNormLocal(cx + radius);
        b = clampNormLocal(cy + radius);
        if (r <= l) r = clampNormLocal(l + 1);
        if (b <= t) b = clampNormLocal(t + 1);
    } else {
        l = 0; t = 0; r = 1000000; b = 1000000;
    }
}

static void normalizeMenuRange(PcMenuBinding& m) {
    int l = m.rangeLeftNorm;
    int t = m.rangeTopNorm;
    int r = m.rangeRightNorm;
    int b = m.rangeBottomNorm;
    if (!validMenuRange(l, t, r, b)) {
        defaultMenuRange(m, l, t, r, b);
    } else {
        l = clampNormLocal(l);
        t = clampNormLocal(t);
        r = clampNormLocal(r);
        b = clampNormLocal(b);
        if (r <= l) r = clampNormLocal(l + 1);
        if (b <= t) b = clampNormLocal(t + 1);
    }

    // 轮盘的中心必须在范围内，否则运行时按圆形半径投影会出现异常。
    if (m.category == PcMenuCategory::MenuRadial) {
        const int cx = clampNormLocal(m.centerXNorm);
        const int cy = clampNormLocal(m.centerYNorm);
        l = (std::min)(l, cx);
        r = (std::max)(r, cx);
        t = (std::min)(t, cy);
        b = (std::max)(b, cy);
    }

    m.rangeLeftNorm = l;
    m.rangeTopNorm = t;
    m.rangeRightNorm = r;
    m.rangeBottomNorm = b;
}

static void menuRangeForWrite(const PcMenuBinding& m, int& l, int& t, int& r, int& b) {
    PcMenuBinding copy = m;
    normalizeMenuRange(copy);
    l = copy.rangeLeftNorm;
    t = copy.rangeTopNorm;
    r = copy.rangeRightNorm;
    b = copy.rangeBottomNorm;
}
}

void PcMappingProfile::Clear() {
    bindings.clear();
    locks.clear();
    compasses.clear();
    menus.clear();
}

bool PcMappingProfile::Empty() const {
    return bindings.empty() && locks.empty() && compasses.empty() && menus.empty();
}

const PcMappingBinding* PcMappingProfile::FindBinding(PcMappingTriggerType type, int code) const {
    for (const auto& b : bindings) {
        if (b.actionType == PcMappingActionType::None) continue;
        if (b.triggerType != type) continue;
        if (!b.comboTriggerCodes.empty()) {
            if (std::find(b.comboTriggerCodes.begin(), b.comboTriggerCodes.end(), code) != b.comboTriggerCodes.end()) {
                return &b;
            }
        } else if (b.triggerCode == code) {
            return &b;
        }
    }
    return nullptr;
}

PcMappingProfile PcMappingProfile::EmptyProfile() {
    PcMappingProfile p;
    p.name = "default";
    return p;
}

int PcMappingProfile::ClampNorm(int v) {
    return std::max(0, std::min(1000000, v));
}

int PcMappingProfile::ClampRadiusNorm(int v) {
    return std::max(8000, std::min(PC_COMPASS_OUTER_RADIUS_MAX_NORM, v));
}


int PcMappingProfile::ClampButtonRadiusNorm(int v) {
    return std::max(PC_BUTTON_RADIUS_MIN_NORM, std::min(PC_BUTTON_RADIUS_MAX_NORM, v));
}

static int ClampCompassInnerRadiusNormLocal(int v) {
    return std::max(30000, std::min(140000, v));
}

static int ClampCompassOuterRadiusNormLocal(int v) {
    return std::max(PC_COMPASS_OUTER_RADIUS_MIN_NORM, std::min(PC_COMPASS_OUTER_RADIUS_MAX_NORM, v));
}

static void NormalizeCompassRadiiLocal(int& innerRadiusNorm, int& outerRadiusNorm) {
    innerRadiusNorm = ClampCompassInnerRadiusNormLocal(innerRadiusNorm);
    outerRadiusNorm = ClampCompassOuterRadiusNormLocal(outerRadiusNorm);
    static constexpr int kMinGap = 10000;
    if (outerRadiusNorm < innerRadiusNorm + kMinGap) {
        outerRadiusNorm = std::min(PC_COMPASS_OUTER_RADIUS_MAX_NORM, innerRadiusNorm + kMinGap);
        if (outerRadiusNorm < innerRadiusNorm + kMinGap) {
            innerRadiusNorm = std::max(30000, outerRadiusNorm - kMinGap);
        }
    }
}

bool PcMappingProfile::SaveToFile(const std::wstring& path) const {
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, path.c_str(), L"wb") != 0 || !fp) return false;
    std::fprintf(fp, "HLMAP16\n");
    std::fprintf(fp, "name %s\n", name.c_str());
    for (const auto& b : bindings) {
        const std::string combo = comboToString(b.comboTriggerCodes);
        const std::string label = wideToUtf8Loose(b.triggerLabel);
        std::fprintf(
            fp,
            "binding %s %d %d %d %d %d %d %d %d %d %d %d %d %d %d %s %s\n",
            b.id.c_str(),
            static_cast<int>(b.triggerType),
            b.triggerCode,
            static_cast<int>(b.actionType),
            b.slot,
            ClampNorm(b.xNorm),
            ClampNorm(b.yNorm),
            b.linuxKeyCode,
            b.consumeEvent ? 1 : 0,
            ClampButtonRadiusNorm(b.radiusNorm),
            static_cast<int>(b.touchMode),
            static_cast<int>(b.specialAction),
            b.randomRadiusNorm,
            b.randomMoveRadiusNorm,
            b.randomMoveIntervalMs,
            combo.empty() ? "-" : combo.c_str(),
            label.empty() ? "-" : label.c_str()
        );
    }
    for (const auto& l : locks) {
        const std::string combo = comboToString(l.comboTriggerCodes);
        const std::string label = wideToUtf8Loose(l.triggerLabel);
        std::fprintf(
            fp,
            "lock %s %d %d %d %d %d %d %d %.3f %.3f %d %d %d %d %s %s\n",
            l.id.c_str(),
            static_cast<int>(l.mode),
            ClampNorm(l.centerXNorm), ClampNorm(l.centerYNorm),
            ClampNorm(l.leftNorm), ClampNorm(l.topNorm), ClampNorm(l.rightNorm), ClampNorm(l.bottomNorm),
            clampRelativeSpeed(l.speedXNorm), clampRelativeSpeed(l.speedYNorm),
            l.rebuildDownDelayMs,
            l.primarySlot,
            l.auxSlot,
            0,
            combo.empty() ? "-" : combo.c_str(),
            label.empty() ? "-" : label.c_str()
        );
    }
    for (const auto& c : compasses) {
        const std::string upCombo = comboToString(c.up.comboTriggerCodes);
        const std::string leftCombo = comboToString(c.left.comboTriggerCodes);
        const std::string downCombo = comboToString(c.down.comboTriggerCodes);
        const std::string rightCombo = comboToString(c.right.comboTriggerCodes);
        const std::string centerCombo = comboToString(c.center.comboTriggerCodes);
        const std::string upLabel = wideToUtf8Loose(c.up.triggerLabel);
        const std::string leftLabel = wideToUtf8Loose(c.left.triggerLabel);
        const std::string downLabel = wideToUtf8Loose(c.down.triggerLabel);
        const std::string rightLabel = wideToUtf8Loose(c.right.triggerLabel);
        const std::string centerLabel = wideToUtf8Loose(c.center.triggerLabel);
        int saveInnerRadius = c.innerRadiusNorm;
        int saveOuterRadius = c.outerRadiusNorm;
        NormalizeCompassRadiiLocal(saveInnerRadius, saveOuterRadius);
        std::fprintf(
            fp,
            "compass %s %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %s %s %s %s %s %s %s %s %s %s %d %d %d %d %d %d %d %d\n",
            c.id.c_str(),
            ClampNorm(c.centerXNorm), ClampNorm(c.centerYNorm),
            saveInnerRadius, saveOuterRadius,
            c.speedXStep, c.speedYStep, c.slot,
            static_cast<int>(c.motionMode),
            c.centerOuterReversed ? 1 : 0,
            (std::max)(60, (std::min)(960, c.touchSampleHz)),
            (std::max)(10, (std::min)(150, c.swaySensitivity)),
            (std::max)(25, (std::min)(140, c.swaySectorPercent)),
            (std::max)(25, (std::min)(90, c.swayDiagonalSectorPercent)),
            (std::max)(25, (std::min)(90, c.swayDiagLeftUpSectorPercent)),
            (std::max)(25, (std::min)(90, c.swayDiagRightUpSectorPercent)),
            (std::max)(25, (std::min)(90, c.swayDiagLeftDownSectorPercent)),
            (std::max)(25, (std::min)(90, c.swayDiagRightDownSectorPercent)),
            (std::max)(5, (std::min)(95, c.swayStepMinPercent)),
            (std::max)(6, (std::min)(100, c.swayStepMaxPercent)),
            (std::max)(2, (std::min)(20, c.swaySpeedPercent)),
            c.swayMouseButtonSmallStep ? 1 : 0,
            (std::max)(5, (std::min)(80, c.swayMouseButtonStepPercent)),
            (std::max)(5, (std::min)(50, c.swayMouseButtonUpdatePercent)),
            (std::max)(1000, (std::min)(8000, c.swayMouseButtonHoldMs)),
            0,
            upCombo.empty() ? "-" : upCombo.c_str(),
            leftCombo.empty() ? "-" : leftCombo.c_str(),
            downCombo.empty() ? "-" : downCombo.c_str(),
            rightCombo.empty() ? "-" : rightCombo.c_str(),
            centerCombo.empty() ? "-" : centerCombo.c_str(),
            upLabel.empty() ? "-" : upLabel.c_str(),
            leftLabel.empty() ? "-" : leftLabel.c_str(),
            downLabel.empty() ? "-" : downLabel.c_str(),
            rightLabel.empty() ? "-" : rightLabel.c_str(),
            centerLabel.empty() ? "-" : centerLabel.c_str(),
            c.diagLeftUpXNorm, c.diagLeftUpYNorm, c.diagRightUpXNorm, c.diagRightUpYNorm,
            c.diagLeftDownXNorm, c.diagLeftDownYNorm, c.diagRightDownXNorm, c.diagRightDownYNorm
        );
    }


    for (const auto& m : menus) {
        const std::string combo = comboToString(m.comboTriggerCodes);
        const std::string label = wideToUtf8Loose(m.triggerLabel);
        int rangeLeft = 0, rangeTop = 0, rangeRight = 1000000, rangeBottom = 1000000;
        menuRangeForWrite(m, rangeLeft, rangeTop, rangeRight, rangeBottom);
        std::fprintf(
            fp,
            "menu %s %d %d %d %d %d %d %d %d %d %d %d %d %d %s %s %d %d %d %d %.3f %.3f %d %d %d %d %d %d %d %d %d %d %d\n",
            m.id.c_str(),
            static_cast<int>(m.category),
            static_cast<int>(m.shapeType),
            static_cast<int>(m.triggerMode),
            m.enabled ? 1 : 0,
            ClampNorm(m.centerXNorm), ClampNorm(m.centerYNorm),
            clampMenuButtonRadiusForProfile(m.radiusNorm),
            (std::max)(1, (std::min)(15, m.slot)),
            (std::max)(2, (std::min)(12, m.segmentCount)),
            m.visualHintEnabled ? 1 : 0,
            ClampNorm(m.freeCursorXNorm), ClampNorm(m.freeCursorYNorm),
            (std::max)(200, (std::min)(8000, m.freeCursorSpeedNorm)),
            combo.empty() ? "-" : combo.c_str(),
            label.empty() ? "-" : label.c_str(),
            rangeLeft, rangeTop, rangeRight, rangeBottom,
            clampRelativeSpeed(m.relativeSpeedX), clampRelativeSpeed(m.relativeSpeedY),
            clampMenuPlacement(static_cast<int>(m.triggerPlacement)),
            clampItemLandingZone(static_cast<int>(m.itemLandingZone)),
            m.itemWheelScrollEnabled ? 1 : 0,
            m.itemWheelInvert ? 1 : 0,
            clampItemWheelSpeed(m.itemWheelScrollSpeed),
            clampItemWheelDistance(m.itemWheelMaxDistancePx),
            clampItemWheelStep(m.itemWheelStepPx),
            ClampNorm(m.triggerXNorm), ClampNorm(m.triggerYNorm),
            ClampNorm(m.closeXNorm), ClampNorm(m.closeYNorm)
        );
    }

    std::fclose(fp);
    return true;
}

bool PcMappingProfile::LoadFromFile(const std::wstring& path, std::string* error) {
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, path.c_str(), L"rb") != 0 || !fp) {
        if (error) *error = "open failed";
        return false;
    }

    Clear();
    char line[2048];
    bool seenHeader = false;
    int mapVersion = 0;
    while (std::fgets(line, sizeof(line), fp)) {
        std::string s(line);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
        if (s.empty() || s[0] == '#') continue;

        std::istringstream iss(s);
        std::string tag;
        iss >> tag;
        if (tag == "HLMAP1" || tag == "HLMAP2" || tag == "HLMAP3" || tag == "HLMAP4" || tag == "HLMAP5" || tag == "HLMAP6" || tag == "HLMAP7" || tag == "HLMAP8" || tag == "HLMAP9" || tag == "HLMAP10" || tag == "HLMAP11" || tag == "HLMAP12" || tag == "HLMAP13" || tag == "HLMAP14" || tag == "HLMAP15" || tag == "HLMAP16") {
            seenHeader = true;
            mapVersion = (tag.size() >= 6) ? std::atoi(tag.c_str() + 5) : 0;
            continue;
        }
        if (tag == "name") {
            iss >> name;
            continue;
        }

        if (tag == "lock") {
            PcLockBinding l;
            int mode = 3;
            std::string combo;
            std::string label;
            int reserved = 0;
            iss >> l.id >> mode >> l.centerXNorm >> l.centerYNorm >> l.leftNorm >> l.topNorm >> l.rightNorm >> l.bottomNorm
                >> l.speedXNorm >> l.speedYNorm >> l.rebuildDownDelayMs >> l.primarySlot >> l.auxSlot >> reserved;
            if (!iss.fail()) {
                if (iss >> combo) {
                    if (combo != "-") l.comboTriggerCodes = comboFromString(combo);
                }
                if (iss >> label) {
                    if (label != "-") l.triggerLabel = utf8ToWideLoose(label);
                }
                if (mode < 1 || mode > 3) mode = 3;
                l.mode = static_cast<PcLockSlideTouchMode>(mode);
                l.centerXNorm = ClampNorm(l.centerXNorm);
                l.centerYNorm = ClampNorm(l.centerYNorm);
                l.leftNorm = ClampNorm(l.leftNorm);
                l.topNorm = ClampNorm(l.topNorm);
                l.rightNorm = ClampNorm(l.rightNorm);
                l.bottomNorm = ClampNorm(l.bottomNorm);
                if (l.leftNorm > l.rightNorm) std::swap(l.leftNorm, l.rightNorm);
                if (l.topNorm > l.bottomNorm) std::swap(l.topNorm, l.bottomNorm);
                l.speedXNorm = normalizeLegacyRelativeSpeed(l.speedXNorm, mapVersion);
                l.speedYNorm = normalizeLegacyRelativeSpeed(l.speedYNorm, mapVersion);
                locks.push_back(l);
            }
            continue;
        }
        if (tag == "compass") {
            PcCompassBinding c;
            int motion = static_cast<int>(PcCompassMotionMode::FixedCenter);
            int reversed = 0;
            iss >> c.id >> c.centerXNorm >> c.centerYNorm >> c.innerRadiusNorm >> c.outerRadiusNorm
                >> c.speedXStep >> c.speedYStep >> c.slot >> motion >> reversed;
            if (!iss.fail()) {
                std::vector<std::string> rest;
                std::string tok;
                while (iss >> tok) rest.push_back(tok);

                size_t pos = 0;
                auto parsePureInt = [](const std::string& t, int& out) -> bool {
                    if (t.empty()) return false;
                    char* endp = nullptr;
                    long v = std::strtol(t.c_str(), &endp, 10);
                    if (!endp || *endp != '\0') return false;
                    out = static_cast<int>(v);
                    return true;
                };
                auto takePrefixInt = [&](int fallback) -> int {
                    if (pos >= rest.size()) return fallback;
                    int v = fallback;
                    if (parsePureInt(rest[pos], v)) { ++pos; return v; }
                    return fallback;
                };
                // 根据 header 版本读取固定数量的前置参数，避免把单键 combo（如 "87"）误当参数吞掉。
                if (mapVersion >= 5) {
                    c.touchSampleHz = takePrefixInt(c.touchSampleHz);
                    c.swaySensitivity = takePrefixInt(c.swaySensitivity);
                }
                if (mapVersion >= 6) {
                    if (mapVersion >= 7) {
                        c.swaySectorPercent = takePrefixInt(c.swaySectorPercent);
                    }
                    if (mapVersion >= 8) {
                        c.swayDiagonalSectorPercent = takePrefixInt(c.swayDiagonalSectorPercent);
                    }
                    if (mapVersion >= 9) {
                        c.swayDiagLeftUpSectorPercent = takePrefixInt(c.swayDiagLeftUpSectorPercent);
                        c.swayDiagRightUpSectorPercent = takePrefixInt(c.swayDiagRightUpSectorPercent);
                        c.swayDiagLeftDownSectorPercent = takePrefixInt(c.swayDiagLeftDownSectorPercent);
                        c.swayDiagRightDownSectorPercent = takePrefixInt(c.swayDiagRightDownSectorPercent);
                    } else {
                        c.swayDiagLeftUpSectorPercent = c.swayDiagonalSectorPercent;
                        c.swayDiagRightUpSectorPercent = c.swayDiagonalSectorPercent;
                        c.swayDiagLeftDownSectorPercent = c.swayDiagonalSectorPercent;
                        c.swayDiagRightDownSectorPercent = c.swayDiagonalSectorPercent;
                    }
                    c.swayStepMinPercent = takePrefixInt(c.swayStepMinPercent);
                    c.swayStepMaxPercent = takePrefixInt(c.swayStepMaxPercent);
                    c.swaySpeedPercent = takePrefixInt(c.swaySpeedPercent);
                    c.swayMouseButtonSmallStep = takePrefixInt(c.swayMouseButtonSmallStep ? 1 : 0) != 0;
                    c.swayMouseButtonStepPercent = takePrefixInt(c.swayMouseButtonStepPercent);
                    c.swayMouseButtonUpdatePercent = takePrefixInt(c.swayMouseButtonUpdatePercent);
                    if (mapVersion >= 10) {
                        c.swayMouseButtonHoldMs = takePrefixInt(c.swayMouseButtonHoldMs);
                    }
                    (void)takePrefixInt(0); // reserved
                }

                const auto take = [&]() -> std::string {
                    if (pos >= rest.size()) return std::string("-");
                    return rest[pos++];
                };
                const std::string upCombo = take();
                const std::string leftCombo = take();
                const std::string downCombo = take();
                const std::string rightCombo = take();
                const std::string centerCombo = take();
                const std::string upLabel = take();
                const std::string leftLabel = take();
                const std::string downLabel = take();
                const std::string rightLabel = take();
                const std::string centerLabel = take();

                if (upCombo != "-") c.up.comboTriggerCodes = comboFromString(upCombo);
                if (leftCombo != "-") c.left.comboTriggerCodes = comboFromString(leftCombo);
                if (downCombo != "-") c.down.comboTriggerCodes = comboFromString(downCombo);
                if (rightCombo != "-") c.right.comboTriggerCodes = comboFromString(rightCombo);
                if (centerCombo != "-") c.center.comboTriggerCodes = comboFromString(centerCombo);
                if (upLabel != "-") c.up.triggerLabel = utf8ToWideLoose(upLabel);
                if (leftLabel != "-") c.left.triggerLabel = utf8ToWideLoose(leftLabel);
                if (downLabel != "-") c.down.triggerLabel = utf8ToWideLoose(downLabel);
                if (rightLabel != "-") c.right.triggerLabel = utf8ToWideLoose(rightLabel);
                if (centerLabel != "-") c.center.triggerLabel = utf8ToWideLoose(centerLabel);

                auto takeInt = [&](int fallback) -> int {
                    if (pos >= rest.size()) return fallback;
                    char* endp = nullptr;
                    long v = std::strtol(rest[pos++].c_str(), &endp, 10);
                    return (endp && *endp == '\0') ? static_cast<int>(v) : fallback;
                };
                c.diagLeftUpXNorm = takeInt(c.diagLeftUpXNorm);
                c.diagLeftUpYNorm = takeInt(c.diagLeftUpYNorm);
                c.diagRightUpXNorm = takeInt(c.diagRightUpXNorm);
                c.diagRightUpYNorm = takeInt(c.diagRightUpYNorm);
                c.diagLeftDownXNorm = takeInt(c.diagLeftDownXNorm);
                c.diagLeftDownYNorm = takeInt(c.diagLeftDownYNorm);
                c.diagRightDownXNorm = takeInt(c.diagRightDownXNorm);
                c.diagRightDownYNorm = takeInt(c.diagRightDownYNorm);

                c.centerXNorm = ClampNorm(c.centerXNorm);
                c.centerYNorm = ClampNorm(c.centerYNorm);
                NormalizeCompassRadiiLocal(c.innerRadiusNorm, c.outerRadiusNorm);
                c.speedXStep = (std::max)(1, (std::min)(30, c.speedXStep));
                c.speedYStep = (std::max)(1, (std::min)(30, c.speedYStep));
                c.touchSampleHz = (std::max)(60, (std::min)(960, c.touchSampleHz));
                c.swaySensitivity = (std::max)(10, (std::min)(150, c.swaySensitivity));
                c.swaySectorPercent = (std::max)(25, (std::min)(140, c.swaySectorPercent));
                c.swayDiagonalSectorPercent = (std::max)(25, (std::min)(90, c.swayDiagonalSectorPercent));
                c.swayDiagLeftUpSectorPercent = (std::max)(25, (std::min)(90, c.swayDiagLeftUpSectorPercent));
                c.swayDiagRightUpSectorPercent = (std::max)(25, (std::min)(90, c.swayDiagRightUpSectorPercent));
                c.swayDiagLeftDownSectorPercent = (std::max)(25, (std::min)(90, c.swayDiagLeftDownSectorPercent));
                c.swayDiagRightDownSectorPercent = (std::max)(25, (std::min)(90, c.swayDiagRightDownSectorPercent));
                c.swayStepMinPercent = (std::max)(5, (std::min)(95, c.swayStepMinPercent));
                c.swayStepMaxPercent = (std::max)(6, (std::min)(100, c.swayStepMaxPercent));
                if (c.swayStepMaxPercent < c.swayStepMinPercent + 1) c.swayStepMaxPercent = c.swayStepMinPercent + 1;
                c.swaySpeedPercent = (std::max)(2, (std::min)(20, c.swaySpeedPercent));
                c.swayMouseButtonStepPercent = (std::max)(5, (std::min)(80, c.swayMouseButtonStepPercent));
                c.swayMouseButtonUpdatePercent = (std::max)(5, (std::min)(50, c.swayMouseButtonUpdatePercent));
                c.swayMouseButtonHoldMs = (std::max)(1000, (std::min)(8000, c.swayMouseButtonHoldMs));
                c.slot = (std::max)(1, (std::min)(15, c.slot));
                if (motion < 1 || motion > 2) motion = 1;
                c.motionMode = static_cast<PcCompassMotionMode>(motion);
                c.centerOuterReversed = reversed != 0;
                compasses.push_back(c);
            }
            continue;
        }


        if (tag == "menu") {
            PcMenuBinding m;
            int category = static_cast<int>(PcMenuCategory::MenuRadial);
            int shape = static_cast<int>(PcMenuShapeType::Radial);
            int triggerMode = static_cast<int>(PcMenuTriggerMode::HoldRelative);
            int enabled = 1;
            int visual = 1;
            std::string combo;
            std::string label;
            iss >> m.id >> category >> shape >> triggerMode >> enabled
                >> m.centerXNorm >> m.centerYNorm >> m.radiusNorm >> m.slot >> m.segmentCount
                >> visual >> m.freeCursorXNorm >> m.freeCursorYNorm >> m.freeCursorSpeedNorm;
            if (!iss.fail()) {
                if (iss >> combo) {
                    if (combo != "-") m.comboTriggerCodes = comboFromString(combo);
                }
                if (iss >> label) {
                    if (label != "-") m.triggerLabel = utf8ToWideLoose(label);
                }
                // HLMAP12：菜单触点活动范围。旧配置没有这 4 个字段，会在 normalizeMenuRange 中按类型补默认值。
                iss >> m.rangeLeftNorm >> m.rangeTopNorm >> m.rangeRightNorm >> m.rangeBottomNorm;
                // HLMAP13：菜单自己的 X/Y 鼠标相对移动倍率。旧配置没有时默认 0.5。
                if (!(iss >> m.relativeSpeedX >> m.relativeSpeedY)) {
                    m.relativeSpeedX = 0.5;
                    m.relativeSpeedY = 0.5;
                }
                if (category < 1 || category > 6) category = static_cast<int>(PcMenuCategory::MenuRadial);
                if (shape < 1 || shape > 5) shape = static_cast<int>(PcMenuShapeType::Radial);
                if (triggerMode < 1 || triggerMode > 2) triggerMode = static_cast<int>(PcMenuTriggerMode::HoldRelative);
                m.category = static_cast<PcMenuCategory>(category);
                m.shapeType = static_cast<PcMenuShapeType>(shape);
                m.triggerMode = static_cast<PcMenuTriggerMode>(triggerMode);
                m.enabled = enabled != 0;
                m.visualHintEnabled = visual != 0;
                m.centerXNorm = ClampNorm(m.centerXNorm);
                m.centerYNorm = ClampNorm(m.centerYNorm);
                m.radiusNorm = clampMenuButtonRadiusForProfile(m.radiusNorm);
                m.slot = (std::max)(1, (std::min)(15, m.slot));
                m.segmentCount = (std::max)(2, (std::min)(12, m.segmentCount));
                m.freeCursorXNorm = ClampNorm(m.freeCursorXNorm);
                m.freeCursorYNorm = ClampNorm(m.freeCursorYNorm);
                m.freeCursorSpeedNorm = (std::max)(200, (std::min)(8000, m.freeCursorSpeedNorm));
                m.relativeSpeedX = normalizeLegacyRelativeSpeed(m.relativeSpeedX, mapVersion);
                m.relativeSpeedY = normalizeLegacyRelativeSpeed(m.relativeSpeedY, mapVersion);
                int triggerPlacement = static_cast<int>(m.triggerPlacement);
                int landingZone = static_cast<int>(m.itemLandingZone);
                int wheelEnabled = m.itemWheelScrollEnabled ? 1 : 0;
                int wheelInvert = m.itemWheelInvert ? 1 : 0;
                int wheelSpeed = m.itemWheelScrollSpeed;
                int wheelMaxDistance = m.itemWheelMaxDistancePx;
                int wheelStep = m.itemWheelStepPx;
                int triggerX = m.centerXNorm;
                int triggerY = m.centerYNorm;
                int closeX = m.centerXNorm;
                int closeY = m.centerYNorm;
                if (iss >> triggerPlacement >> landingZone >> wheelEnabled >> wheelInvert >> wheelSpeed >> wheelMaxDistance >> wheelStep) {
                    m.triggerPlacement = static_cast<PcMenuTriggerPlacement>(clampMenuPlacement(triggerPlacement));
                    m.itemLandingZone = static_cast<PcItemLandingZone>(clampItemLandingZone(landingZone));
                    m.itemWheelScrollEnabled = wheelEnabled != 0;
                    m.itemWheelInvert = wheelInvert != 0;
                    m.itemWheelScrollSpeed = clampItemWheelSpeed(wheelSpeed);
                    m.itemWheelMaxDistancePx = clampItemWheelDistance(wheelMaxDistance);
                    m.itemWheelStepPx = clampItemWheelStep(wheelStep);
                    if (!(iss >> triggerX >> triggerY)) {
                        triggerX = m.centerXNorm;
                        triggerY = m.centerYNorm;
                    }
                    if (!(iss >> closeX >> closeY)) {
                        closeX = m.centerXNorm;
                        closeY = m.centerYNorm;
                    }
                } else {
                    m.triggerPlacement = PcMenuTriggerPlacement::Bottom;
                    m.itemLandingZone = PcItemLandingZone::Bottom;
                    m.itemWheelScrollEnabled = false;
                    m.itemWheelInvert = false;
                    m.itemWheelScrollSpeed = PC_MENU_ITEM_WHEEL_SPEED_DEFAULT;
                    m.itemWheelMaxDistancePx = PC_MENU_ITEM_WHEEL_DISTANCE_DEFAULT;
                    m.itemWheelStepPx = PC_MENU_ITEM_WHEEL_STEP_DEFAULT;
                    triggerX = m.centerXNorm;
                    triggerY = m.centerYNorm;
                    closeX = m.centerXNorm;
                    closeY = m.centerYNorm;
                }
                m.triggerXNorm = ClampNorm(triggerX);
                m.triggerYNorm = ClampNorm(triggerY);
                m.closeXNorm = ClampNorm(closeX);
                m.closeYNorm = ClampNorm(closeY);
                normalizeMenuRange(m);
                menus.push_back(m);
            }
            continue;
        }

        if (tag == "binding") {
            PcMappingBinding b;
            int triggerType = 0;
            int actionType = 0;
            int consume = 1;
            int touchMode = static_cast<int>(PcKeyTouchMode::RandomMove);
            int specialAction = static_cast<int>(PcKeySpecialAction::None);
            std::string combo;
            std::string label;
            iss >> b.id >> triggerType >> b.triggerCode >> actionType >> b.slot >> b.xNorm >> b.yNorm >> b.linuxKeyCode >> consume;
            if (!iss.fail()) {
                // HLMAP3: consume 后是 touchMode/specialAction/random 参数。
                // HLMAP4: consume 后先追加 radiusNorm，再跟 touchMode/specialAction/random 参数。
                std::string maybeNext;
                if (iss >> maybeNext) {
                    bool parsedExtended = false;
                    char* endp = nullptr;
                    long first = std::strtol(maybeNext.c_str(), &endp, 10);
                    if (endp && *endp == '\0') {
                        if (first >= 8000 && first <= 120000) {
                            b.radiusNorm = static_cast<int>(first);
                            if (iss >> touchMode >> specialAction >> b.randomRadiusNorm >> b.randomMoveRadiusNorm >> b.randomMoveIntervalMs) {
                                parsedExtended = true;
                            }
                        } else {
                            touchMode = static_cast<int>(first);
                            if (iss >> specialAction >> b.randomRadiusNorm >> b.randomMoveRadiusNorm >> b.randomMoveIntervalMs) {
                                parsedExtended = true;
                            }
                        }
                        if (parsedExtended) {
                            if (iss >> combo) {
                                if (combo != "-") b.comboTriggerCodes = comboFromString(combo);
                            }
                            if (iss >> label) {
                                if (label != "-") b.triggerLabel = utf8ToWideLoose(label);
                            }
                        }
                    }
                    if (!parsedExtended) {
                        combo = maybeNext;
                        if (combo != "-") b.comboTriggerCodes = comboFromString(combo);
                        if (iss >> label) {
                            if (label != "-") b.triggerLabel = utf8ToWideLoose(label);
                        }
                    }
                }
                b.triggerType = static_cast<PcMappingTriggerType>(triggerType);
                b.actionType = static_cast<PcMappingActionType>(actionType);
                b.xNorm = ClampNorm(b.xNorm);
                b.yNorm = ClampNorm(b.yNorm);
                b.consumeEvent = consume != 0;
                if (touchMode < 1 || touchMode > 2) touchMode = static_cast<int>(PcKeyTouchMode::RandomMove);
                if (specialAction < 0 || specialAction > 2) specialAction = static_cast<int>(PcKeySpecialAction::None);
                b.touchMode = static_cast<PcKeyTouchMode>(touchMode);
                b.specialAction = static_cast<PcKeySpecialAction>(specialAction);
                b.radiusNorm = ClampButtonRadiusNorm(b.radiusNorm);
                b.randomRadiusNorm = std::max(0, std::min(b.radiusNorm, b.randomRadiusNorm));
                b.randomMoveRadiusNorm = std::max(0, std::min(b.radiusNorm, b.randomMoveRadiusNorm));
                b.randomMoveIntervalMs = std::max(8, std::min(200, b.randomMoveIntervalMs));
                bindings.push_back(b);
            }
        }
    }
    std::fclose(fp);

    if (!seenHeader) {
        if (error) *error = "bad header";
        Clear();
        return false;
    }
    return true;
}
