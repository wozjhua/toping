#include "pc_uinput_client.h"
#include "pc_uinput_protocol.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <ws2tcpip.h>
#include <cstdio>
#include <cstring>
#include <vector>
#include <mutex>
#include <algorithm>

#pragma comment(lib, "ws2_32.lib")

PcUinputClient::PcUinputClient() = default;
PcUinputClient::~PcUinputClient() { Stop(); }

int PcUinputClient::clampInt(int v, int lo, int hi) {
    return (std::max)(lo, (std::min)(hi, v));
}

std::string PcUinputClient::quote(const std::string& s) {
    return std::string("\"") + s + "\"";
}

static std::string quoteAdbSerialArg(const std::string& s) {
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


static bool parseLastSizeTokenForPcUinput(const std::string& text, int& outW, int& outH) {
    bool found = false;
    int bestW = 0;
    int bestH = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        unsigned char ch = static_cast<unsigned char>(text[i]);
        if (ch < '0' || ch > '9') continue;
        size_t j = i;
        long long w = 0;
        while (j < text.size()) {
            unsigned char c = static_cast<unsigned char>(text[j]);
            if (c < '0' || c > '9') break;
            w = w * 10 + (c - '0');
            ++j;
        }
        if (j >= text.size() || (text[j] != 'x' && text[j] != 'X')) { i = j; continue; }
        ++j;
        long long h = 0;
        bool hasH = false;
        while (j < text.size()) {
            unsigned char c = static_cast<unsigned char>(text[j]);
            if (c < '0' || c > '9') break;
            h = h * 10 + (c - '0');
            hasH = true;
            ++j;
        }
        if (hasH && w > 0 && h > 0 && w <= 100000 && h <= 100000) {
            // wm size 可能同时输出 Physical 和 Override；后一个通常是当前有效尺寸。
            bestW = static_cast<int>(w);
            bestH = static_cast<int>(h);
            found = true;
        }
        i = j;
    }
    if (!found) return false;
    outW = bestW;
    outH = bestH;
    return true;
}

static std::string pcUinputAdbTargetPrefix(const std::string& adbQuoted, const std::string& serial) {
    if (serial.empty()) return adbQuoted;
    return adbQuoted + " -s " + quoteAdbSerialArg(serial);
}

std::string PcUinputClient::trimAscii(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) ++i;
    if (i) s.erase(0, i);
    return s;
}

std::wstring PcUinputClient::toWide(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 0) n = MultiByteToWideChar(CP_ACP, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 0) return L"";
    std::wstring out(static_cast<size_t>(n - 1), L'\0');
    if (!out.empty()) {
        if (MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), n) <= 0) {
            MultiByteToWideChar(CP_ACP, 0, s.c_str(), -1, out.data(), n);
        }
    }
    return out;
}

std::string PcUinputClient::exeDirA() {
    char path[MAX_PATH]{};
    DWORD len = GetModuleFileNameA(nullptr, path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return std::string();
    char* slash = std::strrchr(path, '\\');
    if (slash) *slash = '\0';
    return std::string(path);
}

bool PcUinputClient::fileExistsA(const std::string& path) {
    if (path.empty()) return false;
    DWORD attr = GetFileAttributesA(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

std::string PcUinputClient::findAdbExe() {
    const std::string base = exeDirA();
    const char* rels[] = {"adb.exe", "platform-tools\\adb.exe", "tools\\adb.exe", "tools\\adb\\adb.exe", "scrcpy\\adb.exe"};
    for (const char* rel : rels) {
        std::string p = base.empty() ? std::string(rel) : (base + "\\" + rel);
        if (fileExistsA(p)) return p;
    }
    return "adb";
}


bool PcUinputClient::QueryAdbDisplaySize(const Config& cfg, int& outW, int& outH) {
    outW = 0;
    outH = 0;
    const std::string adb = cfg.adbExeOverride.empty() ? findAdbExe() : cfg.adbExeOverride;
    const std::string adbTarget = pcUinputAdbTargetPrefix(quote(adb), cfg.adbSerialOverride);

    // 优先用 wm size：输出可能是 Physical size 和 Override size。
    // 这里取最后一个 WxH，通常就是当前有效显示尺寸。
    CommandResult wm = runCommandCapture(adbTarget + " shell wm size 2>&1");
    if (wm.rc == 0 && parseLastSizeTokenForPcUinput(wm.output, outW, outH)) {
        return true;
    }

    // 少数系统 wm size 不可用，保底尝试 dumpsys display。
    CommandResult disp = runCommandCapture(adbTarget + " shell dumpsys display 2>&1");
    if (disp.rc == 0 && parseLastSizeTokenForPcUinput(disp.output, outW, outH)) {
        return true;
    }

    outW = 0;
    outH = 0;
    return false;
}

PcUinputClient::CommandResult PcUinputClient::runCommandCapture(const std::string& cmdLineIn) {
    CommandResult r;
    std::string cmdLine = cmdLineIn;
    const std::string redir = " 2>&1";
    if (cmdLine.size() >= redir.size() && cmdLine.compare(cmdLine.size() - redir.size(), redir.size(), redir) == 0) {
        cmdLine.resize(cmdLine.size() - redir.size());
    }

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE readPipe = nullptr, writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) { r.output = "CreatePipe failed"; return r; }
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = writePipe;
    si.hStdError = writePipe;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    std::vector<char> mutableCmd(cmdLine.begin(), cmdLine.end());
    mutableCmd.push_back('\0');

    BOOL ok = CreateProcessA(nullptr, mutableCmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(writePipe);
    if (!ok) {
        char msg[128]{};
        std::snprintf(msg, sizeof(msg), "CreateProcess failed: %lu", static_cast<unsigned long>(GetLastError()));
        r.output = msg;
        CloseHandle(readPipe);
        return r;
    }

    char buf[1024];
    DWORD got = 0;
    while (ReadFile(readPipe, buf, sizeof(buf) - 1, &got, nullptr) && got > 0) {
        buf[got] = '\0';
        r.output += buf;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 0;
    if (GetExitCodeProcess(pi.hProcess, &exitCode)) r.rc = static_cast<int>(exitCode);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(readPipe);
    return r;
}

void PcUinputClient::setStatus(const std::wstring& s) {
    std::lock_guard<std::mutex> lk(statusMutex_);
    status_ = s;
}

std::wstring PcUinputClient::StatusText() const {
    std::lock_guard<std::mutex> lk(statusMutex_);
    return status_;
}

bool PcUinputClient::IsConnected() const {
    return connected_.load(std::memory_order_acquire);
}

bool PcUinputClient::ensureWinsock() {
    static std::once_flag once;
    static bool ok = false;
    std::call_once(once, [] {
        WSADATA wsa{};
        ok = (WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
    });
    if (!ok) setStatus(L"pc_uinput：WSAStartup失败");
    return ok;
}

bool PcUinputClient::pushDaemonIfNeeded() {
    if (!cfg_.pushDaemon) return true;
    std::string local = cfg_.daemonLocalPath;
    if (!fileExistsA(local)) {
        std::string dir = exeDirA();
        if (!dir.empty()) {
            std::string p = dir + "\\" + local;
            if (fileExistsA(p)) local = p;
        }
    }
    if (!fileExistsA(local)) {
        setStatus(L"pc_uinput：找不到pc_uinputd，请放到EXE同目录或设置路径");
        return false;
    }
    const std::string adb = cfg_.adbExeOverride.empty() ? findAdbExe() : cfg_.adbExeOverride;
    const std::string adbTarget = pcUinputAdbTargetPrefix(quote(adb), cfg_.adbSerialOverride);
    CommandResult push = runCommandCapture(adbTarget + " push " + quote(local) + " " + cfg_.daemonRemotePath + " 2>&1");
    if (push.rc != 0) {
        setStatus(L"pc_uinput：adb push失败 " + toWide(trimAscii(push.output)));
        return false;
    }
    CommandResult chmod = runCommandCapture(adbTarget + " shell chmod 755 " + cfg_.daemonRemotePath + " 2>&1");
    if (chmod.rc != 0) {
        setStatus(L"pc_uinput：chmod失败 " + toWide(trimAscii(chmod.output)));
        return false;
    }
    return true;
}

bool PcUinputClient::cleanupExistingDaemon() {
    const std::string adb = cfg_.adbExeOverride.empty() ? findAdbExe() : cfg_.adbExeOverride;
    const std::string adbTarget = pcUinputAdbTargetPrefix(quote(adb), cfg_.adbSerialOverride);

    // localabstract sockets are released only when the owning process exits.
    // Kill any older pc_uinputd first; otherwise bind() returns EADDRINUSE and input is unavailable.
    (void)runCommandCapture(adbTarget + " forward --remove tcp:" + std::to_string(cfg_.adbPort) + " 2>&1");

    const char* cmds[] = {
        "for p in $(pidof pc_uinputd 2>/dev/null); do kill -9 $p; done",
        "killall -9 pc_uinputd 2>/dev/null || true",
        "pkill -9 -f /data/local/tmp/pc_uinputd 2>/dev/null || true",
        "pkill -9 -f pc_uinputd 2>/dev/null || true",
    };
    for (const char* c : cmds) {
        (void)runCommandCapture(adbTarget + " shell " + quote(c) + " 2>&1");
        Sleep(80);
    }

    (void)runCommandCapture(adbTarget + " shell rm -f /data/local/tmp/pc_uinputd.log 2>/dev/null 2>&1");
    return true;
}

bool PcUinputClient::startDaemonProcess() {
    if (!cfg_.startDaemon) return true;
    const std::string adb = cfg_.adbExeOverride.empty() ? findAdbExe() : cfg_.adbExeOverride;
    const std::string adbTarget = pcUinputAdbTargetPrefix(quote(adb), cfg_.adbSerialOverride);
    cleanupExistingDaemon();
    Sleep(120);

    std::string shell = "\"nohup " + cfg_.daemonRemotePath + " --socket " + cfg_.socketName;
    if (cfg_.rotation >= 0 && cfg_.rotation <= 3) {
        shell += " --rotation " + std::to_string(cfg_.rotation);
    }
    shell += " >/data/local/tmp/pc_uinputd.log 2>&1 &\"";
    CommandResult start = runCommandCapture(adbTarget + " shell " + shell + " 2>&1");
    if (start.rc != 0) {
        setStatus(L"pc_uinput：启动daemon失败 " + toWide(trimAscii(start.output)));
        return false;
    }
    Sleep(250);
    return true;
}

bool PcUinputClient::adbForward() {
    const std::string adb = cfg_.adbExeOverride.empty() ? findAdbExe() : cfg_.adbExeOverride;
    const std::string adbQuoted = quote(adb);
    const std::string adbTarget = pcUinputAdbTargetPrefix(adbQuoted, cfg_.adbSerialOverride);
    CommandResult ver = runCommandCapture(adbQuoted + " version 2>&1");
    if (ver.rc != 0) {
        setStatus(L"pc_uinput：adb不可用 " + toWide(trimAscii(ver.output)));
        return false;
    }
    (void)runCommandCapture(adbQuoted + " start-server 2>&1");
    CommandResult state = runCommandCapture(adbTarget + " get-state 2>&1");
    std::string st = trimAscii(state.output);
    if (state.rc != 0 || st.find("device") == std::string::npos) {
        if (st.empty()) st = "no device";
        setStatus(L"pc_uinput：未连接手机 " + toWide(st));
        return false;
    }
    (void)runCommandCapture(adbTarget + " forward --remove tcp:" + std::to_string(cfg_.adbPort) + " 2>&1");
    CommandResult fwd = runCommandCapture(adbTarget + " forward tcp:" + std::to_string(cfg_.adbPort) + " localabstract:" + cfg_.socketName + " 2>&1");
    if (fwd.rc != 0) {
        setStatus(L"pc_uinput：adb forward失败 " + toWide(trimAscii(fwd.output)));
        return false;
    }
    return true;
}

bool PcUinputClient::connectSocket() {
    for (int attempt = 0; attempt < 30; ++attempt) {
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) {
            setStatus(L"pc_uinput：socket创建失败");
            return false;
        }
        BOOL noDelay = TRUE;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<u_short>(cfg_.adbPort));
        addr.sin_addr.s_addr = inet_addr("127.0.0.1");
        if (::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
            std::lock_guard<std::mutex> lk(socketMutex_);
            socket_ = s;
            connected_.store(true, std::memory_order_release);
            setStatus(L"pc_uinput：已连接");
            return true;
        }
        closesocket(s);
        Sleep(100);
    }
    setStatus(L"pc_uinput：连接daemon失败，请看 /data/local/tmp/pc_uinputd.log");
    return false;
}

bool PcUinputClient::Start(const Config& cfg) {
    Stop();
    cfg_ = cfg;
    if (cfg_.rotation < 0 || cfg_.rotation > 3) cfg_.rotation = -1;
    if (!ensureWinsock()) return false;
    if (!adbForward()) return false;       // remove old forward first / verify adb
    if (!pushDaemonIfNeeded()) return false;
    if (!startDaemonProcess()) return false;
    if (!adbForward()) return false;       // daemon is now listening
    if (!connectSocket()) return false;
    if (cfg_.rotation >= 0) SendConfig(cfg_.rotation);
    Reset();
    return true;
}

void PcUinputClient::Stop() {
    if (connected_.load(std::memory_order_acquire)) {
        Reset();
    }
    SOCKET s = INVALID_SOCKET;
    {
        std::lock_guard<std::mutex> lk(socketMutex_);
        s = socket_;
        socket_ = INVALID_SOCKET;
    }
    if (s != INVALID_SOCKET) closesocket(s);
    connected_.store(false, std::memory_order_release);
    if (cfg_.removeForwardOnStop) {
        const std::string adb = cfg_.adbExeOverride.empty() ? findAdbExe() : cfg_.adbExeOverride;
        const std::string adbTarget = pcUinputAdbTargetPrefix(quote(adb), cfg_.adbSerialOverride);
        (void)runCommandCapture(adbTarget + " forward --remove tcp:" + std::to_string(cfg_.adbPort) + " 2>&1");
    }
    setStatus(L"pc_uinput：已断开");
}

bool PcUinputClient::sendPacket(uint8_t type, int32_t a, int32_t b, int32_t c, int32_t d, int32_t e, int32_t f, int32_t g, int32_t h) {
    SOCKET s = INVALID_SOCKET;
    {
        std::lock_guard<std::mutex> lk(socketMutex_);
        s = socket_;
    }
    if (s == INVALID_SOCKET) return false;
    PcUinputPacket p{};
    p.magic[0] = PCU_MAGIC0;
    p.magic[1] = PCU_MAGIC1;
    p.version = PCU_VERSION;
    p.type = type;
    p.a = a; p.b = b; p.c = c; p.d = d; p.e = e; p.f = f; p.g = g; p.h = h;
    const char* data = reinterpret_cast<const char*>(&p);
    int left = sizeof(p);
    std::lock_guard<std::mutex> lk(sendMutex_);
    while (left > 0) {
        int n = send(s, data, left, 0);
        if (n <= 0) {
            connected_.store(false, std::memory_order_release);
            setStatus(L"pc_uinput：发送失败");
            return false;
        }
        data += n;
        left -= n;
    }
    return true;
}

bool PcUinputClient::SendConfig(int rotation, int uiW, int uiH, int physW, int physH) {
    int r = (rotation >= 0 && rotation <= 3) ? rotation : -1;
    return sendPacket(PCU_CONFIG, r, uiW, uiH, physW, physH);
}
bool PcUinputClient::TouchDown(int slot, int x, int y) { return sendPacket(PCU_TOUCH_DOWN, slot, clampInt(x, 0, PCU_NORM_MAX), clampInt(y, 0, PCU_NORM_MAX), 0); }
bool PcUinputClient::TouchMove(int slot, int x, int y) { return sendPacket(PCU_TOUCH_MOVE, slot, clampInt(x, 0, PCU_NORM_MAX), clampInt(y, 0, PCU_NORM_MAX), 0); }
bool PcUinputClient::TouchUp(int slot) { return sendPacket(PCU_TOUCH_UP, slot, 0, 0, 0); }
bool PcUinputClient::Key(int linuxCode, bool down) { return sendPacket(PCU_KEY, linuxCode, down ? 1 : 0, 0, 0); }
bool PcUinputClient::Wheel(int steps) { return steps ? sendPacket(PCU_WHEEL, steps, 0, 0, 0) : true; }
bool PcUinputClient::Reset() { return sendPacket(PCU_RESET, 0, 0, 0, 0); }
