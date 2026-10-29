#include "dlsseyes.h"
#include "../common/log.h"
#include "MinHook.h"
#include <windows.h>
#include <d3d11.h>
#include <atomic>
#include <mutex>

namespace uxr::dlsseyes {
namespace {
struct NgxHandle { unsigned Id; };
typedef int NgxResult;   // NVSDK_NGX_Result: success unless (r & 0xFFF00000) == 0xBAD00000
inline bool Ok(NgxResult r) { return ((unsigned)r & 0xFFF00000u) != 0xBAD00000u; }
constexpr int kSuperSampling = 1;   // NVSDK_NGX_Feature_SuperSampling

typedef NgxResult (*CreateFn)(ID3D11DeviceContext*, int feature, void* params, NgxHandle** out);
typedef NgxResult (*EvalFn)(ID3D11DeviceContext*, const NgxHandle*, const void* params, void* progress);
typedef NgxResult (*ReleaseFn)(NgxHandle*);
CreateFn o_create = nullptr;
EvalFn o_eval = nullptr, o_evalC = nullptr;
ReleaseFn o_release = nullptr;

std::mutex g_mx;
struct Pair { NgxHandle* a; NgxHandle* b; };
Pair g_pairs[8] = {};                  // game's feature -> right-eye feature
std::atomic<int> g_rtEye{0};
std::atomic<bool> g_enabled{true}, g_recreate{false};
std::atomic<uint32_t> s_created{0}, s_second{0}, s_evalL{0}, s_evalR{0}, s_evalU{0}, s_released{0};
std::atomic<int> s_err{0};
bool g_installed = false;

NgxHandle* SecondFor(const NgxHandle* a) {
    std::lock_guard<std::mutex> l(g_mx);
    for (auto& p : g_pairs) if (p.a == a) return p.b;
    return nullptr;
}
bool Known(const NgxHandle* a) {
    std::lock_guard<std::mutex> l(g_mx);
    for (auto& p : g_pairs) if (p.a == a) return true;
    return false;
}

NgxResult h_create(ID3D11DeviceContext* ctx, int feature, void* params, NgxHandle** out) {
    NgxResult r = o_create(ctx, feature, params, out);
    if (feature != kSuperSampling || !Ok(r) || !out || !*out) return r;
    s_created++;
    NgxHandle* b = nullptr;
    NgxResult r2 = o_create(ctx, feature, params, &b);
    if (!Ok(r2) || !b) {
        s_err = r2;
        LOGW("dlss eyes: second DLSS feature failed (0x%08x) - both eyes share one history", (unsigned)r2);
        b = nullptr;
    } else {
        s_second++;
    }
    std::lock_guard<std::mutex> l(g_mx);
    for (auto& p : g_pairs) if (!p.a) { p = {*out, b}; break; }
    static int n = 0;
    if (n++ < 20) LOGI("dlss eyes: DLSS feature %u created, right-eye feature %u", (*out)->Id, b ? b->Id : 0u);
    return r;
}

const NgxHandle* Pick(const NgxHandle* h) {
    if (!g_enabled) return h;
    if (g_rtEye.load() != 1) { s_evalL++; return h; }
    if (NgxHandle* b = SecondFor(h)) { s_evalR++; return b; }
    if (!Known(h)) { s_evalU++; g_recreate = true; }   // created before the hooks: ask for a re-initialisation
    return h;
}
NgxResult h_eval(ID3D11DeviceContext* ctx, const NgxHandle* h, const void* params, void* cb) {
    return o_eval(ctx, Pick(h), params, cb);
}
NgxResult h_evalC(ID3D11DeviceContext* ctx, const NgxHandle* h, const void* params, void* cb) {
    return o_evalC(ctx, Pick(h), params, cb);
}
NgxResult h_release(NgxHandle* h) {
    NgxHandle* b = nullptr;
    {
        std::lock_guard<std::mutex> l(g_mx);
        for (auto& p : g_pairs) if (p.a == h) { b = p.b; p = {}; break; }
    }
    if (b) o_release(b);
    s_released++;
    return o_release(h);
}

void __stdcall EyeEventFn(int eye) { g_rtEye = eye; }

bool Hook(HMODULE m, const char* name, void* det, void** orig) {
    void* fn = (void*)GetProcAddress(m, name);
    if (!fn) { LOGW("dlss eyes: %s not exported", name); return false; }
    if (MH_CreateHook(fn, det, orig) != MH_OK || MH_EnableHook(fn) != MH_OK) { LOGE("dlss eyes: hook %s failed", name); return false; }
    return true;
}
} // namespace

bool TryInstall() {
    if (g_installed) return true;
    static DWORD lastTry = 0;
    DWORD now = GetTickCount();
    if (lastTry && now - lastTry < 500) return false;
    lastTry = now;
    HMODULE m = GetModuleHandleW(L"OrionGraphics_DLSS.dll");
    if (!m) return false;
    g_installed = true;   // one attempt
    bool ok = Hook(m, "NVSDK_NGX_D3D11_CreateFeature", (void*)h_create, (void**)&o_create) &&
              Hook(m, "NVSDK_NGX_D3D11_ReleaseFeature", (void*)h_release, (void**)&o_release) &&
              Hook(m, "NVSDK_NGX_D3D11_EvaluateFeature", (void*)h_eval, (void**)&o_eval);
    if (ok) Hook(m, "NVSDK_NGX_D3D11_EvaluateFeature_C", (void*)h_evalC, (void**)&o_evalC);
    LOGI("dlss eyes: %s (OrionGraphics_DLSS.dll at %p)", ok ? "hooked NGX create/evaluate/release - one DLSS history per eye" : "hooks failed", m);
    return ok;
}
void SetEnabled(bool on) { g_enabled = on; }
void* EyeEvent() { return (void*)&EyeEventFn; }
bool TakeRecreateRequest() { return g_recreate.exchange(false); }
Stats Take() {
    return {s_created.exchange(0), s_second.exchange(0), s_evalL.exchange(0), s_evalR.exchange(0), s_evalU.exchange(0),
            s_released.exchange(0), s_err.load()};
}
}
