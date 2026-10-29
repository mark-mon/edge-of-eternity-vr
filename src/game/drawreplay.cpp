#include "drawreplay.h"
#include "../unity/il2cpp.h"
#include "../unity/unity.h"
#include "../common/log.h"
#include "MinHook.h"
#include <vector>

namespace uxr::drawreplay {
using namespace uxr::il2cpp;

namespace {
struct Bounds { float c[3], e[3]; };
typedef void (*Fn)(Object* mesh, int submesh, Object* material, Bounds* bounds, Object* argsBuffer, int argsOffset,
                   Object* props, int castShadows, bool receiveShadows, int layer, Object* camera, int lightProbeUsage,
                   Object* lppv);
Fn o_draw = nullptr;
struct Rec { Object *mesh, *material, *args, *props, *camera, *lppv; int submesh, argsOffset, cast, layer, lpu; bool recv; Bounds b; };
std::vector<Rec> g_recs;
int g_frame = -1;
bool g_enabled = true, g_replaying = false;
DWORD g_mainThread = 0;
unsigned s_rec = 0, s_rep = 0, s_frames = 0;

void h_draw(Object* mesh, int submesh, Object* material, Bounds* bounds, Object* argsBuffer, int argsOffset, Object* props,
            int castShadows, bool receiveShadows, int layer, Object* camera, int lpu, Object* lppv) {
    if (g_enabled && !g_replaying && bounds && GetCurrentThreadId() == g_mainThread && g_recs.size() < 20000) {
        Rec r{mesh, material, argsBuffer, props, camera, lppv, submesh, argsOffset, castShadows, layer, lpu, receiveShadows, *bounds};
        g_recs.push_back(r);
        s_rec++;
    }
    o_draw(mesh, submesh, material, bounds, argsBuffer, argsOffset, props, castShadows, receiveShadows, layer, camera, lpu, lppv);
}
} // namespace

bool Install() {
    const char* name = "UnityEngine.Graphics::Internal_DrawMeshInstancedIndirect_Injected(UnityEngine.Mesh,System.Int32,"
                       "UnityEngine.Material,UnityEngine.Bounds&,UnityEngine.ComputeBuffer,System.Int32,"
                       "UnityEngine.MaterialPropertyBlock,UnityEngine.Rendering.ShadowCastingMode,System.Boolean,System.Int32,"
                       "UnityEngine.Camera,UnityEngine.Rendering.LightProbeUsage,UnityEngine.LightProbeProxyVolume)";
    void* fn = api.resolve_icall(name);
    if (!fn) { LOGW("drawreplay: icall not found"); return false; }
    if (MH_CreateHook(fn, (void*)h_draw, (void**)&o_draw) != MH_OK || MH_EnableHook(fn) != MH_OK) {
        LOGE("drawreplay: hook failed");
        return false;
    }
    LOGI("drawreplay: hooked Graphics.DrawMeshInstancedIndirect icall at %p", fn);
    return true;
}

void BeginFrame(int frame) {
    if (frame == g_frame) return;
    g_frame = frame;
    g_mainThread = GetCurrentThreadId();
    g_recs.clear();
    s_frames++;
}

void Replay() {
    if (!g_enabled || !o_draw || g_recs.empty()) return;
    g_replaying = true;
    for (Rec& r : g_recs) {
        Bounds b = r.b;
        o_draw(r.mesh, r.submesh, r.material, &b, r.args, r.argsOffset, r.props, r.cast, r.recv, r.layer, r.camera, r.lpu, r.lppv);
    }
    s_rep += (unsigned)g_recs.size();
    g_replaying = false;
}

void SetEnabled(bool on) { g_enabled = on; if (!on) g_recs.clear(); }

Stats Take() { Stats s{s_rec, s_rep, s_frames}; s_rec = s_rep = s_frames = 0; return s; }

} // namespace uxr::drawreplay
