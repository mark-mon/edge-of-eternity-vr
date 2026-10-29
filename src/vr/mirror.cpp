// Desktop window mirror: draws the headset's left eye (centre-cropped to the window shape, flipped upright) into the
// game's back buffer right before IDXGISwapChain::Present. Used with desktop_view = eye, where the game's own world
// render is skipped. Runs on Unity's render thread (Present is called there).
#include "mirror.h"
#include "../common/log.h"
#include "MinHook.h"
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <atomic>
#include <mutex>

namespace uxr::mirror {
namespace {

typedef HRESULT(STDMETHODCALLTYPE* Present_t)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE* Present1_t)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
Present_t o_Present = nullptr;
Present1_t o_Present1 = nullptr;

std::mutex g_m;
ID3D11Texture2D* g_src = nullptr;         // AddRef'd
DXGI_FORMAT g_srcFmt = DXGI_FORMAT_UNKNOWN;
std::atomic<bool> g_on{false};
std::atomic<uint32_t> s_draws{0};

// render-thread resources (game device)
ID3D11Device* g_dev = nullptr;
ID3D11VertexShader* g_vs = nullptr;
ID3D11PixelShader* g_ps = nullptr;
ID3D11Buffer* g_cb = nullptr;
ID3D11SamplerState* g_smp = nullptr;
ID3D11RasterizerState* g_rs = nullptr;
ID3D11BlendState* g_bs = nullptr;
ID3D11DepthStencilState* g_ds = nullptr;
bool g_failed = false;
ID3D11Texture2D* g_srvTex = nullptr;
ID3D11ShaderResourceView* g_srv = nullptr;
DXGI_FORMAT g_srvFmt = DXGI_FORMAT_UNKNOWN;
ID3D11Texture2D* g_rtvTex = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;

template <class T> void Rel(T*& p) { if (p) { p->Release(); p = nullptr; } }

const char kHlsl[] =
    "Texture2D<float4> src : register(t0); SamplerState smp : register(s0);\n"
    "cbuffer C : register(b0) { float4 so; };\n"   // xy = uv scale, zw = uv offset (of the upright image)
    "struct V { float4 p : SV_Position; float2 uv : TEXCOORD0; };\n"
    "V vs(uint id : SV_VertexID) { V o; float2 t = float2((id << 1) & 2, id & 2);\n"
    "  o.p = float4(t * float2(2, -2) + float2(-1, 1), 0, 1); o.uv = t; return o; }\n"
    "float4 ps(V i) : SV_Target { float2 uv = i.uv * so.xy + so.zw; uv.y = 1 - uv.y;\n"   // Unity RTs are upside down
    "  return float4(src.SampleLevel(smp, uv, 0).rgb, 1); }\n";

bool InitRes(ID3D11Device* dev) {
    if (g_dev == dev && g_vs) return true;
    if (g_failed) return false;
    HMODULE dc = LoadLibraryW(L"d3dcompiler_47.dll");
    auto compile = dc ? (pD3DCompile)GetProcAddress(dc, "D3DCompile") : nullptr;
    if (!compile) { g_failed = true; LOGE("mirror: no d3dcompiler_47"); return false; }
    ID3DBlob *vsb = nullptr, *psb = nullptr, *err = nullptr;
    if (FAILED(compile(kHlsl, sizeof(kHlsl) - 1, "mirror", nullptr, nullptr, "vs", "vs_5_0", 0, 0, &vsb, &err)) ||
        FAILED(compile(kHlsl, sizeof(kHlsl) - 1, "mirror", nullptr, nullptr, "ps", "ps_5_0", 0, 0, &psb, &err))) {
        LOGE("mirror: shader compile failed: %s", err ? (const char*)err->GetBufferPointer() : "?");
        g_failed = true;
        return false;
    }
    g_dev = dev;
    dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &g_vs);
    dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &g_ps);
    vsb->Release(); psb->Release();
    D3D11_BUFFER_DESC bd{16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
    dev->CreateBuffer(&bd, nullptr, &g_cb);
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    dev->CreateSamplerState(&sd, &g_smp);
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
    dev->CreateRasterizerState(&rd, &g_rs);
    D3D11_BLEND_DESC bld{};
    bld.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    dev->CreateBlendState(&bld, &g_bs);
    D3D11_DEPTH_STENCIL_DESC dsd{};
    dev->CreateDepthStencilState(&dsd, &g_ds);
    bool ok = g_vs && g_ps && g_cb && g_smp && g_rs && g_bs && g_ds;
    LOGI("mirror: resources %s", ok ? "ready" : "FAILED");
    if (!ok) g_failed = true;
    return ok;
}

bool IsSrgb(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
}
DXGI_FORMAT ToUnorm(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    default: return f;
    }
}

void Draw(IDXGISwapChain* sc) {
    if (!g_on.load()) return;
    ID3D11Texture2D* src = nullptr;
    DXGI_FORMAT srcFmt;
    {
        std::lock_guard<std::mutex> l(g_m);
        if (!g_src) return;
        src = g_src;
        src->AddRef();
        srcFmt = g_srcFmt;
    }
    ID3D11Device* dev = nullptr;
    ID3D11Texture2D* bb = nullptr;
    if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), (void**)&dev)) || !dev) { src->Release(); return; }
    if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb)) || !bb) { dev->Release(); src->Release(); return; }
    ID3D11Device* srcDev = nullptr;
    src->GetDevice(&srcDev);
    bool sameDev = srcDev == dev;
    if (srcDev) srcDev->Release();
    if (!sameDev || !InitRes(dev)) { bb->Release(); dev->Release(); src->Release(); return; }

    D3D11_TEXTURE2D_DESC bd{}, sd{};
    bb->GetDesc(&bd);
    src->GetDesc(&sd);
    // Raw copy of the encoded values unless the back buffer itself is sRGB (then sample linear and let it encode).
    bool bbSrgb = IsSrgb(bd.Format);
    DXGI_FORMAT srvFmt = bbSrgb ? (srcFmt != DXGI_FORMAT_UNKNOWN ? srcFmt : sd.Format) : ToUnorm(srcFmt != DXGI_FORMAT_UNKNOWN ? srcFmt : sd.Format);
    if (src != g_srvTex || srvFmt != g_srvFmt) {
        Rel(g_srv);
        D3D11_SHADER_RESOURCE_VIEW_DESC v{};
        v.Format = srvFmt;
        v.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        v.Texture2D.MipLevels = 1;
        if (FAILED(dev->CreateShaderResourceView(src, &v, &g_srv))) {
            static int n = 0;
            if (n++ < 3) LOGE("mirror: SRV for source format %d failed", srvFmt);
        }
        g_srvTex = src;   // identity only, not owned
        g_srvFmt = srvFmt;
    }
    if (bb != g_rtvTex) {
        Rel(g_rtv);
        dev->CreateRenderTargetView(bb, nullptr, &g_rtv);
        g_rtvTex = bb;
        LOGI("mirror: back buffer %ux%u format %d", bd.Width, bd.Height, bd.Format);
    }
    if (g_srv && g_rtv) {
        ID3D11DeviceContext* ctx = nullptr;
        dev->GetImmediateContext(&ctx);
        // save the state we touch
        ID3D11RenderTargetView* oRtv[8] = {};
        ID3D11DepthStencilView* oDsv = nullptr;
        ctx->OMGetRenderTargets(8, oRtv, &oDsv);
        ID3D11BlendState* oBs = nullptr; float oBf[4]; UINT oSm = 0;
        ctx->OMGetBlendState(&oBs, oBf, &oSm);
        ID3D11DepthStencilState* oDs = nullptr; UINT oSr = 0;
        ctx->OMGetDepthStencilState(&oDs, &oSr);
        ID3D11RasterizerState* oRs = nullptr; ctx->RSGetState(&oRs);
        UINT nvp = 16; D3D11_VIEWPORT oVp[16]; ctx->RSGetViewports(&nvp, oVp);
        ID3D11VertexShader* oVs = nullptr; ctx->VSGetShader(&oVs, nullptr, nullptr);
        ID3D11PixelShader* oPs = nullptr; ctx->PSGetShader(&oPs, nullptr, nullptr);
        ID3D11GeometryShader* oGs = nullptr; ctx->GSGetShader(&oGs, nullptr, nullptr);
        ID3D11HullShader* oHs = nullptr; ctx->HSGetShader(&oHs, nullptr, nullptr);
        ID3D11DomainShader* oDsh = nullptr; ctx->DSGetShader(&oDsh, nullptr, nullptr);
        ID3D11ShaderResourceView* oSrv = nullptr; ctx->PSGetShaderResources(0, 1, &oSrv);
        ID3D11SamplerState* oSmp = nullptr; ctx->PSGetSamplers(0, 1, &oSmp);
        ID3D11Buffer* oCb = nullptr; ctx->PSGetConstantBuffers(0, 1, &oCb);
        ID3D11InputLayout* oIl = nullptr; ctx->IAGetInputLayout(&oIl);
        D3D11_PRIMITIVE_TOPOLOGY oTop; ctx->IAGetPrimitiveTopology(&oTop);

        float ea = (float)sd.Width / sd.Height, sa = (float)bd.Width / bd.Height;
        float so[4] = {1, 1, 0, 0};
        if (ea < sa) { so[1] = ea / sa; so[3] = (1 - so[1]) * 0.5f; }   // window wider: crop top/bottom
        else { so[0] = sa / ea; so[2] = (1 - so[0]) * 0.5f; }           // window narrower: crop the sides
        ctx->UpdateSubresource(g_cb, 0, nullptr, so, 0, 0);
        D3D11_VIEWPORT vp{0, 0, (float)bd.Width, (float)bd.Height, 0, 1};
        ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
        float bf[4] = {};
        ctx->OMSetBlendState(g_bs, bf, 0xffffffff);
        ctx->OMSetDepthStencilState(g_ds, 0);
        ctx->RSSetState(g_rs);
        ctx->RSSetViewports(1, &vp);
        ctx->IASetInputLayout(nullptr);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(g_vs, nullptr, 0);
        ctx->GSSetShader(nullptr, nullptr, 0);
        ctx->HSSetShader(nullptr, nullptr, 0);
        ctx->DSSetShader(nullptr, nullptr, 0);
        ctx->PSSetShader(g_ps, nullptr, 0);
        ctx->PSSetShaderResources(0, 1, &g_srv);
        ctx->PSSetSamplers(0, 1, &g_smp);
        ctx->PSSetConstantBuffers(0, 1, &g_cb);
        ctx->Draw(3, 0);
        s_draws++;

        ID3D11ShaderResourceView* nullSrv = nullptr;
        ctx->PSSetShaderResources(0, 1, &nullSrv);
        // restore
        ctx->OMSetRenderTargets(8, oRtv, oDsv);
        ctx->OMSetBlendState(oBs, oBf, oSm);
        ctx->OMSetDepthStencilState(oDs, oSr);
        ctx->RSSetState(oRs);
        if (nvp) ctx->RSSetViewports(nvp, oVp);
        ctx->VSSetShader(oVs, nullptr, 0);
        ctx->PSSetShader(oPs, nullptr, 0);
        ctx->GSSetShader(oGs, nullptr, 0);
        ctx->HSSetShader(oHs, nullptr, 0);
        ctx->DSSetShader(oDsh, nullptr, 0);
        ctx->PSSetShaderResources(0, 1, &oSrv);
        ctx->PSSetSamplers(0, 1, &oSmp);
        ctx->PSSetConstantBuffers(0, 1, &oCb);
        ctx->IASetInputLayout(oIl);
        ctx->IASetPrimitiveTopology(oTop);
        for (auto* r : oRtv) if (r) r->Release();
        for (IUnknown* u : std::initializer_list<IUnknown*>{oDsv, oBs, oDs, oRs, oVs, oPs, oGs, oHs, oDsh, oSrv, oSmp, oCb, oIl})
            if (u) u->Release();
        ctx->Release();
    }
    bb->Release();
    dev->Release();
    src->Release();
}

HRESULT STDMETHODCALLTYPE h_Present(IDXGISwapChain* sc, UINT sync, UINT flags) {
    if (!(flags & DXGI_PRESENT_TEST)) Draw(sc);
    return o_Present(sc, sync, flags);
}
HRESULT STDMETHODCALLTYPE h_Present1(IDXGISwapChain1* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* p) {
    if (!(flags & DXGI_PRESENT_TEST)) Draw(sc);
    return o_Present1(sc, sync, flags, p);
}

} // namespace

bool Install() {
    // A throw-away device + swap chain on a hidden window gives the DXGI swap chain vtable.
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"EoeVRDummy";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
    DXGI_SWAP_CHAIN_DESC d{};
    d.BufferCount = 1;
    d.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    d.OutputWindow = hwnd;
    d.SampleDesc.Count = 1;
    d.Windowed = TRUE;
    IDXGISwapChain* sc = nullptr;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &d,
                                               &sc, &dev, nullptr, &ctx);
    bool ok = false;
    if (SUCCEEDED(hr) && sc) {
        void** vt = *(void***)sc;
        ok = MH_CreateHook(vt[8], (void*)h_Present, (void**)&o_Present) == MH_OK && MH_EnableHook(vt[8]) == MH_OK;
        IDXGISwapChain1* sc1 = nullptr;
        if (SUCCEEDED(sc->QueryInterface(__uuidof(IDXGISwapChain1), (void**)&sc1)) && sc1) {
            void** vt1 = *(void***)sc1;
            if (MH_CreateHook(vt1[22], (void*)h_Present1, (void**)&o_Present1) == MH_OK) MH_EnableHook(vt1[22]);
            sc1->Release();
        }
        LOGI("mirror: Present hook %s", ok ? "installed" : "FAILED");
    } else {
        LOGE("mirror: dummy swap chain failed 0x%08lx", hr);
    }
    if (sc) sc->Release();
    if (ctx) ctx->Release();
    if (dev) dev->Release();
    DestroyWindow(hwnd);
    return ok;
}

void SetSource(ID3D11Texture2D* eye, DXGI_FORMAT viewFormat) {
    std::lock_guard<std::mutex> l(g_m);
    if (eye == g_src) { g_srcFmt = viewFormat; return; }
    if (eye) eye->AddRef();
    if (g_src) g_src->Release();
    g_src = eye;
    g_srcFmt = viewFormat;
}

void SetEnabled(bool on) { g_on = on; }
uint32_t TakeDraws() { return s_draws.exchange(0); }

} // namespace uxr::mirror
