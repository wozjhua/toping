#include "d3d11_renderer.h"

// D3D11 device, swap chain, shader resources, and window sizing.

// D3D11Renderer device, texture upload and present path.

namespace {
struct FsrUpscaleCBLocal {
    float srcSize[2];
    float dstSize[2];
    float srcTexelSize[2];
    float sharpness;
    float padding;
};
static_assert((sizeof(FsrUpscaleCBLocal) % 16) == 0, "D3D11 constant buffers must be 16-byte aligned.");

bool CompilePixelShaderLocal(ID3D11Device* device, const char* source, const char* entry, ID3D11PixelShader** outShader) {
    if (!device || !source || !entry || !outShader) return false;
    *outShader = nullptr;

    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
    flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif

    ID3DBlob* bytecode = nullptr;
    ID3DBlob* errors = nullptr;
    HRESULT hr = D3DCompile(
        source,
        std::strlen(source),
        nullptr,
        nullptr,
        nullptr,
        entry,
        "ps_4_0",
        flags,
        0,
        &bytecode,
        &errors);

    if (FAILED(hr)) {
        if (errors) {
            OutputDebugStringA("[HuiLang] FSR-style shader compile failed:\n");
            OutputDebugStringA(static_cast<const char*>(errors->GetBufferPointer()));
            OutputDebugStringA("\n");
            errors->Release();
        }
        if (bytecode) bytecode->Release();
        return false;
    }
    if (errors) errors->Release();

    hr = device->CreatePixelShader(bytecode->GetBufferPointer(), bytecode->GetBufferSize(), nullptr, outShader);
    bytecode->Release();
    return SUCCEEDED(hr);
}

const char* kFsrEasuLikePs = R"HLSL(
Texture2D gTex : register(t0);
SamplerState gSampler : register(s0);

cbuffer FsrUpscaleCB : register(b0)
{
    float2 gSrcSize;
    float2 gDstSize;
    float2 gSrcTexelSize;
    float  gSharpness;
    float  gPadding;
};

struct PSIn
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

float3 LoadSrc(int2 p)
{
    int2 maxP = max(int2(gSrcSize) - int2(1, 1), int2(0, 0));
    p = clamp(p, int2(0, 0), maxP);
    return gTex.Load(int3(p, 0)).rgb;
}

float4 CubicWeights(float f)
{
    float f2 = f * f;
    float f3 = f2 * f;
    return float4(
        -0.5 * f3 + f2 - 0.5 * f,
         1.5 * f3 - 2.5 * f2 + 1.0,
        -1.5 * f3 + 2.0 * f2 + 0.5 * f,
         0.5 * f3 - 0.5 * f2);
}

float Luma(float3 c)
{
    return dot(c, float3(0.299, 0.587, 0.114));
}

float4 main(PSIn i) : SV_Target
{
    // Catmull-Rom upscale with anti-ringing.  This is not AMD's exact FSR EASU code;
    // it is a low-latency FSR1-style spatial upscale pass suitable for video frames.
    float2 srcPos = i.uv * gSrcSize - 0.5;
    int2 base = int2(floor(srcPos));
    float2 f = frac(srcPos);

    float4 wx = CubicWeights(f.x);
    float4 wy = CubicWeights(f.y);

    float3 sum = 0.0;
    [unroll] for (int yy = 0; yy < 4; ++yy) {
        [unroll] for (int xx = 0; xx < 4; ++xx) {
            sum += LoadSrc(base + int2(xx - 1, yy - 1)) * wx[xx] * wy[yy];
        }
    }

    // Clamp against the immediate 2x2 footprint to reduce halos around UI/text.
    float3 c00 = LoadSrc(base + int2(0, 0));
    float3 c10 = LoadSrc(base + int2(1, 0));
    float3 c01 = LoadSrc(base + int2(0, 1));
    float3 c11 = LoadSrc(base + int2(1, 1));
    float3 mn = min(min(c00, c10), min(c01, c11));
    float3 mx = max(max(c00, c10), max(c01, c11));
    float3 range = mx - mn;

    // Slight edge-preserving blend: on very flat regions prefer bilinear, on edges keep Catmull-Rom detail.
    float lumRange = max(max(range.r, range.g), range.b);
    float edge = saturate(lumRange * 8.0);
    float3 bilinear = gTex.SampleLevel(gSampler, i.uv, 0).rgb;
    float3 color = lerp(bilinear, sum, edge);

    color = clamp(color, mn - range * 0.06, mx + range * 0.06);
    return float4(saturate(color), 1.0);
}
)HLSL";

const char* kFsrRcasLikePs = R"HLSL(
Texture2D gTex : register(t0);
SamplerState gSampler : register(s0);

cbuffer FsrUpscaleCB : register(b0)
{
    float2 gSrcSize;
    float2 gDstSize;
    float2 gSrcTexelSize;
    float  gSharpness;
    float  gPadding;
};

struct PSIn
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

float3 SampleRgb(float2 uv)
{
    return gTex.SampleLevel(gSampler, uv, 0).rgb;
}

float Luma(float3 c)
{
    return dot(c, float3(0.299, 0.587, 0.114));
}

float4 main(PSIn i) : SV_Target
{
    // RCAS-like adaptive sharpening.  It avoids boosting very flat/noisy regions too much.
    float2 t = 1.0 / max(gDstSize, float2(1.0, 1.0));
    float3 c = SampleRgb(i.uv);
    float3 l = SampleRgb(i.uv + float2(-t.x, 0.0));
    float3 r = SampleRgb(i.uv + float2( t.x, 0.0));
    float3 u = SampleRgb(i.uv + float2(0.0, -t.y));
    float3 d = SampleRgb(i.uv + float2(0.0,  t.y));

    float3 mn = min(c, min(min(l, r), min(u, d)));
    float3 mx = max(c, max(max(l, r), max(u, d)));
    float3 range = mx - mn;
    float lumRange = max(max(range.r, range.g), range.b);

    float3 blur = (l + r + u + d) * 0.25;
    float3 detail = c - blur;

    float adaptive = saturate((lumRange - 0.01) * 7.0);
    float strength = gSharpness * adaptive;
    float3 color = c + detail * strength;

    color = clamp(color, mn - range * 0.18, mx + range * 0.18);
    return float4(saturate(color), 1.0);
}
)HLSL";
} // namespace


bool D3D11Renderer::createDevice() {
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
#if defined(_DEBUG)
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    D3D_FEATURE_LEVEL fl{};
    D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    HRESULT hr = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        flags,
        levels,
        ARRAYSIZE(levels),
        D3D11_SDK_VERSION,
        &device_,
        &fl,
        &ctx_
    );
    if (FAILED(hr)) return false;
    {
        ID3D10Multithread* mt = nullptr;
        if (SUCCEEDED(device_->QueryInterface(__uuidof(ID3D10Multithread), (void**)&mt)) && mt) {
            mt->SetMultithreadProtected(TRUE);
            mt->Release();
        }
    }

    IDXGIDevice* dxgiDevice{};
    IDXGIAdapter* adapter{};
    IDXGIFactory2* factory{};
    hr = device_->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice);
    if (FAILED(hr)) return false;
    hr = dxgiDevice->GetAdapter(&adapter);
    dxgiDevice->Release();
    if (FAILED(hr)) return false;
    hr = adapter->GetParent(__uuidof(IDXGIFactory2), (void**)&factory);
    adapter->Release();
    if (FAILED(hr)) return false;

    // Enable the low-latency tearing path when the OS/driver supports it.  Without
    // this, a flip-model window can still be paced by DWM and disp may stick near 60/100
    // even when recv is already 115~120.
    BOOL allowTearing = FALSE;
    IDXGIFactory5* factory5{};
    if (SUCCEEDED(factory->QueryInterface(__uuidof(IDXGIFactory5), (void**)&factory5)) && factory5) {
        if (FAILED(factory5->CheckFeatureSupport(
            DXGI_FEATURE_PRESENT_ALLOW_TEARING,
            &allowTearing,
            sizeof(allowTearing)))) {
            allowTearing = FALSE;
        }
        factory5->Release();
    }
    allowTearing_ = (allowTearing == TRUE);

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = 0;
    desc.Height = 0;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.Stereo = FALSE;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 3;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    desc.Flags = allowTearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;

    hr = factory->CreateSwapChainForHwnd(device_, hwnd_, &desc, nullptr, nullptr, &swapChain_);
    factory->Release();
    if (FAILED(hr)) return false;

    hr = swapChain_->QueryInterface(__uuidof(IDXGISwapChain2), (void**)&swapChain2_);
    if (SUCCEEDED(hr) && swapChain2_) {
        swapChain2_->SetMaximumFrameLatency(1);
    }

    recreateRTV();

    D3D11_BUFFER_DESC vb{};
    vb.ByteWidth = sizeof(Vertex) * 6;
    vb.Usage = D3D11_USAGE_DYNAMIC;
    vb.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    vb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = device_->CreateBuffer(&vb, nullptr, &vertexBuffer_);
    if (FAILED(hr)) return false;
    hr = device_->CreateBuffer(&vb, nullptr, &roiVertexBuffer_);
    return SUCCEEDED(hr);
}

// recreateRTV
void D3D11Renderer::recreateRTV() {
    if (!swapChain_) return;
    if (rtv_) { rtv_->Release(); rtv_ = nullptr; }
    ctx_->OMSetRenderTargets(0, nullptr, nullptr);
    swapChain_->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN,
        allowTearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
    ID3D11Texture2D* backBuffer{};
    HRESULT hr = swapChain_->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backBuffer);
    if (SUCCEEDED(hr)) {
        device_->CreateRenderTargetView(backBuffer, nullptr, &rtv_);
        backBuffer->Release();
    }
}

// releaseFsrResources
void D3D11Renderer::releaseFsrResources() {
    if (ctx_) {
        ID3D11ShaderResourceView* nullSrv[1] = { nullptr };
        ctx_->PSSetShaderResources(0, 1, nullSrv);
        ctx_->OMSetRenderTargets(0, nullptr, nullptr);
    }
    if (fsrSrv_) { fsrSrv_->Release(); fsrSrv_ = nullptr; }
    if (fsrRtv_) { fsrRtv_->Release(); fsrRtv_ = nullptr; }
    if (fsrTex_) { fsrTex_->Release(); fsrTex_ = nullptr; }
    fsrTexWidth_ = 0;
    fsrTexHeight_ = 0;
}

// ensureFsrResources
bool D3D11Renderer::ensureFsrResources(int width, int height) {
    width = (std::max)(1, width);
    height = (std::max)(1, height);
    if (fsrTex_ && fsrRtv_ && fsrSrv_ && fsrTexWidth_ == width && fsrTexHeight_ == height) {
        return true;
    }

    releaseFsrResources();

    D3D11_TEXTURE2D_DESC td{};
    td.Width = static_cast<UINT>(width);
    td.Height = static_cast<UINT>(height);
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    HRESULT hr = device_->CreateTexture2D(&td, nullptr, &fsrTex_);
    if (FAILED(hr)) {
        releaseFsrResources();
        return false;
    }
    hr = device_->CreateRenderTargetView(fsrTex_, nullptr, &fsrRtv_);
    if (FAILED(hr)) {
        releaseFsrResources();
        return false;
    }
    hr = device_->CreateShaderResourceView(fsrTex_, nullptr, &fsrSrv_);
    if (FAILED(hr)) {
        releaseFsrResources();
        return false;
    }

    fsrTexWidth_ = width;
    fsrTexHeight_ = height;
    return true;
}

// createShaders
bool D3D11Renderer::createShaders() {
    HRESULT hr = device_->CreateVertexShader(g_MirrorVSBytecode, g_MirrorVSBytecodeSize, nullptr, &vs_);
    if (FAILED(hr)) {
        return false;
    }

    hr = device_->CreatePixelShader(g_MirrorPSBytecode, g_MirrorPSBytecodeSize, nullptr, &ps_);
    if (FAILED(hr)) {
        if (vs_) { vs_->Release(); vs_ = nullptr; }
        return false;
    }

    D3D11_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    hr = device_->CreateInputLayout(
        layout,
        ARRAYSIZE(layout),
        g_MirrorVSBytecode,
        g_MirrorVSBytecodeSize,
        &inputLayout_
    );
    if (FAILED(hr)) {
        if (ps_) { ps_->Release(); ps_ = nullptr; }
        if (vs_) { vs_->Release(); vs_ = nullptr; }
        return false;
    }

    const bool easuOk = CompilePixelShaderLocal(device_, kFsrEasuLikePs, "main", &psFsrEasu_);
    const bool rcasOk = CompilePixelShaderLocal(device_, kFsrRcasLikePs, "main", &psFsrRcas_);

    D3D11_BUFFER_DESC cbd{};
    cbd.ByteWidth = sizeof(FsrUpscaleCBLocal);
    cbd.Usage = D3D11_USAGE_DEFAULT;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    const bool cbOk = SUCCEEDED(device_->CreateBuffer(&cbd, nullptr, &fsrCb_));

    if (!easuOk || !rcasOk || !cbOk) {
        if (psFsrEasu_) { psFsrEasu_->Release(); psFsrEasu_ = nullptr; }
        if (psFsrRcas_) { psFsrRcas_->Release(); psFsrRcas_ = nullptr; }
        if (fsrCb_) { fsrCb_->Release(); fsrCb_ = nullptr; }
        enableFsrUpscale_ = false;
        OutputDebugStringW(HLW(L"[HuiLang] FSR-style upscale disabled because shader/cbuffer creation failed.\n"));
    }

    return true;
}

// createSampler
bool D3D11Renderer::createSampler() {
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    return SUCCEEDED(device_->CreateSamplerState(&sd, &sampler_));
}
// fitWindowToFrameIfNeeded
void D3D11Renderer::fitWindowToFrameIfNeeded() {
        if (fullscreen_ || windowSizedToFrame_ || currentFrame_.width <= 0 || currentFrame_.height <= 0) return;

        MONITORINFO mi{ sizeof(mi) };
        GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &mi);

        const LONG workW = mi.rcWork.right - mi.rcWork.left;
        const LONG workH = mi.rcWork.bottom - mi.rcWork.top;

        float targetW = static_cast<float>(currentFrame_.width);
        float targetH = static_cast<float>(currentFrame_.height);

        const float scale = (std::min)(
            1.0f,
            (std::min)(workW * 0.92f / targetW, workH * 0.88f / targetH)
            );

        targetW *= scale;
        targetH *= scale;

        RECT wr{ 0, 0, (LONG)targetW, (LONG)targetH };
        DWORD style = (DWORD)GetWindowLongW(hwnd_, GWL_STYLE);
        DWORD exStyle = (DWORD)GetWindowLongW(hwnd_, GWL_EXSTYLE);
        AdjustWindowRectEx(&wr, style, FALSE, exStyle);

        const LONG outerW = wr.right - wr.left;
        const LONG outerH = wr.bottom - wr.top;
        const LONG x = mi.rcWork.left + (workW - outerW) / 2;
        const LONG y = mi.rcWork.top + (workH - outerH) / 2;

        SetWindowPos(hwnd_, nullptr, x, y, outerW, outerH, SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        windowSizedToFrame_ = true;
        dirtyWindow_ = true;
    }

