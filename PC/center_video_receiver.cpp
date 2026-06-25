#include "center_video_receiver.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <winsock2.h>
#include <d3d11.h>
#include <d3d10.h>
#include <dxgi1_6.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <wmcodecdsp.h>

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

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "mirror_net.h"
#include "perf_logger.h"
#include "hud_overlay.h"
#include "hl_common.h"

static inline uint8_t ClampU8(int v) {
    return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
}

static int InferNv12LumaRows(DWORD srcLen, int pitch, int visibleHeight) {
    if (pitch <= 0 || srcLen <= 0 || visibleHeight <= 0) return visibleHeight;

    // Some H.264 decoders return NV12 with the luma plane padded to codec block
    // alignment, for example 1920x1202 may be stored as 1920x1216.  If we put
    // the UV plane immediately after visibleHeight rows, chroma is read from the
    // tail of the Y plane, which looks like wrong colors and vertical overlap.
    const int totalRows = static_cast<int>(srcLen / static_cast<DWORD>(pitch));
    if (totalRows <= 0) return visibleHeight;

    int inferred = (totalRows * 2) / 3;
    if ((inferred & 1) != 0) --inferred;
    if (inferred < visibleHeight) inferred = visibleHeight;
    if (inferred > totalRows) inferred = visibleHeight;
    return inferred;
}

static void ConvertNv12ToBgra(const uint8_t* src, DWORD srcLen, int width, int height, int stride, std::vector<uint8_t>& out) {
    if (!src || width <= 0 || height <= 0) return;
    int pitch = stride != 0 ? std::abs(stride) : width;
    if (pitch < width) pitch = width;

    const int lumaRows = InferNv12LumaRows(srcLen, pitch, height);
    const int chromaRows = (height + 1) / 2;
    const size_t yBytes = static_cast<size_t>(pitch) * static_cast<size_t>(lumaRows);
    const size_t need = yBytes + static_cast<size_t>(pitch) * static_cast<size_t>(chromaRows);
    if (srcLen < need) return;

    out.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u);
    const uint8_t* yPlane = src;
    const uint8_t* uvPlane = src + yBytes;
    for (int y = 0; y < height; ++y) {
        const uint8_t* yRow = yPlane + static_cast<size_t>(y) * pitch;
        const uint8_t* uvRow = uvPlane + static_cast<size_t>(y / 2) * pitch;
        uint8_t* dst = out.data() + static_cast<size_t>(y) * static_cast<size_t>(width) * 4u;
        for (int x = 0; x < width; ++x) {
            const int Y = static_cast<int>(yRow[x]);
            const int U = static_cast<int>(uvRow[(x & ~1) + 0]) - 128;
            const int V = static_cast<int>(uvRow[(x & ~1) + 1]) - 128;

            // BT.709 limited-range conversion is the best default for Android
            // screen H.264 at 720p+.  The previous wrong-plane NV12 read was the
            // main color bug; this keeps the fallback path sane if RGB32 output
            // is unavailable.
            const int C = Y - 16;
            const int D = U;
            const int E = V;
            const int r = (298 * C + 459 * E + 128) >> 8;
            const int g = (298 * C - 55 * D - 136 * E + 128) >> 8;
            const int b = (298 * C + 541 * D + 128) >> 8;
            dst[x * 4 + 0] = ClampU8(b);
            dst[x * 4 + 1] = ClampU8(g);
            dst[x * 4 + 2] = ClampU8(r);
            dst[x * 4 + 3] = 255;
        }
    }
}

static void ConvertRgb32ToBgra(const uint8_t* src, DWORD srcLen, int width, int height, int stride, std::vector<uint8_t>& out) {
    if (!src || width <= 0 || height <= 0) return;
    int pitch = stride != 0 ? std::abs(stride) : width * 4;
    if (pitch < width * 4) pitch = width * 4;
    const size_t need = static_cast<size_t>(pitch) * static_cast<size_t>(height);
    if (srcLen < need) return;
    out.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u);
    const bool bottomUp = stride < 0;
    for (int y = 0; y < height; ++y) {
        const int srcY = bottomUp ? (height - 1 - y) : y;
        std::memcpy(out.data() + static_cast<size_t>(y) * static_cast<size_t>(width) * 4u,
                    src + static_cast<size_t>(srcY) * pitch,
                    static_cast<size_t>(width) * 4u);
    }
}

class D3D11GpuVideoConverter {
public:
    D3D11GpuVideoConverter(ID3D11Device* device = nullptr, ID3D11DeviceContext* ctx = nullptr) : device_(device), ctx_(ctx) {
        if (device_) device_->AddRef();
        if (ctx_) ctx_->AddRef();
        if (device_) {
            ID3D10Multithread* mt = nullptr;
            if (SUCCEEDED(device_->QueryInterface(__uuidof(ID3D10Multithread), (void**)&mt)) && mt) {
                mt->SetMultithreadProtected(TRUE);
                mt->Release();
            }
        }
    }
    ~D3D11GpuVideoConverter() {
        release();
        if (ctx_) { ctx_->Release(); ctx_ = nullptr; }
        if (device_) { device_->Release(); device_ = nullptr; }
    }

    bool available() const { return device_ != nullptr && ctx_ != nullptr; }

    // inputW/inputH must be the real decoded texture dimensions.  H.264/D3D11VA
    // commonly pads 1920x1202 to e.g. 1920x1216.  The previous code used the
    // visible height as both input and output size, causing CreateVideoProcessorInputView()
    // or VideoProcessorBlt() to fail and silently fall back to CPU/Map.
    bool ensure(int inputW, int inputH, int outputW, int outputH) {
        if (!available() || inputW <= 0 || inputH <= 0 || outputW <= 0 || outputH <= 0) return false;
        if (inputW == inputWidth_ && inputH == inputHeight_ &&
            outputW == outputWidth_ && outputH == outputHeight_ &&
            processor_ && enumerator_ && !surfaces_.empty()) {
            return true;
        }
        releaseVideoObjects();
        inputWidth_ = inputW;
        inputHeight_ = inputH;
        outputWidth_ = outputW;
        outputHeight_ = outputH;

        HRESULT hr = device_->QueryInterface(__uuidof(ID3D11VideoDevice), (void**)&videoDevice_);
        if (FAILED(hr) || !videoDevice_) {
            logHrOnce("QueryInterface ID3D11VideoDevice failed", hr);
            return false;
        }
        hr = ctx_->QueryInterface(__uuidof(ID3D11VideoContext), (void**)&videoContext_);
        if (FAILED(hr) || !videoContext_) {
            logHrOnce("QueryInterface ID3D11VideoContext failed", hr);
            return false;
        }

        D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd{};
        cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        cd.InputFrameRate.Numerator = 120;
        cd.InputFrameRate.Denominator = 1;
        cd.InputWidth = static_cast<UINT>(inputWidth_);
        cd.InputHeight = static_cast<UINT>(inputHeight_);
        cd.OutputFrameRate.Numerator = 120;
        cd.OutputFrameRate.Denominator = 1;
        cd.OutputWidth = static_cast<UINT>(outputWidth_);
        cd.OutputHeight = static_cast<UINT>(outputHeight_);
        cd.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
        hr = videoDevice_->CreateVideoProcessorEnumerator(&cd, &enumerator_);
        if (FAILED(hr) || !enumerator_) {
            logHrOnce("CreateVideoProcessorEnumerator failed", hr);
            return false;
        }
        hr = videoDevice_->CreateVideoProcessor(enumerator_, 0, &processor_);
        if (FAILED(hr) || !processor_) {
            logHrOnce("CreateVideoProcessor failed", hr);
            return false;
        }

        applyColorSpaceBt709LimitedToFullRgb();

        // Crop only the visible part of the padded decoded texture and scale it to
        // the visible output texture.
        D3D11_RECT srcRect{0, 0, outputWidth_, outputHeight_};
        D3D11_RECT dstRect{0, 0, outputWidth_, outputHeight_};
        videoContext_->VideoProcessorSetStreamSourceRect(processor_, 0, TRUE, &srcRect);
        videoContext_->VideoProcessorSetStreamDestRect(processor_, 0, TRUE, &dstRect);
        videoContext_->VideoProcessorSetOutputTargetRect(processor_, TRUE, &dstRect);

        surfaces_.resize(3);
        for (auto& surface : surfaces_) {
            if (!createSurface(surface)) return false;
        }

        char line[256];
        std::snprintf(line, sizeof(line),
            "[center-roi-video] FFmpeg GPU converter ready inputTex=%dx%d output=%dx%d\n",
            inputWidth_, inputHeight_, outputWidth_, outputHeight_);
        OutputDebugStringA(line);
        return true;
    }

    void applyColorSpaceBt709LimitedToFullRgb() {
        if (!videoContext_ || !processor_) return;

        D3D11_VIDEO_PROCESSOR_COLOR_SPACE inCs{};
        inCs.Usage = 0;
        inCs.RGB_Range = 0;
        inCs.YCbCr_Matrix = 1;  // BT.709 for screen/game H.264 content.
        inCs.YCbCr_xvYCC = 0;
        // H.264/NV12 decoder output is normally studio range. Explicitly expand to full RGB
        // so the center H.264 ROI matches the TurboJPEG full-range BGRX background instead of
        // looking like a grey/transparent glass pane.
        inCs.Nominal_Range = 1;  // D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235

        D3D11_VIDEO_PROCESSOR_COLOR_SPACE outCs{};
        outCs.Usage = 0;
        outCs.RGB_Range = 0;
        outCs.YCbCr_Matrix = 1;
        outCs.YCbCr_xvYCC = 0;
        outCs.Nominal_Range = 2; // D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255

        videoContext_->VideoProcessorSetStreamColorSpace(processor_, 0, &inCs);
        videoContext_->VideoProcessorSetOutputColorSpace(processor_, &outCs);
    }

    std::shared_ptr<GpuFrameResource> convert(ID3D11Texture2D* inputTex, UINT inputArraySlice, int visibleW, int visibleH) {
        if (!inputTex || visibleW <= 0 || visibleH <= 0) return nullptr;
        D3D11_TEXTURE2D_DESC inputDesc{};
        inputTex->GetDesc(&inputDesc);
        if (inputDesc.Width == 0 || inputDesc.Height == 0) return nullptr;

        if (!ensure(static_cast<int>(inputDesc.Width), static_cast<int>(inputDesc.Height), visibleW, visibleH)) {
            return nullptr;
        }
        if (surfaces_.empty()) return nullptr;
        Surface& surface = surfaces_[surfaceIndex_++ % surfaces_.size()];
        if (!surface.texture || !surface.srv || !surface.outputView) return nullptr;

        ID3D11VideoProcessorInputView* inputView = nullptr;
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd{};
        ivd.FourCC = 0;
        ivd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        ivd.Texture2D.MipSlice = 0;
        ivd.Texture2D.ArraySlice = inputArraySlice;
        HRESULT hr = videoDevice_->CreateVideoProcessorInputView(inputTex, enumerator_, &ivd, &inputView);
        if (FAILED(hr) || !inputView) {
            logHrOnce("CreateVideoProcessorInputView failed", hr);
            return nullptr;
        }

        D3D11_VIDEO_PROCESSOR_STREAM stream{};
        stream.Enable = TRUE;
        stream.pInputSurface = inputView;
        hr = videoContext_->VideoProcessorBlt(processor_, surface.outputView, 0, 1, &stream);
        inputView->Release();
        if (FAILED(hr)) {
            logHrOnce("VideoProcessorBlt failed", hr);
            return nullptr;
        }

        return std::make_shared<GpuFrameResource>(surface.texture, surface.srv);
    }

private:
    struct Surface {
        ID3D11Texture2D* texture = nullptr;
        ID3D11ShaderResourceView* srv = nullptr;
        ID3D11VideoProcessorOutputView* outputView = nullptr;
    };

    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* ctx_ = nullptr;
    ID3D11VideoDevice* videoDevice_ = nullptr;
    ID3D11VideoContext* videoContext_ = nullptr;
    ID3D11VideoProcessorEnumerator* enumerator_ = nullptr;
    ID3D11VideoProcessor* processor_ = nullptr;
    int inputWidth_ = 0;
    int inputHeight_ = 0;
    int outputWidth_ = 0;
    int outputHeight_ = 0;
    size_t surfaceIndex_ = 0;
    std::vector<Surface> surfaces_;
    bool loggedHrFailure_ = false;

    void logHrOnce(const char* what, HRESULT hr) {
        if (loggedHrFailure_) return;
        loggedHrFailure_ = true;
        char line[256];
        std::snprintf(line, sizeof(line), "[center-roi-video] FFmpeg GPU converter %s hr=0x%08lx\n",
            what ? what : "failed", static_cast<unsigned long>(hr));
        OutputDebugStringA(line);
    }

    bool createSurface(Surface& surface) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = static_cast<UINT>(outputWidth_);
        td.Height = static_cast<UINT>(outputHeight_);
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        HRESULT hr = device_->CreateTexture2D(&td, nullptr, &surface.texture);
        if (FAILED(hr) || !surface.texture) {
            logHrOnce("CreateTexture2D output surface failed", hr);
            return false;
        }
        hr = device_->CreateShaderResourceView(surface.texture, nullptr, &surface.srv);
        if (FAILED(hr) || !surface.srv) {
            logHrOnce("CreateShaderResourceView output surface failed", hr);
            return false;
        }

        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd{};
        ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        ovd.Texture2D.MipSlice = 0;
        hr = videoDevice_->CreateVideoProcessorOutputView(surface.texture, enumerator_, &ovd, &surface.outputView);
        if (FAILED(hr) || !surface.outputView) {
            logHrOnce("CreateVideoProcessorOutputView failed", hr);
            return false;
        }
        return true;
    }

    void releaseSurface(Surface& surface) {
        if (surface.outputView) { surface.outputView->Release(); surface.outputView = nullptr; }
        if (surface.srv) { surface.srv->Release(); surface.srv = nullptr; }
        if (surface.texture) { surface.texture->Release(); surface.texture = nullptr; }
    }

    void releaseVideoObjects() {
        for (auto& surface : surfaces_) releaseSurface(surface);
        surfaces_.clear();
        if (processor_) { processor_->Release(); processor_ = nullptr; }
        if (enumerator_) { enumerator_->Release(); enumerator_ = nullptr; }
        if (videoContext_) { videoContext_->Release(); videoContext_ = nullptr; }
        if (videoDevice_) { videoDevice_->Release(); videoDevice_ = nullptr; }
        surfaceIndex_ = 0;
        loggedHrFailure_ = false;
    }


    void release() { releaseVideoObjects(); inputWidth_ = 0; inputHeight_ = 0; outputWidth_ = 0; outputHeight_ = 0; }
};

static std::string HudSafeAscii(std::string s) {
    static const char* allowed = "0123456789abcdefghijklmnopqrstuvwxyz .-|:";
    for (char& ch : s) {
        unsigned char c = static_cast<unsigned char>(ch);
        if (c >= 'A' && c <= 'Z') c = static_cast<unsigned char>(c - 'A' + 'a');
        ch = static_cast<char>(c);
        if (!std::strchr(allowed, ch)) ch = ' ';
    }
    return s;
}

static std::shared_ptr<std::vector<uint8_t>> CreateDiagnosticPixels(int width, int height, const std::string& reason) {
    width = (std::max)(320, (std::min)(width, 3840));
    height = (std::max)(180, (std::min)(height, 2160));
    auto pixels = std::make_shared<std::vector<uint8_t>>(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            size_t idx = (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 4u;
            (*pixels)[idx + 0] = 18;   // B
            (*pixels)[idx + 1] = 18;   // G
            (*pixels)[idx + 2] = 92;   // R
            (*pixels)[idx + 3] = 255;
        }
    }

    const int scale = (width >= 1600) ? 4 : 3;
    int x = 48;
    int y = 48;
    auto draw = [&](const std::string& raw) {
        std::string line = HudSafeAscii(raw);
        HudDrawText(*pixels, width, height, x, y, line.c_str(), scale, 255, 255, 255, 255);
        y += 8 * scale + 18;
    };

    draw("ffmpeg d3d11va hard fail");
    draw("not gpu zero copy video");
    draw("reason: " + reason.substr(0, 96));
    draw("see debugview for exact hr");
    draw("no media foundation fallback");
    draw("no cpu video fallback");
    return pixels;
}

static std::shared_ptr<GpuFrameResource> CreateDiagnosticGpuTexture(
        ID3D11Device* device,
        int width,
        int height,
        const std::string& reason) {
    if (!device) return nullptr;
    width = (std::max)(320, (std::min)(width, 3840));
    height = (std::max)(180, (std::min)(height, 2160));
    auto pixels = CreateDiagnosticPixels(width, height, reason);
    if (!pixels || pixels->empty()) return nullptr;

    D3D11_TEXTURE2D_DESC td{};
    td.Width = static_cast<UINT>(width);
    td.Height = static_cast<UINT>(height);
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA init{};
    init.pSysMem = pixels->data();
    init.SysMemPitch = static_cast<UINT>(width * 4);

    ID3D11Texture2D* tex = nullptr;
    HRESULT hr = device->CreateTexture2D(&td, &init, &tex);
    if (FAILED(hr) || !tex) {
        char line[256];
        std::snprintf(line, sizeof(line),
            "[center-roi-video] hardfail diagnostic CreateTexture2D failed hr=0x%08lx\n",
            static_cast<unsigned long>(hr));
        OutputDebugStringA(line);
        return nullptr;
    }

    ID3D11ShaderResourceView* srv = nullptr;
    hr = device->CreateShaderResourceView(tex, nullptr, &srv);
    if (FAILED(hr) || !srv) {
        char line[256];
        std::snprintf(line, sizeof(line),
            "[center-roi-video] hardfail diagnostic CreateShaderResourceView failed hr=0x%08lx\n",
            static_cast<unsigned long>(hr));
        OutputDebugStringA(line);
        tex->Release();
        return nullptr;
    }

    auto out = std::make_shared<GpuFrameResource>(tex, srv);
    srv->Release();
    tex->Release();
    return out;
}

class H264MfDecoder {
public:
    ~H264MfDecoder() { release(); }

    bool ensure(int width, int height) {
        if (decoder_ && width == width_ && height == height_) return true;
        release();
        width_ = width;
        height_ = height;
        if (width_ <= 0 || height_ <= 0) return false;

        HRESULT hr = CoCreateInstance(CLSID_CMSH264DecoderMFT, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&decoder_));
        if (FAILED(hr) || !decoder_) {
            OutputDebugStringA("[center-roi-video] CoCreateInstance H264 decoder failed\n");
            release();
            return false;
        }

        // 尽量要求 MFT 走低延迟；不支持时忽略。
        IMFAttributes* attrs = nullptr;
        if (SUCCEEDED(decoder_->GetAttributes(&attrs)) && attrs) {
            attrs->SetUINT32(MF_LOW_LATENCY, TRUE);
            attrs->Release();
        }

        IMFMediaType* inType = nullptr;
        hr = MFCreateMediaType(&inType);
        if (SUCCEEDED(hr)) hr = inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        if (SUCCEEDED(hr)) hr = inType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        if (SUCCEEDED(hr)) hr = MFSetAttributeSize(inType, MF_MT_FRAME_SIZE, static_cast<UINT32>(width_), static_cast<UINT32>(height_));
        if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(inType, MF_MT_FRAME_RATE, 120, 1);
        if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(inType, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        if (SUCCEEDED(hr)) hr = inType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        if (SUCCEEDED(hr)) hr = decoder_->SetInputType(0, inType, 0);
        if (inType) inType->Release();
        if (FAILED(hr)) {
            OutputDebugStringA("[center-roi-video] SetInputType H264 failed\n");
            release();
            return false;
        }

        if (!selectOutputType()) {
            OutputDebugStringA("[center-roi-video] select output type failed\n");
            release();
            return false;
        }

        decoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        decoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        return true;
    }

    void flush() {
        if (decoder_) {
            decoder_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
            decoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        }
    }

    bool decode(const uint8_t* data, size_t size, int64_t ptsUs, std::vector<DecodedFrame>& frames) {
        if (!decoder_ || !data || size == 0) return false;

        IMFMediaBuffer* mem = nullptr;
        IMFSample* sample = nullptr;
        HRESULT hr = MFCreateMemoryBuffer(static_cast<DWORD>(size), &mem);
        if (SUCCEEDED(hr)) {
            BYTE* dst = nullptr;
            DWORD maxLen = 0;
            DWORD curLen = 0;
            hr = mem->Lock(&dst, &maxLen, &curLen);
            if (SUCCEEDED(hr)) {
                std::memcpy(dst, data, size);
                mem->Unlock();
                mem->SetCurrentLength(static_cast<DWORD>(size));
            }
        }
        if (SUCCEEDED(hr)) hr = MFCreateSample(&sample);
        if (SUCCEEDED(hr)) hr = sample->AddBuffer(mem);
        if (SUCCEEDED(hr)) {
            sample->SetSampleTime(ptsUs > 0 ? ptsUs * 10 : 0); // Media Foundation uses 100ns units.
        }
        if (SUCCEEDED(hr)) hr = decoder_->ProcessInput(0, sample, 0);
        if (sample) sample->Release();
        if (mem) mem->Release();

        if (hr == MF_E_NOTACCEPTING) {
            drain(frames);
            // Retry once after draining.
            return decode(data, size, ptsUs, frames);
        }
        if (FAILED(hr)) {
            return false;
        }
        drain(frames);
        return true;
    }

private:
    IMFTransform* decoder_ = nullptr;
    int width_ = 0;
    int height_ = 0;
    GUID outputSubtype_ = GUID_NULL;
    int outputStride_ = 0;
    bool outputProvidesSamples_ = false;
    MFT_OUTPUT_STREAM_INFO outputInfo_{};


    void release() {
        if (decoder_) {
            decoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
            decoder_->Release();
            decoder_ = nullptr;
        }
        width_ = 0;
        height_ = 0;
        outputSubtype_ = GUID_NULL;
        outputStride_ = 0;
        outputProvidesSamples_ = false;
        outputInfo_ = MFT_OUTPUT_STREAM_INFO{};
    }

    bool selectOutputType() {
        if (!decoder_) return false;
        IMFMediaType* chosen = nullptr;
        GUID chosenSubtype = GUID_NULL;
        HRESULT chosenHr = E_FAIL;

        // Prefer RGB32 so Media Foundation performs the decoder color conversion and
        // handles padded/cropped surfaces internally.  This avoids the common NV12
        // pitfall where 1920x1202 is stored as 1920x1216 and our old converter read
        // UV data from the wrong offset, causing top/bottom overlap and bad colors.
        // Fall back to NV12 if a system decoder does not expose RGB32.
        for (DWORD pass = 0; pass < 2 && !chosen; ++pass) {
            const GUID wanted = (pass == 0) ? MFVideoFormat_RGB32 : MFVideoFormat_NV12;
            for (DWORD i = 0; i < 32; ++i) {
                IMFMediaType* type = nullptr;
                HRESULT hr = decoder_->GetOutputAvailableType(0, i, &type);
                if (hr == MF_E_NO_MORE_TYPES) break;
                if (FAILED(hr) || !type) continue;
                GUID sub = GUID_NULL;
                type->GetGUID(MF_MT_SUBTYPE, &sub);
                if (sub == wanted) {
                    chosen = type;
                    chosenSubtype = sub;
                    break;
                }
                type->Release();
            }
        }
        if (!chosen) return false;
        chosen->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeSize(chosen, MF_MT_FRAME_SIZE, static_cast<UINT32>(width_), static_cast<UINT32>(height_));
        chosenHr = decoder_->SetOutputType(0, chosen, 0);
        if (FAILED(chosenHr)) {
            chosen->Release();
            return false;
        }
        outputSubtype_ = chosenSubtype;
        OutputDebugStringA(chosenSubtype == MFVideoFormat_RGB32
            ? "[center-roi-video] decoder output RGB32\n"
            : "[center-roi-video] decoder output NV12 fallback\n");
        UINT32 strideU = 0;
        if (SUCCEEDED(chosen->GetUINT32(MF_MT_DEFAULT_STRIDE, &strideU))) {
            outputStride_ = static_cast<int>(strideU);
        } else {
            outputStride_ = (chosenSubtype == MFVideoFormat_RGB32) ? width_ * 4 : width_;
        }
        chosen->Release();
        if (FAILED(decoder_->GetOutputStreamInfo(0, &outputInfo_))) {
            outputInfo_ = MFT_OUTPUT_STREAM_INFO{};
        }
        outputProvidesSamples_ = (outputInfo_.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) != 0;
        return true;
    }

    void drain(std::vector<DecodedFrame>& frames) {
        if (!decoder_) return;
        for (;;) {
            IMFSample* outSample = nullptr;
            IMFMediaBuffer* outBuffer = nullptr;
            if (!outputProvidesSamples_) {
                DWORD allocSize = outputInfo_.cbSize;
                const DWORD minSize = static_cast<DWORD>((std::max)(width_ * height_ * 4, width_ * height_ * 3 / 2 + width_ * 16));
                if (allocSize < minSize) allocSize = minSize;
                if (FAILED(MFCreateSample(&outSample)) || FAILED(MFCreateMemoryBuffer(allocSize, &outBuffer)) || FAILED(outSample->AddBuffer(outBuffer))) {
                    if (outBuffer) outBuffer->Release();
                    if (outSample) outSample->Release();
                    return;
                }
            }

            MFT_OUTPUT_DATA_BUFFER out{};
            out.dwStreamID = 0;
            out.pSample = outSample;
            DWORD status = 0;
            HRESULT hr = decoder_->ProcessOutput(0, 1, &out, &status);
            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
                if (out.pEvents) out.pEvents->Release();
                if (out.pSample) out.pSample->Release();
                if (outBuffer) outBuffer->Release();
                break;
            }
            if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
                if (out.pEvents) out.pEvents->Release();
                if (out.pSample) out.pSample->Release();
                if (outBuffer) outBuffer->Release();
                selectOutputType();
                continue;
            }
            if (FAILED(hr)) {
                if (out.pEvents) out.pEvents->Release();
                if (out.pSample) out.pSample->Release();
                if (outBuffer) outBuffer->Release();
                break;
            }

            IMFSample* sample = out.pSample;
            IMFMediaBuffer* contiguous = nullptr;
            if (sample && SUCCEEDED(sample->ConvertToContiguousBuffer(&contiguous)) && contiguous) {
                BYTE* ptr = nullptr;
                DWORD maxLen = 0;
                DWORD curLen = 0;
                if (SUCCEEDED(contiguous->Lock(&ptr, &maxLen, &curLen))) {
                    std::vector<uint8_t> bgra;
                    if (outputSubtype_ == MFVideoFormat_NV12) {
                        ConvertNv12ToBgra(ptr, curLen, width_, height_, outputStride_, bgra);
                    } else if (outputSubtype_ == MFVideoFormat_RGB32) {
                        ConvertRgb32ToBgra(ptr, curLen, width_, height_, outputStride_, bgra);
                    }
                    contiguous->Unlock();
                    if (!bgra.empty()) {
                        DecodedFrame frame;
                        frame.width = width_;
                        frame.height = height_;
                        frame.pixelsBGRA = std::make_shared<std::vector<uint8_t>>(std::move(bgra));
                        frame.decodePartCount = 1;
                        frame.generation = ++generation_;
                        frames.push_back(std::move(frame));
                    }
                }
                contiguous->Release();
            }
            if (sample) sample->Release();
            if (out.pEvents) out.pEvents->Release();
            if (outBuffer) outBuffer->Release();
        }
    }

    uint64_t generation_ = 1000000000ULL;
};


#if HUILANG_ENABLE_FFMPEG_D3D11VA
static std::string AvErrorToString(int err) {
    char buf[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(err, buf, sizeof(buf));
    return std::string(buf);
}

static bool IsD3D11vaPixelFormat(AVPixelFormat fmt) {
    // AV_PIX_FMT_D3D11 / AV_PIX_FMT_D3D11VA_VLD are enum constants, not preprocessor macros.
    // The previous #if defined(AV_PIX_FMT_D3D11) checks were always false, so FFmpeg hardware
    // frames such as fmt=171 were misclassified as software and the GPU path never ran.
    return fmt == AV_PIX_FMT_D3D11 || fmt == AV_PIX_FMT_D3D11VA_VLD;
}

class H264FfmpegD3D11vaDecoder {
public:
    H264FfmpegD3D11vaDecoder(ID3D11Device* d3dDevice = nullptr, ID3D11DeviceContext* d3dCtx = nullptr)
        : d3dDevice_(d3dDevice), d3dCtx_(d3dCtx), gpuConverter_(d3dDevice, d3dCtx) {
        if (d3dDevice_) d3dDevice_->AddRef();
        if (d3dCtx_) d3dCtx_->AddRef();
    }
    ~H264FfmpegD3D11vaDecoder() {
        release();
        if (d3dCtx_) { d3dCtx_->Release(); d3dCtx_ = nullptr; }
        if (d3dDevice_) { d3dDevice_->Release(); d3dDevice_ = nullptr; }
    }

    bool ensure(int width, int height) {
        if (ctx_ && width == width_ && height == height_) return true;
        release();
        width_ = width;
        height_ = height;
        if (width_ <= 0 || height_ <= 0) return false;

        const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
        if (!codec) {
            OutputDebugStringA("[center-roi-video] FFmpeg H.264 decoder not found\n");
            return false;
        }

        int err = createHardwareDeviceContext();
        if (err < 0 || !hwDeviceCtx_) {
            std::string line = "[center-roi-video] FFmpeg D3D11VA device create failed: " + AvErrorToString(err) + "\n";
            OutputDebugStringA(line.c_str());
            release();
            return false;
        }

        ctx_ = avcodec_alloc_context3(codec);
        if (!ctx_) {
            release();
            return false;
        }
        ctx_->opaque = this;
        ctx_->get_format = &H264FfmpegD3D11vaDecoder::GetHwFormat;
        ctx_->hw_device_ctx = av_buffer_ref(hwDeviceCtx_);
        ctx_->thread_count = 1;
        ctx_->thread_type = 0;
        ctx_->flags |= AV_CODEC_FLAG_LOW_DELAY;
        ctx_->flags2 |= AV_CODEC_FLAG2_FAST;
        ctx_->width = width_;
        ctx_->height = height_;

        err = avcodec_open2(ctx_, codec, nullptr);
        if (err < 0) {
            std::string line = "[center-roi-video] FFmpeg avcodec_open2 failed: " + AvErrorToString(err) + "\n";
            OutputDebugStringA(line.c_str());
            release();
            return false;
        }

        packet_ = av_packet_alloc();
        frame_ = av_frame_alloc();
        swFrame_ = av_frame_alloc();
        if (!packet_ || !frame_ || !swFrame_) {
            release();
            return false;
        }

        char line[256];
        std::snprintf(line, sizeof(line),
            "[center-roi-video] FFmpeg D3D11VA zero-copy decoder ready codec=%dx%d low_delay=1 output=GPU texture\n",
            width_, height_);
        OutputDebugStringA(line);
        return true;
    }

    void flush() {
        if (ctx_) avcodec_flush_buffers(ctx_);
        if (packet_) av_packet_unref(packet_);
        if (frame_) av_frame_unref(frame_);
        if (swFrame_) av_frame_unref(swFrame_);
    }

    bool decode(const uint8_t* data, size_t size, int64_t ptsUs, std::vector<DecodedFrame>& frames) {
        if (!ctx_ || !packet_ || !frame_ || !data || size == 0) return false;
        av_packet_unref(packet_);
        packet_->data = const_cast<uint8_t*>(data);
        packet_->size = static_cast<int>(size);
        packet_->pts = ptsUs;
        packet_->dts = ptsUs;

        int err = avcodec_send_packet(ctx_, packet_);
        if (err == AVERROR(EAGAIN)) {
            receiveFrames(frames);
            err = avcodec_send_packet(ctx_, packet_);
        }
        if (err < 0) {
            // Recoverable stream hiccup: this can happen after reconnect/ROI switch when the
            // decoder sees a stale or non-IDR packet before the next codec config + IDR.
            // Do not publish a red diagnostic texture over the ROI; keep the last good frame
            // and let the receiver request a fresh sync frame.
            lastHardFailReason_ = "send packet fail " + AvErrorToString(err);
            std::string line = "[center-roi-video] FFmpeg packet rejected, skip and resync: " + lastHardFailReason_ + "\n";
            OutputDebugStringA(line.c_str());
            flush();
            return false;
        }
        receiveFrames(frames);
        return true;
    }

    bool isHardwareActive() const { return hardwareActive_; }

    DecodedFrame makeDiagnosticFrame(const std::string& reason, int w = 0, int h = 0) {
        lastHardFailReason_ = reason.empty() ? std::string("unknown hard fail") : reason;
        const int outW = (w > 0) ? w : ((width_ > 0) ? width_ : 1280);
        const int outH = (h > 0) ? h : ((height_ > 0) ? height_ : 720);

        std::string line = "[center-roi-video] FFmpeg D3D11VA HARD FAIL: " + lastHardFailReason_ + "\n";
        OutputDebugStringA(line.c_str());

        DecodedFrame frame;
        frame.width = outW;
        frame.height = outH;
        frame.diagnosticFrame = true;
        frame.diagnosticText = ToWide(lastHardFailReason_.c_str());
        frame.gpuFrame = CreateDiagnosticGpuTexture(d3dDevice_, outW, outH, lastHardFailReason_);
        if (!frame.gpuFrame || !frame.gpuFrame->srv) {
            // Diagnostic-only last resort. This is not a video fallback; it only makes the
            // failure visible if a GPU diagnostic texture cannot be created.
            frame.pixelsBGRA = CreateDiagnosticPixels(outW, outH, "gpu diag texture fail " + lastHardFailReason_);
        }
        frame.decodePartCount = 1;
        frame.generation = ++generation_;
        return frame;
    }

private:
    ID3D11Device* d3dDevice_ = nullptr;
    ID3D11DeviceContext* d3dCtx_ = nullptr;
    D3D11GpuVideoConverter gpuConverter_;
    AVCodecContext* ctx_ = nullptr;
    AVBufferRef* hwDeviceCtx_ = nullptr;
    AVPacket* packet_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVFrame* swFrame_ = nullptr;
    SwsContext* sws_ = nullptr;
    int width_ = 0;
    int height_ = 0;
    bool hardwareActive_ = false;
    bool loggedSoftwareOutput_ = false;
    bool loggedZeroCopy_ = false;
    bool loggedGpuConvertFail_ = false;
    bool loggedD3D11FrameFormat_ = false;
    std::string lastHardFailReason_ = "not yet";
    uint64_t generation_ = 2000000000ULL;

    int createHardwareDeviceContext() {
        if (d3dDevice_) {
            AVBufferRef* ref = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
            if (!ref) return AVERROR(ENOMEM);
            AVHWDeviceContext* hwctx = reinterpret_cast<AVHWDeviceContext*>(ref->data);
            if (!hwctx || !hwctx->hwctx) {
                av_buffer_unref(&ref);
                return AVERROR(EINVAL);
            }
            AVD3D11VADeviceContext* d3d11ctx = reinterpret_cast<AVD3D11VADeviceContext*>(hwctx->hwctx);
            d3d11ctx->device = d3dDevice_;
            d3dDevice_->AddRef();
            if (d3dCtx_) {
                d3d11ctx->device_context = d3dCtx_;
                d3dCtx_->AddRef();
            }
            int err = av_hwdevice_ctx_init(ref);
            if (err < 0) {
                av_buffer_unref(&ref);
                return err;
            }
            hwDeviceCtx_ = ref;
            OutputDebugStringA("[center-roi-video] FFmpeg D3D11VA using renderer D3D11 device\n");
            return 0;
        }
        return av_hwdevice_ctx_create(&hwDeviceCtx_, AV_HWDEVICE_TYPE_D3D11VA, nullptr, nullptr, 0);
    }

    void release() {
        if (sws_) {
            sws_freeContext(sws_);
            sws_ = nullptr;
        }
        if (packet_) {
            av_packet_free(&packet_);
            packet_ = nullptr;
        }
        if (frame_) {
            av_frame_free(&frame_);
            frame_ = nullptr;
        }
        if (swFrame_) {
            av_frame_free(&swFrame_);
            swFrame_ = nullptr;
        }
        if (ctx_) {
            avcodec_free_context(&ctx_);
            ctx_ = nullptr;
        }
        if (hwDeviceCtx_) {
            av_buffer_unref(&hwDeviceCtx_);
            hwDeviceCtx_ = nullptr;
        }
        width_ = 0;
        height_ = 0;
        hardwareActive_ = false;
        loggedSoftwareOutput_ = false;
        loggedZeroCopy_ = false;
        loggedGpuConvertFail_ = false;
        loggedD3D11FrameFormat_ = false;
    }

    static AVPixelFormat GetHwFormat(AVCodecContext* ctx, const AVPixelFormat* pixFmts) {
        auto* self = reinterpret_cast<H264FfmpegD3D11vaDecoder*>(ctx ? ctx->opaque : nullptr);
        for (const AVPixelFormat* p = pixFmts; p && *p != AV_PIX_FMT_NONE; ++p) {
            if (IsD3D11vaPixelFormat(*p)) {
                if (self) self->hardwareActive_ = true;
                OutputDebugStringA("[center-roi-video] FFmpeg selected D3D11VA hardware pixel format\n");
                return *p;
            }
        }
        if (self) self->hardwareActive_ = false;
        OutputDebugStringA("[center-roi-video] FFmpeg D3D11VA pixel format unavailable; using software output\n");
        return pixFmts && pixFmts[0] != AV_PIX_FMT_NONE ? pixFmts[0] : AV_PIX_FMT_YUV420P;
    }

    void receiveFrames(std::vector<DecodedFrame>& frames) {
        for (;;) {
            av_frame_unref(frame_);
            int err = avcodec_receive_frame(ctx_, frame_);
            if (err == AVERROR(EAGAIN) || err == AVERROR_EOF) return;
            if (err < 0) {
                // Treat decoder receive errors as transient stream errors.  Publishing a
                // diagnostic frame here makes the user see a red H.264 failure rectangle even
                // though the next IDR normally recovers the stream.
                lastHardFailReason_ = "receive frame fail " + AvErrorToString(err);
                std::string line = "[center-roi-video] FFmpeg receive failed, skip and resync: " + lastHardFailReason_ + "\n";
                OutputDebugStringA(line.c_str());
                flush();
                return;
            }
            if (!appendFrame(frame_, frames)) {
                frames.push_back(makeDiagnosticFrame(lastHardFailReason_));
                return;
            }
        }
    }

    bool appendFrame(AVFrame* srcFrame, std::vector<DecodedFrame>& frames) {
        if (!srcFrame) return false;
        AVFrame* usable = srcFrame;
        const AVPixelFormat srcFmt0 = static_cast<AVPixelFormat>(srcFrame->format);
        if (IsD3D11vaPixelFormat(srcFmt0)) {
            ID3D11Texture2D* decodedTex = nullptr;
            UINT arraySlice = 0;
            bool releaseDecodedTex = false;

            // FFmpeg may output either AV_PIX_FMT_D3D11 or AV_PIX_FMT_D3D11VA_VLD.
            // AV_PIX_FMT_D3D11:       data[0] = ID3D11Texture2D*, data[1] = array slice.
            // AV_PIX_FMT_D3D11VA_VLD: data[0] = ID3D11VideoDecoderOutputView*.
            // The previous code treated both as Texture2D, so VLD failed GPU conversion and fell back to Map.
            if (srcFmt0 == AV_PIX_FMT_D3D11) {
                decodedTex = reinterpret_cast<ID3D11Texture2D*>(srcFrame->data[0]);
                arraySlice = static_cast<UINT>(reinterpret_cast<uintptr_t>(srcFrame->data[1]));
            }
            if (srcFmt0 == AV_PIX_FMT_D3D11VA_VLD) {
                ID3D11VideoDecoderOutputView* outView = reinterpret_cast<ID3D11VideoDecoderOutputView*>(srcFrame->data[0]);
                if (outView) {
                    D3D11_VIDEO_DECODER_OUTPUT_VIEW_DESC vd{};
                    outView->GetDesc(&vd);
                    arraySlice = vd.Texture2D.ArraySlice;
                    ID3D11Resource* res = nullptr;
                    outView->GetResource(&res);
                    if (res) {
                        if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&decodedTex)) && decodedTex) {
                            releaseDecodedTex = true;
                        }
                        res->Release();
                    }
                }
            }

            if (decodedTex && gpuConverter_.available()) {
                D3D11_TEXTURE2D_DESC desc{};
                decodedTex->GetDesc(&desc);
                if (!loggedD3D11FrameFormat_) {
                    char line[256];
                    std::snprintf(line, sizeof(line),
                        "[center-roi-video] FFmpeg D3D11 frame fmt=%d tex=%ux%u arraySlice=%u visible=%dx%d\n",
                        static_cast<int>(srcFmt0), desc.Width, desc.Height, arraySlice, width_, height_);
                    OutputDebugStringA(line);
                    loggedD3D11FrameFormat_ = true;
                }
                auto gpuFrame = gpuConverter_.convert(decodedTex, arraySlice, width_, height_);
                if (releaseDecodedTex) { decodedTex->Release(); decodedTex = nullptr; }
                if (gpuFrame && gpuFrame->srv) {
                    if (!loggedZeroCopy_) {
                        OutputDebugStringA("[center-roi-video] FFmpeg D3D11VA zero-copy frame published as GPU texture\n");
                        loggedZeroCopy_ = true;
                    }
                    DecodedFrame frame;
                    frame.width = width_;
                    frame.height = height_;
                    frame.gpuFrame = gpuFrame;
                    frame.decodePartCount = 1;
                    frame.generation = ++generation_;
                    frames.push_back(std::move(frame));
                    return true;
                }
            } else if (releaseDecodedTex && decodedTex) {
                decodedTex->Release();
                decodedTex = nullptr;
            }
            std::string reason;
            if (!decodedTex) {
                reason = "no d3d11 texture fmt " + std::to_string(static_cast<int>(srcFmt0));
            } else if (!gpuConverter_.available()) {
                reason = "gpu converter unavailable";
            } else {
                reason = "gpu convert failed fmt " + std::to_string(static_cast<int>(srcFmt0));
            }
            if (!loggedGpuConvertFail_) {
                std::string line = "[center-roi-video] FFmpeg D3D11VA HARD FAIL: " + reason + "\n";
                OutputDebugStringA(line.c_str());
                loggedGpuConvertFail_ = true;
            }
            frames.push_back(makeDiagnosticFrame(reason));
            return true;
        } else {
            std::string reason = "software output fmt " + std::to_string(static_cast<int>(srcFmt0));
            if (!loggedSoftwareOutput_) {
                std::string line = "[center-roi-video] FFmpeg D3D11VA HARD FAIL: " + reason + "\n";
                OutputDebugStringA(line.c_str());
                loggedSoftwareOutput_ = true;
            }
            frames.push_back(makeDiagnosticFrame(reason));
            return true;
        }

        const int srcW = usable->width > 0 ? usable->width : width_;
        const int srcH = usable->height > 0 ? usable->height : height_;
        if (srcW <= 0 || srcH <= 0) return false;

        const AVPixelFormat swFmt = static_cast<AVPixelFormat>(usable->format);
        sws_ = sws_getCachedContext(
            sws_, srcW, srcH, swFmt,
            width_, height_, AV_PIX_FMT_BGRA,
            SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
        if (!sws_) {
            OutputDebugStringA("[center-roi-video] FFmpeg sws_getCachedContext failed\n");
            return false;
        }

        auto outPixels = std::make_shared<std::vector<uint8_t>>(static_cast<size_t>(width_) * static_cast<size_t>(height_) * 4u);
        uint8_t* dstData[4] = { outPixels->data(), nullptr, nullptr, nullptr };
        int dstLinesize[4] = { width_ * 4, 0, 0, 0 };
        int scaled = sws_scale(sws_, usable->data, usable->linesize, 0, srcH, dstData, dstLinesize);
        if (scaled <= 0) {
            OutputDebugStringA("[center-roi-video] FFmpeg sws_scale failed\n");
            return false;
        }

        DecodedFrame frame;
        frame.width = width_;
        frame.height = height_;
        frame.pixelsBGRA = std::move(outPixels);
        frame.decodePartCount = 1;
        frame.generation = ++generation_;
        frames.push_back(std::move(frame));
        return true;
    }
};
#endif

class CenterRoiVideoDisplayReceiver::Impl {
public:
    explicit Impl(SharedState& state, ID3D11Device* d3dDevice = nullptr, ID3D11DeviceContext* d3dCtx = nullptr)
        : state_(state), d3dDevice_(d3dDevice), d3dCtx_(d3dCtx) {
        if (d3dDevice_) d3dDevice_->AddRef();
        if (d3dCtx_) d3dCtx_->AddRef();
    }
    ~Impl() {
        stop();
        if (d3dCtx_) { d3dCtx_->Release(); d3dCtx_ = nullptr; }
        if (d3dDevice_) { d3dDevice_->Release(); d3dDevice_ = nullptr; }
    }

    void start() {
        stopped_.store(false);
        thread_ = std::thread([this] { run(); });
    }

    void stop() {
        if (stopped_.exchange(true)) return;
        closesocket(sock_);
        if (thread_.joinable()) thread_.join();
    }

private:
    static constexpr int32_t kMagic = 0x484C5631; // HLV1
    static constexpr int kHeaderV1Size = 56;
    static constexpr int kHeaderV2Size = 112;
    static constexpr int kFlagCodecConfig = 1;
    static constexpr int kFlagKeyFrame = 2;

    SharedState& state_;
    ID3D11Device* d3dDevice_ = nullptr;
    ID3D11DeviceContext* d3dCtx_ = nullptr;
    std::thread thread_;
    std::atomic<bool> stopped_{false};
    SOCKET sock_ = INVALID_SOCKET;

    static bool sendLineToSocketNoLock(SOCKET s, const char* line) {
        if (s == INVALID_SOCKET || line == nullptr || line[0] == '\0') return false;
        const char* p = line;
        int left = static_cast<int>(std::strlen(line));
        while (left > 0) {
            const int n = send(s, p, left, 0);
            if (n <= 0) return false;
            p += n;
            left -= n;
        }
        return true;
    }

    void requestVideoSyncFrameBestEffort(SOCKET currentSocket) {
        // CenterRoiVideoStreamSender treats HLVIDEO/HLVIDPRESET as a sync-frame request.
        // Broadcast to both channels because after reconnect one side may be ready before the other.
        constexpr const char* kCmd = "HLVIDEO\n";
        std::lock_guard<std::mutex> lk(state_.controlSocketMutex);
        if (currentSocket != INVALID_SOCKET) {
            (void)sendLineToSocketNoLock(currentSocket, kCmd);
        }
        if (state_.videoControlSocket != INVALID_SOCKET && state_.videoControlSocket != currentSocket) {
            (void)sendLineToSocketNoLock(state_.videoControlSocket, kCmd);
        }
        if (state_.controlSocket != INVALID_SOCKET && state_.controlSocket != currentSocket && state_.controlSocket != state_.videoControlSocket) {
            (void)sendLineToSocketNoLock(state_.controlSocket, kCmd);
        }
    }

    void publishFrame(DecodedFrame&& frame, int fullW, int fullH, int roiL, int roiT, int roiW, int roiH, int codecW, int codecH, int64_t ptsUs, double decodeMs) {
        frame.decodeMs = decodeMs;
        frame.decodeCpuSumMs = decodeMs;
        frame.decodeMaxPartMs = decodeMs;
        frame.decodeTailWaitMs = 0.0;
        frame.decodeOverlapSavedMs = 0.0;
        frame.centerRoiOverlay = true;
        frame.centerFullWidth = fullW;
        frame.centerFullHeight = fullH;
        frame.centerRoiLeft = roiL;
        frame.centerRoiTop = roiT;
        frame.centerRoiWidth = roiW;
        frame.centerRoiHeight = roiH;
        frame.centerCodecWidth = codecW;
        frame.centerCodecHeight = codecH;
        frame.frameProducedNs = ptsUs > 0 ? ptsUs * 1000LL : 0LL;
        const bool isGpuFrame = frame.gpuFrame && frame.gpuFrame->srv;
        const bool isDiagnostic = frame.diagnosticFrame;
        const std::wstring diagnosticText = frame.diagnosticText;
        {
            std::lock_guard<std::mutex> lk(state_.mutex);
            // Do not overwrite the JPEG background frame. The renderer consumes this
            // as a separate overlay and composites it at roiL/roiT/roiW/roiH.
            state_.latestCenterRoi = std::move(frame);
            state_.hasCenterRoiFrame = true;
            if (isDiagnostic) {
                state_.status = L"Hybrid JPEG + ROI: FFmpeg D3D11VA HARD FAIL: " + diagnosticText;
            } else {
                state_.status = isGpuFrame ? L"Hybrid JPEG background + H.264 center ROI GPU" : L"Hybrid JPEG background + H.264 center ROI CPU/diagnostic";
            }
        }
        if (state_.frameReadyEvent) SetEvent(state_.frameReadyEvent);
    }

    void run() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        std::vector<uint8_t> header(kHeaderV2Size);
        std::vector<uint8_t> payload;
#if HUILANG_ENABLE_FFMPEG_D3D11VA
        H264FfmpegD3D11vaDecoder ffmpegDecoder(d3dDevice_, d3dCtx_);
        bool ffmpegDisabledForThisConnection = false;
#endif
        H264MfDecoder decoder;

        while (!stopped_.load() && !state_.stop.load()) {
            const AdbSetupResult adb = RunAdbCenterRoiVideoForward();
            if (!adb.ok) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                continue;
            }

            SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (s == INVALID_SOCKET) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                continue;
            }

            BOOL noDelay = TRUE;
            setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));
            // Keep enough room for a few large ROI access units.  A too-small buffer can
            // make reconnect/ROI-switch bursts look like corrupt H.264 input to FFmpeg.
            int rcvBuf = 4 * 1024 * 1024;
            setsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&rcvBuf), sizeof(rcvBuf));

            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(CENTER_ROI_VIDEO_PORT);
            addr.sin_addr.s_addr = inet_addr("127.0.0.1");

            if (connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
                closesocket(s);
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }

            sock_ = s;
            {
                std::lock_guard<std::mutex> lk(state_.controlSocketMutex);
                state_.videoControlSocket = s;
            }
            // Do not dump H.264 in the low-latency display path; disk I/O adds jitter.
            FILE* dump = nullptr;
            int packets = 0;
            int decoded = 0;
            int droppedForLatency = 0;
            uint64_t bytes = 0;
            double decodeMsSum = 0.0;
            int decodeMsSamples = 0;
            int lastDecoderMode = 0;
            double h264BeforeQueueMsSum = 0.0;
            double h264UploadMsSum = 0.0;
            double h264SwapMsSum = 0.0;
            double h264CodecPipeMsSum = 0.0;
            double h264WriteWaitMsSum = 0.0;
            double h264SocketWriteMsSum = 0.0;
            double h264AndroidTotalMsSum = 0.0;
            int h264TimingSamples = 0;
            auto windowStart = std::chrono::steady_clock::now();

            OutputDebugStringA("[center-roi-video] connected; 120fps latest-only H.264 display, dump disabled\n");
            requestVideoSyncFrameBestEffort(s);

            while (!stopped_.load() && !state_.stop.load()) {
                if (!RecvAll(s, header.data(), kHeaderV1Size)) break;
                const int32_t magic = ReadBE32(header.data() + 0);
                const int32_t version = ReadBE32(header.data() + 4);
                if (magic != kMagic || version < 1 || version > 2) {
                    OutputDebugStringA("[center-roi-video] invalid packet magic/version\n");
                    break;
                }
                if (version >= 2) {
                    if (!RecvAll(s, header.data() + kHeaderV1Size, kHeaderV2Size - kHeaderV1Size)) break;
                }

                const int32_t flags = ReadBE32(header.data() + 8);
                const int32_t fullW = ReadBE32(header.data() + 12);
                const int32_t fullH = ReadBE32(header.data() + 16);
                const int32_t roiL = ReadBE32(header.data() + 20);
                const int32_t roiT = ReadBE32(header.data() + 24);
                const int32_t roiW = ReadBE32(header.data() + 28);
                const int32_t roiH = ReadBE32(header.data() + 32);
                const int32_t codecW = ReadBE32(header.data() + 36);
                const int32_t codecH = ReadBE32(header.data() + 40);
                const int32_t payloadSize = ReadBE32(header.data() + 44);
                const int64_t ptsUs = ReadBE64(header.data() + 48);

                int64_t h264FrameProducedNs = 0;
                int64_t h264QueueStartNs = 0;
                int64_t h264UploadDoneNs = 0;
                int64_t h264SwapDoneNs = 0;
                int64_t h264OutputNs = 0;
                int64_t h264WriteStartNs = 0;
                int64_t h264WriteEndNs = 0;
                if (version >= 2) {
                    h264FrameProducedNs = ReadBE64(header.data() + 56);
                    h264QueueStartNs = ReadBE64(header.data() + 64);
                    h264UploadDoneNs = ReadBE64(header.data() + 72);
                    h264SwapDoneNs = ReadBE64(header.data() + 80);
                    h264OutputNs = ReadBE64(header.data() + 88);
                    h264WriteStartNs = ReadBE64(header.data() + 96);
                    h264WriteEndNs = ReadBE64(header.data() + 104);
                }

                if (payloadSize <= 0 || payloadSize > 4 * 1024 * 1024 || codecW <= 0 || codecH <= 0) {
                    OutputDebugStringA("[center-roi-video] invalid packet payload/codec\n");
                    break;
                }
                payload.resize(static_cast<size_t>(payloadSize));
                if (!RecvAll(s, payload.data(), payloadSize)) break;

                ++packets;
                bytes += payload.size();

                if ((flags & kFlagCodecConfig) == 0 && version >= 2) {
                    bool timingOk = true;
                    timingOk = timingOk && h264FrameProducedNs > 0;
                    timingOk = timingOk && h264QueueStartNs >= h264FrameProducedNs;
                    timingOk = timingOk && h264UploadDoneNs >= h264QueueStartNs;
                    timingOk = timingOk && h264SwapDoneNs >= h264UploadDoneNs;
                    timingOk = timingOk && h264OutputNs >= h264SwapDoneNs;
                    timingOk = timingOk && h264WriteStartNs >= h264OutputNs;
                    timingOk = timingOk && h264WriteEndNs >= h264WriteStartNs;
                    if (timingOk) {
                        h264BeforeQueueMsSum += double(h264QueueStartNs - h264FrameProducedNs) / 1000000.0;
                        h264UploadMsSum += double(h264UploadDoneNs - h264QueueStartNs) / 1000000.0;
                        h264SwapMsSum += double(h264SwapDoneNs - h264UploadDoneNs) / 1000000.0;
                        h264CodecPipeMsSum += double(h264OutputNs - h264SwapDoneNs) / 1000000.0;
                        h264WriteWaitMsSum += double(h264WriteStartNs - h264OutputNs) / 1000000.0;
                        h264SocketWriteMsSum += double(h264WriteEndNs - h264WriteStartNs) / 1000000.0;
                        h264AndroidTotalMsSum += double(h264WriteEndNs - h264FrameProducedNs) / 1000000.0;
                        ++h264TimingSamples;
                    }
                }

                if (dump) std::fwrite(payload.data(), 1, payload.size(), dump);

                // The Android sender is configured as all-IDR for no-ghost low latency.  Every
                // non-config access unit is independently decodable, so if another packet is
                // already queued in the TCP buffer, this one is already stale.  Drop it before
                // feeding Media Foundation; otherwise the decoder faithfully displays old frames
                // and adds 1+ frame of latency.
                if ((flags & kFlagCodecConfig) == 0 && !state_.centerRoiStrictSync.load(std::memory_order_acquire) && SocketPendingBytes(s) >= kHeaderV1Size) {
                    ++droppedForLatency;
                    continue;
                }

                std::vector<DecodedFrame> frames;
                const int64_t decodeStart = NowNs();
                bool ok = false;
#if HUILANG_ENABLE_FFMPEG_D3D11VA
                (void)ffmpegDisabledForThisConnection;
                if (ffmpegDecoder.ensure(codecW, codecH)) {
                    ok = ffmpegDecoder.decode(payload.data(), payload.size(), ptsUs, frames);
                    if (!ok) {
                        // Transient bad/stale packet.  Keep the last good ROI frame visible,
                        // flush decoder state, and ask Android for a fresh sync frame instead
                        // of publishing the red diagnostic texture.
                        frames.clear();
                        ffmpegDecoder.flush();
                        requestVideoSyncFrameBestEffort(s);
                        continue;
                    }
                    if ((flags & kFlagCodecConfig) == 0) {
                        bool hasGpuVideo = false;
                        bool hasDiagnostic = false;
                        for (const auto& f : frames) {
                            if (f.diagnosticFrame) hasDiagnostic = true;
                            if (!f.diagnosticFrame && f.gpuFrame && f.gpuFrame->srv) hasGpuVideo = true;
                        }
                        if (!hasGpuVideo && !hasDiagnostic) {
                            frames.clear();
                            frames.push_back(ffmpegDecoder.makeDiagnosticFrame("no gpu texture published", codecW, codecH));
                            ok = true;
                        }
                    }
                } else {
                    frames.clear();
                    frames.push_back(ffmpegDecoder.makeDiagnosticFrame("ensure d3d11va fail", codecW, codecH));
                    ok = true;
                }
#else
                {
                    if (!decoder.ensure(codecW, codecH)) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                        continue;
                    }
                    ok = decoder.decode(payload.data(), payload.size(), ptsUs, frames);
                    if (!ok && (flags & kFlagCodecConfig) != 0) {
                        decoder.flush();
                    }
                }
#endif
                const double decodeMs = DiffMs(NowNs(), decodeStart);
                int packetDecoderMode = 0;
#if HUILANG_ENABLE_FFMPEG_D3D11VA
                packetDecoderMode = 4; // FFmpeg compiled but frame was not GPU unless proven below.
                for (const auto& f : frames) {
                    if (f.diagnosticFrame) {
                        packetDecoderMode = 2;
                        break;
                    }
                    if (f.gpuFrame && f.gpuFrame->srv) {
                        packetDecoderMode = 1;
                    }
                }
#else
                packetDecoderMode = 3; // Media Foundation path.
#endif
                if ((flags & kFlagCodecConfig) == 0 && !frames.empty()) {
                    decodeMsSum += decodeMs;
                    ++decodeMsSamples;
                    lastDecoderMode = packetDecoderMode;
                }
                for (auto& f : frames) {
                    publishFrame(std::move(f), fullW, fullH, roiL, roiT, roiW, roiH, codecW, codecH, ptsUs, decodeMs);
                    ++decoded;
                }

                auto now = std::chrono::steady_clock::now();
                double elapsed = std::chrono::duration<double>(now - windowStart).count();
                if (elapsed >= 1.0) {
                    const double fps = decoded / elapsed;
                    const double mbps = (double(bytes) * 8.0 / 1000000.0) / elapsed;
                    const double avgKb = packets > 0 ? double(bytes) / 1024.0 / double(packets) : 0.0;
                    {
                        std::lock_guard<std::mutex> lk(state_.mutex);
                        state_.centerVideoFps = fps;
                        state_.centerVideoMbps = mbps;
                        state_.centerVideoAvgKb = avgKb;
                        state_.centerVideoDropFps = droppedForLatency / elapsed;
                        state_.centerVideoDecodeMs = decodeMsSamples > 0 ? (decodeMsSum / double(decodeMsSamples)) : 0.0;
                        state_.centerVideoBeforeQueueMs = h264TimingSamples > 0 ? (h264BeforeQueueMsSum / double(h264TimingSamples)) : 0.0;
                        state_.centerVideoUploadMs = h264TimingSamples > 0 ? (h264UploadMsSum / double(h264TimingSamples)) : 0.0;
                        state_.centerVideoSwapMs = h264TimingSamples > 0 ? (h264SwapMsSum / double(h264TimingSamples)) : 0.0;
                        state_.centerVideoCodecPipeMs = h264TimingSamples > 0 ? (h264CodecPipeMsSum / double(h264TimingSamples)) : 0.0;
                        state_.centerVideoWriteWaitMs = h264TimingSamples > 0 ? (h264WriteWaitMsSum / double(h264TimingSamples)) : 0.0;
                        state_.centerVideoSocketWriteMs = h264TimingSamples > 0 ? (h264SocketWriteMsSum / double(h264TimingSamples)) : 0.0;
                        state_.centerVideoAndroidTotalMs = h264TimingSamples > 0 ? (h264AndroidTotalMsSum / double(h264TimingSamples)) : 0.0;
                        state_.centerVideoTimingSamples = h264TimingSamples;
                        state_.centerVideoDecoderMode = lastDecoderMode;
                        state_.centerVideoFullWidth = fullW;
                        state_.centerVideoFullHeight = fullH;
                        state_.centerVideoRoiLeft = roiL;
                        state_.centerVideoRoiTop = roiT;
                        state_.centerVideoRoiWidth = roiW;
                        state_.centerVideoRoiHeight = roiH;
                        state_.centerVideoCodecWidth = codecW;
                        state_.centerVideoCodecHeight = codecH;
                    }
                    char line[448];
                    std::snprintf(line, sizeof(line),
                        "[center-roi-video] packets=%.1f decoded=%.1f drop=%.1f mbps=%.2f avgKB=%.1f decodeMs=%.3f h264Ms pre=%.2f upload=%.2f swap=%.2f pipe=%.2f write=%.2f total=%.2f samples=%d mode=%d full=%dx%d roi=%d,%d %dx%d codec=%dx%d flags=%d\n",
                        packets / elapsed,
                        fps,
                        droppedForLatency / elapsed,
                        mbps,
                        avgKb,
                        decodeMsSamples > 0 ? (decodeMsSum / double(decodeMsSamples)) : 0.0,
                        h264TimingSamples > 0 ? (h264BeforeQueueMsSum / double(h264TimingSamples)) : 0.0,
                        h264TimingSamples > 0 ? (h264UploadMsSum / double(h264TimingSamples)) : 0.0,
                        h264TimingSamples > 0 ? (h264SwapMsSum / double(h264TimingSamples)) : 0.0,
                        h264TimingSamples > 0 ? (h264CodecPipeMsSum / double(h264TimingSamples)) : 0.0,
                        h264TimingSamples > 0 ? ((h264WriteWaitMsSum + h264SocketWriteMsSum) / double(h264TimingSamples)) : 0.0,
                        h264TimingSamples > 0 ? (h264AndroidTotalMsSum / double(h264TimingSamples)) : 0.0,
                        h264TimingSamples,
                        lastDecoderMode,
                        fullW, fullH, roiL, roiT, roiW, roiH, codecW, codecH, flags);
                    OutputDebugStringA(line);
                    packets = 0;
                    decoded = 0;
                    droppedForLatency = 0;
                    bytes = 0;
                    decodeMsSum = 0.0;
                    decodeMsSamples = 0;
                    h264BeforeQueueMsSum = 0.0;
                    h264UploadMsSum = 0.0;
                    h264SwapMsSum = 0.0;
                    h264CodecPipeMsSum = 0.0;
                    h264WriteWaitMsSum = 0.0;
                    h264SocketWriteMsSum = 0.0;
                    h264AndroidTotalMsSum = 0.0;
                    h264TimingSamples = 0;
                    windowStart = now;
                }
            }

            if (dump) std::fclose(dump);
            {
                std::lock_guard<std::mutex> lk(state_.controlSocketMutex);
                if (state_.videoControlSocket == s) state_.videoControlSocket = INVALID_SOCKET;
            }
            closesocket(s);
            sock_ = INVALID_SOCKET;
#if HUILANG_ENABLE_FFMPEG_D3D11VA
            ffmpegDecoder.flush();
            ffmpegDisabledForThisConnection = false;
#endif
            decoder.flush();
            OutputDebugStringA("[center-roi-video] disconnected\n");
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
        }
        CoUninitialize();
    }
};



CenterRoiVideoDisplayReceiver::CenterRoiVideoDisplayReceiver(SharedState& state, ID3D11Device* d3dDevice, ID3D11DeviceContext* d3dCtx)
    : impl_(std::make_unique<Impl>(state, d3dDevice, d3dCtx)) {}

CenterRoiVideoDisplayReceiver::~CenterRoiVideoDisplayReceiver() = default;

void CenterRoiVideoDisplayReceiver::start() {
    impl_->start();
}

void CenterRoiVideoDisplayReceiver::stop() {
    impl_->stop();
}
