// Edge of Eternity VR - native per-eye stereo for Unity's built-in pipeline (engine layer from NEO VR / ONINAKI VR).
//
// Per frame (Application.onBeforeRender, after Cinemachine placed the camera):
//   * the world camera (RawCam_v2) and the 3D-UI overlay camera (UICam) are rendered once per eye with Camera.Render()
//     and the headset view/projection into the eye RenderTextures;
//   * the game's Screen Space Overlay canvases are switched to Screen Space Camera on our own HUD camera, which
//     renders them into the HUD texture (black background, by Unity) and once more over white (by us) for the matte;
//   * a GL.IssuePluginEvent callback copies eyes + HUD into xrout's shared texture ring on the render thread.
#include "stereo.h"
#include "../version.h"
#include "xrout.h"
#include "mirror.h"
#include "vrs.h"
#include "perf.h"
#include "vrkit_options.h"
#include <atomic>
#include "../unity/unity.h"
#include "../common/log.h"
#include "../common/crash_log.h"
#include "../game/drawreplay.h"
#include "../game/dlsseyes.h"
#include <set>
#include <map>
#include <d3d11.h>
#include <cmath>
#include <vector>
#include <sstream>

namespace uxr::stereo {
using namespace uxr::unity;

namespace {

struct Cfg {
    bool vr = true;
    std::vector<std::string> worldCams, overlayCams, hudCams, menuCanvases, disableComponents;
    bool menuPanel = true;            // full-screen game menus on the "game menu" panel (menu_distance / menu_size)
    float renderScale = 1.0f;         // used when renderPreset = custom
    std::string renderPreset = "native";
    float hudDistance = 1.6f, hudSize = 1.6f, hudOffsetY = -0.05f, menuDistance = 2.0f, menuSize = 2.4f;
    bool hudFollowHead = false;
    int eyeW = 0, eyeH = 0;          // 0 = from headset
    int hudW = 1920, hudH = 1080;
    float worldScale = 1.0f;          // game units per metre of head motion
    bool headTracking = true;
    bool levelCamera = true;          // drop the game camera's pitch/roll, keep yaw
    bool hudInHeadset = true;
    bool hudCanvases = true;          // move Screen Space Overlay canvases onto our HUD camera
    bool hudMatte = true;             // HUD alpha from a black and a white render
    std::string gameAA = "smaa";      // PPv2 anti-aliasing in VR: keep | none | fxaa | smaa | taa
    int foveation = 1;                // 0 off, 1 fixed, 2 eye tracked (fixed when there is no gaze)
    std::string foveationStr = "fixed";
    int foveationStrength = 2;        // 0 low, 1 medium, 2 high
    std::string foveationStrengthStr = "high";
    float foveationInner = 0.40f, foveationOuter = 0.70f;
    bool fakeGaze = false;
    bool blockDancingShadows = true;  // keep NGSS_FrustumShadows off (its screen-space shadows dance with head motion)
    std::string effectOff = "none";   // debug: one world-camera effect kept off (to find what makes distant shadows sway)
    float headSway = 0;               // debug: oscillating head yaw (degrees) to test temporal effects on a still headset
    int dlssDebugView = 0;            // debug: 1 = show DLSS input, 2 = DLSS output texture (F6 cycles with debug_keys)
    bool dlssInVr = false;            // EXPERIMENTAL: run the game's DLSS per eye (when DLSS is on in the game's graphics settings)
    bool dlssEyeHistory = true;       // a separate DLSS feature (history) for the right eye
    int gpuiMode = 1;                 // GPU Instancer (grass, trees): 0 = game only, 1 = re-issue its culling/draws per render
    bool swapEyes = false;            // debug: render the right eye first
    bool debugKeys = false;           // F6 toggles veg_all_cameras, F7 swaps the eye order
    bool drawReplay = true;           // re-queue the frame's DrawMeshInstancedIndirect calls before each further render
    int terrainMode = 0;              // debug: 1 = no GPU occlusion culling, 2 = + no draw-call culler, 3 = + no depth prepass
    bool vegAllCams = true;           // OrionGraphicDriver.DRAW_TO_ALL_CAMERAS (vegetation drawn for every camera render)
    int keyRecenter = VK_F9, keyHud = VK_F10, keyDump = VK_F11;
    float debugDumpAtSec = 0;
    bool pacing = true;          // render at most one game frame per headset frame
    int pacingTimeoutMs = 30;
    bool desktopWorld = true;    // let the game also render the world camera to the monitor
    bool desktopEye = true;      // desktop_view = eye: skip the game's own world render, mirror the left eye instead
    bool perfLog = false;        // one performance line per second
    float perfSlowMs = 0;        // >0: only log seconds whose slowest game frame exceeded this
} C;

// ---- render resolution presets (per-eye size = headset recommended size x scale)
struct Preset { const char* name; const char* label; float scale; };
const Preset kPresets[] = {
    {"performance", "Performance (70%)", 0.70f}, {"balanced", "Balanced (85%)", 0.85f}, {"native", "Native (100%)", 1.00f},
    {"high", "High (120%)", 1.20f}, {"ultra", "Ultra (140%)", 1.40f}, {"custom", "Custom (render_scale)", -1.f},
};
std::atomic<float> g_effScale{1.0f};   // written by the menu (XR thread) and Init; read by the main thread
std::atomic<bool> g_aaDirty{true};     // re-apply the anti-aliasing choice on the main thread

void ApplyRenderPreset() {
    float s = C.renderScale;
    for (auto& p : kPresets)
        if (C.renderPreset == p.name && p.scale > 0) s = p.scale;
    if (s < 0.25f) s = 0.25f;
    if (s > 2.0f) s = 2.0f;
    if (s != g_effScale.load()) LOGI("stereo: render scale -> %.2f (preset '%s')", s, C.renderPreset.c_str());
    g_effScale = s;
}

std::vector<std::string> Split(const std::string& s) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ',')) {
        size_t a = item.find_first_not_of(" \t"), b = item.find_last_not_of(" \t");
        if (a != std::string::npos) out.push_back(item.substr(a, b - a + 1));
    }
    return out;
}

// --- state (main thread)
Handle g_eyeRT[2], g_hudRT, g_hudWhiteRT;
ID3D11Texture2D* g_eyeTex[2] = {};
ID3D11Texture2D* g_hudTex = nullptr;
ID3D11Texture2D* g_hudWhiteTex = nullptr;
int g_eyeW = 0, g_eyeH = 0;
Handle g_worldCam;
Handle g_dlss;                   // the game's DLSS component on the world camera
std::string g_worldName;
std::vector<Handle> g_overlayCams, g_hudCams;
Handle g_hudCam;                 // our own camera for the converted overlay canvases
std::vector<Handle> g_menuCanvases;   // converted canvases that are full-screen menus (HUD goes on the menu panel)
std::set<std::string> g_menuNames;    // canvases that came from the game's UI camera
int g_hudCamMask = 0;
std::vector<Object*> g_renderedThisFrame, g_renderedLastFrame;
bool g_inOwnRender = false;
Object* g_curCam = nullptr;      // camera being rendered for an eye right now (projection hooks)
Matrix4x4 g_curProj{}, g_curView{};
float g_curTan[4] = {};          // eye frustum tangents: left, right, up, down
uint32_t s_viewIntercepts = 0, s_extents = 0;
bool g_hudVisible = true;
DWORD g_lastScan = 0;
bool g_keyState[256] = {};
DWORD g_firstVrTick = 0;
bool g_dumped = false;
uint32_t g_dumpId = 0;
DXGI_FORMAT g_viewFormat = DXGI_FORMAT_UNKNOWN;
uint32_t s_projIntercepts = 0;
bool s_warnedTarget = false;
std::vector<Object*> g_skipped;      // stereo cameras disabled for the rest of this frame (desktop_view = eye)
bool g_skippedLastFrame = false;
bool g_mirrorReady = false;
uint32_t s_skips = 0, s_hudPost = 0, s_mirrors = 0, s_reenabled = 0;

// stats
uint32_t s_frames = 0, s_stereo = 0, s_menu = 0, s_noPose = 0;
DWORD s_lastReport = 0;
// per-second performance
double p_lastFrame = 0, p_lastLog = 0;
perf::Acc p_frame, p_pace, p_stereoCpu;
uint32_t p_frames = 0, p_gameRenders = 0, p_skippedRenders = 0;
std::atomic<float> g_statGameFps{0}, g_statXrFps{0}, g_statGpuMs{0};

// --- frames handed to the render thread (GL.IssuePluginEvent carries only an int)
constexpr int kRing = 16;
xrout::FrameInfo g_ring[kRing];
int g_ringIdx = 0;

struct RingTex { ID3D11Texture2D *eye[2], *hud, *hudWhite; };
RingTex g_ringTex[kRing];

// The ring holds a D3D reference on each texture until the render thread has copied it, so a RenderTexture that Unity
// (or a camera script) releases or recreates in between can never be read after it was freed.
void RingAddRef(RingTex& t) {
    for (ID3D11Texture2D* x : {t.eye[0], t.eye[1], t.hud, t.hudWhite}) if (x) x->AddRef();
}
void RingRelease(RingTex& t) {
    for (ID3D11Texture2D** x : {&t.eye[0], &t.eye[1], &t.hud, &t.hudWhite}) if (*x) { (*x)->Release(); *x = nullptr; }
}

void __stdcall RenderThreadSubmit(int id) {
    WatchdogSetThread(1, GetCurrentThreadId());
    const xrout::FrameInfo& fi = g_ring[id & (kRing - 1)];
    RingTex& t = g_ringTex[id & (kRing - 1)];   // the textures this frame was rendered into (survive a resize)
    xrout::SubmitRenderThread(t.eye[0], t.eye[1], t.hud, g_viewFormat, fi, t.hudWhite);
    RingRelease(t);
}

bool Alive(Object* o);
ID3D11Texture2D* NativeTex(Object* rt);
// Unity can release and recreate the GPU texture behind a RenderTexture object (device events, scripts calling
// Release() on a camera target): read the native pointers again every frame.
void RefreshNativePointers() {
    auto upd = [](Handle& h, ID3D11Texture2D*& p, const char* what) {
        Object* rt = h.get();
        if (!rt || !Alive(rt)) return;
        ID3D11Texture2D* n = NativeTex(rt);
        if (n != p) {
            static int logs = 0;
            if (logs++ < 20) LOGW("stereo: %s native texture changed %p -> %p (released/recreated by Unity or a script)", what, p, n);
            p = n;
        }
    };
    upd(g_eyeRT[0], g_eyeTex[0], "left eye");
    upd(g_eyeRT[1], g_eyeTex[1], "right eye");
    upd(g_hudRT, g_hudTex, "HUD");
    upd(g_hudWhiteRT, g_hudWhiteTex, "HUD white");
}

bool KeyPressed(int vk) {
    if (vk <= 0 || vk > 255) return false;
    bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
    bool edge = down && !g_keyState[vk];
    g_keyState[vk] = down;
    return edge;
}

// ---- math (Unity: left-handed, +Z forward, +Y up)
Quat QMul(const Quat& a, const Quat& b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
Vec3 QRot(const Quat& q, const Vec3& v) {
    Vec3 u{q.x, q.y, q.z};
    Vec3 t{2 * (u.y * v.z - u.z * v.y), 2 * (u.z * v.x - u.x * v.z), 2 * (u.x * v.y - u.y * v.x)};
    return {v.x + q.w * t.x + (u.y * t.z - u.z * t.y), v.y + q.w * t.y + (u.z * t.x - u.x * t.z),
            v.z + q.w * t.z + (u.x * t.y - u.y * t.x)};
}
float Dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

// OpenXR (right-handed, -Z forward) -> Unity (left-handed, +Z forward): mirror Z.
Vec3 XrPosToUnity(const xrout::Pose& p) { return {p.px, p.py, -p.pz}; }
Quat XrRotToUnity(const xrout::Pose& p) { return {-p.qx, -p.qy, p.qz, p.qw}; }

Quat YawOnly(const Quat& q) {
    Vec3 f = QRot(q, {0, 0, 1});
    float yaw = atan2f(f.x, f.z);
    return {0, sinf(yaw * 0.5f), 0, cosf(yaw * 0.5f)};
}

// Unity worldToCameraMatrix: camera space is right-handed (-Z forward), i.e. view = scale(1,1,-1) * inverse(TRS).
Matrix4x4 ViewMatrix(const Vec3& pos, const Quat& rot) {
    Vec3 r = QRot(rot, {1, 0, 0}), u = QRot(rot, {0, 1, 0}), f = QRot(rot, {0, 0, 1});
    Matrix4x4 m{};
    m.at(0, 0) = r.x; m.at(0, 1) = r.y; m.at(0, 2) = r.z; m.at(0, 3) = -Dot(r, pos);
    m.at(1, 0) = u.x; m.at(1, 1) = u.y; m.at(1, 2) = u.z; m.at(1, 3) = -Dot(u, pos);
    m.at(2, 0) = -f.x; m.at(2, 1) = -f.y; m.at(2, 2) = -f.z; m.at(2, 3) = Dot(f, pos);
    m.at(3, 3) = 1;
    return m;
}

// OpenGL-convention off-axis projection (Unity converts it for D3D itself).
Matrix4x4 ProjMatrix(const xrout::Fov& fov, float n, float f) {
    float l = tanf(fov.left), r = tanf(fov.right), t = tanf(fov.up), b = tanf(fov.down);
    Matrix4x4 m{};
    m.at(0, 0) = 2 / (r - l);
    m.at(0, 2) = (r + l) / (r - l);
    m.at(1, 1) = 2 / (t - b);
    m.at(1, 2) = (t + b) / (t - b);
    m.at(2, 2) = -(f + n) / (f - n);
    m.at(2, 3) = -2 * f * n / (f - n);
    m.at(3, 2) = -1;
    return m;
}

bool Alive(Object* o) {
    // A destroyed UnityEngine.Object keeps its managed shell; m_CachedPtr (first field after the header) becomes null.
    return o && *(void**)((char*)o + 16) != nullptr;
}

ID3D11Texture2D* NativeTex(Object* rt) { return rt ? (ID3D11Texture2D*)I.Texture_GetNativeTexturePtr(rt) : nullptr; }

bool EnsureTargets() {
    unsigned rw = 0, rh = 0;
    int w = C.eyeW, h = C.eyeH;
    if (w <= 0 || h <= 0) {
        if (!xrout::RecommendedSize(&rw, &rh) || !rw) return false;
        float sc = g_effScale.load();
        w = (int)(rw * sc);
        h = (int)(rh * sc);
    }
    w = (w + 1) & ~1;
    h = (h + 1) & ~1;
    bool eyesOk = g_eyeRT[0].get() && Alive(g_eyeRT[0].get()) && w == g_eyeW && h == g_eyeH;
    if (!eyesOk) {
        for (int e = 0; e < 2; e++) {
            if (Object* old = g_eyeRT[e].get()) if (Alive(old) && I.RenderTexture_Release) I.RenderTexture_Release(old);
            Object* rt = NewRenderTexture(w, h, 24, 0 /* ARGB32 */, e ? "EoeVR_EyeR" : "EoeVR_EyeL");
            if (!rt) return false;
            g_eyeRT[e].set(rt);
            g_eyeTex[e] = NativeTex(rt);
        }
    }
    if (!g_hudRT.get() || !Alive(g_hudRT.get())) {
        Object* rt = NewRenderTexture(C.hudW, C.hudH, 24, 0, "EoeVR_HUD");
        if (!rt) return false;
        g_hudRT.set(rt);
        g_hudTex = NativeTex(rt);
        if (Object* c = g_hudCam.get()) if (Alive(c)) I.Camera_set_targetTexture(c, rt);
    }
    if (C.hudMatte && (!g_hudWhiteRT.get() || !Alive(g_hudWhiteRT.get()))) {
        Object* rt = NewRenderTexture(C.hudW, C.hudH, 24, 0, "EoeVR_HUDWhite");
        if (!rt) return false;
        g_hudWhiteRT.set(rt);
        g_hudWhiteTex = NativeTex(rt);
    }
    if (!eyesOk) {
        g_eyeW = w;
        g_eyeH = h;
        if (g_eyeTex[0]) {
            D3D11_TEXTURE2D_DESC d{};
            g_eyeTex[0]->GetDesc(&d);
            int cs = 0;
            InvokeValue(M.QualitySettings_get_activeColorSpace, nullptr, cs);
            // ARGB32 is R8G8B8A8_TYPELESS on D3D11; Unity writes gamma values in Gamma space, linear->sRGB in Linear space.
            if (d.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS) g_viewFormat = cs == 1 ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
            else g_viewFormat = DXGI_FORMAT_UNKNOWN;
            {
                ID3D11Device* dev = nullptr;
                g_eyeTex[0]->GetDevice(&dev);
                if (dev) { vrs::Install(dev); dev->Release(); }
            }
            LOGI("stereo: eye targets %dx%d (headset %ux%u x %.2f), native format %d, colour space %s -> view format %d", w, h,
                 rw, rh, g_effScale.load(), d.Format, cs == 1 ? "Linear" : "Gamma", g_viewFormat);
        }
    }
    return g_eyeTex[0] && g_eyeTex[1];
}

void SetList(std::vector<Handle>& dst, const std::vector<Object*>& src) {
    for (auto& h : dst) h.reset();
    dst.clear();
    for (Object* o : src) { Handle h; h.set(o); dst.push_back(h); }
}

bool IsActive(Object* c) {
    bool act = false;
    InvokeValue(M.Behaviour_get_isActiveAndEnabled, c, act);
    return act;
}

// ---- our HUD camera for the game's Screen Space Overlay canvases
Object* EnsureHudCamera() {
    Object* cam = g_hudCam.get();
    if (cam && Alive(cam)) return cam;
    Object* go = NewGameObject("EoeVR_HUDCamera");
    if (!go) return nullptr;
    cam = AddComponent(go, M.Camera);
    if (!cam) { LOGE("stereo: AddComponent(Camera) failed"); return nullptr; }
    Object* tr = Invoke(M.Component_get_transform, cam);
    Vec3 hudPos{0, -20000, 0};
    Call(M.Transform, "set_position", tr, {&hudPos});
    int clear = 2;                          // SolidColor
    Color bg{0, 0, 0, 0};
    float depth = 100, nearc = 0.05f, farc = 50, fov = 60;
    int mask = 1 << 5;                      // UI layer until the canvases tell us theirs
    bool no = false;
    Call(M.Camera, "set_clearFlags", cam, {&clear});
    Call(M.Camera, "set_backgroundColor", cam, {&bg});
    Call(M.Camera, "set_depth", cam, {&depth});
    Call(M.Camera, "set_nearClipPlane", cam, {&nearc});
    Call(M.Camera, "set_farClipPlane", cam, {&farc});
    Call(M.Camera, "set_fieldOfView", cam, {&fov});
    Call(M.Camera, "set_cullingMask", cam, {&mask});
    Call(M.Camera, "set_allowHDR", cam, {&no});
    Call(M.Camera, "set_allowMSAA", cam, {&no});
    I.Camera_set_targetTexture(cam, g_hudRT.get());
    g_hudCam.set(cam);
    g_hudCamMask = mask;
    LOGI("stereo: created HUD camera [%p]", cam);
    return cam;
}

// A camera that draws nothing, to the screen, after every other camera (Unity renders texture cameras first): its
// OnPostRender is the end of the frame's rendering. Used to re-enable the skipped stereo cameras and mirror the eye.
Handle g_mirrorCam;
Object* EnsureMirrorCamera() {
    Object* cam = g_mirrorCam.get();
    if (cam && Alive(cam)) return cam;
    Object* go = NewGameObject("EoeVR_MirrorCamera");
    if (!go) return nullptr;
    cam = AddComponent(go, M.Camera);
    if (!cam) return nullptr;
    Object* tr = Invoke(M.Component_get_transform, cam);
    Vec3 p{0, -21000, 0};
    Call(M.Transform, "set_position", tr, {&p});
    int clear = 4;   // Nothing
    float depth = 1000, nearc = 0.05f, farc = 1;
    int mask = 0;
    bool no = false;
    Call(M.Camera, "set_clearFlags", cam, {&clear});
    Call(M.Camera, "set_depth", cam, {&depth});
    Call(M.Camera, "set_nearClipPlane", cam, {&nearc});
    Call(M.Camera, "set_farClipPlane", cam, {&farc});
    Call(M.Camera, "set_cullingMask", cam, {&mask});
    Call(M.Camera, "set_allowHDR", cam, {&no});
    Call(M.Camera, "set_allowMSAA", cam, {&no});
    g_mirrorCam.set(cam);
    LOGI("stereo: created mirror camera [%p]", cam);
    return cam;
}

// Screen Space Overlay canvases draw straight into the back buffer; move them onto our HUD camera.
bool GameMenuOpen() {
    static bool last = false;
    bool open = false;
    std::string which;
    for (auto& h : g_menuCanvases) {
        Object* c = h.get();
        if (c && Alive(c) && IsActive(c)) { open = true; which = Name(c); break; }
    }
    if (open != last) { last = open; LOGI("stereo: game menu %s%s%s", open ? "open ('" : "closed", which.c_str(), open ? "') -> menu panel" : ""); }
    return open;
}

void ConvertCanvases() {
    if (!C.hudInHeadset || !C.hudCanvases || !M.Canvas) return;
    Object* hudCam = EnsureHudCamera();
    if (!hudCam) return;
    int mask = g_hudCamMask;
    std::vector<Object*> menus;
    for (Object* c : FindObjectsOfType(M.Canvas)) {
        bool root = false;
        InvokeValue(M.Canvas_get_isRootCanvas, c, root);
        if (!root) continue;
        int mode = 0;
        InvokeValue(M.Canvas_get_renderMode, c, mode);
        Object* wc = Invoke(M.Canvas_get_worldCamera, c);
        bool overlay = mode == 0 || (mode == 1 && !wc);
        Object* go = GameObjectOf(c);
        int layer = CallValue<int>(GameObjectClass, "get_layer", go);
        std::string cname = Name(c);
        auto isMenu = [&] {
            if (g_menuNames.count(cname)) return true;
            for (auto& m : C.menuCanvases) if (!m.empty() && cname.find(m) != std::string::npos) return true;
            return false;
        };
        if (wc == hudCam) { mask |= 1 << layer; if (isMenu()) menus.push_back(c); continue; }
        // Screen Space Camera canvases of the game's UI camera (status / Y menu ...): that camera renders per eye with the
        // full headset view, so they filled the whole view right in front of the eyes. Moved to our HUD camera, shown
        // on the game-menu panel.
        bool onUiCam = false;
        if (mode == 1 && wc) for (auto& h : g_overlayCams) if (h.get() == wc) onUiCam = true;
        if (onUiCam) { g_menuNames.insert(cname); overlay = true; }
        if (!overlay) {   // camera / world space canvases the game draws itself: log each one once
            static std::set<std::string> seen;
            std::string key = Name(c) + "|" + std::to_string(mode) + "|" + (wc ? Name(wc) : std::string("-"));
            if (seen.size() < 200 && seen.insert(key).second) {
                float pd = 0;
                if (mode == 1) pd = CallValue<float>(M.Canvas, "get_planeDistance", c);
                LOGI("stereo: game canvas '%s' (layer %d) mode %s camera '%s' plane distance %.2f - left as is", Name(c).c_str(),
                     layer, mode == 1 ? "ScreenSpaceCamera" : "WorldSpace", wc ? Name(wc).c_str() : "-", pd);
            }
            continue;
        }
        float dist = 1.0f;
        int newMode = 1;   // ScreenSpaceCamera
        Call(M.Canvas, "set_worldCamera", c, {hudCam});
        Call(M.Canvas, "set_planeDistance", c, {&dist});
        Call(M.Canvas, "set_renderMode", c, {&newMode});
        mask |= 1 << layer;
        int order = 0;
        InvokeValue(M.Canvas_get_sortingOrder, c, order);
        bool menu = isMenu();
        if (menu) menus.push_back(c);
        LOGI("stereo: canvas '%s' (layer %d, order %d%s) -> HUD camera%s", cname.c_str(), layer, order,
             onUiCam ? ", was on the game's UI camera" : "", menu ? " (game menu panel)" : "");
    }
    SetList(g_menuCanvases, menus);
    if (mask != g_hudCamMask) {
        Call(M.Camera, "set_cullingMask", hudCam, {&mask});
        LOGI("stereo: HUD camera culling mask 0x%08x", mask);
        g_hudCamMask = mask;
    }
}

void ScanBlockedComponents(Object* cam);
// ---- per-camera effects that cannot work per eye
void ConfigureWorldCamera(Object* cam) {
    std::string list;
    std::string off;
    for (Object* c : Components(cam)) {
        list += ClassName(c) + " ";
        std::string cn = ClassName(c);   // only Behaviours have "enabled"
        bool en = (cn == "UnityEngine.Transform" || cn == "UnityEngine.Rigidbody" || cn == "UnityEngine.BoxCollider") ? true
                  : I.Behaviour_get_enabled(c);
        if (!en) off += ClassName(c) + " ";
    }
    LOGI("stereo: world camera components: %s", list.c_str());
    LOGI("stereo: world camera components currently disabled: %s", off.empty() ? "(none)" : off.c_str());
    if (Object* ppl = FindComponent(cam, "PostProcessLayer")) {
        int off = FieldOffset(ClassOf(ppl), "antialiasingMode");
        if (off > 0) {
            int* aa = (int*)((char*)ppl + off);
            static const char* names[] = {"none", "fxaa", "smaa", "taa"};
            static int gameAA = -1;
            if (gameAA < 0) gameAA = *aa;
            int want = gameAA;
            for (int i = 0; i < 4; i++) if (C.gameAA == names[i]) want = i;
            LOGI("stereo: PostProcessLayer antialiasing: game %s, VR %s", gameAA >= 0 && gameAA < 4 ? names[gameAA] : "?",
                 want >= 0 && want < 4 ? names[want] : "?");
            *aa = want;
        }
    }
    ScanBlockedComponents(cam);
    if (Object* dlss = FindComponent(cam, "DLSS")) {
        int off = FieldOffset(ClassOf(dlss), "m_dlssEnabled");
        bool on = off > 0 && *((bool*)((char*)dlss + off));
        bool beh = false;
        InvokeValue(M.Behaviour_get_enabled, dlss, beh);
        LOGI("stereo: DLSS component present, dlss enabled=%d behaviour enabled=%d", on, beh);
        g_dlss.set(dlss);
    } else {
        g_dlss.reset();
    }
}

// ---- DLSS per eye (dlss_in_vr = 1): the game's own DLSS runs inside every eye render.
// RegenBuffers is hooked so its buffers are eye-sized (target = eye size, source = eye size x the game's DLSS ratio);
// RawCam_v2 renders into DLSS's low-res colour texture (DLSS.OnPreCull), then the DLSS imposter camera (which carries
// the evaluate command buffer and blits the upscaled result in ImposterCameraDLSSOutput.OnRenderImage) renders with
// the eye texture as its target. DLSS's "previous frame" matrices are kept per eye.
struct DlssOffsets { int imposter = -1, output = -1, color = -1, prevW = -1, prevJit = -1, prevCam = -1, targW = -1, targH = -1, srcW = -1, srcH = -1; } g_do;
struct Mat64 { float m[16]; };
Mat64 g_dlssPrevJit[2], g_dlssPrevCam[2];
bool g_dlssPrevValid[2] = {};
uint32_t s_dlssEyes = 0;

bool DlssActive() {
    if (!C.dlssInVr) return false;
    Object* d = g_dlss.get();
    return d && Alive(d) && I.Behaviour_get_enabled(d);
}
void DlssOffsetsInit(Object* d) {
    if (g_do.imposter >= 0 || !d) return;
    Class* k = ClassOf(d);
    g_do.imposter = FieldOffset(k, "m_imposterCamera");
    g_do.output = FieldOffset(k, "m_outputTexture");
    g_do.color = FieldOffset(k, "m_colorTexture");
    g_do.prevW = FieldOffset(k, "m_prevWidth");
    g_do.prevJit = FieldOffset(k, "m_previousJitteredProjectionMatrix");
    g_do.prevCam = FieldOffset(k, "m_previousCameraMatrix");
    g_do.targW = FieldOffset(k, "m_targWidth");
    g_do.targH = FieldOffset(k, "m_targHeight");
    g_do.srcW = FieldOffset(k, "m_srcWidth");
    g_do.srcH = FieldOffset(k, "m_srcHeight");
    LOGI("stereo: DLSS field offsets imposter %d output %d color %d prevW %d prevJit %d prevCam %d", g_do.imposter, g_do.output,
         g_do.color, g_do.prevW, g_do.prevJit, g_do.prevCam);
}
template <class T> T& DField(Object* d, int off) { return *(T*)((char*)d + off); }

// Keep DLSS sized for the eyes: when its target is not the eye size, invalidate its remembered screen size so its own
// CheckDirty re-initialises it (RegenBuffers is then given eye sizes by the hook below).
void DlssEnsureEyeSize() {
    Object* d = g_dlss.get();
    if (!d || g_eyeW <= 0) return;
    DlssOffsetsInit(d);
    if (g_do.targW < 0 || g_do.prevW < 0) return;
    bool recreate = dlsseyes::TakeRecreateRequest();
    if (recreate) LOGI("stereo: DLSS feature was created before the per-eye hooks -> re-initialise");
    if (recreate || DField<int>(d, g_do.targW) != g_eyeW || DField<int>(d, g_do.targH) != g_eyeH) {
        static DWORD last = 0;
        DWORD now = GetTickCount();
        if (recreate || now - last > 1000) {   // at most once a second (re-initialising allocates the DLSS buffers)
            last = now;
            LOGI("stereo: DLSS target %dx%d != eye %dx%d -> re-initialise", DField<int>(d, g_do.targW), DField<int>(d, g_do.targH), g_eyeW, g_eyeH);
            DField<int>(d, g_do.prevW) = -1;
            g_dlssPrevValid[0] = g_dlssPrevValid[1] = false;
        }
    }
}

// The game's upscalers and temporal AA are built for one window and break the stereo eye renders:
//   DLSS (Orion.Graphics.DLSS.DLSS): redirects the camera into a low-res texture, an imposter camera shows the result;
//   AMD_CAS (the game's FSR option): the same design (CameraTarget + imposter camera) -> black image in VR;
//   TAA2 / CTAA_PC (the game's TAA): one history per camera, game-FOV jitter -> doubled image stuck to the head.
// While VR runs they are kept off (the game re-enables them whenever it applies its graphics settings) and the camera
// gets its screen target back. The mod's own anti-aliasing (game_aa, PPv2 SMAA by default) is used instead.
// `vr_disable_components` lists them; DLSS is skipped when dlss_in_vr = 1 (experimental per-eye DLSS).
std::vector<Handle> g_blocked;
void ScanBlockedComponents(Object* cam) {
    std::vector<Object*> found;
    std::string names;
    for (auto& n : C.disableComponents) {
        if (n.empty() || (n == "DLSS" && C.dlssInVr)) continue;
        if (Object* c = FindComponent(cam, n.c_str())) { found.push_back(c); names += ClassName(c) + " "; }
    }
    SetList(g_blocked, found);
    LOGI("stereo: kept off in VR: %s", names.empty() ? "(none found)" : names.c_str());
}
void EnforceDlss(Object* cam) {
    if (!cam) return;
    for (auto& h : g_blocked) {
        Object* c = h.get();
        if (!c || !Alive(c)) continue;
        Class* k = ClassOf(c);
        if (I.Behaviour_get_enabled(c)) {
            I.Behaviour_set_enabled(c, false);
            static std::map<Class*, int> logged;
            if (logged[k]++ < 5) LOGI("stereo: game's %s switched off for VR", ClassName(c).c_str());
        }
        int oT = FieldOffset(k, "m_colorTexture");
        if (oT <= 0) oT = FieldOffset(k, "CameraTarget");
        Object* rt = oT > 0 ? *(Object**)((char*)c + oT) : nullptr;
        if (Object* tt = I.Camera_get_targetTexture(cam); tt && tt == rt) {
            static int n = 0;
            if (n++ < 10) LOGI("stereo: world camera target '%s' left by %s -> screen", Name(tt).c_str(), ClassName(c).c_str());
            I.Camera_set_targetTexture(cam, nullptr);
        }
        // their imposter / output cameras: keep them from drawing over the screen or the HUD
        int oI = FieldOffset(k, "m_imposterCamera");
        if (Object* imp = oI > 0 ? *(Object**)((char*)c + oI) : nullptr; imp && Alive(imp) && I.Behaviour_get_enabled(imp)) {
            I.Behaviour_set_enabled(imp, false);
            static int n = 0;
            if (n++ < 10) LOGI("stereo: %s imposter camera switched off", ClassName(c).c_str());
        }
    }
}

// ---- GPU Instancer: its managers cull for the game camera and queue DrawMeshInstancedIndirect calls from Update();
// those reach only the next camera render. Re-running Update() before each further render re-issues them (and culls
// with the camera's current, per-eye matrices).
std::vector<Handle> g_gpuiManagers;
Class* g_gpuiClass = nullptr;
uint32_t s_gpuiCalls = 0;
void ScanGpui() {
    if (!g_gpuiClass) {
        static bool tried = false;
        if (tried) return;
        tried = true;
        g_gpuiClass = FindClass("GPUInstancer", "GPUInstancerManager", "GPUInstancer.dll");
        if (!g_gpuiClass) return;
    }
    std::vector<Object*> ms;
    for (Object* m : FindObjectsOfType(g_gpuiClass)) if (IsActive(m)) ms.push_back(m);
    if (ms.size() != g_gpuiManagers.size()) {
        std::string names;
        for (Object* m : ms) names += ClassName(m) + " ";
        LOGI("stereo: %zu GPU Instancer managers: %s", ms.size(), names.c_str());
    }
    SetList(g_gpuiManagers, ms);
}
void RerunGpui() {
    if (C.gpuiMode != 1) return;
    for (auto& h : g_gpuiManagers) {
        Object* m = h.get();
        if (!m || !Alive(m)) continue;
        if (const MethodInfo* up = Method(ClassOf(m), "Update", 0)) { Invoke(up, m); s_gpuiCalls++; }
    }
}

// The game's vegetation (OrionGraphicDriver) queues Graphics.DrawMeshInstancedIndirect for one camera; Unity drops
// those after that camera's first render of the frame, so only the first eye got grass.
void ApplyVegetation() {
    static Class* k = FindClass("", "OrionGraphicDriver", "Assembly-CSharp-firstpass.dll");
    if (!k) return;
    bool cur = false;
    GetStatic(k, "DRAW_TO_ALL_CAMERAS", &cur);
    static bool logged = false;
    if (!logged) {
        logged = true;
        bool legacy = false;
        GetStatic(k, "USE_UNITY_LEGACY_DRAW", &legacy);
        LOGI("stereo: OrionGraphicDriver DRAW_TO_ALL_CAMERAS=%d USE_UNITY_LEGACY_DRAW=%d (game values)", cur, legacy);
    }
    bool want = C.vegAllCams;
    if (cur != want) { SetStatic(k, "DRAW_TO_ALL_CAMERAS", &want); LOGI("stereo: DRAW_TO_ALL_CAMERAS -> %d", want); }
}

// TerrainMaster (terrain + vegetation streaming) culling switches. GPU occlusion culling tests against the previous
// render's depth with the game camera's matrices, which no longer match with two eye renders per frame.
// Shadow settings, logged once (build 10: distant shadows sway with head motion; the game already uses StableFit, so
// the cascade fit is not the cause; QualitySettings.set_shadowProjection is stripped from this build anyway).
void LogShadows() {
    static bool done = false;
    if (done) return;
    done = true;
    static Class* qs = FindClass("UnityEngine", "QualitySettings", "UnityEngine.CoreModule.dll");
    if (!qs) return;
    int cur = CallValue<int>(qs, "get_shadowProjection", nullptr, {}, -1);
    int casc = CallValue<int>(qs, "get_shadowCascades", nullptr, {}, -1);
    float dist = CallValue<float>(qs, "get_shadowDistance", nullptr, {}, -1.f);
    LOGI("stereo: game shadows: projection %s, cascades %d, distance %.0f", cur == 0 ? "CloseFit" : cur == 1 ? "StableFit" : "?", casc, dist);
}

// ---- "Block Dancing Shadows": NGSS_FrustumShadows (the game's screen-space contact shadows; temporal, built for one
// view) makes distant shadows on rocks and vegetation sway with head motion in both eyes (found with the effect test,
// build 11). While on, the component is kept disabled; switching the option off gives it back to the game.
Handle g_dancing;
void ApplyDancingShadows(Object* cam) {
    static bool wasBlocking = false;
    if (C.blockDancingShadows) {
        Object* c = g_dancing.get();
        if (!c || !Alive(c)) {
            c = cam ? FindComponent(cam, "NGSS_FrustumShadows") : nullptr;
            if (c) g_dancing.set(c); else g_dancing.reset();
        }
        if (c && I.Behaviour_get_enabled(c)) {
            I.Behaviour_set_enabled(c, false);
            static int n = 0;
            if (n++ < 5) LOGI("stereo: Block Dancing Shadows: NGSS_FrustumShadows switched off");
        }
        wasBlocking = true;
    } else if (wasBlocking) {
        wasBlocking = false;
        if (Object* c = g_dancing.get(); c && Alive(c)) { I.Behaviour_set_enabled(c, true); LOGI("stereo: Block Dancing Shadows off: NGSS_FrustumShadows back on"); }
        g_dancing.reset();
    }
}

// ---- Debug: keep one world-camera effect off (menu Debug > "Switch off effect"), to find which one makes distant
// shadows sway with head motion. The menu runs on the XR thread: it only stores the choice; applied here.
static const std::vector<std::string> kEffectTest = {"none", "HBAO", "AmplifyOcclusionEffect", "NNAO", "SSRT", "SEScreenSpaceShadows",
    "NGSS_FrustumShadows", "SSR2", "GlobalFog", "StylisticFog", "TenkokuSkyFog", "HxVolumetricCamera", "AuraCamera",
    "FogVolumeRenderer", "SSMSGlobalFog", "SSMS", "GlobalMaterialIDRenderer", "PostProcessLayer", "FantasyImageEffect",
    "HPFSharpen", "MotionBlur", "MadGoatSSAA", "BlurEffect", "VRS"};
Handle g_effectTest;
std::string g_effectTestName = "none";
void ApplyEffectTest(Object* cam) {
    if (C.effectOff != g_effectTestName) {
        if (Object* c = g_effectTest.get(); c && Alive(c)) { I.Behaviour_set_enabled(c, true); LOGI("stereo: effect test: %s back on", g_effectTestName.c_str()); }
        g_effectTest.reset();
        g_effectTestName = C.effectOff;
        if (C.effectOff != "none" && cam) {
            Object* c = FindComponent(cam, C.effectOff.c_str());
            if (!c) LOGI("stereo: effect test: %s not on the world camera", C.effectOff.c_str());
            else if (!I.Behaviour_get_enabled(c)) LOGI("stereo: effect test: %s is already off (game setting) - not the cause", C.effectOff.c_str());
            else { g_effectTest.set(c); LOGI("stereo: effect test: %s switched off", C.effectOff.c_str()); }
        }
    }
    if (Object* c = g_effectTest.get(); c && Alive(c) && I.Behaviour_get_enabled(c)) I.Behaviour_set_enabled(c, false);
}

void ApplyTerrain() {
    static Class* k = FindClass("", "TerrainMaster", "Assembly-CSharp-firstpass.dll");
    if (!k) return;
    static int oOcc = FieldOffset(k, "EnableGPUOcclusionCulling"), oDcc = FieldOffset(k, "EnableDrawCallCuller"),
               oPre = FieldOffset(k, "DebugDisableDepthPrepass");
    for (Object* t : FindObjectsOfType(k)) {
        bool* occ = oOcc > 0 ? (bool*)((char*)t + oOcc) : nullptr;
        bool* dcc = oDcc > 0 ? (bool*)((char*)t + oDcc) : nullptr;
        bool* pre = oPre > 0 ? (bool*)((char*)t + oPre) : nullptr;
        static bool logged = false;
        if (!logged) {
            logged = true;
            LOGI("stereo: TerrainMaster game values: GPUOcclusionCulling=%d DrawCallCuller=%d DisableDepthPrepass=%d",
                 occ ? *occ : -1, dcc ? *dcc : -1, pre ? *pre : -1);
        }
        if (C.terrainMode >= 1 && occ && *occ) { *occ = false; LOGI("stereo: TerrainMaster GPU occlusion culling off"); }
        if (C.terrainMode >= 2 && dcc && *dcc) { *dcc = false; LOGI("stereo: TerrainMaster draw-call culler off"); }
        if (C.terrainMode >= 3 && pre && !*pre) { *pre = true; LOGI("stereo: TerrainMaster depth prepass off"); }
    }
}

// DLSS component state (Orion.Graphics.DLSS.DLSS on the world camera), for the 10 s stats line.
std::string DlssState() {
    Object* cam = g_worldCam.get();
    if (!cam || !Alive(cam)) return "-";
    Object* d = FindComponent(cam, "DLSS");
    if (!d) return "none";
    Class* k = ClassOf(d);
    static int oQ = FieldOffset(k, "Quality"), oF = FieldOffset(k, "UpscaleFactor"), oE = FieldOffset(k, "m_dlssEnabled"),
               oSW = FieldOffset(k, "m_srcWidth"), oSH = FieldOffset(k, "m_srcHeight"), oTW = FieldOffset(k, "m_targWidth"),
               oTH = FieldOffset(k, "m_targHeight");
    auto i32 = [&](int o) { return o > 0 ? *(int*)((char*)d + o) : -1; };
    char b[200];
    snprintf(b, sizeof(b), "behaviour %d dlss %d quality %d factor %.2f src %dx%d -> target %dx%d", I.Behaviour_get_enabled(d),
             oE > 0 ? *((bool*)((char*)d + oE)) : -1, i32(oQ), oF > 0 ? *(float*)((char*)d + oF) : -1.f, i32(oSW), i32(oSH),
             i32(oTW), i32(oTH));
    return b;
}

void ScanCameras() {
    auto cams = FindObjectsOfType(M.Camera);
    // World camera: first configured name that actually rendered last frame (to the screen); else first enabled match.
    Object* best = nullptr;
    std::string bestName;
    for (auto& want : C.worldCams) {
        for (Object* c : cams) {
            if (Name(c) != want) continue;
            bool rendered = false;
            for (Object* r : g_renderedLastFrame) rendered |= (r == c);
            if (rendered || IsActive(c)) { best = c; bestName = want; break; }
        }
        if (best) break;
    }
    if (best != g_worldCam.get()) {
        LOGI("stereo: world camera -> '%s' [%p]", best ? bestName.c_str() : "<none>", best);
        g_worldCam.set(best);
        g_worldName = bestName;
        if (best) { g_aaDirty = false; ConfigureWorldCamera(best); }
    } else if (best && g_aaDirty.exchange(false)) {
        ConfigureWorldCamera(best);
    }
    // Overlay cameras: drawn per eye on top of the world (3D UI, same pose as the world camera).
    std::vector<Object*> ov;
    for (auto& want : C.overlayCams)
        for (Object* c : cams)
            if (Name(c) == want && c != best) { ov.push_back(c); break; }
    SetList(g_overlayCams, ov);
    // HUD cameras (game's own screen-space UI cameras, e.g. the loading screen): redirect into the HUD texture.
    std::vector<Object*> hud;
    for (Object* c : cams) {
        std::string n = Name(c);
        for (auto& want : C.hudCams) {
            if (n != want) continue;
            Object* tt = I.Camera_get_targetTexture(c);
            if (C.hudInHeadset && tt != g_hudRT.get()) {
                if (tt) { LOGW("stereo: HUD camera '%s' already renders to a texture; left alone", n.c_str()); break; }
                I.Camera_set_targetTexture(c, g_hudRT.get());
                LOGI("stereo: HUD camera '%s' -> HUD texture", n.c_str());
            }
            hud.push_back(c);
            break;
        }
    }
    ConvertCanvases();
    ScanGpui();
    ApplyVegetation();
    ApplyTerrain();
    LogShadows();
    if (Object* hc = g_hudCam.get()) hud.push_back(hc);   // our camera renders last (depth 100)
    SetList(g_hudCams, hud);
}

void ClearRT(Object* rt, Color c) {
    Object* prev = I.RenderTexture_GetActive();
    I.RenderTexture_SetActive(rt);
    I.GL_Clear(true, true, &c, 1.0f);
    I.RenderTexture_SetActive(prev);
}
void ClearHud() { ClearRT(g_hudRT.get(), {0, 0, 0, 0}); }

// The UI over white, rendered by us now; Unity renders the same UI over black into the HUD texture later this frame.
void RenderHudWhite() {
    Object* white = g_hudWhiteRT.get();
    if (!white) return;
    ClearRT(white, {1, 1, 1, 0});
    g_inOwnRender = true;
    Object* own = g_hudCam.get();
    for (auto& h : g_hudCams) {
        Object* c = h.get();
        if (!Alive(c) || !IsActive(c)) continue;
        if (c == own) { Color w{1, 1, 1, 0}; Call(M.Camera, "set_backgroundColor", c, {&w}); }
        I.Camera_set_targetTexture(c, white);
        I.Camera_Render(c);
        I.Camera_set_targetTexture(c, g_hudRT.get());
        if (c == own) { Color b{0, 0, 0, 0}; Call(M.Camera, "set_backgroundColor", c, {&b}); }
    }
    g_inOwnRender = false;
}

void RenderEye(Object* cam, int eye, const Vec3& gp, const Quat& gr, const xrout::Pose& pose, const xrout::Fov& fov,
               float n, float f) {
    Vec3 hp = XrPosToUnity(pose);
    Quat hr = XrRotToUnity(pose);
    if (!C.headTracking) { hp = {hp.x, 0, 0}; hr = {0, 0, 0, 1}; }  // keep only the eye separation
    if (C.headSway != 0) {   // debug: a slow yaw oscillation of the head (same for both eyes of a frame)
        float a = C.headSway * 0.0174533f * sinf((float)(FrameCount() % 100000) * 0.05f);
        hr = QMul(Quat{0, sinf(a * 0.5f), 0, cosf(a * 0.5f)}, hr);
    }
    Vec3 off = QRot(gr, {hp.x * C.worldScale, hp.y * C.worldScale, hp.z * C.worldScale});
    Vec3 pos{gp.x + off.x, gp.y + off.y, gp.z + off.z};
    Quat rot = QMul(gr, hr);
    Matrix4x4 view = ViewMatrix(pos, rot);
    g_curProj = ProjMatrix(fov, n, f);
    g_curView = view;
    g_curTan[0] = tanf(fov.left); g_curTan[1] = tanf(fov.right); g_curTan[2] = tanf(fov.up); g_curTan[3] = tanf(fov.down);
    g_curCam = cam;
    Object* oldTarget = I.Camera_get_targetTexture(cam);
    Object* dlss = (cam == g_worldCam.get() && DlssActive()) ? g_dlss.get() : nullptr;
    Object* imposter = dlss && g_do.imposter > 0 ? DField<Object*>(dlss, g_do.imposter) : nullptr;
    if (imposter && !Alive(imposter)) imposter = nullptr;
    if (!imposter) dlss = nullptr;
    if (dlss) {   // this eye's "previous frame" matrices for DLSS's motion vector conversion
        if (g_dlssPrevValid[eye]) {
            if (g_do.prevJit > 0) DField<Mat64>(dlss, g_do.prevJit) = g_dlssPrevJit[eye];
            if (g_do.prevCam > 0) DField<Mat64>(dlss, g_do.prevCam) = g_dlssPrevCam[eye];
        }
    }
    // With DLSS the camera renders into DLSS's low-res colour texture: set it before the render so Unity sizes its
    // internal buffers for it (DLSS.OnPreCull sets it again; switching sizes mid-render left depth and colour mismatched).
    Object* dlssColor = dlss && g_do.color > 0 ? DField<Object*>(dlss, g_do.color) : nullptr;
    I.Camera_set_targetTexture(cam, dlssColor && Alive(dlssColor) ? dlssColor : g_eyeRT[eye].get());
    I.Camera_set_worldToCameraMatrix(cam, &view);
    I.Camera_set_projectionMatrix(cam, &g_curProj);
    bool vrsOn = cam == g_worldCam.get() && vrs::Enabled();
    if (vrsOn) {
        unsigned sw = g_eyeW, sh = g_eyeH;
        if (dlss && g_do.srcW > 0) { sw = DField<int>(dlss, g_do.srcW); sh = DField<int>(dlss, g_do.srcH); }
        float fv[4] = {fov.left, fov.right, fov.up, fov.down};
        float gx = 0, gy = 0;
        bool gz = xrout::GazeTangents(&gx, &gy);
        int slot = vrs::PrepareEye(eye, sw, sh, fv, gz, gx, gy);
        I.GL_IssuePluginEvent(vrs::BeginEvent(), slot);
    }
    if (dlss) I.GL_IssuePluginEvent(dlsseyes::EyeEvent(), eye);   // render thread: which DLSS history to use
    I.Camera_Render(cam);
    if (vrsOn) I.GL_IssuePluginEvent(vrs::EndEvent(), 0);
    if (dlss) {
        // DLSS evaluate + output blit happen when its imposter camera renders: into this eye's texture.
        Object* impOld = I.Camera_get_targetTexture(imposter);
        I.Camera_set_targetTexture(imposter, g_eyeRT[eye].get());
        I.Camera_Render(imposter);
        I.Camera_set_targetTexture(imposter, impOld);
        I.GL_IssuePluginEvent(dlsseyes::EyeEvent(), 0);
        if (C.dlssDebugView == 1 || C.dlssDebugView == 2) {   // debug: show DLSS's input / output texture instead
            Object* src = DField<Object*>(dlss, C.dlssDebugView == 1 ? g_do.color : g_do.output);
            float sc[2] = {1, 1}, of[2] = {0, 0};
            if (src && Alive(src)) I.Graphics_Blit4(src, g_eyeRT[eye].get(), sc, of);
            static bool logged = false;
            if (!logged && src) {
                logged = true;
                int w = 0, h = 0, fmt = -1;
                InvokeValue(M.Texture_get_width, src, w); InvokeValue(M.Texture_get_height, src, h);
                fmt = CallValue<int>(M.RenderTexture, "get_format", src);
                LOGI("stereo: DLSS debug view %d: '%s' %dx%d format %d", C.dlssDebugView, Name(src).c_str(), w, h, fmt);
            }
        }
        if (g_do.prevJit > 0) g_dlssPrevJit[eye] = DField<Mat64>(dlss, g_do.prevJit);
        if (g_do.prevCam > 0) g_dlssPrevCam[eye] = DField<Mat64>(dlss, g_do.prevCam);
        g_dlssPrevValid[eye] = true;
        s_dlssEyes++;
    }
    g_curCam = nullptr;
    I.Camera_ResetWorldToCameraMatrix(cam);
    I.Camera_ResetProjectionMatrix(cam);
    I.Camera_set_targetTexture(cam, oldTarget);
}

void PerfSecond() {
    double now = perf::NowMs();
    if (!p_lastLog) { p_lastLog = now; return; }
    if (now - p_lastLog < 1000) return;
    double secs = (now - p_lastLog) / 1000.0;
    p_lastLog = now;
    auto x = xrout::TakeXrPerf();
    auto g = perf::TakeGpu();
    g_statGameFps = (float)(p_frames / secs);
    g_statXrFps = (float)(x.frames / secs);
    g_statGpuMs = (float)g.stereo.avg();
    if (C.perfLog && (C.perfSlowMs <= 0 || p_frame.max > C.perfSlowMs || x.late > 0)) {
        LOGI("perf: %s | game %.0f fps, frame avg %.1f max %.1f ms, pacing wait %.1f ms, stereo cpu %.1f/%.1f ms | "
             "gpu stereo %.1f/%.1f ms, gpu frame %.1f/%.1f ms | xr %.0f fps, late %u, interval max %.1f ms, wait %.1f, copy %.1f/%.1f, end %.1f | "
             "desktop renders %u skipped %u",
             ActiveSceneName().c_str(), p_frames / secs, p_frame.avg(), p_frame.max, p_pace.avg(), p_stereoCpu.avg(), p_stereoCpu.max,
             g.stereo.avg(), g.stereo.max, g.frame.avg(), g.frame.max, x.frames / secs, x.late, x.intervalMax, x.waitAvg,
             x.copyAvg, x.copyMax, x.endAvg, p_gameRenders, p_skippedRenders);
    }
    p_frame.reset(); p_pace.reset(); p_stereoCpu.reset();
    p_frames = p_gameRenders = p_skippedRenders = 0;
}

void Report() {
    DWORD now = GetTickCount();
    if (now - s_lastReport < 10000) return;
    s_lastReport = now;
    auto st = xrout::TakeStats();
    auto ds = drawreplay::Take();
    LOGI("dlss: %s | per-eye evaluations %u | camera matrix intercepts: projection %u view %u, eye extents %u", DlssState().c_str(),
         s_dlssEyes, s_projIntercepts, s_viewIntercepts, s_extents);
    s_viewIntercepts = 0; s_extents = 0;
    { auto e = dlsseyes::Take();
      LOGI("dlss eyes: %s, features created %u (+%u right eye), evaluations left %u right %u unpaired %u, released %u%s",
           C.dlssEyeHistory ? "separate histories" : "shared history", e.created, e.createdSecond, e.evalLeft, e.evalRight,
           e.evalUnknown, e.released, e.lastError ? " (second feature failed)" : ""); }
    { auto vs = vrs::Take(); LOGI("vrs: %s, scene binds %u, other binds %u, pattern uploads %u, eye tracking %s", vrs::Enabled() ? "on" : "off",
                                  vs.sceneBinds, vs.otherBinds, vs.uploads, xrout::EyeTrackingSupported() ? "yes" : "no"); }
    s_dlssEyes = 0;
    LOGI("stats 10s: game frames %u stereo %u menu-only %u no-pose %u | submitted %u no-slot %u | xr frames %u new %u empty %u | %s | world '%s' | proj intercepts %u | gpui updates %u | draws rec %u replayed %u | desktop skips %u hudpost %u reenabled %u mirrors %u | %.0f fps",
         s_frames, s_stereo, s_menu, s_noPose, st.submitted, st.noSlot, st.xrFrames, st.xrNewFrames, st.xrNoFrame,
         xrout::StateName(), g_worldName.c_str(), s_projIntercepts, s_gpuiCalls, ds.recorded, ds.replayed, s_skips, s_hudPost, s_reenabled, mirror::TakeDraws(), g_statGameFps.load());
    s_skips = s_hudPost = s_reenabled = s_mirrors = 0;
    s_frames = s_stereo = s_menu = s_noPose = s_projIntercepts = s_gpuiCalls = 0;
}

void ApplyHud() { xrout::SetHud(C.hudDistance, C.hudSize, C.hudOffsetY, C.hudFollowHead); xrout::SetMenuPanel(C.menuDistance, C.menuSize); }

void ApplyFoveation() {
    C.foveation = C.foveationStr == "off" ? 0 : (C.foveationStr == "fixed" ? 1 : 2);
    C.foveationStrength = C.foveationStrengthStr == "low" ? 0 : (C.foveationStrengthStr == "high" ? 2 : 1);
    vrs::Configure(C.foveation, C.foveationStrength, C.foveationInner, C.foveationOuter);
    xrout::SetFakeGaze(C.fakeGaze);
}

// In-headset settings menu (vrkit). Each option points at the config field it edits; keys are the config.txt keys.
// onChange runs on the XR thread: only plain values / atomics are touched there.
void RegisterMenuOptions() {
    auto& o = vrkit::Options::Get();
    if (!o.All().empty()) return;
    vrkit::SetLog([](const char* m) { LOGI("%s", m); });

    // ---- View
    o.AddBool("View", "head_tracking", "Head tracking", &C.headTracking,
              "Off: the view stays locked to the game camera (only the eye separation is kept).");
    o.AddFloat("View", "world_scale", "World scale", &C.worldScale, 0.1f, 10.f, 0.1f, 1,
               "Game units per metre of head movement. Higher = the world feels smaller.");
    o.AddBool("View", "level_camera", "Level horizon", &C.levelCamera,
              "Remove the game camera's up/down tilt (keep its heading) so the horizon stays level.");
    o.AddAction("View", "Recenter view", [] { xrout::Recenter(); }, "Make the current head position the centre (F9).");

    // ---- Display
    std::vector<std::string> names, labels;
    for (auto& p : kPresets) { names.push_back(p.name); labels.push_back(p.label); }
    auto& pr = o.AddChoice("Display", "render_preset", "Resolution", &C.renderPreset, names,
                           "Per-eye render size relative to the headset's recommended size.", [] { ApplyRenderPreset(); });
    pr.choiceLabels = labels;
    o.AddFloat("Display", "render_scale", "Custom scale", &C.renderScale, 0.25f, 2.f, 0.05f, 2,
               "Per-eye scale used by the Custom preset.", [] { ApplyRenderPreset(); })
        .visible = [] { return C.renderPreset == "custom"; };
    auto& aa = o.AddChoice("Display", "game_aa", "Anti-aliasing", &C.gameAA, {"keep", "none", "fxaa", "smaa", "taa"},
                           "Post-processing anti-aliasing in VR. TAA shares its history between the eyes (ghosting).",
                           [] { g_aaDirty = true; });
    aa.choiceLabels = {"Game setting", "Off", "FXAA", "SMAA", "TAA"};
    o.AddBool("Display", "frame_pacing", "Frame pacing", &C.pacing,
              "Render at most one game frame per headset frame (keeps the GPU from being flooded).");
    static std::string desktopView = C.desktopEye ? "eye" : "game";
    auto& dv = o.AddChoice("Display", "desktop_view", "Monitor view", &desktopView, {"eye", "game"},
                           "Headset eye: the monitor mirrors the left eye and the game's own full render is skipped (faster). "
                           "Game view: the game also renders its normal view for the monitor.",
                           [] { C.desktopEye = desktopView != "game"; });
    dv.choiceLabels = {"Headset eye (faster)", "Game view"};
    auto& fv = o.AddChoice("Display", "foveation", "Foveated rendering", &C.foveationStr, {"off", "fixed", "eye"},
                           "NVIDIA VRS: lower shading rate in the periphery of each eye. Eye tracked follows your gaze "
                           "(headsets with eye tracking; fixed otherwise).", [] { ApplyFoveation(); });
    fv.choiceLabels = {"Off", "Fixed", "Eye tracked"};
    auto& fs = o.AddChoice("Display", "foveation_strength", "Foveation strength", &C.foveationStrengthStr, {"low", "medium", "high"},
                           "How much the periphery is reduced (low 2x1 / 2x2, medium 2x2 / 4x2, high 2x2 / 4x4).",
                           [] { ApplyFoveation(); });
    fs.choiceLabels = {"Low", "Medium", "High"};
    o.AddBool("Display", "block_dancing_shadows", "Block Dancing Shadows", &C.blockDancingShadows,
              "Switches off the game's screen-space contact shadows (NGSS frustum shadows), which sway with head motion. "
              "Off: the game's look, with dancing distant shadows.");
    fs.visible = [] { return C.foveationStr != "off"; };
    o.AddFloat("Display", "foveation_inner", "Full-rate radius", &C.foveationInner, 0.1f, 1.0f, 0.05f, 2,
               "Radius of the full-rate centre (1.0 = half the image width).", [] { ApplyFoveation(); })
        .visible = [] { return C.foveationStr != "off"; };
    o.AddFloat("Display", "foveation_outer", "Outer ring radius", &C.foveationOuter, 0.2f, 1.5f, 0.05f, 2,
               "Beyond this radius the outer (lowest) rate is used.", [] { ApplyFoveation(); })
        .visible = [] { return C.foveationStr != "off"; };
    o.AddBool("Display", "draw_replay", "Grass in both eyes", &C.drawReplay,
              "Queue the game's grass / vegetation draws again for the second eye (the game draws them for one view only).");

    // ---- HUD
    o.AddFloat("HUD", "hud_distance", "HUD distance (m)", &C.hudDistance, 0.3f, 10.f, 0.1f, 1, "How far away the HUD panel floats.", ApplyHud);
    o.AddFloat("HUD", "hud_size", "HUD width (m)", &C.hudSize, 0.2f, 10.f, 0.1f, 1, "Width of the HUD panel.", ApplyHud);
    o.AddFloat("HUD", "hud_offset_y", "HUD height (m)", &C.hudOffsetY, -2.f, 2.f, 0.05f, 2, "Up / down offset of the HUD panel.", ApplyHud);
    o.AddBool("HUD", "hud_follow_head", "HUD follows head", &C.hudFollowHead,
              "On: the HUD stays in front of your eyes. Off: it stays where you recentred.", ApplyHud);
    o.AddFloat("HUD", "menu_distance", "Game menu distance (m)", &C.menuDistance, 0.3f, 10.f, 0.1f, 1,
               "Distance of the panel used for the game's full-screen menus.", ApplyHud);
    o.AddFloat("HUD", "menu_size", "Game menu width (m)", &C.menuSize, 0.2f, 10.f, 0.1f, 1,
               "Width of the panel used for the game's full-screen menus.", ApplyHud);
    o.AddBool("HUD", "menu_panel", "Game menus on the menu panel", &C.menuPanel,
              "Status (Y) menu, pause/options menus and the full map use the game menu distance and width.", [] {});
    o.AddBool("HUD", "hud_matte", "Clean HUD edges", &C.hudMatte,
              "Transparency from a second render of the UI over white (sharper, correct glow). Off: one render, cheaper.");
    static bool hudLayerSaved = C.hudInHeadset;   // edited copy: only takes effect after a restart
    o.AddBool("HUD", "hud_layer", "Separate HUD layer", &hudLayerSaved,
              "Show the game's UI on its own panel (off: the UI is not shown in the headset).").needsRestart = true;

    // ---- Debug
    o.AddBool("Debug", "perf_log", "Performance log", &C.perfLog, "One line per second in eoevr.log.");
    o.AddChoice("Debug", "debug_effect_off", "Switch off effect", &C.effectOff, kEffectTest,
                "Keeps one of the game's camera effects off (live), to find the one that makes distant shadows sway.", [] {});
    o.AddBool("Debug", "debug_fake_gaze", "Fake eye gaze", &C.fakeGaze, "Test eye-tracked foveation without a tracker: a slow circle.",
              [] { ApplyFoveation(); });
    o.AddAction("Debug", "Save headset images", [] { static uint32_t n = 1000; xrout::RequestDump(++n); },
                "Writes eoevr_dump_<n>_left/right/hud.bmp into the game folder (F11).");
}

} // namespace

bool Init(const Config& cfg, const std::wstring& gameDir) {
    C.vr = cfg.GetBool("vr", true);
    if (!C.vr) { LOGI("stereo: vr = 0, VR output off"); return false; }
    C.worldCams = Split(cfg.GetStr("world_cameras", "RawCam_v2"));
    C.overlayCams = Split(cfg.GetStr("overlay_cameras", "UICam"));
    C.hudCams = Split(cfg.GetStr("hud_cameras", "LoadingCam"));
    C.menuCanvases = Split(cfg.GetStr("menu_canvases", "PauseMenu,StatusUI,MinimapFullUI,MultiOptionsUI,ParametricSettingsUI"));
    C.menuPanel = cfg.GetBool("menu_panel", true);
    C.renderScale = cfg.GetFloat("render_scale", 1.0f);
    C.renderPreset = cfg.GetStr("render_preset", "native");
    ApplyRenderPreset();
    C.eyeW = cfg.GetInt("eye_width", 0);
    C.eyeH = cfg.GetInt("eye_height", 0);
    C.hudW = cfg.GetInt("hud_tex_width", 1920);
    C.hudH = cfg.GetInt("hud_tex_height", 1080);
    C.worldScale = cfg.GetFloat("world_scale", 1.0f);
    C.headTracking = cfg.GetBool("head_tracking", true);
    C.levelCamera = cfg.GetBool("level_camera", true);
    C.hudInHeadset = cfg.GetBool("hud_layer", true);
    C.hudCanvases = cfg.GetBool("hud_canvases", true);
    C.hudMatte = cfg.GetBool("hud_matte", true);
    C.gameAA = cfg.GetStr("game_aa", "smaa");
    C.dlssInVr = cfg.GetBool("dlss_in_vr", false);
    C.disableComponents = Split(cfg.GetStr("vr_disable_components", "DLSS,AMD_CAS,TAA2,CTAA_PC"));
    C.dlssDebugView = cfg.GetInt("dlss_debug_view", 0);
    C.dlssEyeHistory = cfg.GetBool("dlss_eye_history", true);
    dlsseyes::SetEnabled(C.dlssEyeHistory);
    C.foveationStr = cfg.GetStr("foveation", "fixed");
    C.foveationStrengthStr = cfg.GetStr("foveation_strength", "high");
    C.foveationInner = cfg.GetFloat("foveation_inner", 0.40f);
    C.foveationOuter = cfg.GetFloat("foveation_outer", 0.70f);
    C.fakeGaze = cfg.GetBool("debug_fake_gaze", false);
    C.headSway = cfg.GetFloat("debug_head_sway", 0);
    C.effectOff = cfg.GetStr("debug_effect_off", "none");
    C.blockDancingShadows = cfg.GetBool("block_dancing_shadows", true);
    ApplyFoveation();
    C.gpuiMode = cfg.GetInt("gpui_mode", 1);
    C.debugKeys = cfg.GetBool("debug_keys", false);
    C.vegAllCams = cfg.GetBool("veg_all_cameras", false);
    C.terrainMode = cfg.GetInt("terrain_mode", 0);
    C.drawReplay = cfg.GetBool("draw_replay", true);
    C.keyRecenter = cfg.GetInt("key_recenter", VK_F9);
    C.keyHud = cfg.GetInt("key_hud", VK_F10);
    C.keyDump = cfg.GetInt("key_dump", VK_F11);
    C.debugDumpAtSec = cfg.GetFloat("debug_dump_at_sec", 0);
    C.pacing = cfg.GetBool("frame_pacing", true);
    C.pacingTimeoutMs = cfg.GetInt("frame_pacing_timeout_ms", 30);
    C.desktopWorld = cfg.GetBool("desktop_world", true);
    C.desktopEye = cfg.GetStr("desktop_view", "eye") != "game";
    C.perfLog = cfg.GetBool("perf_log", false);
    C.perfSlowMs = cfg.GetFloat("perf_log_slow_ms", 0);

    C.hudDistance = cfg.GetFloat("hud_distance", 1.6f);
    C.hudSize = cfg.GetFloat("hud_size", 1.6f);
    C.hudOffsetY = cfg.GetFloat("hud_offset_y", -0.05f);
    C.hudFollowHead = cfg.GetBool("hud_follow_head", false);
    C.menuDistance = cfg.GetFloat("menu_distance", 2.0f);
    C.menuSize = cfg.GetFloat("menu_size", 2.4f);
    xrout::Settings s;
    s.appName = "Edge of Eternity VR";
    s.dumpDir = gameDir;
    s.hudDistance = C.hudDistance;
    s.hudWidth = C.hudSize;
    s.hudOffsetY = C.hudOffsetY;
    s.hudFollowHead = C.hudFollowHead;
    s.menuDistance = C.menuDistance;
    s.menuWidth = C.menuSize;
    s.flipEyesY = cfg.GetBool("flip_eyes_y", true);
    s.flipHudY = cfg.GetBool("flip_hud_y", true);
    s.flipEyesX = cfg.GetBool("flip_eyes_x", false);
    s.flipHudX = cfg.GetBool("flip_hud_x", false);
    s.settingsMenu = cfg.GetBool("settings_menu", true);
    s.settingsMenuDistance = cfg.GetFloat("settings_menu_distance", 1.0f);
    s.settingsMenuSize = cfg.GetFloat("settings_menu_size", 0.9f);
    s.settingsMenuFollowHead = cfg.GetBool("settings_menu_follow_head", false);
    s.configPath = gameDir + L"\\config.txt";
    s.menuTitle = "Edge of Eternity VR  " EOEVR_VERSION;
    s.menuStatus = [] {
        char b[96];
        snprintf(b, sizeof(b), "game %.0f fps | headset %.0f fps | eyes %.1f ms GPU", g_statGameFps.load(), g_statXrFps.load(),
                 g_statGpuMs.load());
        return std::string(b);
    };
    RegisterMenuOptions();
    if (!xrout::Init(s)) { LOGE("stereo: OpenXR thread failed to start"); C.vr = false; return false; }
    LOGI("stereo: initialised (world cams %zu, overlay cams %zu, hud cams %zu, scale %.2f, aa %s)", C.worldCams.size(),
         C.overlayCams.size(), C.hudCams.size(), C.renderScale, C.gameAA.c_str());
    return true;
}

bool InOwnRender() { return g_inOwnRender; }

bool DlssRegenOverride(int* sw, int* sh, int* tw, int* th) {
    if (!C.vr || !C.dlssInVr || g_eyeW <= 0 || *tw <= 0 || *th <= 0) return false;
    float rx = (float)*sw / *tw, ry = (float)*sh / *th;
    int nsw = (int)(g_eyeW * rx + 0.5f) & ~1, nsh = (int)(g_eyeH * ry + 0.5f) & ~1;
    LOGI("stereo: DLSS RegenBuffers %dx%d -> %dx%d  becomes  %dx%d -> %dx%d (per eye, ratio %.3f)", *sw, *sh, *tw, *th, nsw, nsh,
         g_eyeW, g_eyeH, rx);
    *sw = nsw; *sh = nsh; *tw = g_eyeW; *th = g_eyeH;
    return true;
}

bool OnSetProjection(Object* cam, const Matrix4x4* m) {
    if (!g_curCam || cam != g_curCam) return false;
    // Keep the eye projection; carry over a sub-pixel jitter (TAA) from the matrix the script built.
    Matrix4x4 p = g_curProj;
    if (m) { p.m[2 * 4 + 0] += m->m[2 * 4 + 0]; p.m[2 * 4 + 1] += m->m[2 * 4 + 1]; }
    I.Camera_set_projectionMatrix(cam, &p);
    // DLSS (StartJitterFrame) stores the matrix it built from the game's symmetric field of view and uses it for its
    // motion vectors: give it the eye's jittered projection instead.
    if (Object* d = g_dlss.get(); d && cam == g_worldCam.get() && DlssActive()) {
        static int oCur = FieldOffset(ClassOf(d), "m_currJitteredProjectionMatrix");
        if (oCur > 0) DField<Matrix4x4>(d, oCur) = p;
    }
    s_projIntercepts++;
    return true;
}

bool OnSetView(Object* cam, const Matrix4x4* m) {
    if (!g_curCam || cam != g_curCam) return false;
    I.Camera_set_worldToCameraMatrix(cam, &g_curView);
    s_viewIntercepts++;
    return true;
}
bool OnResetView(Object* cam) { return OnSetView(cam, nullptr); }

// DLSS.GetProjectionExtents(camera, jitterX, jitterY): frustum half-extents at distance 1 (xy) and the jitter (zw), from
// the camera's symmetric field of view; its shader rebuilds positions from depth with it. For an eye: the eye frustum,
// its off-centre part added to zw.
bool DlssProjectionExtents(Object* cam, float jx, float jy, float out[4]) {
    if (!g_curCam || cam != g_curCam) return false;
    float l = g_curTan[0], r = g_curTan[1], u = g_curTan[2], d = g_curTan[3];
    float w = (float)g_eyeW, h = (float)g_eyeH;
    if (Object* dl = g_dlss.get(); dl && g_do.srcW > 0 && DField<int>(dl, g_do.srcW) > 0) { w = (float)DField<int>(dl, g_do.srcW); h = (float)DField<int>(dl, g_do.srcH); }
    out[0] = (r - l) * 0.5f;
    out[1] = (u - d) * 0.5f;
    out[2] = (r + l) * 0.5f + (r - l) / w * jx;
    out[3] = (u + d) * 0.5f + (u - d) / h * jy;
    s_extents++;
    return true;
}

bool OnResetProjection(Object* cam) {
    if (!g_curCam || cam != g_curCam) return false;
    I.Camera_set_projectionMatrix(cam, &g_curProj);
    s_projIntercepts++;
    return true;
}

void OnCameraPostRender(Object* cam) {
    if (g_inOwnRender || !cam || cam != g_mirrorCam.get()) return;
    s_hudPost++;
    for (Object* c : g_skipped) if (Alive(c)) { I.Behaviour_set_enabled(c, true); s_reenabled++; }
    g_skipped.clear();
    g_mirrorReady = false;
}

bool OnGameCameraRender(Object* cam) {
    if (!C.vr || g_inOwnRender || !cam || cam != g_worldCam.get()) return true;
    p_gameRenders++;
    if (C.desktopWorld || !xrout::Ready()) return true;
    if (g_renderedThisFrame.size() < 64) g_renderedThisFrame.push_back(cam);
    p_skippedRenders++;
    return false;
}

void OnCameraPreCull(Object* cam) {
    if (!g_inOwnRender && g_renderedThisFrame.size() < 64) g_renderedThisFrame.push_back(cam);
}

void OnFrame() {
    if (!C.vr) return;
    // Pacing first, so the poses read below are the freshest for the headset frame that just started.
    if (C.pacing && xrout::Ready()) p_pace.add(xrout::WaitForHeadsetFrame(C.pacingTimeoutMs));
    double tf = perf::NowMs();
    if (p_lastFrame) p_frame.add(tf - p_lastFrame);
    p_lastFrame = tf;
    p_frames++;
    g_renderedLastFrame.swap(g_renderedThisFrame);
    g_renderedThisFrame.clear();
    s_frames++;
    Report();
    PerfSecond();

    if (KeyPressed(C.keyRecenter)) { xrout::Recenter(); LOGI("F9: recenter"); }
    if (KeyPressed(C.keyHud)) { g_hudVisible = !g_hudVisible; LOGI("F10: HUD %s", g_hudVisible ? "on" : "off"); }
    bool dumpNow = KeyPressed(C.keyDump);
    if (C.debugKeys) {
        if (KeyPressed(VK_F6)) { C.dlssDebugView = (C.dlssDebugView + 1) % 3; LOGI("F6: dlss_debug_view = %d", C.dlssDebugView); }
        if (KeyPressed(VK_F7)) { C.swapEyes = !C.swapEyes; LOGI("F7: right eye first = %d", C.swapEyes); }
    }

    if (!xrout::Ready()) return;
    if (!EnsureTargets()) return;
    RefreshNativePointers();

    DWORD now = GetTickCount();
    if (now - g_lastScan > 1000 || (g_worldCam.get() && !Alive(g_worldCam.get())) || g_aaDirty.load()) {
        g_lastScan = now;
        ScanCameras();
    }
    xrout::FrameInfo fi{};
    if (!xrout::LatestPoses(fi.pose, fi.fov, &fi.time)) { s_noPose++; return; }
    fi.flags = g_hudVisible && C.hudInHeadset ? xrout::FRAME_HUD_VISIBLE : 0;
    if (C.menuPanel && GameMenuOpen()) fi.flags |= xrout::FRAME_HUD_MENU_PANEL | xrout::FRAME_HUD_VISIBLE;

    Object* cam = g_worldCam.get();
    bool worldRendered = g_skippedLastFrame && cam && IsActive(cam);   // we skipped the game's own render
    for (Object* r : g_renderedLastFrame) worldRendered |= (r == cam);
    g_skippedLastFrame = false;
    // While the game redirects the world camera into a texture (loading screen upscaler, cutscene capture ...) it is
    // not the gameplay view: treat the frame like a menu.
    if (cam && Alive(cam)) { EnforceDlss(cam); ApplyDancingShadows(cam); ApplyEffectTest(cam); }
    bool dlssEyes = cam && Alive(cam) && DlssActive();
    dlsseyes::TryInstall();
    if (dlssEyes) DlssEnsureEyeSize();
    Object* camTarget = cam && Alive(cam) ? I.Camera_get_targetTexture(cam) : nullptr;
    bool toScreen = cam && Alive(cam) && (camTarget == nullptr ||
                    (dlssEyes && g_do.color > 0 && camTarget == DField<Object*>(g_dlss.get(), g_do.color)));
    if (cam && Alive(cam) && worldRendered && !toScreen && !s_warnedTarget) {
        s_warnedTarget = true;
        LOGI("stereo: world camera renders into a texture (loading?) - no stereo until it renders to the screen");
    }
    if (toScreen) s_warnedTarget = false;
    if (cam && Alive(cam) && worldRendered && toScreen) {
        Object* tr = Invoke(M.Component_get_transform, cam);
        Vec3 gp{};
        Quat gr{0, 0, 0, 1};
        if (tr) { I.Transform_get_position(tr, &gp); I.Transform_get_rotation(tr, &gr); }
        if (C.levelCamera) gr = YawOnly(gr);
        float n = I.Camera_get_nearClipPlane(cam), f = I.Camera_get_farClipPlane(cam);
        double ts0 = perf::NowMs();
        perf::SetGameTexture(g_eyeTex[0]);
        I.GL_IssuePluginEvent((void*)&perf::GpuEvent, 1);
        g_inOwnRender = true;
        for (int k = 0; k < 2; k++) {
            int e = C.swapEyes ? 1 - k : k;
            if (k > 0) { g_curCam = cam; RerunGpui(); g_curCam = nullptr; if (C.drawReplay) drawreplay::Replay(); }
            RenderEye(cam, e, gp, gr, fi.pose[e], fi.fov[e], n, f);
            for (auto& h : g_overlayCams) {
                Object* oc = h.get();
                if (!oc || !Alive(oc) || !IsActive(oc)) continue;
                float on = I.Camera_get_nearClipPlane(oc), of = I.Camera_get_farClipPlane(oc);
                RenderEye(oc, e, gp, gr, fi.pose[e], fi.fov[e], on, of);
            }
        }
        g_inOwnRender = false;
        bool skipDesktop = C.desktopEye && EnsureMirrorCamera();
        if (skipDesktop) {
            // The game's own render of the stereo cameras is not needed: disable them until our HUD camera (the last
            // camera of the frame) has rendered; the window gets the left eye instead.
            auto skip = [](Object* c) { if (c && Alive(c) && I.Behaviour_get_enabled(c)) { I.Behaviour_set_enabled(c, false); g_skipped.push_back(c); } };
            skip(cam);
            for (auto& h : g_overlayCams) skip(h.get());
            if (DlssActive() && g_do.imposter > 0) skip(DField<Object*>(g_dlss.get(), g_do.imposter));
            s_skips++;
            static bool logged = false;
            if (!logged) { logged = true; LOGI("stereo: desktop_view = eye: skipping the game's own render of %zu camera(s)", g_skipped.size()); }
            g_skippedLastFrame = true;
            g_mirrorReady = true;
            mirror::SetSource(g_eyeTex[0], g_viewFormat);
            mirror::SetEnabled(true);
        } else if (C.desktopWorld) {
            mirror::SetEnabled(false); RerunGpui(); if (C.drawReplay) drawreplay::Replay(); }   // and for the game's own (monitor) render
        I.GL_IssuePluginEvent((void*)&perf::GpuEvent, 2);
        p_stereoCpu.add(perf::NowMs() - ts0);
        s_stereo++;
    } else {
        // No 3D world this frame (full-screen menu, loading): show the UI on the large menu panel.
        fi.flags |= xrout::FRAME_MENU_MODE | xrout::FRAME_HUD_VISIBLE;
        mirror::SetEnabled(false);
        s_menu++;
    }

    if (!g_firstVrTick) g_firstVrTick = now;
    if (dumpNow || (C.debugDumpAtSec > 0 && !g_dumped && now - g_firstVrTick > (DWORD)(C.debugDumpAtSec * 1000))) {
        g_dumped = true;
        xrout::RequestDump(++g_dumpId);
        LOGI("dump %u requested (eoevr_dump_%u_*.bmp)", g_dumpId, g_dumpId);
    }

    // Queued in order on the render thread: eyes rendered above -> copy (eyes + last frame's UI pair) -> the white UI
    // for this frame -> clear the HUD for the UI cameras that render later this frame (over black).
    int id = g_ringIdx++ & (kRing - 1);
    g_ring[id] = fi;
    // Normally the render thread released this slot 16 frames ago. If it is that far behind, keep its references
    // (a small leak) rather than racing it with a second Release.
    if (g_ringTex[id].eye[0]) { static int n = 0; if (n++ < 5) LOGW("stereo: render thread 16 frames behind"); }
    g_ringTex[id] = {{g_eyeTex[0], g_eyeTex[1]}, C.hudInHeadset ? g_hudTex : nullptr,
                     C.hudInHeadset && C.hudMatte ? g_hudWhiteTex : nullptr};
    RingAddRef(g_ringTex[id]);
    I.GL_IssuePluginEvent((void*)&RenderThreadSubmit, id);
    drawreplay::BeginFrame(FrameCount());
    if (C.hudInHeadset) {
        if (C.hudMatte) RenderHudWhite();
        ClearHud();
    }
}

} // namespace uxr::stereo
