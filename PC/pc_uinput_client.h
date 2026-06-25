#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <windows.h>
#include <atomic>
#include <mutex>
#include <string>

class PcUinputClient final {
public:
    struct Config {
        int adbPort = 18889;
        std::string socketName = "huilang_pc_uinput";
        std::string daemonLocalPath = "pc_uinputd";   // next to exe, or absolute path
        std::string daemonRemotePath = "/data/local/tmp/pc_uinputd";
        std::string adbExeOverride;
        std::string adbSerialOverride; // e.g. 192.168.1.23:5555 after WiFi adb connect
        bool pushDaemon = true;
        bool startDaemon = true;
        bool removeForwardOnStop = true;
        int rotation = -1; // -1=Android daemon auto-detect; 0..3=force Surface rotation.
    };

    PcUinputClient();
    ~PcUinputClient();

    PcUinputClient(const PcUinputClient&) = delete;
    PcUinputClient& operator=(const PcUinputClient&) = delete;

    bool Start(const Config& cfg = Config{});
    void Stop();
    bool IsConnected() const;
    std::wstring StatusText() const;

    // 读取当前 adb 设备的有效显示尺寸（adb shell wm size）。
    // 返回的是当前设备/override 的显示尺寸；PC 端用它计算标准投屏帧里的内部黑边，不能再写死某一台设备。
    static bool QueryAdbDisplaySize(const Config& cfg, int& outW, int& outH);

    bool SendConfig(int rotation, int uiW = 0, int uiH = 0, int physW = 0, int physH = 0);
    bool TouchDown(int slot, int xNorm1000000, int yNorm1000000);
    bool TouchMove(int slot, int xNorm1000000, int yNorm1000000);
    bool TouchUp(int slot);
    bool Key(int linuxCode, bool down);
    bool Wheel(int steps);
    bool Reset();

private:
    struct CommandResult { int rc = -1; std::string output; };

    bool ensureWinsock();
    bool pushDaemonIfNeeded();
    bool cleanupExistingDaemon();
    bool startDaemonProcess();
    bool adbForward();
    bool connectSocket();
    bool sendPacket(uint8_t type, int32_t a, int32_t b, int32_t c, int32_t d, int32_t e = 0, int32_t f = 0, int32_t g = 0, int32_t h = 0);

    static CommandResult runCommandCapture(const std::string& cmdLine);
    static std::string trimAscii(std::string s);
    static std::wstring toWide(const std::string& s);
    static std::string exeDirA();
    static bool fileExistsA(const std::string& path);
    static std::string findAdbExe();
    static std::string quote(const std::string& s);
    static int clampInt(int v, int lo, int hi);

    void setStatus(const std::wstring& s);

private:
    Config cfg_;
    std::atomic<bool> connected_{false};
    SOCKET socket_ = INVALID_SOCKET;
    mutable std::mutex socketMutex_;
    std::mutex sendMutex_;
    mutable std::mutex statusMutex_;
    std::wstring status_ = L"pc_uinput未连接";
};
