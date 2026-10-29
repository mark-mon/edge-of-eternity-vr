// Native per-eye stereo for Unity (built-in pipeline): renders the game's world camera once per eye with the headset
// view/projection, captures the UI cameras into a HUD texture, and hands both to xrout on the render thread.
#pragma once
#include <string>
#include "../common/config.h"
#include "../unity/il2cpp.h"

namespace uxr::stereo {

bool Init(const Config& cfg, const std::wstring& gameDir); // main thread, after unity::Init
void OnFrame();                                              // main thread, once per frame (PostLateUpdate)
void OnCameraPreCull(il2cpp::Object* cam);                   // main thread, every camera render
bool InOwnRender();                                          // true while we are calling Camera.Render ourselves
// The game is about to Render() a camera itself. Returns false to skip it (desktop_world = 0 for the world camera).
bool OnGameCameraRender(il2cpp::Object* cam);
// A script set / reset the projection of a camera. Returns true when it was one of our eye renders and the eye
// projection was re-applied instead (a TAA jitter in m is carried over).
bool OnSetProjection(il2cpp::Object* cam, const il2cpp::Matrix4x4* m);
bool OnResetProjection(il2cpp::Object* cam);
bool OnSetView(il2cpp::Object* cam, const il2cpp::Matrix4x4* m);
bool OnResetView(il2cpp::Object* cam);
bool DlssProjectionExtents(il2cpp::Object* cam, float jx, float jy, float out[4]);
void OnCameraPostRender(il2cpp::Object* cam);
// The game's DLSS is (re)creating its buffers: with dlss_in_vr = 1 the sizes are replaced by per-eye sizes.
bool DlssRegenOverride(int* srcW, int* srcH, int* targW, int* targH);                // main thread, after every camera render

} // namespace uxr::stereo
