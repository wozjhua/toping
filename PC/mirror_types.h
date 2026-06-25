#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <winsock2.h>
#include <d3d11.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

static constexpr int MAX_RUNTIME_SPLIT_PARTS = 24;

static constexpr int PORT = 27183;
static constexpr int AUDIO_PORT = 27184;
static constexpr int CENTER_ROI_VIDEO_PORT = 27185;
static constexpr const char* SOCKET_NAME = "huilang_screen_mirror";
static constexpr const char* AUDIO_SOCKET_NAME = "huilang_screen_audio";
static constexpr const char* CENTER_ROI_VIDEO_SOCKET_NAME = "huilang_center_roi_video";
static constexpr uint32_t FRAME_MAGIC = 0x484C4D32;
static constexpr int64_t NS_PER_MS = 1000000LL;


struct FrameHeader {
    int32_t magic;
    int32_t version;
    int32_t width;
    int32_t height;
    int32_t jpegSize;
    int64_t frameProducedNs;
    int64_t callbackStartNs;
    int64_t encodeStartNs;
    int64_t encodeEndNs;
    int64_t sendStartNs;
    int64_t sendStartWallMs;
};

struct GpuFrameResource {
    ID3D11Texture2D* texture = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;

    GpuFrameResource() = default;
    GpuFrameResource(ID3D11Texture2D* t, ID3D11ShaderResourceView* s) : texture(t), srv(s) {
        if (texture) texture->AddRef();
        if (srv) srv->AddRef();
    }
    ~GpuFrameResource() {
        if (srv) srv->Release();
        if (texture) texture->Release();
    }
    GpuFrameResource(const GpuFrameResource&) = delete;
    GpuFrameResource& operator=(const GpuFrameResource&) = delete;
};

struct DecodedFrame {
    int width = 0;
    int height = 0;
    // Shared BGRA buffer lets the receiver keep a small reusable ring while the renderer
    // still holds the currently displayed frame. This removes frequent 8-10MB vector steals/frees.
    std::shared_ptr<std::vector<uint8_t>> pixelsBGRA;

    // H.264 FFmpeg D3D11VA zero-copy path: decoded GPU NV12 texture is
    // converted by D3D11 VideoProcessor to a BGRA shader resource.
    // This avoids GPU->CPU transfer, sws_scale, CPU BGRA allocation, and Map upload.
    std::shared_ptr<GpuFrameResource> gpuFrame;

    // Hard-fail diagnostic: true means this is not a decoded video frame.
    // It is a GPU diagnostic texture created only to show why zero-copy failed.
    bool diagnosticFrame = false;
    std::wstring diagnosticText;

    double captureMs = 0.0;
    double encodeMs = 0.0;
    double queueMs = 0.0;
    double socketMs = 0.0;
    // PC 解码墙钟耗时：从最早一个分块开始解码，到最后一个分块解码完成。
    // split 流水线下它会包含“尾块还没到/worker排队”的等待，所以 HUD 里单独显示细分。
    double decodeMs = 0.0;
    double decodeCpuSumMs = 0.0;      // 所有分块解码 CPU 时间累计；用于判断总 CPU 压力。
    double decodeMaxPartMs = 0.0;     // 最慢单块解码耗时；更接近真实并行解码下限。
    double decodeTailWaitMs = 0.0;    // 墙钟 - 最慢块；主要是尾块到达/worker排队/组帧等待。
    double decodeOverlapSavedMs = 0.0;// 累计 - 墙钟；越大说明分块并行越有效。
    int decodePartCount = 1;

    uint64_t generation = 0;
    // Original Android Image timestamp used to pair JPEG background and H.264 center ROI.
    int64_t frameProducedNs = 0;

    // Hybrid overlay metadata. For normal JPEG frames these stay zero.
    // H.264 center ROI frames use width/height as codec texture size and
    // full/roi fields to place the overlay over the JPEG background.
    bool centerRoiOverlay = false;
    int centerFullWidth = 0;
    int centerFullHeight = 0;
    int centerRoiLeft = 0;
    int centerRoiTop = 0;
    int centerRoiWidth = 0;
    int centerRoiHeight = 0;
    int centerCodecWidth = 0;
    int centerCodecHeight = 0;
};

struct SharedState {
    std::mutex mutex;
    DecodedFrame latest;
    bool hasFrame = false;
    DecodedFrame latestCenterRoi;
    bool hasCenterRoiFrame = false;
    std::atomic<bool> centerRoiStrictSync{false};
    std::wstring status = L"connecting to android";
    std::atomic<bool> stop{false};
    double recvFps = 0.0;
    double decodeFps = 0.0;
    double displayFps = 0.0;
    double recvMbps = 0.0;
    double avgJpegKb = 0.0;
    double avgPart0Kb = 0.0;
    double avgPart1Kb = 0.0;
    // Center ROI H.264 stream statistics, independent from JPEG/background stats.
    double centerVideoFps = 0.0;
    double centerVideoMbps = 0.0;
    double centerVideoAvgKb = 0.0;
    double centerVideoDropFps = 0.0;
    double centerVideoDecodeMs = 0.0;
    // Android H.264 ROI sender timing, averaged per 1s stats window.
    // All values are ms and come from HLV2 packets emitted by CenterRoiVideoStreamSender.
    double centerVideoBeforeQueueMs = 0.0;     // queueRgbaFrame start - ImageReader frame timestamp.
    double centerVideoUploadMs = 0.0;          // queue start -> GL draw/upload finished.
    double centerVideoSwapMs = 0.0;            // GL draw/upload finished -> eglSwapBuffers finished.
    double centerVideoCodecPipeMs = 0.0;       // eglSwapBuffers finished -> MediaCodec output buffer available.
    double centerVideoWriteWaitMs = 0.0;       // MediaCodec output available -> socket write start.
    double centerVideoSocketWriteMs = 0.0;     // currently 0 in HLV2; exact write cost remains in Android logcat.
    double centerVideoAndroidTotalMs = 0.0;    // ImageReader frame timestamp -> socket write start.
    int centerVideoTimingSamples = 0;
    int centerVideoDecoderMode = 0; // 0=unknown, 1=FFmpeg/D3D11VA GPU, 2=FFmpeg diagnostic/fail, 3=Media Foundation, 4=FFmpeg software/unexpected
    int centerVideoFullWidth = 0;
    int centerVideoFullHeight = 0;
    int centerVideoRoiLeft = 0;
    int centerVideoRoiTop = 0;
    int centerVideoRoiWidth = 0;
    int centerVideoRoiHeight = 0;
    int centerVideoCodecWidth = 0;
    int centerVideoCodecHeight = 0;
    int recvParts = 1;
    int latestPartStatCount = 0;
    double latestPartKb[MAX_RUNTIME_SPLIT_PARTS]{};
    double latestPartMs[MAX_RUNTIME_SPLIT_PARTS]{};
    int latestPartCpu[MAX_RUNTIME_SPLIT_PARTS]{};
    int latestPartCpuFreqKhz[MAX_RUNTIME_SPLIT_PARTS]{};
    int latestPartLeft[MAX_RUNTIME_SPLIT_PARTS]{};
    int latestPartTop[MAX_RUNTIME_SPLIT_PARTS]{};
    int latestPartWidth[MAX_RUNTIME_SPLIT_PARTS]{};
    int latestPartHeight[MAX_RUNTIME_SPLIT_PARTS]{};
    int latestPartSharePermille[MAX_RUNTIME_SPLIT_PARTS]{};
    int availableEncodeCpuCount = 0;
    uint64_t streamResetGeneration = 0;
    HANDLE frameReadyEvent = nullptr;

    // PC 设置窗口向安卓下发控制命令复用同一条 TCP 连接的反向通道。
    // controlSocket: 旧 JPEG/设置通道；videoControlSocket: H.264 直连视频通道。
    // 视频参数必须优先走 videoControlSocket，否则 video-only 模式下可能只显示“已发送”，
    // 但命令没有真正进入 CenterRoiVideoStreamSender。
    std::mutex controlSocketMutex;
    SOCKET controlSocket = INVALID_SOCKET;
    SOCKET videoControlSocket = INVALID_SOCKET;
};
