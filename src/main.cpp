// Edge of Eternity VR - entry point. Loaded by UnityPlayer.dll as version.dll (see loader/version_proxy.cpp).
// Stage 0: diagnostics probe (no VR yet). Waits for the IL2CPP runtime, hooks Unity's per-camera and per-frame
// callbacks on the main thread, and logs the graphics API, cameras, canvases and per-frame render order.
#include <windows.h>
#include <string>
#include "common/log.h"
#include "common/config.h"
#include "unity/il2cpp.h"
#include "unity/unity.h"
#include "unity/probe.h"
#include "vr/stereo.h"
#include "vr/xrout.h"
#include "game/drawreplay.h"
#include "vr/mirror.h"
#include "MinHook.h"
#include "common/crash_log.h"

#include "version.h"
#include "unity/typedump.h"

using namespace uxr;

static std::wstring g_gameDir;
static Config g_cfg;
static bool g_probe = true;
static bool g_vr = false;

static const char kDefaultConfig[] =
    "# Edge of Eternity VR - configuration\n"
    "# key = value. Lines starting with # are comments. Most settings can also be changed in the headset menu\n"
    "# (hold L3+R3 or press Insert); the menu saves them back into this file.\n"
    "\n"
    "# 0 = mod does nothing at all\n"
    "enabled = 1\n"
    "# 0 error, 1 warn, 2 info, 3 debug, 4 trace\n"
    "log_level = 2\n"
    "\n"
    "# ---- VR output (native OpenXR, SteamVR or any OpenXR runtime)\n"
    "vr = 1\n"
    "# Per-eye render size: headset recommended size x preset (performance 70%, balanced 85%, native 100%, high 120%,\n"
    "# ultra 140%, custom = render_scale), or eye_width/eye_height if both > 0\n"
    "render_preset = native\n"
    "render_scale = 1.0\n"
    "eye_width = 0\n"
    "eye_height = 0\n"
    "# Post-processing anti-aliasing in VR: keep (game setting), none, fxaa, smaa, taa\n"
    "# The game's DLSS, FSR and TAA options are switched off while VR runs (they break the stereo image); the game\n"
    "# then renders at full eye resolution with this anti-aliasing. For performance use the resolution presets and foveation.\n"
    "game_aa = smaa\n"
    "# Camera components kept off in VR (DLSS = the game's DLSS, AMD_CAS = FSR, TAA2 / CTAA_PC = TAA)\n"
    "vr_disable_components = DLSS,AMD_CAS,TAA2,CTAA_PC\n"
    "# EXPERIMENTAL, not working correctly (wrong scale / stereo, no speed gain): 1 = run the game's DLSS per eye\n"
    "dlss_in_vr = 0\n"
    "# Foveated rendering (NVIDIA VRS): off, fixed, eye (eye tracked; fixed when the headset has no eye tracking)\n"
    "foveation = fixed\n"
    "# low, medium, high\n"
    "foveation_strength = high\n"
    "# Block Dancing Shadows: 1 = the game's screen-space contact shadows (NGSS frustum shadows) are switched off; they\n"
    "# sway with head motion in VR. 0 = the game's look (slightly richer, but distant shadows dance)\n"
    "block_dancing_shadows = 1\n"
    "# Ring radii (1.0 = half the image width): full rate inside foveation_inner, lowest rate outside foveation_outer\n"
    "foveation_inner = 0.40\n"
    "foveation_outer = 0.70\n"
    "# The game's grass / vegetation draws reach only the first camera render of a frame; 1 = queue them again for the\n"
    "# second eye and the monitor\n"
    "draw_replay = 1\n"
    "# Head tracking moves the game camera with your head. world_scale = game units per metre of head motion\n"
    "head_tracking = 1\n"
    "world_scale = 1.0\n"
    "# 1 = remove the game camera's up/down tilt (keep its heading) so the horizon stays level\n"
    "level_camera = 1\n"
    "\n"
    "# Unity renders into D3D textures upside down; the headset images are flipped back (set 0 if upside down)\n"
    "flip_eyes_y = 1\n"
    "flip_hud_y = 1\n"
    "flip_eyes_x = 0\n"
    "flip_hud_x = 0\n"
    "\n"
    "# ---- Performance\n"
    "# Render at most one game frame per headset frame (stops the game from flooding the GPU)\n"
    "frame_pacing = 1\n"
    "# Monitor: eye = show the headset's left eye (skips the game's own full render: faster); game = the game's own view\n"
    "desktop_view = eye\n"
    "# One performance line per second in eoevr.log (perf_log_slow_ms > 0: only seconds with a slower game frame)\n"
    "perf_log = 0\n"
    "perf_log_slow_ms = 0\n"
    "\n"
    "# ---- HUD / UI on its own panel\n"
    "hud_layer = 1\n"
    "# Move the game's screen-overlay UI onto the HUD panel, and compute its transparency from a black + white render\n"
    "hud_canvases = 1\n"
    "hud_matte = 1\n"
    "hud_tex_width = 1920\n"
    "hud_tex_height = 1080\n"
    "hud_distance = 1.6\n"
    "hud_size = 1.6\n"
    "hud_offset_y = -0.05\n"
    "hud_follow_head = 0\n"
    "# Full-screen menus and loading screens (no 3D world) are shown on a larger panel\n"
    "menu_distance = 2.0\n"
    "menu_size = 2.4\n"
    "# Game menus (status / Y menu, pause and options menus, full map) on the game-menu panel above instead of the HUD panel\n"
    "menu_panel = 1\n"
    "# Canvas names (parts) treated as game menus; canvases of the game's UI camera always are\n"
    "menu_canvases = PauseMenu,StatusUI,MinimapFullUI,MultiOptionsUI,ParametricSettingsUI\n"
    "\n"
    "# ---- In-headset settings menu: hold L3+R3 (both stick clicks) or press Insert. Changes are saved here on close.\n"
    "settings_menu = 1\n"
    "settings_menu_distance = 1.0\n"
    "settings_menu_size = 0.9\n"
    "settings_menu_follow_head = 0\n"
    "\n"
    "# ---- Cameras (names as logged in eoevr.log)\n"
    "# world: rendered per eye (first that renders is used); overlay: drawn per eye on top of it (3D UI);\n"
    "# hud: the game's own screen UI cameras, redirected to the HUD panel\n"
    "world_cameras = RawCam_v2\n"
    "overlay_cameras = UICam\n"
    "hud_cameras = LoadingCam\n"
    "\n"
    "# ---- Keys (virtual-key codes): F9 recenter, F10 HUD on/off, F11 debug dump of the headset images\n"
    "key_recenter = 0x78\n"
    "key_hud = 0x79\n"
    "key_dump = 0x7A\n"
    "\n"
    "# ---- Diagnostics\n"
    "# Logs every camera / canvas on scene changes and on the hotkey (F8) - for adapting the mod\n"
    "probe = 0\n"
    "probe_hotkey = 0x77\n"
    "# Testing: request a headset image dump this many seconds after VR starts (0 = off)\n"
    "debug_dump_at_sec = 0\n"
    "# Writes eoevr_types.txt (all game classes / fields / methods) once at startup\n"
    "dump_types = 0\n"
    "# Keeps one of the game's world-camera effects off (also Debug > Switch off effect in the menu), to find an effect\n"
    "# that misbehaves in VR: none, HBAO, AmplifyOcclusionEffect, NNAO, SSRT, SEScreenSpaceShadows, NGSS_FrustumShadows,\n"
    "# SSR2, GlobalFog, StylisticFog, TenkokuSkyFog, HxVolumetricCamera, AuraCamera, FogVolumeRenderer, SSMSGlobalFog, SSMS,\n"
    "# GlobalMaterialIDRenderer, PostProcessLayer, FantasyImageEffect, HPFSharpen, MotionBlur, MadGoatSSAA, BlurEffect, VRS\n"
    "debug_effect_off = none\n";

// ---- hooks (Unity main thread)
typedef void (*FireOnPreCull_t)(il2cpp::Object* cam, const il2cpp::MethodInfo* mi);
typedef void (*SendWillRenderCanvases_t)(const il2cpp::MethodInfo* mi);
static FireOnPreCull_t o_FireOnPreCull = nullptr;
static SendWillRenderCanvases_t o_SendWillRenderCanvases = nullptr;
static SendWillRenderCanvases_t o_InvokeOnBeforeRender = nullptr;   // same signature: static void (MethodInfo*)
static int g_lastStereoFrame = -2;
static int g_beforeRenderFrames = 0;   // how often the preferred per-frame hook has fired

// Stereo runs once per frame after LateUpdate and before rendering. Preferred: Application.InvokeOnBeforeRender
// (exactly once per frame). Fallback: the first Canvas.SendWillRenderCanvases of a frame (the game can force extra
// canvas updates, so it may fire several times per frame).
static void StereoTick(bool fromBeforeRender) {
    if (!g_vr) return;
    if (fromBeforeRender) g_beforeRenderFrames++;
    else if (g_beforeRenderFrames > 0) return;
    int f = unity::FrameCount();
    if (f == g_lastStereoFrame) return;
    g_lastStereoFrame = f;
    stereo::OnFrame();
}
static int g_lastTickFrame = -2;
static bool g_firstTick = true;

static void FrameTick() {
    WatchdogBeat(0);
    int f = unity::FrameCount();
    if (f == g_lastTickFrame) return;
    g_lastTickFrame = f;
    if (g_firstTick) {
        g_firstTick = false;
        LOGI("first main-thread tick on thread %lu (frame %d)", GetCurrentThreadId(), f);
        WatchdogSetThread(0, GetCurrentThreadId());
        probe::LogSystem();
    }
    if (g_probe) probe::Tick();
}

static void h_SendWillRenderCanvases(const il2cpp::MethodInfo* mi) {
    FrameTick();
    StereoTick(false);
    o_SendWillRenderCanvases(mi);
}

typedef void (*CameraRender_t)(il2cpp::Object* cam, const il2cpp::MethodInfo* mi);
static CameraRender_t o_CameraRender = nullptr;
// Camera.Render() called by game scripts (our own eye renders call the icall directly and never come through here).
static void h_CameraRender(il2cpp::Object* cam, const il2cpp::MethodInfo* mi) {
    if (g_vr && !stereo::OnGameCameraRender(cam)) return;
    o_CameraRender(cam, mi);
}

typedef void (*SetProj_t)(il2cpp::Object* cam, il2cpp::Matrix4x4* m, const il2cpp::MethodInfo* mi);
typedef void (*ResetProj_t)(il2cpp::Object* cam, const il2cpp::MethodInfo* mi);
static SetProj_t o_SetProj = nullptr;
static ResetProj_t o_ResetProj = nullptr;
// Scripts (post-processing TAA, effects) that set or reset the projection during our eye renders.
static void h_SetProj(il2cpp::Object* cam, il2cpp::Matrix4x4* m, const il2cpp::MethodInfo* mi) {
    if (g_vr && stereo::OnSetProjection(cam, m)) return;
    o_SetProj(cam, m, mi);
}
static void h_ResetProj(il2cpp::Object* cam, const il2cpp::MethodInfo* mi) {
    if (g_vr && stereo::OnResetProjection(cam)) return;
    o_ResetProj(cam, mi);
}

static void h_InvokeOnBeforeRender(const il2cpp::MethodInfo* mi) {
    StereoTick(true);
    o_InvokeOnBeforeRender(mi);
}

static void h_FireOnPreCull(il2cpp::Object* cam, const il2cpp::MethodInfo* mi) {
    FrameTick(); // fallback when no canvas exists
    if (g_vr) stereo::OnCameraPreCull(cam);
    if (g_probe && !stereo::InOwnRender()) probe::OnCameraPreCull(cam);
    o_FireOnPreCull(cam, mi);
}

typedef void (*FireOnPostRender_t)(il2cpp::Object* cam, const il2cpp::MethodInfo* mi);
static FireOnPostRender_t o_FireOnPostRender = nullptr;
static void h_FireOnPostRender(il2cpp::Object* cam, const il2cpp::MethodInfo* mi) {
    o_FireOnPostRender(cam, mi);
    if (g_vr) stereo::OnCameraPostRender(cam);
}

typedef void (*DlssRegen_t)(il2cpp::Object* self, int sw, int sh, int tw, int th, const il2cpp::MethodInfo* mi);
static DlssRegen_t o_DlssRegen = nullptr;
static void h_DlssRegen(il2cpp::Object* self, int sw, int sh, int tw, int th, const il2cpp::MethodInfo* mi) {
    if (g_vr) stereo::DlssRegenOverride(&sw, &sh, &tw, &th);
    o_DlssRegen(self, sw, sh, tw, th, mi);
}

// Native camera-matrix icalls. IL2CPP inlines the tiny managed wrappers (Camera.set_projectionMatrix ->
// set_projectionMatrix_Injected), so scripts such as DLSS.StartJitterFrame and PPv2 call these directly; hooking only
// the managed methods missed them (eyes rendered with the game's symmetric projection while DLSS/TAA was on).
// unity::I keeps the trampolines, so the mod's own calls are not intercepted.
typedef void (*IcMat_t)(il2cpp::Object* cam, const il2cpp::Matrix4x4* m);
typedef void (*IcReset_t)(il2cpp::Object* cam);
static void h_icSetProj(il2cpp::Object* cam, const il2cpp::Matrix4x4* m) {
    if (g_vr && stereo::OnSetProjection(cam, m)) return;
    unity::I.Camera_set_projectionMatrix(cam, m);
}
static void h_icResetProj(il2cpp::Object* cam) {
    if (g_vr && stereo::OnResetProjection(cam)) return;
    unity::I.Camera_ResetProjectionMatrix(cam);
}
static void h_icSetView(il2cpp::Object* cam, const il2cpp::Matrix4x4* m) {
    if (g_vr && stereo::OnSetView(cam, m)) return;
    unity::I.Camera_set_worldToCameraMatrix(cam, m);
}
static void h_icResetView(il2cpp::Object* cam) {
    if (g_vr && stereo::OnResetView(cam)) return;
    unity::I.Camera_ResetWorldToCameraMatrix(cam);
}
template <class F> static bool HookIcall(F& slot, void* detour, const char* what) {
    void* target = (void*)slot;
    void* tramp = nullptr;
    if (!target || MH_CreateHook(target, detour, &tramp) != MH_OK || MH_EnableHook(target) != MH_OK) {
        LOGE("hook icall %s failed", what);
        return false;
    }
    slot = (F)tramp;
    LOGI("hooked icall %s at %p", what, target);
    return true;
}

typedef float* (*DlssExtents_t)(float* ret, il2cpp::Object* cam, float jx, float jy, const il2cpp::MethodInfo* mi);
static DlssExtents_t o_DlssExtents = nullptr;
static float* h_DlssExtents(float* ret, il2cpp::Object* cam, float jx, float jy, const il2cpp::MethodInfo* mi) {
    float v[4];
    if (g_vr && stereo::DlssProjectionExtents(cam, jx, jy, v)) { ret[0] = v[0]; ret[1] = v[1]; ret[2] = v[2]; ret[3] = v[3]; return ret; }
    return o_DlssExtents(ret, cam, jx, jy, mi);
}

static bool HookMethod(const il2cpp::MethodInfo* m, void* detour, void** orig, const char* what) {
    void* target = il2cpp::MethodPointer(m);
    if (!target) { LOGE("hook %s: no method pointer", what); return false; }
    if (MH_CreateHook(target, detour, orig) != MH_OK || MH_EnableHook(target) != MH_OK) {
        LOGE("hook %s: MinHook failed", what);
        return false;
    }
    LOGI("hooked %s at GameAssembly+0x%llx", what, (unsigned long long)((char*)target - (char*)il2cpp::gameAssembly));
    return true;
}

static DWORD WINAPI InitThread(void*) {
    HMODULE ga = nullptr;
    for (int i = 0; i < 120000 / 5 && !(ga = GetModuleHandleW(L"GameAssembly.dll")); i++) Sleep(5);
    if (!ga) { LOGE("GameAssembly.dll never loaded (not an IL2CPP game?)"); return 0; }
    LOGI("GameAssembly.dll at %p", ga);
    if (!il2cpp::LoadApi(ga)) { LOGE("il2cpp API incomplete; giving up"); return 0; }
    if (!il2cpp::WaitForRuntime(120000)) { LOGE("il2cpp runtime did not initialise"); return 0; }
    Sleep(200);
    il2cpp::AttachThisThread();
    LOGI("il2cpp runtime up");
    if (g_cfg.GetBool("dump_types", false))
        typedump::Dump((g_gameDir + L"\\eoevr_types.txt").c_str(), "System,Mono.,mscorlib,Newtonsoft,YamlDotNet,Google.,RestSharp,websocket,Unity.Burst.Cecil,Ionic");
    if (!unity::Init()) { LOGE("UnityEngine classes not found"); return 0; }
    g_vr = stereo::Init(g_cfg, g_gameDir);

    if (MH_Initialize() != MH_OK) { LOGE("MH_Initialize failed"); return 0; }
    if (g_vr) xrout::EnableSettingsMenu();
    if (g_vr) drawreplay::Install();
    if (g_vr) mirror::Install();
    if (g_vr) {
        if (il2cpp::Class* k = il2cpp::FindClass("Orion.Graphics.DLSS", "DLSS", "Assembly-CSharp.dll"))
            if (const il2cpp::MethodInfo* m = il2cpp::FindMethod(k, "RegenBuffers", 4))
                HookMethod(m, (void*)h_DlssRegen, (void**)&o_DlssRegen, "DLSS.RegenBuffers");
        if (il2cpp::Class* k = il2cpp::FindClass("Orion.Graphics.DLSS", "DLSS", "Assembly-CSharp.dll"))
            if (const il2cpp::MethodInfo* m = il2cpp::FindMethod(k, "GetProjectionExtents", 3))
                HookMethod(m, (void*)h_DlssExtents, (void**)&o_DlssExtents, "DLSS.GetProjectionExtents");
        HookIcall(unity::I.Camera_set_projectionMatrix, (void*)h_icSetProj, "Camera.set_projectionMatrix_Injected");
        HookIcall(unity::I.Camera_ResetProjectionMatrix, (void*)h_icResetProj, "Camera.ResetProjectionMatrix");
        HookIcall(unity::I.Camera_set_worldToCameraMatrix, (void*)h_icSetView, "Camera.set_worldToCameraMatrix_Injected");
        HookIcall(unity::I.Camera_ResetWorldToCameraMatrix, (void*)h_icResetView, "Camera.ResetWorldToCameraMatrix");
    }   // also when desktop_view = game: it can be switched in the menu
    if (g_vr && unity::M.Camera_FireOnPostRender)
        HookMethod(unity::M.Camera_FireOnPostRender, (void*)h_FireOnPostRender, (void**)&o_FireOnPostRender, "Camera.FireOnPostRender");
    HookMethod(unity::M.Camera_FireOnPreCull, (void*)h_FireOnPreCull, (void**)&o_FireOnPreCull, "Camera.FireOnPreCull");
    if (unity::M.Canvas_SendWillRenderCanvases)
        HookMethod(unity::M.Canvas_SendWillRenderCanvases, (void*)h_SendWillRenderCanvases,
                   (void**)&o_SendWillRenderCanvases, "Canvas.SendWillRenderCanvases");
    if (const il2cpp::MethodInfo* render = il2cpp::FindMethod(unity::M.Camera, "Render", 0))
        HookMethod(render, (void*)h_CameraRender, (void**)&o_CameraRender, "Camera.Render (managed)");
    if (g_vr) {
        if (const il2cpp::MethodInfo* m = il2cpp::FindMethod(unity::M.Camera, "set_projectionMatrix", 1))
            HookMethod(m, (void*)h_SetProj, (void**)&o_SetProj, "Camera.set_projectionMatrix");
        if (const il2cpp::MethodInfo* m = il2cpp::FindMethod(unity::M.Camera, "ResetProjectionMatrix", 0))
            HookMethod(m, (void*)h_ResetProj, (void**)&o_ResetProj, "Camera.ResetProjectionMatrix");
    }
    if (unity::M.Application_InvokeOnBeforeRender)
        HookMethod(unity::M.Application_InvokeOnBeforeRender, (void*)h_InvokeOnBeforeRender,
                   (void**)&o_InvokeOnBeforeRender, "Application.InvokeOnBeforeRender");
    LogFlush();
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(inst);

    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    g_gameDir = exe;
    g_gameDir = g_gameDir.substr(0, g_gameDir.find_last_of(L"\\/"));
    // Only activate inside the game itself (UnityCrashHandler etc. may also load version.dll from here).
    std::wstring exeName = std::wstring(exe).substr(g_gameDir.size() + 1);
    if (_wcsicmp(exeName.c_str(), L"UnityCrashHandler64.exe") == 0) return TRUE;

    std::wstring cfgPath = g_gameDir + L"\\config.txt";
    WriteDefaultConfig(cfgPath.c_str(), kDefaultConfig);
    g_cfg.Load(cfgPath.c_str());
    if (!g_cfg.GetBool("enabled", true)) return TRUE;

    LogInit((g_gameDir + L"\\eoevr.log").c_str(), g_cfg.GetInt("log_level", LOG_INFO));
    LOGI("Edge of Eternity VR " EOEVR_VERSION " built " __DATE__ " " __TIME__ "; exe '%ls'", exeName.c_str());
    g_cfg.Dump();
    g_probe = g_cfg.GetBool("probe", false);
    probe::SetHotkey(g_cfg.GetInt("probe_hotkey", VK_F8));

    if (g_cfg.GetBool("crash_log", true)) InstallCrashLog();
    CloseHandle(CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr));
    return TRUE;
}
