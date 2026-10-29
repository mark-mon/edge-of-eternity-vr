// Unity consumes draws queued with Graphics.DrawMeshInstancedIndirect (the game's vegetation, flocks, swarms) in the
// first camera render after they were queued. With two eye renders + the desktop render per frame only the first got
// grass. The native icall is hooked; this frame's draws are recorded and queued again before every further render.
#pragma once
namespace uxr::drawreplay {
bool Install();          // after unity::Init and MH_Initialize
void BeginFrame(int frame);
void Replay();           // queue this frame's recorded draws again (before the next camera render)
void SetEnabled(bool on);
struct Stats { unsigned recorded, replayed, frames; };
Stats Take();
}
