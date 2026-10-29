// Foveated rendering with NVIDIA Variable Rate Shading (D3D11 through NVAPI, no SDK lib) for the Unity eye renders.
// Ported from Divine Force VR (so6vr/src/vrs.cpp, itself from GranblueVR).
//
// The main thread brackets each eye's Camera.Render with GL.IssuePluginEvent markers (VrsBegin / VrsEnd) carrying a
// slot in a small parameter ring (eye, scene size, eye FOV, gaze). On Unity's render thread the game's immediate
// context OMSetRenderTargets is hooked: inside an eye render, a bind whose RT0 has the eye's scene size and comes with
// a depth buffer (G-buffer, deferred lighting, forward, sky) gets the eye's shading-rate pattern; everything else
// (shadow maps, post-processing, DLSS, UI, our copies) runs at full rate.
// Pattern: R8_UINT, one texel per 16x16 tile, 0 = full rate (centre), 1 = mid ring, 2 = outer ring.
#include "vrs.h"
#include "../common/log.h"
#include "MinHook.h"
#include <windows.h>
#include <d3d11.h>
#include <atomic>
#include <cmath>
#include <cstring>
#include <vector>

namespace uxr::vrs {
namespace {

// ---- NVAPI subset (IDs and layouts from the public NVAPI headers) ----
using NvAPI_Status = int;
constexpr NvAPI_Status NVAPI_OK = 0;
using PFN_QueryInterface = void*(__cdecl*)(unsigned);
using PFN_Initialize = NvAPI_Status(__cdecl*)();
enum NvPixelShadingRate : int {
    NV_PSR_NULL = 0, NV_PSR_16X, NV_PSR_8X, NV_PSR_4X, NV_PSR_2X, NV_PSR_1X_PER_PIXEL,
    NV_PSR_1X_PER_2X1, NV_PSR_1X_PER_1X2, NV_PSR_1X_PER_2X2, NV_PSR_1X_PER_4X2, NV_PSR_1X_PER_2X4, NV_PSR_1X_PER_4X4,
};
struct NvViewportShadingRateDesc { bool enableVariablePixelShadingRate; NvPixelShadingRate shadingRateTable[16]; };
struct NvViewportsShadingRateDesc { unsigned version; unsigned numViewports; NvViewportShadingRateDesc* pViewports; };
struct NvShadingRateResourceViewDesc { unsigned version; DXGI_FORMAT Format; unsigned ViewDimension; unsigned MipSlice, pad0, pad1; };
struct ID3D11NvShadingRateResourceView;
using PFN_CreateSRRV = NvAPI_Status(__cdecl*)(ID3D11Device*, ID3D11Resource*, NvShadingRateResourceViewDesc*, ID3D11NvShadingRateResourceView**);
using PFN_SetSRRV = NvAPI_Status(__cdecl*)(IUnknown*, ID3D11NvShadingRateResourceView*);
using PFN_SetRates = NvAPI_Status(__cdecl*)(IUnknown*, NvViewportsShadingRateDesc*);
PFN_CreateSRRV p_CreateSRRV = nullptr;
PFN_SetSRRV p_SetSRRV = nullptr;
PFN_SetRates p_SetRates = nullptr;
bool g_nvTried = false, g_nvOk = false;

bool InitNvapi() {
    if (g_nvTried) return g_nvOk;
    g_nvTried = true;
    HMODULE m = LoadLibraryW(L"nvapi64.dll");
    if (!m) { LOGI("vrs: nvapi64.dll not found (not an NVIDIA GPU?) - foveated rendering off"); return false; }
    auto qi = (PFN_QueryInterface)GetProcAddress(m, "nvapi_QueryInterface");
    if (!qi) { LOGW("vrs: nvapi_QueryInterface missing"); return false; }
    auto init = (PFN_Initialize)qi(0x0150e828);
    p_CreateSRRV = (PFN_CreateSRRV)qi(0x99ca2dff);
    p_SetSRRV = (PFN_SetSRRV)qi(0x1b0c2f83);
    p_SetRates = (PFN_SetRates)qi(0x34f7938f);
    if (!init || !p_CreateSRRV || !p_SetSRRV || !p_SetRates) { LOGW("vrs: NVAPI VRS entry points missing"); return false; }
    NvAPI_Status s = init();
    if (s != NVAPI_OK) { LOGW("vrs: NvAPI_Initialize failed (%d)", s); return false; }
    g_nvOk = true;
    LOGI("vrs: NVAPI VRS available");
    return true;
}

// ---- settings (written by the menu / config on other threads; plain values) ----
std::atomic<int> g_mode{0}, g_strength{1};
std::atomic<float> g_inner{0.35f}, g_outer{0.65f};

// ---- per-eye parameter ring (main thread writes, render thread reads by slot id) ----
struct Params { int eye; unsigned w, h; float tl, tr, tu, td; float gx, gy; bool gaze; };
constexpr int kRing = 16;
Params g_ring[kRing];
std::atomic<int> g_ringNext{0};

// ---- render thread state ----
int g_activeSlot = -1;   // inside an eye render
struct EyePattern { ID3D11Texture2D* tex = nullptr; ID3D11NvShadingRateResourceView* srrv = nullptr; UINT tw = 0, th = 0; float key[9] = {}; };
EyePattern g_pat[2];
bool g_bound = false;
int g_boundEye = -1;
bool g_failed = false;
std::atomic<uint32_t> s_scene{0}, s_other{0}, s_uploads{0};

template <class T> void Rel(T*& p) { if (p) { reinterpret_cast<IUnknown*>(p)->Release(); p = nullptr; } }

void RatesFor(int strength, NvPixelShadingRate& mid, NvPixelShadingRate& outer) {
    switch (strength) {
    case 0: mid = NV_PSR_1X_PER_2X1; outer = NV_PSR_1X_PER_2X2; break;   // Low
    case 2: mid = NV_PSR_1X_PER_2X2; outer = NV_PSR_1X_PER_4X4; break;   // High
    default: mid = NV_PSR_1X_PER_2X2; outer = NV_PSR_1X_PER_4X2; break;  // Medium
    }
}

void SetRates(ID3D11DeviceContext* ctx, bool on) {
    NvViewportShadingRateDesc vp[16];
    NvPixelShadingRate mid, outer;
    RatesFor(g_strength.load(), mid, outer);
    for (auto& v : vp) {
        v.enableVariablePixelShadingRate = on;
        for (auto& r : v.shadingRateTable) r = NV_PSR_1X_PER_PIXEL;
        v.shadingRateTable[1] = mid;
        v.shadingRateTable[2] = outer;
    }
    NvViewportsShadingRateDesc d{};
    d.version = sizeof(NvViewportsShadingRateDesc) | (1u << 16);
    d.numViewports = 16;
    d.pViewports = vp;
    NvAPI_Status s = p_SetRates(ctx, &d);
    static int errs = 0;
    if (s != NVAPI_OK && errs++ < 3) LOGW("vrs: RSSetViewportsPixelShadingRates failed (%d)", s);
}

bool EnsurePattern(ID3D11DeviceContext* ctx, const Params& p, UINT w, UINT h) {
    EyePattern& pt = g_pat[p.eye & 1];
    // Centre of the sharp area in the image: the eye's optical axis, or the gaze (kept inside the image).
    float tl = tanf(p.tl), tr = tanf(p.tr), tu = tanf(p.tu), td = tanf(p.td);
    float gx = 0, gy = 0;
    if (p.gaze) {
        gx = fminf(fmaxf(p.gx, tl * 0.8f), tr * 0.8f);
        gy = fminf(fmaxf(p.gy, td * 0.8f), tu * 0.8f);
    }
    float cx = tr > tl ? (gx - tl) / (tr - tl) : 0.5f;
    float cy = tu > td ? (tu - gy) / (tu - td) : 0.5f;
    UINT tw = (w + 15) / 16, th = (h + 15) / 16;
    cx = floorf(cx * tw + 0.5f) / tw;   // snap to whole tiles (fewer uploads)
    cy = floorf(cy * th + 0.5f) / th;
    float aspect = (tu > td) ? (tr - tl) / (tu - td) : float(w) / float(h ? h : 1);   // tangent-space aspect: round rings
    float inner = g_inner.load(), outer = fmaxf(g_outer.load(), inner);
    float key[9] = {float(w), float(h), cx, cy, inner, outer, aspect, float(g_strength.load()), 0};
    if (pt.tex && pt.tw == tw && pt.th == th && memcmp(key, pt.key, sizeof(key)) == 0) return true;
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return false;
    if (!pt.tex || pt.tw != tw || pt.th != th) {
        Rel(pt.srrv); Rel(pt.tex);
        D3D11_TEXTURE2D_DESC d{};
        d.Width = tw; d.Height = th; d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_R8_UINT;
        d.SampleDesc = {1, 0}; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        bool ok = SUCCEEDED(dev->CreateTexture2D(&d, nullptr, &pt.tex));
        if (ok) {
            NvShadingRateResourceViewDesc v{};
            v.version = sizeof(NvShadingRateResourceViewDesc) | (1u << 16);
            v.Format = DXGI_FORMAT_R8_UINT; v.ViewDimension = 4; v.MipSlice = 0;
            NvAPI_Status s = p_CreateSRRV(dev, pt.tex, &v, &pt.srrv);
            if (s != NVAPI_OK || !pt.srrv) { LOGW("vrs: CreateShadingRateResourceView failed (%d) - off", s); ok = false; }
        }
        if (!ok) { Rel(pt.srrv); Rel(pt.tex); dev->Release(); g_failed = true; return false; }
        pt.tw = tw; pt.th = th;
    }
    dev->Release();
    std::vector<uint8_t> px(size_t(tw) * th);
    for (UINT y = 0; y < th; ++y)
        for (UINT x = 0; x < tw; ++x) {
            float u = (x + 0.5f) * 16.0f / w, v = (y + 0.5f) * 16.0f / h;
            float dx = (u - cx) * 2.0f, dy = (v - cy) * 2.0f / aspect;
            float r = sqrtf(dx * dx + dy * dy);
            px[size_t(y) * tw + x] = r < inner ? 0 : (r < outer ? 1 : 2);
        }
    // Unity's render textures are upside down in D3D: flip the pattern rows so "up" in the eye is up in the pattern.
    std::vector<uint8_t> fl(px.size());
    for (UINT y = 0; y < th; ++y) memcpy(&fl[size_t(y) * tw], &px[size_t(th - 1 - y) * tw], tw);
    ctx->UpdateSubresource(pt.tex, 0, nullptr, fl.data(), tw, 0);
    memcpy(pt.key, key, sizeof(key));
    uint32_t n = ++s_uploads;
    if (n <= 4 || n % 3000 == 0)
        LOGI("vrs: eye %d pattern %ux%u tiles for %ux%u, centre (%.3f, %.3f)%s, rings %.2f / %.2f, strength %d (upload %u)", p.eye,
             tw, th, w, h, cx, cy, p.gaze ? " gaze" : "", inner, outer, g_strength.load(), n);
    return true;
}

void Unbind(ID3D11DeviceContext* ctx) {
    if (!g_bound) return;
    SetRates(ctx, false);
    p_SetSRRV(ctx, nullptr);
    g_bound = false;
    g_boundEye = -1;
}

void OnBind(ID3D11DeviceContext* ctx, UINT n, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv) {
    if (g_activeSlot < 0 || g_failed) { if (g_bound) Unbind(ctx); return; }
    const Params& p = g_ring[g_activeSlot];
    bool scene = false;
    UINT w = 0, h = 0;
    if (n > 0 && rtvs && rtvs[0] && dsv) {
        ID3D11Resource* r = nullptr;
        rtvs[0]->GetResource(&r);
        if (r) {
            D3D11_RESOURCE_DIMENSION dim;
            r->GetType(&dim);
            if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
                D3D11_TEXTURE2D_DESC d;
                static_cast<ID3D11Texture2D*>(r)->GetDesc(&d);
                w = d.Width; h = d.Height;
                scene = d.Width == p.w && d.Height == p.h && d.SampleDesc.Count == 1;
            }
            r->Release();
        }
    }
    if (!scene) { s_other++; Unbind(ctx); return; }
    s_scene++;
    if (!EnsurePattern(ctx, p, w, h)) { Unbind(ctx); return; }
    if (g_bound && g_boundEye == p.eye) return;
    p_SetSRRV(ctx, g_pat[p.eye & 1].srrv);
    SetRates(ctx, true);
    g_bound = true;
    g_boundEye = p.eye;
}

// ---- immediate context hooks ----
typedef void(STDMETHODCALLTYPE* OMSetRT_t)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*);
typedef void(STDMETHODCALLTYPE* OMSetRTUAV_t)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*, UINT, UINT,
                                              ID3D11UnorderedAccessView* const*, const UINT*);
OMSetRT_t o_OMSetRT = nullptr;
OMSetRTUAV_t o_OMSetRTUAV = nullptr;
ID3D11DeviceContext* g_imm = nullptr;

void STDMETHODCALLTYPE h_OMSetRT(ID3D11DeviceContext* ctx, UINT n, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv) {
    o_OMSetRT(ctx, n, rtvs, dsv);
    if (ctx == g_imm && g_activeSlot >= 0) OnBind(ctx, n, rtvs, dsv);
}
void STDMETHODCALLTYPE h_OMSetRTUAV(ID3D11DeviceContext* ctx, UINT n, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv,
                                    UINT us, UINT un, ID3D11UnorderedAccessView* const* uavs, const UINT* counts) {
    o_OMSetRTUAV(ctx, n, rtvs, dsv, us, un, uavs, counts);
    if (ctx == g_imm && g_activeSlot >= 0 && n != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL) OnBind(ctx, n, rtvs, dsv);
}

void __stdcall EvBegin(int slot) {
    if (!g_nvOk || g_failed) return;
    g_activeSlot = slot & (kRing - 1);
}
void __stdcall EvEnd(int) {
    g_activeSlot = -1;
    if (g_imm && g_bound) Unbind(g_imm);
}

} // namespace

bool Install(ID3D11Device* dev) {
    static bool done = false;
    if (done) return g_nvOk;
    done = true;
    if (!dev || !InitNvapi()) return false;
    dev->GetImmediateContext(&g_imm);
    if (!g_imm) return false;
    void** vt = *(void***)g_imm;
    bool ok = MH_CreateHook(vt[33], (void*)h_OMSetRT, (void**)&o_OMSetRT) == MH_OK && MH_EnableHook(vt[33]) == MH_OK;
    ok = ok && MH_CreateHook(vt[34], (void*)h_OMSetRTUAV, (void**)&o_OMSetRTUAV) == MH_OK && MH_EnableHook(vt[34]) == MH_OK;
    LOGI("vrs: render target hooks %s", ok ? "installed" : "FAILED");
    if (!ok) g_failed = true;
    return ok;
}

void Configure(int mode, int strength, float inner, float outer) {
    g_mode = mode; g_strength = strength; g_inner = inner; g_outer = outer;
}

bool Enabled() { return g_mode.load() != 0 && g_nvOk && !g_failed; }

int PrepareEye(int eye, unsigned sceneW, unsigned sceneH, const float fov[4], bool gazeValid, float gx, float gy) {
    int slot = g_ringNext.fetch_add(1) & (kRing - 1);
    Params& p = g_ring[slot];
    p.eye = eye; p.w = sceneW; p.h = sceneH;
    p.tl = fov[0]; p.tr = fov[1]; p.tu = fov[2]; p.td = fov[3];
    p.gaze = gazeValid && g_mode.load() == 2;
    p.gx = gx; p.gy = gy;
    return slot;
}

void* BeginEvent() { return (void*)&EvBegin; }
void* EndEvent() { return (void*)&EvEnd; }

Stats Take() { Stats s{s_scene.exchange(0), s_other.exchange(0), s_uploads.load()}; return s; }

} // namespace uxr::vrs
