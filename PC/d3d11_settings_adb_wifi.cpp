#include "d3d11_renderer.h"

namespace {
std::string WideToUtf8Local(const wchar_t* s) {
    if (!s) return std::string();
    int needed = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return std::string();
    std::string out(static_cast<size_t>(needed - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s, -1, out.data(), needed, nullptr, nullptr);
    return out;
}
}

// getControlText
std::wstring D3D11Renderer::getControlText(int id) {
    wchar_t buf[128]{};
    GetWindowTextW(GetDlgItem(settingsHwnd_, id), buf, 128);
    return buf;
}

// quoteAdbSerialForSettings
std::string D3D11Renderer::quoteAdbSerialForSettings(const std::string& s) const {
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

// parseIpv4String
bool D3D11Renderer::parseIpv4String(const std::string& raw, int out[4]) const {
    std::string s = TrimAscii(raw);
    const size_t colon = s.find(':');
    if (colon != std::string::npos) s.resize(colon);
    int vals[4] = {0, 0, 0, 0};
    size_t pos = 0;
    for (int part = 0; part < 4; ++part) {
        if (pos >= s.size() || !std::isdigit(static_cast<unsigned char>(s[pos]))) return false;
        int value = 0;
        int digits = 0;
        while (pos < s.size() && std::isdigit(static_cast<unsigned char>(s[pos]))) {
            value = value * 10 + (s[pos] - '0');
            ++pos;
            ++digits;
            if (digits > 3 || value > 255) return false;
        }
        vals[part] = value;
        if (part < 3) {
            if (pos >= s.size() || s[pos] != '.') return false;
            ++pos;
        }
    }
    if (pos != s.size()) return false;
    for (int i = 0; i < 4; ++i) out[i] = vals[i];
    return true;
}

// isUsableWifiIpv4
bool D3D11Renderer::isUsableWifiIpv4(const int parts[4]) const {
    if (parts[0] <= 0 || parts[0] >= 224) return false;
    if (parts[0] == 127) return false;
    if (parts[0] == 169 && parts[1] == 254) return false;
    if (parts[3] == 0 || parts[3] == 255) return false;
    return true;
}

// isPrivateLanIpv4
bool D3D11Renderer::isPrivateLanIpv4(const int parts[4]) const {
    return (parts[0] == 10) ||
        (parts[0] == 172 && parts[1] >= 16 && parts[1] <= 31) ||
        (parts[0] == 192 && parts[1] == 168);
}

// findIpv4InText
bool D3D11Renderer::findIpv4InText(const std::string& text, std::string& outIp) const {
    std::string firstUsable;
    for (size_t i = 0; i < text.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(text[i]))) continue;
        if (i > 0 && (std::isdigit(static_cast<unsigned char>(text[i - 1])) || text[i - 1] == '.')) continue;

        int parts[4] = {0, 0, 0, 0};
        size_t pos = i;
        bool ok = true;
        for (int part = 0; part < 4 && ok; ++part) {
            if (pos >= text.size() || !std::isdigit(static_cast<unsigned char>(text[pos]))) { ok = false; break; }
            int value = 0;
            int digits = 0;
            while (pos < text.size() && std::isdigit(static_cast<unsigned char>(text[pos]))) {
                value = value * 10 + (text[pos] - '0');
                ++pos;
                ++digits;
                if (digits > 3 || value > 255) { ok = false; break; }
            }
            if (!ok) break;
            parts[part] = value;
            if (part < 3) {
                if (pos >= text.size() || text[pos] != '.') { ok = false; break; }
                ++pos;
            }
        }
        if (!ok) continue;
        if (pos < text.size() && (std::isdigit(static_cast<unsigned char>(text[pos])) || text[pos] == '.')) continue;
        if (!isUsableWifiIpv4(parts)) continue;

        char buf[32]{};
        std::snprintf(buf, sizeof(buf), "%d.%d.%d.%d", parts[0], parts[1], parts[2], parts[3]);
        if (isPrivateLanIpv4(parts)) {
            outIp = buf;
            return true;
        }
        if (firstUsable.empty()) firstUsable = buf;
    }
    if (!firstUsable.empty()) {
        outIp = firstUsable;
        return true;
    }
    return false;
}

// setWifiIpFieldsFromIpString
bool D3D11Renderer::setWifiIpFieldsFromIpString(const std::string& ipText) {
    int parts[4] = {192, 168, 1, 0};
    if (!parseIpv4String(ipText, parts)) return false;
    wifiIpFirstOctet_ = clampInt(parts[0], 1, 223);
    if (settingsHwnd_) {
        wchar_t first[16]{};
        std::swprintf(first, 16, L"%d.", wifiIpFirstOctet_);
        setCtrlText(IDC_STATIC_WIFI_IP_1, first);
        setIntEdit(IDC_EDIT_WIFI_IP_2, parts[1]);
        setIntEdit(IDC_EDIT_WIFI_IP_3, parts[2]);
        setIntEdit(IDC_EDIT_WIFI_IP_4, parts[3]);
    }
    return true;
}

// setWifiIpFieldsFromEndpoint
void D3D11Renderer::setWifiIpFieldsFromEndpoint(const std::wstring& endpoint) {
    if (!settingsHwnd_) return;
    std::string endpointUtf8 = WideToUtf8Local(endpoint.c_str());
    if (!endpointUtf8.empty() && setWifiIpFieldsFromIpString(endpointUtf8)) return;

    wchar_t first[16]{};
    std::swprintf(first, 16, L"%d.", wifiIpFirstOctet_);
    setCtrlText(IDC_STATIC_WIFI_IP_1, first);

    // 默认按常见局域网 192.168.1.x 展示；最后一段留给自动获取或用户输入。
    if (getControlText(IDC_EDIT_WIFI_IP_2).empty()) setIntEdit(IDC_EDIT_WIFI_IP_2, 168);
    if (getControlText(IDC_EDIT_WIFI_IP_3).empty()) setIntEdit(IDC_EDIT_WIFI_IP_3, 1);
}

// readWifiIpOctetFromEdit
bool D3D11Renderer::readWifiIpOctetFromEdit(int id, int& value) const {
    if (!settingsHwnd_) return false;
    wchar_t buf[16]{};
    GetWindowTextW(GetDlgItem(settingsHwnd_, id), buf, 16);
    std::wstring s = trimKeyText(buf);
    if (s.empty()) return false;
    int v = 0;
    for (wchar_t ch : s) {
        if (ch < L'0' || ch > L'9') return false;
        v = v * 10 + int(ch - L'0');
        if (v > 255) return false;
    }
    value = v;
    return true;
}

// normalizeWifiAdbEndpointFromFields
std::string D3D11Renderer::normalizeWifiAdbEndpointFromFields() const {
    int b = 0, c = 0, d = 0;
    if (!readWifiIpOctetFromEdit(IDC_EDIT_WIFI_IP_2, b) ||
        !readWifiIpOctetFromEdit(IDC_EDIT_WIFI_IP_3, c) ||
        !readWifiIpOctetFromEdit(IDC_EDIT_WIFI_IP_4, d)) {
        return std::string();
    }
    const int a = clampInt(wifiIpFirstOctet_, 1, 223);
    char buf[64]{};
    std::snprintf(buf, sizeof(buf), "%d.%d.%d.%d:5555", a, b, c, d);
    return buf;
}

// pickUsbAdbSerialForSettings
std::string D3D11Renderer::pickUsbAdbSerialForSettings(const std::string& adbQuoted, std::wstring* detail) const {
    CommandResult devs = RunCommandCapture(adbQuoted + " devices 2>&1");
    if (devs.rc != 0) {
        if (detail) *detail = L"adb devices失败：" + ToWideLoose(TrimAscii(devs.output));
        return std::string();
    }

    std::string firstOnline;
    std::string firstUsb;
    std::istringstream iss(devs.output);
    std::string line;
    while (std::getline(iss, line)) {
        line = TrimAscii(line);
        if (line.empty() || line.find("List of devices") != std::string::npos) continue;
        std::istringstream ls(line);
        std::string serial;
        std::string state;
        ls >> serial >> state;
        if (serial.empty() || state != "device") continue;
        if (firstOnline.empty()) firstOnline = serial;
        // WiFi adb serial 通常是 ip:port；有线模式必须优先选择不带冒号的 USB serial。
        if (serial.find(':') == std::string::npos) {
            firstUsb = serial;
            break;
        }
    }

    if (!firstUsb.empty()) {
        if (detail) *detail = L"USB设备 " + ToWideLoose(firstUsb);
        return firstUsb;
    }
    if (detail) {
        *detail = firstOnline.empty()
            ? L"未发现在线ADB设备"
            : (L"未发现USB设备，仅发现 " + ToWideLoose(firstOnline));
    }
    return std::string();
}

// buildUsbAdbTargetForSettings
std::string D3D11Renderer::buildUsbAdbTargetForSettings(const std::string& adbQuoted, std::wstring* detail) const {
    std::string chosen = pickUsbAdbSerialForSettings(adbQuoted, detail);
    if (!chosen.empty()) return adbQuoted + " -s " + quoteAdbSerialForSettings(chosen);
    return adbQuoted;
}

// queryUsbWifiIp
bool D3D11Renderer::queryUsbWifiIp(std::string& outIp, std::wstring& detail) {
    std::string adbSource;
    const std::string adbPath = ResolveAdbPath(&adbSource);
    const std::string adbQuoted = std::string("\"") + adbPath + "\"";
    (void)RunCommandCapture(adbQuoted + " start-server 2>&1");

    std::wstring targetDetail;
    const std::string usbSerial = pickUsbAdbSerialForSettings(adbQuoted, &targetDetail);
    if (usbSerial.empty()) {
        detail = targetDetail.empty() ? L"未发现USB设备" : targetDetail;
        return false;
    }
    const std::string adbTarget = adbQuoted + " -s " + quoteAdbSerialForSettings(usbSerial);
    CommandResult state = RunCommandCapture(adbTarget + " get-state 2>&1");
    const std::string stateText = TrimAscii(state.output);
    if (state.rc != 0 || stateText.find("device") == std::string::npos) {
        detail = L"未发现USB设备";
        return false;
    }

    const char* cmds[] = {
        " shell ip -f inet addr show wlan0 2>&1",
        " shell ip route 2>&1",
        " shell ifconfig wlan0 2>&1",
        " shell getprop dhcp.wlan0.ipaddress 2>&1"
    };
    for (const char* suffix : cmds) {
        CommandResult r = RunCommandCapture(adbTarget + suffix);
        if (findIpv4InText(r.output, outIp)) {
            detail = targetDetail;
            return true;
        }
    }

    detail = L"已连接USB，但未读到 wlan0 IPv4";
    return false;
}

// tryAutoFillWifiIpFromUsb
bool D3D11Renderer::tryAutoFillWifiIpFromUsb(bool force, bool showFail) {
    if (!settingsHwnd_) return false;
    if (!force && !wifiAdbEndpoint_.empty()) return false;

    int dummy = 0;
    const bool fieldsComplete =
        readWifiIpOctetFromEdit(IDC_EDIT_WIFI_IP_2, dummy) &&
        readWifiIpOctetFromEdit(IDC_EDIT_WIFI_IP_3, dummy) &&
        readWifiIpOctetFromEdit(IDC_EDIT_WIFI_IP_4, dummy);
    if (!force && fieldsComplete) return false;

    std::string ip;
    std::wstring detail;
    if (!queryUsbWifiIp(ip, detail)) {
        if (showFail) {
            std::wstring msg = L"WiFi：自动获取IP失败，";
            msg += detail;
            setWifiStatusText(msg);
            MessageBoxW(settingsHwnd_, msg.c_str(), L"WiFi模式", MB_ICONWARNING);
        } else {
            setWifiStatusText(L"WiFi：可USB连接后自动获取，或手动填写后三段");
        }
        return false;
    }

    setWifiIpFieldsFromIpString(ip);
    std::wstring msg = L"WiFi：自动获取到 ";
    msg += ToWideLoose(ip);
    if (!detail.empty()) {
        msg += L"（";
        msg += detail;
        msg += L"）";
    }
    setWifiStatusText(msg);
    return true;
}

// setWifiStatusText
void D3D11Renderer::setWifiStatusText(const std::wstring& text) {
    if (!settingsHwnd_) return;
    HWND status = GetDlgItem(settingsHwnd_, IDC_STATIC_WIFI_STATUS);
    if (status) SetWindowTextW(status, text.c_str());
}

// updateAdbToggleButtonText
void D3D11Renderer::updateAdbToggleButtonText() {
    if (!settingsHwnd_) return;
    HWND btn = GetDlgItem(settingsHwnd_, IDC_BUTTON_ADB_TOGGLE);
    if (btn) SetWindowTextW(btn, adbWifiMode_ ? L"切到有线" : L"切到无线");
}

// resetAdbForwardSocketsForReconnect
void D3D11Renderer::resetAdbForwardSocketsForReconnect() {
    // 切换 ADB 目标后必须断开旧 localhost forward socket；接收线程会重新 RunAdbForward。
    std::lock_guard<std::mutex> lk(state_.controlSocketMutex);
    if (state_.controlSocket != INVALID_SOCKET) {
        shutdown(state_.controlSocket, SD_BOTH);
        closesocket(state_.controlSocket);
        state_.controlSocket = INVALID_SOCKET;
    }
    if (state_.videoControlSocket != INVALID_SOCKET) {
        shutdown(state_.videoControlSocket, SD_BOTH);
        closesocket(state_.videoControlSocket);
        state_.videoControlSocket = INVALID_SOCKET;
    }
}

// applyAdbTargetAndReconnect
void D3D11Renderer::applyAdbTargetAndReconnect(const std::string& serial, bool wifiMode, const std::wstring& status) {
    adbSerialOverride_ = serial;
    adbWifiMode_ = wifiMode;
    SetAdbSerialOverride(serial);
    resetAdbForwardSocketsForReconnect();
    startPcUinputAuto();
    pendingRuntimeSettingsSync_ = true;
    updateAdbToggleButtonText();
    setWifiStatusText(status);
    updateStatusText(status);
}

// switchToWiredAdbMode
bool D3D11Renderer::switchToWiredAdbMode() {
    std::string adbSource;
    const std::string adbPath = ResolveAdbPath(&adbSource);
    const std::string adbQuoted = std::string("\"") + adbPath + "\"";
    (void)RunCommandCapture(adbQuoted + " start-server 2>&1");

    std::wstring detail;
    std::string usbSerial = pickUsbAdbSerialForSettings(adbQuoted, &detail);
    if (usbSerial.empty()) {
        std::wstring msg = L"ADB：切换有线失败，";
        msg += detail.empty() ? L"请先插入USB设备并授权调试" : detail;
        setWifiStatusText(msg);
        MessageBoxW(settingsHwnd_, msg.c_str(), L"ADB连接模式", MB_ICONWARNING);
        return false;
    }

    // 明确写入 USB serial，而不是清空 override；这样 USB + WiFi 同时在线时不会被 adb 判定为 ambiguous。
    std::wstring msg = L"ADB：已切到有线 " + ToWideLoose(usbSerial);
    applyAdbTargetAndReconnect(usbSerial, false, msg);
    return true;
}

// switchAdbModeByButton
bool D3D11Renderer::switchAdbModeByButton() {
    if (adbWifiMode_) {
        return switchToWiredAdbMode();
    }
    return runWifiAdbCommand(false);
}

// runWifiAdbCommand
bool D3D11Renderer::runWifiAdbCommand(bool tcpipFirst) {
    if (!settingsHwnd_) return false;
    if (tcpipFirst) {
        tryAutoFillWifiIpFromUsb(false, false);
    }

    std::string endpoint = normalizeWifiAdbEndpointFromFields();
    if (endpoint.empty()) {
        tryAutoFillWifiIpFromUsb(true, false);
        endpoint = normalizeWifiAdbEndpointFromFields();
    }
    if (endpoint.empty()) {
        MessageBoxW(settingsHwnd_, L"请输入手机局域网 IP 后三段，例如 168 / 1 / 4；USB连接时会自动读取当前设备IP。", L"WiFi模式", MB_ICONWARNING);
        return false;
    }

    std::string adbSource;
    const std::string adbPath = ResolveAdbPath(&adbSource);
    const std::string adbQuoted = std::string("\"") + adbPath + "\"";
    (void)RunCommandCapture(adbQuoted + " start-server 2>&1");

    if (tcpipFirst) {
        std::wstring usbDetail;
        const std::string usbSerial = pickUsbAdbSerialForSettings(adbQuoted, &usbDetail);
        if (usbSerial.empty()) {
            std::wstring msg = L"WiFi：USB转无线失败，";
            msg += usbDetail.empty() ? L"请先插入USB设备并授权调试" : usbDetail;
            setWifiStatusText(msg);
            MessageBoxW(settingsHwnd_, msg.c_str(), L"WiFi模式", MB_ICONWARNING);
            return false;
        }
        const std::string usbTarget = adbQuoted + " -s " + quoteAdbSerialForSettings(usbSerial);
        CommandResult tcpip = RunCommandCapture(usbTarget + " tcpip 5555 2>&1");
        if (tcpip.rc != 0) {
            std::wstring msg = L"WiFi：USB转无线失败 ";
            msg += ToWideLoose(TrimAscii(tcpip.output));
            setWifiStatusText(msg);
            MessageBoxW(settingsHwnd_, msg.c_str(), L"WiFi模式", MB_ICONWARNING);
            return false;
        }
        Sleep(800);
    }

    CommandResult conn = RunCommandCapture(adbQuoted + " connect " + endpoint + " 2>&1");
    const std::string out = TrimAscii(conn.output);
    const std::string lower = NarrowAsciiLower(ToWideLoose(out));
    const bool ok = (conn.rc == 0) &&
        (lower.find("connected") != std::string::npos || lower.find("already connected") != std::string::npos || lower.find("already") != std::string::npos);

    if (!ok) {
        std::wstring msg = L"WiFi：连接失败 ";
        msg += ToWideLoose(out.empty() ? std::string("adb connect failed") : out);
        setWifiStatusText(msg);
        MessageBoxW(settingsHwnd_, msg.c_str(), L"WiFi模式", MB_ICONWARNING);
        return false;
    }

    wifiAdbEndpoint_ = ToWideLoose(endpoint);

    std::wstring msg = L"WiFi：已连接 ";
    msg += ToWideLoose(endpoint);
    msg += L"，已切到无线";
    applyAdbTargetAndReconnect(endpoint, true, msg);
    return true;
}

