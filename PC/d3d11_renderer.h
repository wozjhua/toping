#pragma once
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <mmsystem.h>
#include <commctrl.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <shellapi.h>
#include <d3d11.h>
#include <d3d10.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <wmcodecdsp.h>
#include <imm.h>
#include <turbojpeg.h>

#ifndef HUILANG_ENABLE_FFMPEG_D3D11VA
#define HUILANG_ENABLE_FFMPEG_D3D11VA 1
#endif

#if HUILANG_ENABLE_FFMPEG_D3D11VA
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}
#endif

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <condition_variable>
#include <string>
#include <sstream>
#include <thread>
#include <vector>
#include <deque>
#include <algorithm>
#include <cctype>
#include <memory>
#include <thread>
#include <cwctype>
#include <cwchar>
#include <utility>
#include <array>
#include "d3d11_shaders_embedded.h"
#include "mirror_types.h"
#include "hl_common.h"
#include "perf_logger.h"
#include "hud_overlay.h"
#include "pc_uinput_mirror_controller.h"
#include "pc_mapping_runtime.h"
#include "pc_mapping_toolbar.h"
#include "pc_mapping_overlay.h"
#include "pc_lock_runtime.h"
#include "pc_compass_runtime.h"
#include "pc_menu_runtime.h"
#include "pc_macro_runtime.h"
#include "pc_menu_create_dialog.h"
#include "pc_menu_options_dialog.h"
#include "pc_normal_key_options_dialog.h"
#include "pc_lock_options_dialog.h"
#include "pc_compass_options_dialog.h"
#include "mirror_net.h"
#include "mirror_receiver.h"
#include "audio_receiver.h"
#include "center_video_receiver.h"
#include "video_stream_tuning.h"
#ifndef D3D11_CREATE_DEVICE_VIDEO_SUPPORT
#define D3D11_CREATE_DEVICE_VIDEO_SUPPORT 0x00000800
#endif

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "wmcodecdspuuid.lib")
#pragma comment(lib, "imm32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "comctl32.lib")
#if HUILANG_ENABLE_FFMPEG_D3D11VA
#pragma comment(lib, "avcodec.lib")
#pragma comment(lib, "avutil.lib")
#pragma comment(lib, "swscale.lib")
#endif
#include "protected_string.h"

const wchar_t* WindowClassName();
const wchar_t* WindowTitleBase();
bool IsProbablyTcpAdbSerialLocal(const std::string& serial);
std::string PickStartupAdbSerialBestEffort();
extern int g_initialFullscreenSplitParts;
extern std::string g_initialAdbSerialOverride;

struct Vertex {
    float x, y, z;
    float u, v;
};
struct MirrorFrameViewport {
    float x = 0.0f;
    float y = 0.0f;
    float w = 1.0f;
    float h = 1.0f;
    int frameW = 0;
    int frameH = 0;
};

class D3D11Renderer {
public:
    explicit D3D11Renderer(HINSTANCE inst, SharedState& state);
    ~D3D11Renderer();
    bool init();
    ID3D11Device* d3dDevice() const;
    ID3D11DeviceContext* d3dContext() const;
    int run();
private:
    HINSTANCE inst_{};
    SharedState& state_;
    PcUinputMirrorController pcUinput_;
    MirrorFrameViewport lastMirrorFrameViewport_{};
    PcMappingRuntime mappingRuntime_;
    PcLockRuntime lockRuntime_;
    PcCompassRuntime compassRuntime_;
    PcMenuRuntime menuRuntime_;
    PcMacroRuntime macroRuntime_;
    std::vector<PcMacroRuntime::Binding> pcMacros_{};
    std::array<bool, 16> runtimeTouchSlots_{};
    PcMappingToolbarPanel mappingToolbar_;
    PcMappingOverlayFrame mappingOverlayFrame_;
    bool mappingOverlayDirty_{ true };
    bool mappingOverlayHidden_{ false };
    HWND hwnd_{};
    RECT windowedRect_{ 100, 100, 820, 320 };
    bool fullscreen_{ false };
    bool stretch_{ true };
    // 默认直接拉伸填满窗口/全屏，不保比例、不裁剪
        bool hudVisible_{ true };
    bool dirtyWindow_{ true };
    bool frameVerticesInitialized_{ false };
    bool windowSizedToFrame_{ false };
    bool allowTearing_{ false };
    bool splitDebugOverlayVisible_{ false };
    static constexpr int ROI_REGION_CENTER = 0;
    static constexpr int ROI_REGION_BOTTOM = 1;
    struct RuntimeSettings {
        RuntimeSettings();
        UINT hudKey;
        UINT fullscreenKey1;
        UINT fullscreenKey2;
        UINT settingsKey;
        UINT splitDebugKey;
        UINT exitKey;
        int hudRefreshMs;
        int audioVolumePercent;
        int splitWeightPercent[MAX_RUNTIME_SPLIT_PARTS];
        int jpegSubsamplingMode;
        int jpegQuality;
        bool keepFrameRate;
        int targetFps;
        int fullscreenPreset;
        int fullscreenSplitParts;
        int cropSize;
        int cropSplitParts;
        int captureMode;
        int videoPreset;
        int videoWidth;
        int videoHeight;
        int videoBitrateMbps;
        int videoFps;
        int videoQualityMode;
        bool centerRoiUseVideo;
        int roiRegion;
        int roiEdgeQualityReduction;
        int roiEdgeScalePercent;
        int roiWidthPercent;
        int roiHeightPercent;
        int roiTopLowPercent;
        int roiJpegCenterWidthPercent;
        int roiCenterCpuWeightPercent;
        int roiBigCoreWeightPercent;
        bool jpegCenterOnly;
        bool stretchFrame;
        bool strictHybridSync;
        bool compactHudMode;
        bool debugHudMode;
    } settings_;
    bool splitWeightCustomized_{ false };
    int splitWeightInitializedCount_{ 0 };
    HWND settingsHwnd_{};
    HFONT settingsFont_{};
    HFONT settingsTitleFont_{};
    HBRUSH settingsBgBrush_{};
    HBRUSH settingsCardBrush_{};
    HBRUSH settingsFieldBrush_{};
    bool settingsHotkeyCaptureArmed_{ false };
    int activeHotkeyEditId_{ 0 };
    enum class MappingCreateMode { None, Key, Compass, Lock, MenuRadial, MenuItem, MenuBag, Macro };
    MappingCreateMode mappingCreateMode_{ MappingCreateMode::None };
    std::wstring mappingEditorStatus_{ HLW(L"就绪") };
    int mappingNextSlot_{ 1 };
    bool mappingEditMode_{ false };
    int selectedMappingBindingIndex_{ -1 };
    bool mappingBindingDrag_{ false };
    bool mappingBindingDragMoved_{ false };
    int mappingBindingDragStartXNorm_{ 0 };
    int mappingBindingDragStartYNorm_{ 0 };
    PcMappingBinding mappingBindingDragStart_{};
    bool compassSelected_{ false };
    bool compassDrag_{ false };
    int compassDragStartXNorm_{ 0 };
    int compassDragStartYNorm_{ 0 };
    PcCompassBinding compassDragStart_{};
    bool compassDiagDrag_{ false };
    int compassDiagDragSlot_{ -1 };
    enum class MenuEditDrag { None, Body, TriggerAnchor, LandingAnchor, CloseAnchor };
    MenuEditDrag menuEditDrag_{ MenuEditDrag::None };
    bool menuEditDragMoved_{ false };
    int selectedMenuIndex_{ -1 };
    int selectedMacroIndex_{ -1 };
    int selectedMacroStepId_{ 0 };
    bool macroStepEditorVisible_{ false };
    bool macroDrag_{ false };
    bool macroStepDrag_{ false };
    bool macroStepRangeDrag_{ false };
    bool macroSwipeEndDrag_{ false };
    int macroDragStartXNorm_{ 0 };
    int macroDragStartYNorm_{ 0 };
    PcMacroRuntime::Binding macroDragStart_{};
    PcMacroRuntime::Step macroStepDragStart_{};
    int menuDragStartXNorm_{ 0 };
    int menuDragStartYNorm_{ 0 };
    PcMenuBinding menuDragStart_{};
    bool compassSectorDrag_{ false };
    int compassSectorDragGroup_{ -1 };
    int compassSectorDragIndex_{ -1 };
    enum class LockEditDrag { None, Move, Left, Right, Top, Bottom, TopLeft, TopRight, BottomLeft, BottomRight };
    LockEditDrag lockEditDrag_{ LockEditDrag::None };
    int lockEditStartXNorm_{ 0 };
    int lockEditStartYNorm_{ 0 };
    PcLockBinding lockEditStart_{};
    ID3D11Device* device_{};
    std::chrono::steady_clock::time_point lastTitleUpdate_{ std::chrono::steady_clock::now() };
    std::chrono::steady_clock::time_point lastHudUpdate_{ std::chrono::steady_clock::now() - std::chrono::seconds(2) };
    bool lastControlSocketConnected_{ false };
    bool pendingRuntimeSettingsSync_{ true };
    std::chrono::steady_clock::time_point lastRuntimeSyncAttempt_{ std::chrono::steady_clock::now() - std::chrono::seconds(2) };
    std::chrono::steady_clock::time_point lastSettingsLiveUpdate_{ std::chrono::steady_clock::now() - std::chrono::seconds(2) };
    double cachedRecvFps_ = 0.0;
    double cachedDecodeFps_ = 0.0;
    double cachedDisplayFps_ = 0.0;
    double cachedRecvMbps_ = 0.0;
    double cachedAvgJpegKb_ = 0.0;
    double cachedCenterVideoFps_ = 0.0;
    double cachedCenterVideoMbps_ = 0.0;
    double cachedCenterVideoAvgKb_ = 0.0;
    double cachedCenterVideoDropFps_ = 0.0;
    double cachedCenterVideoDecodeMs_ = 0.0;
    double cachedCenterVideoBeforeQueueMs_ = 0.0;
    double cachedCenterVideoUploadMs_ = 0.0;
    double cachedCenterVideoSwapMs_ = 0.0;
    double cachedCenterVideoCodecPipeMs_ = 0.0;
    double cachedCenterVideoWriteWaitMs_ = 0.0;
    double cachedCenterVideoSocketWriteMs_ = 0.0;
    double cachedCenterVideoAndroidTotalMs_ = 0.0;
    int cachedCenterVideoTimingSamples_ = 0;
    int cachedCenterVideoDecoderMode_ = 0;
    int cachedCenterVideoFullWidth_ = 0;
    int cachedCenterVideoFullHeight_ = 0;
    int cachedCenterVideoRoiLeft_ = 0;
    int cachedCenterVideoRoiTop_ = 0;
    int cachedCenterVideoRoiWidth_ = 0;
    int cachedCenterVideoRoiHeight_ = 0;
    int cachedCenterVideoCodecWidth_ = 0;
    int cachedCenterVideoCodecHeight_ = 0;
    double cachedPart0Kb_ = 0.0;
    double cachedPart1Kb_ = 0.0;
    int cachedRecvParts_ = 1;
    int cachedPartStatCount_ = 0;
    double cachedPartKb_[MAX_RUNTIME_SPLIT_PARTS]{};
    double cachedPartMs_[MAX_RUNTIME_SPLIT_PARTS]{};
    int cachedPartCpu_[MAX_RUNTIME_SPLIT_PARTS]{};
    int cachedPartCpuFreqKhz_[MAX_RUNTIME_SPLIT_PARTS]{};
    int cachedPartLeft_[MAX_RUNTIME_SPLIT_PARTS]{};
    int cachedPartTop_[MAX_RUNTIME_SPLIT_PARTS]{};
    int cachedPartWidth_[MAX_RUNTIME_SPLIT_PARTS]{};
    int cachedPartHeight_[MAX_RUNTIME_SPLIT_PARTS]{};
    int cachedPartSharePermille_[MAX_RUNTIME_SPLIT_PARTS]{};
    // F4 区域内文字不要跟随每一帧刷新，否则肉眼看不清。
        // 这里做一份显示快照，刷新频率跟随设置里的 HUD 刷新间隔。
        std::chrono::steady_clock::time_point lastRegionOverlayUpdate_{ std::chrono::steady_clock::now() - std::chrono::seconds(2) };
    bool regionOverlaySnapshotValid_{ false };
    int overlayPartStatCount_ = 0;
    double overlayPartKb_[MAX_RUNTIME_SPLIT_PARTS]{};
    double overlayPartMs_[MAX_RUNTIME_SPLIT_PARTS]{};
    int overlayPartCpu_[MAX_RUNTIME_SPLIT_PARTS]{};
    int overlayPartCpuFreqKhz_[MAX_RUNTIME_SPLIT_PARTS]{};
    int overlayPartLeft_[MAX_RUNTIME_SPLIT_PARTS]{};
    int overlayPartTop_[MAX_RUNTIME_SPLIT_PARTS]{};
    int overlayPartWidth_[MAX_RUNTIME_SPLIT_PARTS]{};
    int overlayPartHeight_[MAX_RUNTIME_SPLIT_PARTS]{};
    int overlayPartSharePermille_[MAX_RUNTIME_SPLIT_PARTS]{};
    int cachedAvailableEncodeCpuCount_ = 0;
    double cachedCaptureMs_ = 0.0;
    double cachedEncodeMs_ = 0.0;
    double cachedQueueMs_ = 0.0;
    double cachedSocketMs_ = 0.0;
    double cachedDecodeWallMs_ = 0.0;
    double cachedDecodeCpuSumMs_ = 0.0;
    double cachedDecodeMaxPartMs_ = 0.0;
    double cachedDecodeTailWaitMs_ = 0.0;
    double cachedDecodeOverlapSavedMs_ = 0.0;
    int cachedDecodePartCount_ = 1;
    uint64_t observedStreamResetGeneration_{ 0 };
    bool titleDirty_{ true };
    bool hudDirty_{ true };
    ID3D11DeviceContext* ctx_{};
    IDXGISwapChain1* swapChain_{};
    IDXGISwapChain2* swapChain2_{};
    ID3D11RenderTargetView* rtv_{};
    ID3D11VertexShader* vs_{};
    ID3D11PixelShader* ps_{};
    ID3D11PixelShader* psFsrEasu_{};
    ID3D11PixelShader* psFsrRcas_{};
    ID3D11InputLayout* inputLayout_{};
    ID3D11Buffer* vertexBuffer_{};
    ID3D11Buffer* roiVertexBuffer_{};
    ID3D11Buffer* hudVertexBuffer_{};
    ID3D11Buffer* fsrCb_{};
    ID3D11SamplerState* sampler_{};
    ID3D11Texture2D* fsrTex_{};
    ID3D11RenderTargetView* fsrRtv_{};
    ID3D11ShaderResourceView* fsrSrv_{};
    int fsrTexWidth_{ 0 };
    int fsrTexHeight_{ 0 };
    bool enableFsrUpscale_{ true };
    float fsrSharpness_{ 0.28f };
    ID3D11BlendState* alphaBlend_{};
    ID3D11Texture2D* frameTex_{};
    ID3D11ShaderResourceView* frameSrv_{};
    ID3D11Texture2D* centerRoiTex_{};
    ID3D11ShaderResourceView* centerRoiSrv_{};
    ID3D11Texture2D* hudTex_{};
    ID3D11ShaderResourceView* hudSrv_{};
    ID3D11Texture2D* mappingOverlayTex_{};
    ID3D11ShaderResourceView* mappingOverlaySrv_{};
    int mappingOverlayTexWidth_{ 0 };
    int mappingOverlayTexHeight_{ 0 };
    std::vector<uint8_t> hudPixels_;
    int hudTexWidth_{ 1600 };
    int hudTexHeight_{ 720 };
    int hudUsedWidth_{ 0 };
    int hudUsedHeight_{ 0 };
    std::chrono::steady_clock::time_point presentStatsWindowStart_{ std::chrono::steady_clock::now() };
    int64_t lastPresentDoneNs_{ 0 };
    int64_t lastRenderBeginNs_{ 0 };
    double lastUploadCpuMs_{ 0.0 };
    int lastUploadMode_{ 0 };
    // 0=无, 1=Map动态纹理, 2=Fallback UpdateSubresource, 3=失败, 4=FFmpeg/D3D11VA GPU, 5=GPU诊断
        UINT lastUploadRowPitch_{ 0 };
    int uploadMapCountWindow_{ 0 };
    int uploadFallbackCountWindow_{ 0 };
    int uploadFailedCountWindow_{ 0 };
    int uploadMapCountShown_{ 0 };
    int uploadFallbackCountShown_{ 0 };
    int uploadFailedCountShown_{ 0 };
    double lastDrawCpuMs_{ 0.0 };
    double lastPresentCpuMs_{ 0.0 };
    double lowerBoundMs_{ 0.0 };
    double lowerBoundEqFps_{ 0.0 };
    double presentIntervalAvgMs_{ 0.0 };
    double presentIntervalMaxMs_{ 0.0 };
    double presentIntervalAccumMs_{ 0.0 };
    double presentIntervalLastMs_{ 0.0 };
    double presentIntervalShownAvgMs_{ 0.0 };
    double presentIntervalShownMaxMs_{ 0.0 };
    double presentIntervalP95Ms_{ 0.0 };
    double presentIntervalP99Ms_{ 0.0 };
    int presentIntervalSamples_{ 0 };
    std::vector<double> presentIntervalsWindow_{};
    int skippedFramesWindow_{ 0 };
    int skippedFramesLast_{ 0 };
    uint64_t uploadedGeneration_{ 0 };
    uint64_t uploadedCenterRoiGeneration_{ 0 };
    DecodedFrame currentFrame_;
    std::shared_ptr<GpuFrameResource> currentGpuFrame_;
    DecodedFrame currentCenterRoiFrame_;
    std::shared_ptr<GpuFrameResource> currentCenterRoiGpuFrame_;
    // Strict hybrid sync uses small per-side queues, not a single latest frame.
        // JPEG and H.264 often arrive in different order; replacing the pending frame
        // with only the newest side makes the matcher ping-pong and can collapse to
        // ~20fps even when both streams are only 1-2fps apart.
        std::deque<DecodedFrame> pendingSyncJpegQueue_;
    std::deque<DecodedFrame> pendingSyncCenterQueue_;
    int droppedSyncJpegFrames_ = 0;
    int droppedSyncCenterFrames_ = 0;
    std::wstring currentStatus_ = HLW(L"connecting to android");
    std::wstring wifiAdbEndpoint_;
    int wifiIpFirstOctet_ = 192;
    std::string adbSerialOverride_;
    bool adbWifiMode_ = false;
    // false=手动有线/USB目标，true=手动WiFi目标
    
        // pc_uinput 和画面/控制通道是两套 ADB forward。启动早于 Android/ADB 完全就绪时，
        // pc_uinputd 可能没有被拉起；后续在控制通道连上后自动重试，等价于用户手动点一次“连接”。
        bool pcUinputAutoStarted_ = false;
    std::chrono::steady_clock::time_point lastPcUinputAutoAttempt_{ std::chrono::steady_clock::now() - std::chrono::seconds(10) };
    // 视角 RawInput 诊断窗口：展示 fast path + 2ms 合并后的真实路径。
        // 低频刷新到现有状态/Overlay 文本，避免每帧重绘。
        static constexpr int kLockDebugOverlayRefreshMs = 250;
    static constexpr int kLockDebugIdleFlushMs = 80;
    std::chrono::steady_clock::time_point lastLockDebugOverlayUpdate_{ std::chrono::steady_clock::now() - std::chrono::milliseconds(kLockDebugOverlayRefreshMs) };
    std::chrono::steady_clock::time_point lockDebugWindowStart_{ std::chrono::steady_clock::now() };
    std::chrono::steady_clock::time_point lockDebugLastInput_{ std::chrono::steady_clock::now() };
    bool lockDebugWindowActive_ = false;
    uint64_t lockDebugRawMessages_ = 0;
    uint64_t lockDebugMenuChecks_ = 0;
    uint64_t lockDebugMenuHandled_ = 0;
    uint64_t lockDebugMenuActiveDrops_ = 0;
    uint64_t lockDebugLockChecks_ = 0;
    uint64_t lockDebugLockHandled_ = 0;
    uint64_t lockDebugCompassChecks_ = 0;
    uint64_t lockDebugPcUinputChecks_ = 0;
    // 累计统计：从进入视角锁定开始累计；“采样累计”只累计真实收到输入的窗口，不把空闲等待算进去。
        std::chrono::steady_clock::time_point lockDebugTotalsStart_{ std::chrono::steady_clock::now() };
    uint64_t lockDebugTotalActiveMs_ = 0;
    uint64_t lockDebugTotalRawMessages_ = 0;
    uint64_t lockDebugTotalRawMouse_ = 0;
    uint64_t lockDebugTotalApplyCalls_ = 0;
    uint64_t lockDebugTotalCoalesceFlushes_ = 0;
    uint64_t lockDebugTotalCoalesceForcedFlushes_ = 0;
    uint64_t lockDebugTotalMoveAttempts_ = 0;
    uint64_t lockDebugTotalTouchMoves_ = 0;
    uint64_t lockDebugTotalReanchors_ = 0;
    uint64_t lockDebugTotalTouchDowns_ = 0;
    uint64_t lockDebugTotalTouchUps_ = 0;
    int64_t lockDebugTotalRawDx_ = 0;
    int64_t lockDebugTotalRawDy_ = 0;
    // 菜单 RawInput 诊断窗口：菜单 active 时展示轻量入口 + 2ms 合并后的真实路径。
        static constexpr int kMenuDebugOverlayRefreshMs = 250;
    static constexpr int kMenuDebugIdleFlushMs = 80;
    std::chrono::steady_clock::time_point lastMenuDebugOverlayUpdate_{ std::chrono::steady_clock::now() - std::chrono::milliseconds(kMenuDebugOverlayRefreshMs) };
    std::chrono::steady_clock::time_point menuDebugWindowStart_{ std::chrono::steady_clock::now() };
    std::chrono::steady_clock::time_point menuDebugLastInput_{ std::chrono::steady_clock::now() };
    bool menuDebugWindowActive_ = false;
    uint64_t menuDebugRawMessages_ = 0;
    uint64_t menuDebugMenuEntry_ = 0;
    uint64_t menuDebugMenuAccepted_ = 0;
    uint64_t menuDebugTotalActiveMs_ = 0;
    uint64_t menuDebugTotalRawMessages_ = 0;
    uint64_t menuDebugTotalRawMouse_ = 0;
    uint64_t menuDebugTotalApplyCalls_ = 0;
    uint64_t menuDebugTotalCoalesceFlushes_ = 0;
    uint64_t menuDebugTotalCoalesceForcedFlushes_ = 0;
    int64_t menuDebugTotalRawDx_ = 0;
    int64_t menuDebugTotalRawDy_ = 0;
    void markPcUinputNeedsRetry();
    bool runtimeTouchDown(int slot, int xNorm, int yNorm);
    bool runtimeTouchMove(int slot, int xNorm, int yNorm);
    bool runtimeTouchUp(int slot);
    bool runtimeTapWithUnusedSlot(int xNorm, int yNorm, int avoidSlot);
    void updateStatusText(const std::wstring& text);
    void configureMappingRuntimeCallbacks();
    static bool clientPointToNormLocal(HWND hwnd, int x, int y, int& nx, int& ny);
    static int normToClientCoord(int norm, int size);

public:
    // 映射编辑坐标专用：坐标空间统一为 Android 有效画面区域，不包含 Android 录屏内部黑边。
    struct MappingContentRect {
        double x = 0.0;
        double y = 0.0;
        double w = 1.0;
        double h = 1.0;
        double frameW = 1.0;
        double frameH = 1.0;
    };
    MappingContentRect currentMappingContentRect() const;
    bool clientPointToMappingNorm(int x, int y, int& nx, int& ny) const;
    bool mappingNormToClientPoint(int nx, int ny, int& x, int& y) const;
    int mappingNormToClientX(int nx) const;
    int mappingNormToClientY(int ny) const;
    PcMappingProfile profileToOverlayFrameSpace(const PcMappingProfile& src, int overlayW, int overlayH) const;
    std::vector<PcMacroRuntime::Binding> macrosToOverlayFrameSpace(const std::vector<PcMacroRuntime::Binding>& src, int overlayW, int overlayH) const;
private:
    void warpSystemCursorToNorm(int xNorm, int yNorm) const;
    bool handleLockToggleWhileMenuFree(UINT msg, WPARAM wp, LPARAM lp, LRESULT* outResult = nullptr);
    int currentLockModeInt() const;
    void setCurrentLockModeInt(int mode);
    LockEditDrag hitTestLockEditPoint(int x, int y) const;
    static void setLockRectSymmetric(PcLockBinding& l, int halfW, int halfH);
    static void clampLockRectAndCenter(PcLockBinding& l);
    static int lockHalfW(const PcLockBinding& l);
    static int lockHalfH(const PcLockBinding& l);
    static void shiftLockBinding(PcLockBinding& l, int dx, int dy);
    bool updateLockEditDrag(int nx, int ny);
    bool handleLockEditMessage(UINT msg, WPARAM, LPARAM lp);
    void setMappingEditMode(bool editMode);
    void refreshMappingToolbarStatus();
    std::wstring mappingToolbarStatusText() const;
    static const wchar_t* lockDebugModeText(int mode);
    void resetMenuDebugDispatchCounters();
    void resetMenuDebugTotalCounters();
    void resetMenuDebugAllCounters();
    void markMenuDebugRawMessage();
    void maybeRefreshMenuDebugOverlay(bool force = false);
    void resetLockDebugDispatchCounters();
    void markLockDebugRawMessage();
    void resetLockDebugTotalCounters();
    void resetLockDebugAllCounters();
    void maybeRefreshLockDebugOverlay(bool force = false);
    int mappingToolbarCount() const;
    void beginCreateMappingKey();
    void beginCreateMappingCompass();
    void beginCreateMappingLock();
    void beginCreateMappingMenu(PcMenuCategory cat);
    int nextMacroStepId(const PcMacroRuntime::Binding& macro) const;
    void normalizeMacroSteps(PcMacroRuntime::Binding& macro);
    void syncMacroRuntimeAndOverlay();
    PcMacroRuntime::Step makeDefaultMacroStep(const PcMacroRuntime::Binding& macro, int xNorm, int yNorm) const;
    bool addMacroStep(size_t macroIndex, int afterStepId = 0, bool selectNew = true);
    bool rebindMacroBinding(size_t macroIndex);
    void beginCreateMappingMacro();
    static std::wstring mappingExeDirW();
    static std::wstring mappingProfilePathForSlot(int slot);
    static std::wstring legacyDefaultMappingProfilePath();
    static std::wstring defaultMappingProfilePath();
    static std::string intListToStringLocal(const std::vector<int>& values);
    static std::vector<int> intListFromStringLocal(const std::string& text);
    static std::string wideToHexLocal(const std::wstring& text);
    static int hexNibbleLocal(char c);
    static std::wstring wideFromHexLocal(const std::string& text);
    bool appendMacrosToMappingFile(const std::wstring& path) const;
    bool loadMacrosFromMappingFile(const std::wstring& path, std::vector<PcMacroRuntime::Binding>& outMacros);
    static int clampIntLocal(int v, int lo, int hi);
    bool appendRuntimeSettingsToMappingFile(const std::wstring& path);
    bool loadRuntimeSettingsFromMappingFile(const std::wstring& path);
    void applyLoadedMappings(PcMappingProfile profile, std::vector<PcMacroRuntime::Binding> macros, const std::wstring& path);
    bool loadMappingProfileSlot(int slot, bool silentIfMissing = false);
    void loadDefaultMappingProfileIfPresent();
    void saveMappingProfileSlot(int slot);
    void saveDefaultMappingProfile();
    void toggleMappingOverlayHidden();
    void showMappingProfiles();
    void startMappingToolbar();
    int allocateMappingSlot();
    static PcLockSlideTouchMode chooseLockModeDialog(HWND hwnd);
    bool editMappingBindingOptions(size_t index);
    void applyNormalKeyOptionsResult(size_t index, const PcNormalKeyOptions& opt);
    static int lockMouseButtonCodeToVk(int mouseButtonCode);
    static bool makeLockTriggerCodesFromCapture(const PcCapturedKeyCombo& combo, std::vector<int>& outCodes, std::wstring& outError);
    bool editLockOptions();
    void applyLockOptionsResult(const PcLockOptions& opt);
    bool rebindLockBinding();
    bool rebindMappingBinding(size_t index);
    bool deleteMappingBinding(size_t index);
    static PcCompassButtonBinding* compassButtonByIndex(PcCompassBinding& c, int index);
    bool deleteCompassBinding();
    bool rebindCompassButton(int buttonIndex);
    static int menuSlotForCategory(PcMenuCategory cat);
    static bool isMenuFreeCursorCategoryLocal(PcMenuCategory cat);
    static int clampMenuButtonRadiusNormLocal(int v);
    bool menuAnchorPointToNormFromClient(const PcMenuBinding& m, int x, int y, int& outX, int& outY);
    static void resetMenuRangeToDefault(PcMenuBinding& m);
    bool rebindMenuBinding(size_t index);
    bool deleteMenuBinding(size_t index);
    bool editMenuOptions(size_t index);
    void applyMenuOptionsResult(size_t index, const PcMenuOptions& opt);
    bool updateMenuPositionFromPoint(int x, int y);
    bool setCompassMotionMode(int mode);
    bool toggleCompassRadiusReverse();
    bool editCompassOptions();
    static int clampCompassInnerRadiusNorm(int v);
    static int clampCompassOuterRadiusNorm(int v);
    static void normalizeCompassRadii(PcCompassBinding& c);
    //摇杆轮盘半径限制
        static int compassVisualRadiusPxForClamp(const PcCompassBinding& c, int width, int height);
    static int pxMarginToNorm(int marginPx, int sizePx);
    static void clampCompassToBounds(PcCompassBinding& c);
    void clampCompassUiPositionToWindow(PcCompassBinding& c) const;
    static void resetCompassDiagonalAnchors(PcCompassBinding& c);
    static void shiftCompassDiagonalAnchors(PcCompassBinding& c, int dx, int dy);
    static void clampCompassDiagonalAnchor(PcCompassBinding& c, int slot);
    bool updateCompassDiagonalAnchorFromPoint(int x, int y);
    static double compassNormAngleDiff(double a, double b);
    static double compassFixedSectorCenterAngle(int index);
    static double compassDiagonalSectorCenterAngle(const PcCompassBinding& c, int index);
    static int* compassDiagonalSectorPercentPtr(PcCompassBinding& c, int index);
    static int averageCompassDiagonalSectorPercent(const PcCompassBinding& c);
    bool updateCompassSectorFromPoint(int x, int y);
    void applyCompassOptionsResult(const PcCompassOptions& opt);
    bool updateCompassPositionFromPoint(int x, int y);
    static void clampBindingToBounds(PcMappingBinding& b);
    bool updateSelectedBindingPositionFromPoint(int x, int y);
    bool handleMappingOverlayEditMessage(UINT msg, WPARAM, LPARAM lp);
    bool handleMappingCreateClick(LPARAM lp);
    void startPcUinputAuto();
    void maybeRetryPcUinputAuto(bool force = false);
    void togglePerfLogRecording();
    void checkPerfLogAutoStop();
    void disableImeForMirrorWindow() const;
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT handleMessage(UINT msg, WPARAM wp, LPARAM lp);
    bool createWindow();
    // D3D11Renderer rendering / texture upload / HUD implementation.
    // This file is intentionally included inside class D3D11Renderer.
    // Keep member definitions here to avoid exposing renderer internals while shrinking d3d11_native.cpp.
    
    bool createDevice();
    bool createShaders();
    bool createSampler();
    bool createHudResources();
    static double percentileFromSorted(const std::vector<double>& values, double pct);
    void drawChineseHudLines(const std::vector<std::wstring>& lines);
    static void blendBgraPixel(uint8_t* px, uint8_t b, uint8_t g, uint8_t r, uint8_t alpha);
    static void fillRectBlend(uint8_t* bgra, int width, int height, int pitch, int left, int top, int right, int bottom,
        uint8_t b, uint8_t g, uint8_t r, uint8_t alpha);
    static void drawVerticalLine(uint8_t* bgra, int width, int height, int pitch, int x, int thickness,
        uint8_t b, uint8_t g, uint8_t r);
    static void drawVerticalLineSpan(uint8_t* bgra, int width, int height, int pitch, int x, int top, int bottom, int thickness,
        uint8_t b, uint8_t g, uint8_t r, uint8_t alpha = 200);
    static void drawHorizontalLineSpan(uint8_t* bgra, int width, int height, int pitch, int y, int left, int right, int thickness,
        uint8_t b, uint8_t g, uint8_t r, uint8_t alpha = 200);
    static void drawRectOutline(uint8_t* bgra, int width, int height, int pitch, int left, int top, int right, int bottom, int thickness,
        uint8_t b, uint8_t g, uint8_t r, uint8_t alpha = 200);
    void drawSplitDebugOverlay(uint8_t* bgra, int width, int height, int pitch);
    bool currentH264DebugRect(int frameW, int frameH, int& left, int& top, int& right, int& bottom) const;
    static void fillRectSetBgra(uint8_t* bgra, int width, int height, int pitch, int left, int top, int right, int bottom,
        uint8_t b, uint8_t g, uint8_t r, uint8_t alpha);
    static void drawRectOutlineSetBgra(uint8_t* bgra, int width, int height, int pitch, int left, int top, int right, int bottom, int thickness,
        uint8_t b, uint8_t g, uint8_t r, uint8_t alpha);
    void drawH264DebugOverlayAfterComposite(uint8_t* bgra, int overlayW, int overlayH, int pitch, int frameW, int frameH) const;
    void appendPartStatsHudLines(std::vector<std::wstring>& lines) const;
    double totalHudKb() const;
    double totalHudMbps() const;
    void updateHudTextureIfNeeded(bool force);
    void drawHud(const D3D11_VIEWPORT& vp);
    void recreateRTV();
    void releaseFsrResources();
    bool ensureFsrResources(int width, int height);
    bool ensureFrameTexture(int width, int height);
    bool uploadFrameTextureDynamic(const DecodedFrame& frame);
    void clearCenterRoiOverlay();
    void clearDisplayedFrameAfterStreamReset();
    bool framesAreSyncMatch(const DecodedFrame& jpeg, const DecodedFrame& center, int64_t toleranceNs) const;
    void pruneStrictSyncQueues();
    bool popMatchedStrictSyncPair(DecodedFrame& outJpeg, DecodedFrame& outCenter);
    bool consumeLatestFrame();
    bool ensureCenterRoiTexture(int width, int height);
    bool uploadCenterRoiTextureDynamic(const DecodedFrame& frame);
    static bool validCenterRoiPlacement(const DecodedFrame& f);
    bool updateCenterRoiVertices(const DecodedFrame& f);
    void updateVertices();
    bool ensureMappingOverlayTexture(int width, int height);
    void updateMappingOverlayTextureIfNeeded(const D3D11_VIEWPORT& vp);
    void drawMappingOverlay(const D3D11_VIEWPORT& vp);
    void render(bool presentedNewFrame);
    enum : int {
            IDC_EDIT_HUD_KEY = 3001,
            IDC_EDIT_FULLSCREEN_KEY1 = 3002,
            IDC_EDIT_FULLSCREEN_KEY2 = 3003,
            IDC_EDIT_SETTINGS_KEY = 3004,
            IDC_EDIT_EXIT_KEY = 3005,
            IDC_SLIDER_HUD_INTERVAL = 3006,
            IDC_STATIC_HUD_INTERVAL_VALUE = 3007,
            IDC_SLIDER_AUDIO_VOLUME = 3008,
            IDC_STATIC_AUDIO_VOLUME_VALUE = 3009,
            IDC_TIMER_HOTKEY_CAPTURE = 3013,
            IDC_BUTTON_APPLY = 3010,
            IDC_BUTTON_CLOSE = 3011,
            IDC_BUTTON_RESET = 3012,
            IDC_STATIC_WEIGHT_COUNT = 3020,
            IDC_BUTTON_WEIGHT_APPLY = 3021,
            IDC_BUTTON_WEIGHT_RESET = 3022,
            IDC_STATIC_WEIGHT_STATUS = 3023,
            IDC_RADIO_JPEG_420 = 3024,
            IDC_RADIO_JPEG_444 = 3025,
            IDC_STATIC_JPEG_STATUS = 3026,
            IDC_SLIDER_JPEG_QUALITY = 3030,
            IDC_STATIC_JPEG_QUALITY_VALUE = 3031,
            IDC_SLIDER_FPS = 3032,
            IDC_STATIC_FPS_VALUE = 3033,
            IDC_SLIDER_FULLSCREEN_SPLIT = 3034,
            IDC_STATIC_FULLSCREEN_SPLIT_VALUE = 3035,
            IDC_SLIDER_CROP_SPLIT = 3036,
            IDC_STATIC_CROP_SPLIT_VALUE = 3037,
            IDC_SLIDER_CROP_SIZE = 3038,
            IDC_STATIC_CROP_SIZE_VALUE = 3039,
            IDC_CHECK_KEEP_FRAME = 3040,
            IDC_RADIO_PRESET_720P = 3041,
            IDC_RADIO_PRESET_900P = 3042,
            IDC_RADIO_PRESET_1080P = 3043,
            IDC_RADIO_PRESET_2K = 3044,
            IDC_RADIO_PRESET_3K = 3045,
            IDC_RADIO_PRESET_NATIVE = 3046,
            IDC_RADIO_CAPTURE_FULLSCREEN = 3047,
            IDC_RADIO_CAPTURE_CROP = 3048,
            IDC_BUTTON_STREAM_APPLY = 3049,
            IDC_EDIT_VIDEO_WIDTH = 3050, // kept for compatibility; preset UI no longer creates it
            IDC_EDIT_VIDEO_HEIGHT = 3051,
            IDC_EDIT_VIDEO_BITRATE = 3052,
            IDC_EDIT_VIDEO_FPS = 3053,
            IDC_RADIO_VIDEO_Q_LOW = 3054,
            IDC_RADIO_VIDEO_Q_HIGH = 3055,
            IDC_RADIO_VIDEO_Q_GOP = 3056,
            IDC_BUTTON_VIDEO_APPLY = 3057,
            IDC_STATIC_VIDEO_STATUS = 3058,
            IDC_RADIO_VIDEO_PRESET_640P = 3059,
            IDC_RADIO_VIDEO_PRESET_1080P = 3060,
            IDC_RADIO_VIDEO_PRESET_2K = 3061,
            IDC_RADIO_VIDEO_PRESET_3K = 3062,
            IDC_RADIO_VIDEO_PRESET_NATIVE = 3063,
            IDC_RADIO_CENTER_ROI_VIDEO = 3064,
            IDC_RADIO_CENTER_ROI_JPEG = 3065,
            IDC_SLIDER_ROI_EDGE_Q = 3066,
            IDC_STATIC_ROI_EDGE_Q_VALUE = 3067,
            IDC_SLIDER_ROI_EDGE_SCALE = 3068,
            IDC_STATIC_ROI_EDGE_SCALE_VALUE = 3069,
            IDC_BUTTON_ROI_APPLY = 3070,
            IDC_STATIC_ROI_STATUS = 3071,
            IDC_SLIDER_ROI_WIDTH_PERCENT = 3072,
            IDC_STATIC_ROI_WIDTH_VALUE = 3073,
            IDC_SLIDER_ROI_HEIGHT_PERCENT = 3074,
            IDC_STATIC_ROI_HEIGHT_VALUE = 3075,
            IDC_CHECK_STRICT_HYBRID_SYNC = 3076,
            IDC_CHECK_COMPACT_HUD_MODE = 3077,
            IDC_CHECK_DEBUG_HUD_MODE = 3078,
            IDC_RADIO_ROI_REGION_CENTER = 3079,
            IDC_RADIO_ROI_REGION_BOTTOM = 3080,
            IDC_STATIC_VIDEO_BITRATE_LABEL = 3081,
            IDC_STATIC_VIDEO_FPS_LABEL = 3082,
            IDC_STATIC_VIDEO_QUALITY_LABEL = 3083,
            IDC_STATIC_VIDEO_REGION_LABEL = 3084,
            IDC_STATIC_ROI_CONTENT_LABEL = 3085,
            IDC_STATIC_ROI_VIDEO_WIDTH_LABEL = 3086,
            IDC_STATIC_ROI_VIDEO_HEIGHT_LABEL = 3087,
            IDC_STATIC_ROI_EDGE_Q_LABEL = 3088,
            IDC_STATIC_ROI_EDGE_SCALE_LABEL = 3089,
            IDC_STATIC_ROI_TOP_LOW_LABEL = 3090,
            IDC_SLIDER_ROI_TOP_LOW_PERCENT = 3091,
            IDC_STATIC_ROI_TOP_LOW_VALUE = 3092,
            IDC_STATIC_ROI_JPEG_CENTER_WIDTH_LABEL = 3093,
            IDC_SLIDER_ROI_JPEG_CENTER_WIDTH_PERCENT = 3094,
            IDC_STATIC_ROI_JPEG_CENTER_WIDTH_VALUE = 3095,
            IDC_STATIC_ROI_CENTER_WEIGHT_LABEL = 3096,
            IDC_SLIDER_ROI_CENTER_WEIGHT_PERCENT = 3097,
            IDC_STATIC_ROI_CENTER_WEIGHT_VALUE = 3098,
            IDC_CHECK_JPEG_CENTER_ONLY = 3099,
            // ROI 大核心权重控件。
            // 注意：3100/3200/3300/3400 已用于分块权重控件基 ID，
            // 3500~3508 已用于 ADB/WiFi/拉伸开关，所以这里从 3509 开始。
            IDC_STATIC_ROI_BIG_CORE_WEIGHT_LABEL = 3509,
            IDC_SLIDER_ROI_BIG_CORE_WEIGHT_PERCENT = 3510,
            IDC_STATIC_ROI_BIG_CORE_WEIGHT_VALUE = 3511,
            IDC_CHECK_STRETCH_FRAME = 3508,
            IDC_EDIT_WIFI_IP_2 = 3500,
            IDC_EDIT_WIFI_IP_3 = 3501,
            IDC_EDIT_WIFI_IP_4 = 3502,
            IDC_BUTTON_WIFI_CONNECT = 3503,
            IDC_BUTTON_WIFI_TCPIP = 3504,
            IDC_STATIC_WIFI_STATUS = 3505,
            IDC_STATIC_WIFI_IP_1 = 3506,
            IDC_BUTTON_ADB_TOGGLE = 3507,
            IDC_SLIDER_WEIGHT_BASE = 3100,
            IDC_STATIC_WEIGHT_VALUE_BASE = 3200,
            IDC_STATIC_WEIGHT_NAME_BASE = 3300,
            IDC_GROUP_WEIGHT_CARD_BASE = 3400,
        };
    static std::wstring trimKeyText(std::wstring s);
    static std::wstring upperKeyText(std::wstring s);
    static UINT parseVirtualKey(const std::wstring& raw);
    static std::wstring keyName(UINT vk);
    static const UINT* supportedHotkeyKeys(size_t& count);
    static bool isAnySupportedHotkeyDown();
    static bool isSupportedHotkey(UINT vk);
    void setHotkeyTextForControl(HWND edit, UINT vk);
    static LRESULT CALLBACK HotkeyEditProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    static int focusedHotkeyEditId(HWND settingsHwnd);
    static int clampInt(int v, int lo, int hi);
    HWND createLabel(HWND parent, const wchar_t* text, int x, int y, int w, int h);
    HWND createEdit(HWND parent, int id, int x, int y, int w, int h);
    HWND createValueEdit(HWND parent, int id, int x, int y, int w, int h);
    HWND createTextEdit(HWND parent, int id, int x, int y, int w, int h, const wchar_t* placeholder = L"");
    HWND createButton(HWND parent, const wchar_t* text, int id, int x, int y, int w, int h, DWORD extraStyle = 0);
    HWND createLabelWithId(HWND parent, const wchar_t* text, int id, int x, int y, int w, int h);
    void setCtrlVisible(int id, bool visible);
    void setCtrlEnabled(int id, bool enabled);
    void setCtrlText(int id, const wchar_t* text);
    void armSettingsHotkeyCapture(HWND hwnd);
    void stopSettingsHotkeyCapture(HWND hwnd);
    void captureHotkeyByTimer();
    void hideSettingsWindow(HWND hwnd);
    void updateHudIntervalLabel();
    void updateAudioVolumeLabel();
    void updateSettingsCompactLayout();
    static int fpsFromSliderPos(int pos);
    static int sliderPosFromFps(int fps);
    int selectedFullscreenPresetFromControls() const;
    int selectedCaptureModeFromControls() const;
    const wchar_t* fullscreenPresetText(int preset) const;
    void updateStreamControlLabels();
    void readStreamSettingsFromControlsIfOpen();
    bool sendRuntimeSettingsToAndroid();
    bool applyStreamSettingsFromControls(bool showMessage);
    bool isControlSocketConnected() const;
    void syncRuntimeSettingsOnConnection();
    int maxFullscreenSplitPartsForSettings() const;
    void computeDefaultSplitWeightPercent(int count, int outPercent[MAX_RUNTIME_SPLIT_PARTS]);
    void ensureSplitWeightDefaultsForCount(int count, bool forceReset);
    int currentSplitWeightCountForSettings();
    bool sendControlLineToSocket(SOCKET s, const std::string& line);
    bool sendControlLineToAndroid(const std::string& line);
    bool sendVideoControlLineToAndroid(const std::string& line);
    void readSplitWeightSliders(int count, int outPercent[MAX_RUNTIME_SPLIT_PARTS]);
    std::wstring formatCpuFreqText(int khz) const;
    void updateSplitWeightLabels();
    bool applySplitWeightsFromControls(bool /*showMessage*/);
    int getIntFromEdit(int id, int fallback, int lo, int hi) const;
    void setIntEdit(int id, int value);
    const wchar_t* videoQualityLabel(int mode) const;
    const wchar_t* videoPresetLabel(int /*preset*/) const;
    int selectedVideoQualityModeFromControls() const;
    int selectedVideoPresetFromControls() const;
    void updateVideoControlLabels();
    //视频流 帧率和码率配置
    void readVideoSettingsFromControlsIfOpen();
    bool applyVideoSettingsFromControls(bool showMessage);
    void updateRoiControlLabels();
    void readRoiSettingsFromControlsIfOpen();
    bool sendRoiSettingsToAndroid();
    bool applyRoiSettingsFromControls(bool showMessage);
    int selectedJpegSubsamplingModeFromControls();
    const wchar_t* jpegSubsamplingLabel() const;
    void updateJpegSubsamplingControls();
    bool applyJpegSubsamplingFromControls(bool showMessage);
    void resetSplitWeightsToDefault(bool /*sendToAndroid*/);
    bool isButtonChecked(int id) const;
    void fillSettingsControls();
    std::wstring getControlText(int id);
    std::string quoteAdbSerialForSettings(const std::string& s) const;
    bool parseIpv4String(const std::string& raw, int out[4]) const;
    bool isUsableWifiIpv4(const int parts[4]) const;
    bool isPrivateLanIpv4(const int parts[4]) const;
    bool findIpv4InText(const std::string& text, std::string& outIp) const;
    bool setWifiIpFieldsFromIpString(const std::string& ipText);
    void setWifiIpFieldsFromEndpoint(const std::wstring& endpoint);
    bool readWifiIpOctetFromEdit(int id, int& value) const;
    std::string normalizeWifiAdbEndpointFromFields() const;
    std::string pickUsbAdbSerialForSettings(const std::string& adbQuoted, std::wstring* detail = nullptr) const;
    std::string buildUsbAdbTargetForSettings(const std::string& adbQuoted, std::wstring* detail = nullptr) const;
    bool queryUsbWifiIp(std::string& outIp, std::wstring& detail);
    bool tryAutoFillWifiIpFromUsb(bool force, bool showFail);
    void setWifiStatusText(const std::wstring& text);
    void updateAdbToggleButtonText();
    void resetAdbForwardSocketsForReconnect();
    void applyAdbTargetAndReconnect(const std::string& serial, bool wifiMode, const std::wstring& status);
    bool switchToWiredAdbMode();
    bool switchAdbModeByButton();
    bool runWifiAdbCommand(bool tcpipFirst);
    bool applySettingsFromControls();
    void resetSettingsControlsToDefault();
    void createSettingsControls(HWND hwnd);
    static LRESULT CALLBACK SettingsWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    void showSettingsWindow();
    static int debugTextWidth(const char* text, int scale);
    static int debugTextHeight(int scale);
    static int drawDebugTextRaw(uint8_t* bgra, int width, int height, int pitch, int x, int y,
            const char* text, int scale,
            uint8_t b, uint8_t g, uint8_t r, uint8_t alpha);
    static int roiEdgePartsForCount(int partCount);
    const wchar_t* uploadModeText() const;
    const wchar_t* centerVideoDecoderModeText() const;
    int actualCpuCoreCountForHud() const;
    static int64_t absI64(int64_t v);
    int64_t strictSyncToleranceNs() const;
    void maybeUpdateWindowTitle(bool /*force*/);
    void toggleFullscreen();
    void fitWindowToFrameIfNeeded();
    void cleanup();
};
