#include "d3d11_renderer.h"

const wchar_t* WindowClassName() { return HLW(L"HuiLangD3D11MirrorWindow"); }
const wchar_t* WindowTitleBase() { return HLW(L"灰狼投屏"); }
int g_initialFullscreenSplitParts = 7;
static std::string WideToUtf8(const wchar_t* s) {
    if (!s) return std::string();
    int needed = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return std::string();
    std::string out(static_cast<size_t>(needed - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s, -1, out.data(), needed, nullptr, nullptr);
    return out;
}

static bool IsArg(const std::wstring& s, const wchar_t* name) {
    return _wcsicmp(s.c_str(), name) == 0;
}

struct StartupOptions {
    std::string directHost;
    int directPort = PORT;
    int initialFullscreenSplitParts = 7;
    std::wstring adbSerial;
    bool turnScreenOffOnStart = false;
};

static StartupOptions ParseStartupOptions() {
    StartupOptions opt;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return opt;

    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i] ? argv[i] : L"";
        if ((IsArg(a, HLW(L"--host")) || IsArg(a, HLW(L"--tcp")) || IsArg(a, HLW(L"/host")) || IsArg(a, HLW(L"/tcp"))) && i + 1 < argc) {
            opt.directHost = WideToUtf8(argv[++i]);
            continue;
        }
        if ((IsArg(a, HLW(L"--port")) || IsArg(a, HLW(L"/port"))) && i + 1 < argc) {
            int port = _wtoi(argv[++i]);
            if (port >= 1024 && port <= 65535) opt.directPort = port;
            continue;
        }
        if ((IsArg(a, HLW(L"--split")) || IsArg(a, HLW(L"/split"))) && i + 1 < argc) {
            int parts = _wtoi(argv[++i]);
            if (parts >= 2 && parts <= MAX_RUNTIME_SPLIT_PARTS) opt.initialFullscreenSplitParts = parts;
            continue;
        }
        if ((IsArg(a, HLW(L"--serial")) || IsArg(a, HLW(L"--adb-serial")) || IsArg(a, HLW(L"-s")) || IsArg(a, HLW(L"/serial"))) && i + 1 < argc) {
            opt.adbSerial = argv[++i] ? argv[i] : L"";
            continue;
        }
    }

    LocalFree(argv);
    return opt;
}



#include "mirror_net.h"
#include "mirror_receiver.h"
#include "audio_receiver.h"
#include "center_video_receiver.h"

#include "video_stream_tuning.h"

std::string g_initialAdbSerialOverride;

bool IsProbablyTcpAdbSerialLocal(const std::string& serial) {
    return serial.find(':') != std::string::npos;
}

static bool IsProbablyUsbAdbSerialLocal(const std::string& serial) {
    return !serial.empty() &&
        serial.find(':') == std::string::npos &&
        serial.rfind("emulator-", 0) != 0;
}

static std::string QuoteAdbSerialLocal(const std::string& serial) {
    if (serial.empty()) return "\"\"";
    bool plain = true;
    for (unsigned char c : serial) {
        if (!(std::isalnum(c) || c == '_' || c == '-' || c == '.' || c == ':' || c == '/')) {
            plain = false;
            break;
        }
    }
    if (plain) return serial;
    std::string out = "\"";
    for (char c : serial) {
        if (c == '"') out += "\\\"";
        else out += c;
    }
    out += "\"";
    return out;
}

std::string PickStartupAdbSerialBestEffort() {
    std::string adbSource;
    const std::string adbPath = ResolveAdbPath(&adbSource);
    if (adbPath.empty()) return std::string();

    const std::string adbQuoted = std::string("\"") + adbPath + "\"";
    (void)RunCommandCapture(adbQuoted + " start-server 2>&1");

    CommandResult devs = RunCommandCapture(adbQuoted + " devices 2>&1");
    if (devs.rc != 0) return std::string();

    std::vector<std::string> online;
    std::vector<std::string> usb;
    std::vector<std::string> nonEmulator;
    std::istringstream iss(devs.output);
    std::string line;
    while (std::getline(iss, line)) {
        // adb devices: "<serial>\tdevice"
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ' || line.back() == '\t')) {
            line.pop_back();
        }
        size_t first = 0;
        while (first < line.size() && (line[first] == ' ' || line[first] == '\t')) ++first;
        if (first > 0) line.erase(0, first);
        if (line.empty() || line.find("List of devices") != std::string::npos) continue;

        std::istringstream ls(line);
        std::string serial;
        std::string state;
        ls >> serial >> state;
        if (serial.empty() || state != "device") continue;

        online.push_back(serial);
        if (IsProbablyUsbAdbSerialLocal(serial)) usb.push_back(serial);
        if (serial.rfind("emulator-", 0) != 0) nonEmulator.push_back(serial);
    }

    // 首次启动时不能让 adb 自己在多设备中选择，否则 pc_uinput/forward 会报
    // "more than one device/emulator"。优先选 USB 真机；没有 USB 时，只在目标唯一时选择。
    if (!usb.empty()) return usb.front();
    if (nonEmulator.size() == 1) return nonEmulator.front();
    if (online.size() == 1) return online.front();
    return std::string();
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int) {
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        MessageBoxW(nullptr, HLW(L"WSAStartup failed"), WindowTitleBase(), MB_ICONERROR);
        return 1;
    }
    HRESULT mfHr = MFStartup(MF_VERSION, MFSTARTUP_FULL);
    if (FAILED(mfHr)) {
        WSACleanup();
        MessageBoxW(nullptr, HLW(L"Media Foundation startup failed"), WindowTitleBase(), MB_ICONERROR);
        return 1;
    }

    StartupOptions startup = ParseStartupOptions();
    g_initialFullscreenSplitParts = (std::max)(2, (std::min)(MAX_RUNTIME_SPLIT_PARTS, startup.initialFullscreenSplitParts));

    if (!startup.adbSerial.empty()) {
        g_initialAdbSerialOverride = WideToUtf8(startup.adbSerial.c_str());
    }
    if (g_initialAdbSerialOverride.empty()) {
        g_initialAdbSerialOverride = PickStartupAdbSerialBestEffort();
    }
    if (!g_initialAdbSerialOverride.empty()) {
        SetAdbSerialOverride(g_initialAdbSerialOverride);
    }

    SharedState state;
    state.frameReadyEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    Receiver receiver(state, startup.directHost, startup.directPort);
    receiver.start();

    // Audio is optional: keep trying in the background. If Android audio capture is disabled,
    // this thread simply reconnects without affecting video mirroring.
    AudioReceiver audioReceiver(state);
    audioReceiver.start();

    D3D11Renderer app(hInstance, state);
    if (!app.init()) {
        state.stop.store(true);
        audioReceiver.stop();
        receiver.stop();
        MFShutdown();
        WSACleanup();
        MessageBoxW(nullptr, HLW(L"D3D11 initialization failed"), WindowTitleBase(), MB_ICONERROR);
        return 1;
    }

    // Start H.264 video receiver after the renderer has created the D3D11 device,
    // so FFmpeg D3D11VA can decode into the same device and publish GPU textures.
    CenterRoiVideoDisplayReceiver centerRoiVideoDisplay(state, app.d3dDevice(), app.d3dContext());
    centerRoiVideoDisplay.start();

    int rc = app.run();
    std::wstring pendingLogPath;
    g_perfLog.stop(pendingLogPath);
    state.stop.store(true);
    centerRoiVideoDisplay.stop();
    audioReceiver.stop();
    receiver.stop();
    if (state.frameReadyEvent) {
        CloseHandle(state.frameReadyEvent);
        state.frameReadyEvent = nullptr;
    }
    MFShutdown();
    WSACleanup();
    return rc;
}
