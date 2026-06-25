#include "mirror_net.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <ws2tcpip.h>
#include <algorithm>
#include <cstring>
#include <cctype>
#include <mutex>
#include <sstream>
#include <vector>

#include "hl_common.h"


namespace {
std::mutex g_adbSerialMutex;
std::string g_adbSerialOverride;

std::string QuoteAdbArg(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('"');
    for (char ch : s) {
        if (ch == '"') out += "\\\"";
        else out.push_back(ch);
    }
    out.push_back('"');
    return out;
}

bool LooksLikeWifiAdbSerial(const std::string& serial) {
    return serial.find(':') != std::string::npos;
}


std::string LowerAsciiCopy(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

std::wstring MakeAdbMissingStatus(const wchar_t* channelName, const std::string& detail, const std::string& adbSource) {
    std::wstring s = L"ADB";
    if (channelName && channelName[0] != L'\0') {
        s += channelName;
    }
    s += L"连接失败：未找到 adb.exe";
    s += L"\r\n操作：请安装 Android Platform Tools，或把 adb.exe 放到程序目录/加入 PATH。";
    const std::string trimmed = TrimAscii(detail);
    if (!trimmed.empty()) {
        s += L"\r\n原始信息：";
        s += ToWideLoose(trimmed);
    }
    if (!adbSource.empty()) {
        s += L"\r\nADB路径：";
        s += ToWideLoose(adbSource);
    }
    return s;
}

std::wstring MakeAdbDeviceUnavailableStatus(const wchar_t* channelName, const std::string& rawState, const std::string& serial) {
    std::string detail = TrimAscii(rawState);
    if (detail.empty()) detail = "no device";

    const std::string lower = LowerAsciiCopy(detail);

    std::wstring s = L"ADB";
    if (channelName && channelName[0] != L'\0') {
        s += channelName;
    }
    s += L"连接失败：";

    if (lower.find("offline") != std::string::npos) {
        s += L"设备离线";
        s += L"\r\n操作：拔插数据线，在手机弹窗点“允许 USB 调试”；仍失败则到开发者选项里“撤销 USB 调试授权”后重连。";
    }
    else if (lower.find("unauthorized") != std::string::npos) {
        s += L"设备未授权";
        s += L"\r\n操作：解锁手机，在“允许 USB 调试”弹窗中点允许；没有弹窗时，先撤销 USB 调试授权再拔插数据线。";
    }
    else if (lower.find("more than one") != std::string::npos || lower.find("multiple") != std::string::npos) {
        s += L"检测到多个设备";
        s += L"\r\n操作：在设置里选择目标设备，或先拔掉其他手机/模拟器后重试。";
    }
    else if (lower.find("unable to connect") != std::string::npos ||
             lower.find("cannot connect") != std::string::npos ||
             lower.find("connection refused") != std::string::npos ||
             lower.find("timed out") != std::string::npos) {
        s += L"WiFi ADB 连接失败";
        s += L"\r\n操作：确认手机和电脑在同一网络，手机端已开启无线调试；不确定时请切回 USB 连接。";
    }
    else if (lower.find("no device") != std::string::npos ||
             lower.find("no devices") != std::string::npos ||
             lower.find("not found") != std::string::npos) {
        s += L"未检测到设备";
        s += L"\r\n操作：确认 USB 数据线可传输数据，手机已开启开发者选项和 USB 调试，然后重新连接。";
    }
    else {
        s += L"设备未就绪";
        s += L"\r\n操作：请先确认命令行执行 adb devices 时，目标设备状态显示为 device。";
    }

    s += L"\r\n原始信息：";
    s += ToWideLoose(detail);
    if (!serial.empty()) {
        s += L"\r\n目标设备：";
        s += ToWideLoose(serial);
    }
    return s;
}

std::wstring MakeAdbForwardFailStatus(const wchar_t* channelName, int port, const std::string& rawForwardText) {
    std::string detail = TrimAscii(rawForwardText);
    if (detail.empty()) detail = "forward failed";

    std::wstring s = L"ADB";
    if (channelName && channelName[0] != L'\0') {
        s += channelName;
    }
    s += L"连接失败：端口转发失败";
    s += L"\r\n操作：关闭其它占用端口的程序，或执行 adb kill-server 后重新连接；仍失败时请重启手机端投屏服务。";
    s += L"\r\n端口：";
    s += std::to_wstring(port);
    s += L"\r\n原始信息：";
    s += ToWideLoose(detail);
    return s;
}

std::wstring MakeAdbReadyStatus(const wchar_t* channelName, int port, const std::string& adbSource, const std::string& serial) {
    std::wstring s = L"ADB";
    if (channelName && channelName[0] != L'\0') {
        s += channelName;
    }
    s += L"已连接：端口 ";
    s += std::to_wstring(port);
    if (!serial.empty()) {
        s += L" | 设备 ";
        s += ToWideLoose(serial);
    }
    if (!adbSource.empty()) {
        s += L" | ";
        s += ToWideLoose(adbSource);
    }
    return s;
}

std::string PickBestOnlineAdbSerialUnlocked(const std::string& adbQuoted) {
    CommandResult devices = RunCommandCapture(adbQuoted + " devices 2>&1");
    if (devices.rc != 0) return std::string();

    std::vector<std::string> online;
    std::istringstream iss(devices.output);
    std::string line;
    while (std::getline(iss, line)) {
        line = TrimAscii(line);
        if (line.empty()) continue;
        if (line.find("List of devices") != std::string::npos) continue;

        std::istringstream ls(line);
        std::string serial;
        std::string state;
        ls >> serial >> state;
        if (serial.empty() || state != "device") continue;
        online.push_back(serial);
    }

    if (online.empty()) return std::string();

    // USB 有线优先：adb WiFi 设备通常是 192.168.x.x:5555 这种带冒号的 serial。
    // 这样 USB + WiFi 同时存在时，不会因为 adb 默认目标不明确导致主画面 forward 失败。
    for (const std::string& serial : online) {
        if (!LooksLikeWifiAdbSerial(serial)) return serial;
    }
    return online.front();
}
}

void SetAdbSerialOverride(const std::string& serial) {
    std::lock_guard<std::mutex> lk(g_adbSerialMutex);
    g_adbSerialOverride = TrimAscii(serial);
}

std::string GetAdbSerialOverride() {
    std::lock_guard<std::mutex> lk(g_adbSerialMutex);
    return g_adbSerialOverride;
}

std::string BuildAdbTargetPrefix(const std::string& adbQuoted) {
    std::string serial = GetAdbSerialOverride();
    if (serial.empty()) {
        serial = PickBestOnlineAdbSerialUnlocked(adbQuoted);
        if (!serial.empty()) SetAdbSerialOverride(serial);
    }
    if (serial.empty()) return adbQuoted;
    return adbQuoted + " -s " + QuoteAdbArg(serial);
}

int32_t ReadBE32(const uint8_t* p) {
    return (int32_t)((uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]));
}

int64_t ReadBE64(const uint8_t* p) {
    uint64_t v =
        (uint64_t(p[0]) << 56) | (uint64_t(p[1]) << 48) | (uint64_t(p[2]) << 40) | (uint64_t(p[3]) << 32) |
        (uint64_t(p[4]) << 24) | (uint64_t(p[5]) << 16) | (uint64_t(p[6]) << 8) | uint64_t(p[7]);
    return static_cast<int64_t>(v);
}

FrameHeader ParseHeader(const uint8_t* buf) {
    FrameHeader h{};
    h.magic = ReadBE32(buf + 0);
    h.version = ReadBE32(buf + 4);
    h.width = ReadBE32(buf + 8);
    h.height = ReadBE32(buf + 12);
    h.jpegSize = ReadBE32(buf + 16);
    h.frameProducedNs = ReadBE64(buf + 20);
    h.callbackStartNs = ReadBE64(buf + 28);
    h.encodeStartNs = ReadBE64(buf + 36);
    h.encodeEndNs = ReadBE64(buf + 44);
    h.sendStartNs = ReadBE64(buf + 52);
    h.sendStartWallMs = ReadBE64(buf + 60);
    return h;
}

AdbSetupResult RunAdbForward() {
    std::string adbSource;
    const std::string adbPath = ResolveAdbPath(&adbSource);
    const std::string adbQuoted = std::string("\"") + adbPath + "\"";
    const std::string adbTarget = BuildAdbTargetPrefix(adbQuoted);

    AdbSetupResult result;

    CommandResult ver = RunCommandCapture(adbQuoted + " version 2>&1");
    if (ver.rc != 0) {
        result.status = MakeAdbMissingStatus(L"", ver.output, adbSource);
        return result;
    }

    (void)RunCommandCapture(adbQuoted + " start-server 2>&1");

    CommandResult state = RunCommandCapture(adbTarget + " get-state 2>&1");
    std::string stateText = TrimAscii(state.output);
    if (state.rc != 0 || stateText.find("device") == std::string::npos) {
        // ADB 目标不再在失败时自动从 WiFi 回退到 USB；由设置窗口按钮显式切换。
        if (stateText.empty()) stateText = "no device";
        const std::string serial = GetAdbSerialOverride();
        result.status = MakeAdbDeviceUnavailableStatus(L"", stateText, serial);
        return result;
    }

    (void)RunCommandCapture(adbTarget + " forward --remove tcp:" + std::to_string(PORT) + " 2>&1");

    CommandResult forward = RunCommandCapture(
        adbTarget + " forward tcp:" + std::to_string(PORT) + " localabstract:" + SOCKET_NAME + " 2>&1");
    std::string forwardText = TrimAscii(forward.output);
    if (forward.rc != 0) {
        if (forwardText.empty()) forwardText = "forward failed";
        result.status = MakeAdbForwardFailStatus(L"", PORT, forwardText);
        return result;
    }

    result.ok = true;
    result.status = MakeAdbReadyStatus(L"", PORT, adbSource, GetAdbSerialOverride());
    return result;
}

AdbSetupResult RunAdbAudioForward() {
    std::string adbSource;
    const std::string adbPath = ResolveAdbPath(&adbSource);
    const std::string adbQuoted = std::string("\"") + adbPath + "\"";
    const std::string adbTarget = BuildAdbTargetPrefix(adbQuoted);

    AdbSetupResult result;

    CommandResult ver = RunCommandCapture(adbQuoted + " version 2>&1");
    if (ver.rc != 0) {
        result.status = MakeAdbMissingStatus(L"音频", ver.output, adbSource);
        return result;
    }

    (void)RunCommandCapture(adbQuoted + " start-server 2>&1");

    CommandResult state = RunCommandCapture(adbTarget + " get-state 2>&1");
    std::string stateText = TrimAscii(state.output);
    if (state.rc != 0 || stateText.find("device") == std::string::npos) {
        if (stateText.empty()) stateText = "no device";
        result.status = MakeAdbDeviceUnavailableStatus(L"音频", stateText, GetAdbSerialOverride());
        return result;
    }

    (void)RunCommandCapture(adbTarget + " forward --remove tcp:" + std::to_string(AUDIO_PORT) + " 2>&1");

    CommandResult forward = RunCommandCapture(
        adbTarget + " forward tcp:" + std::to_string(AUDIO_PORT) + " localabstract:" + AUDIO_SOCKET_NAME + " 2>&1");
    std::string forwardText = TrimAscii(forward.output);
    if (forward.rc != 0) {
        if (forwardText.empty()) forwardText = "audio forward failed";
        result.status = MakeAdbForwardFailStatus(L"音频", AUDIO_PORT, forwardText);
        return result;
    }

    result.ok = true;
    result.status = MakeAdbReadyStatus(L"音频", AUDIO_PORT, adbSource, GetAdbSerialOverride());
    return result;
}

AdbSetupResult RunAdbCenterRoiVideoForward() {
    std::string adbSource;
    const std::string adbPath = ResolveAdbPath(&adbSource);
    const std::string adbQuoted = std::string("\"") + adbPath + "\"";
    const std::string adbTarget = BuildAdbTargetPrefix(adbQuoted);

    AdbSetupResult result;

    CommandResult ver = RunCommandCapture(adbQuoted + " version 2>&1");
    if (ver.rc != 0) {
        result.status = MakeAdbMissingStatus(L"中心视频", ver.output, adbSource);
        return result;
    }

    (void)RunCommandCapture(adbQuoted + " start-server 2>&1");

    CommandResult state = RunCommandCapture(adbTarget + " get-state 2>&1");
    std::string stateText = TrimAscii(state.output);
    if (state.rc != 0 || stateText.find("device") == std::string::npos) {
        if (stateText.empty()) stateText = "no device";
        result.status = MakeAdbDeviceUnavailableStatus(L"中心视频", stateText, GetAdbSerialOverride());
        return result;
    }

    (void)RunCommandCapture(adbTarget + " forward --remove tcp:" + std::to_string(CENTER_ROI_VIDEO_PORT) + " 2>&1");

    CommandResult forward = RunCommandCapture(
        adbTarget + " forward tcp:" + std::to_string(CENTER_ROI_VIDEO_PORT) + " localabstract:" + CENTER_ROI_VIDEO_SOCKET_NAME + " 2>&1");
    std::string forwardText = TrimAscii(forward.output);
    if (forward.rc != 0) {
        if (forwardText.empty()) forwardText = "center video forward failed";
        result.status = MakeAdbForwardFailStatus(L"中心视频", CENTER_ROI_VIDEO_PORT, forwardText);
        return result;
    }

    result.ok = true;
    result.status = MakeAdbReadyStatus(L"中心视频", CENTER_ROI_VIDEO_PORT, adbSource, GetAdbSerialOverride());
    return result;
}

bool RecvAll(SOCKET s, uint8_t* dst, int size) {
    int got = 0;
    while (got < size) {
        int n = recv(s, reinterpret_cast<char*>(dst + got), size - got, 0);
        if (n <= 0) return false;
        got += n;
    }
    return true;
}

int SocketPendingBytes(SOCKET s) {
    u_long available = 0;
    if (ioctlsocket(s, FIONREAD, &available) != 0) return 0;
    return static_cast<int>(available);
}

int GetSocketOptInt(SOCKET s, int level, int optName) {
    int value = 0;
    int len = sizeof(value);
    if (getsockopt(s, level, optName, reinterpret_cast<char*>(&value), &len) != 0) return 0;
    return value;
}

void ConfigureVideoSocketForLowLatency(SOCKET s) {
    BOOL noDelay = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));

    int rcvbuf = 32 * 1024 * 1024;
    int sndbuf = 8 * 1024 * 1024;
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&rcvbuf), sizeof(rcvbuf));
    setsockopt(s, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&sndbuf), sizeof(sndbuf));
}
