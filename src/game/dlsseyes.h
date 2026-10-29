// One DLSS history per eye. The game's DLSS plugin (OrionGraphics_DLSS.dll, NGX statically linked) creates ONE NGX
// DLSS feature, so with two eye renders per frame each eye's frame was blended with the other eye's history
// (ghosted/doubled characters, eyes not lining up). The plugin's NVSDK_NGX_D3D11_CreateFeature / EvaluateFeature /
// ReleaseFeature are hooked: a second feature is created with the same parameters and used for the right eye.
// The eye is known on the render thread from a plugin event issued before each eye's render.
#pragma once
#include <cstdint>
namespace uxr::dlsseyes {
bool TryInstall();                 // main thread, every frame until the plugin is loaded; true once hooked
void SetEnabled(bool on);          // off = the game's single feature for both eyes (old behaviour)
void* EyeEvent();                  // GL.IssuePluginEvent(EyeEvent(), eye) before an eye's renders
bool TakeRecreateRequest();        // an evaluate saw a feature created before the hooks -> re-initialise DLSS
struct Stats { uint32_t created, createdSecond, evalLeft, evalRight, evalUnknown, released; int lastError; };
Stats Take();
}
