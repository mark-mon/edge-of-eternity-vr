// Foveated rendering (NVIDIA VRS through NVAPI) for the Unity eye renders. See vrs.cpp.
#pragma once
#include <d3d11.h>
#include <cstdint>
namespace uxr::vrs {
bool Install(ID3D11Device* gameDevice);      // hooks the game's immediate context OMSetRenderTargets (once)
// mode 0 off, 1 fixed (centre of each eye), 2 eye tracked (falls back to fixed without gaze); strength 0 low, 1 medium,
// 2 high; inner / outer ring radii (1.0 = half the image width)
void Configure(int mode, int strength, float inner, float outer);
bool Enabled();
// Main thread, before an eye render: stores the eye's parameters; returns the slot for the plugin events.
// fov = left, right, up, down angles (radians); scene size = the camera's render size for this eye.
int PrepareEye(int eye, unsigned sceneW, unsigned sceneH, const float fov[4], bool gazeValid, float gx, float gy);
void* BeginEvent();   // GL.IssuePluginEvent(BeginEvent(), slot) before the eye's Camera.Render
void* EndEvent();     // ... and EndEvent() after it
struct Stats { uint32_t sceneBinds, otherBinds, uploads; };
Stats Take();
}
