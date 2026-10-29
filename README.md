# Edge of Eternity VR

A native OpenXR VR mod for **Edge of Eternity** (Steam, Unity 2021.3 IL2CPP, Direct3D 11).

- **True stereo:** each eye is rendered separately at the headset's resolution and with its exact projection. There is no alternate-eye rendering.
- **Head tracking:** a level horizon option and recentering.
- **Gamepad play:** you play with your controller as usual. The mod reads the pad itself, so its settings menu works even with Steam Input.
- **UI on its own panel:**
  - The game's HUD is moved onto a floating panel.
  - Full-screen game menus (status/Y menu, pause and options menus, full map) and loading screens go on a larger panel.
- **In-headset settings menu** (hold **L3 + R3**, or press **Insert**): every option can be changed live and is saved to `config.txt`.
- **Foveated rendering** (NVIDIA Variable Rate Shading):
  - **Fixed:** full detail in the centre of each eye, reduced detail in the periphery.
  - **Eye tracked:** the full-detail area follows your gaze, on headsets that expose eye tracking through OpenXR.
- **Desktop mirror:** the monitor shows the left eye. The game's own flat render is skipped, which saves GPU time.

Version **1.0.0**.

## DLSS, FSR and TAA are disabled in VR. Use foveated rendering instead

The game's own **DLSS** and **FSR** upscalers, and its **TAA** anti-aliasing, are built around a single flat screen. In VR they break the stereo image:
- DLSS gives the wrong scale and stereo.
- FSR gives a black screen.
- TAA gives a doubled image that sticks to your head.

We also could not get a real performance gain out of DLSS in this game. So while VR is running, the mod **switches all three off**, whatever is selected in the game's graphics options. The game then renders at full eye resolution with the mod's own anti-aliasing (SMAA by default).

To make up for that, the mod adds **foveated rendering**, which gave **almost 50% more performance** in our testing. On an RTX 5090 at 3980 x 3424 per eye, the frame rate went from 65 fps to the headset's 90 fps limit, with the GPU still at only 92%. It is on by default (fixed foveation, strength high). With an eye-tracked headset, set it to *Eye* in the settings menu.

## Requirements

- Edge of Eternity (Steam, Windows, Direct3D 11).
- A PC VR headset with an OpenXR runtime (SteamVR, Oculus/Meta, WMR, Varjo, Pimax ...). The mod was tested with SteamVR.
- An NVIDIA GPU (GTX 16xx / RTX or newer) for foveated rendering. Other GPUs run the mod without foveation.
- An Xbox-style controller (or one that Steam presents as one).

## Install

1. Download `version.dll` from the [latest release](https://github.com/mark-mon/edge-of-eternity-vr/releases/latest).
2. Copy it into the game folder, next to `EdgeOfEternity.exe`. In Steam: *right-click the game > Manage > Browse local files*.
3. Start your VR runtime (for example SteamVR), then start the game.

On first start the mod writes a commented `config.txt` and a log, `eoevr.log`, into the game folder.

To uninstall, delete `version.dll` (and `config.txt` / `eoevr.log` if you like).

After updating the mod, delete `config.txt` to get the new defaults and comments. The mod writes a new one.

## Controls

| Input | Action |
|---|---|
| Gamepad | Plays the game as usual |
| Hold **L3 + R3** (both stick clicks), or **Insert** | Open / close the VR settings menu |
| **F9** | Recenter the view |
| **F10** | Show / hide the HUD |
| **F11** | Save the current headset images (`eoevr_dump_*.bmp`) for bug reports |

## Settings menu

| Page | Options |
|---|---|
| **View** | Head tracking, world scale, level horizon, recenter |
| **Display** | Resolution preset (70 / 85 / 100 / 120 / 140 % of the headset's recommended size, or custom), anti-aliasing, monitor view, **foveated rendering** (off / fixed / eye; strength; inner and outer ring size), **Block Dancing Shadows**, grass in both eyes, frame pacing |
| **HUD** | HUD distance / size / height / follow head, game menu panel distance / width, game menus on the menu panel, clean HUD edges |
| **Debug** | Performance log, fake eye gaze, switch off a single camera effect (for tracking down effects that misbehave in VR), image dumps |

All settings are also in `config.txt`, with comments.

### Block Dancing Shadows

This option is on by default. The game's screen-space contact shadows (NGSS frustum shadows) make distant shadows on rocks and vegetation sway when you move your head, so the mod keeps them off. Turning the option off restores the game's slightly richer look, along with the dancing shadows.

## Troubleshooting

- **No VR image:** start the OpenXR runtime (SteamVR) before the game. Check `eoevr.log` in the game folder.
- **Upside-down or mirrored image:** see `flip_eyes_y` / `flip_hud_y` in `config.txt`.
- **The controller doesn't work in the settings menu:** only Xbox-type pads are supported. The log line `vrkit: pad found on user N via ...` shows where the pad was found.
- **Performance:** lower the resolution preset (Display > Resolution) or raise the foveation strength.
- **Bug reports:** please attach `eoevr.log`, and F11 dumps if the image looks wrong.

## How it works (short)

- **Loading:** the mod is a `version.dll` proxy, which the game's Unity player loads at startup. It talks to the game through the IL2CPP runtime API and hooks managed methods and engine internal calls with MinHook.
- **Rendering each eye:** every frame, the world camera (`RawCam_v2`) is rendered once per eye into the mod's own render textures, with the eye's view and projection matrices.
  - A few game systems are adapted for the second render:
    - grass and vegetation draws are replayed for the second eye;
    - camera matrix resets by the game's scripts are intercepted;
    - the game's upscalers and TAA are kept off.
  - A separate OpenXR thread with its own D3D11 device submits the frames.
- **UI:** the game's screen-overlay UI canvases are redirected to a HUD camera. Their transparency is computed from a black and a white render, and the result is shown as an OpenXR quad layer.
- **Foveation:** NVIDIA VRS (through NVAPI) is applied to the eye renders. With eye tracking, `XR_EXT_eye_gaze_interaction` provides the gaze.

[`docs/FINDINGS.md`](docs/FINDINGS.md) has the full technical notes (game internals, what was tried, what worked and what didn't). [`docs/build-history/`](docs/build-history) has the notes for each development build.

## Building from source

The mod is cross-built on Linux with clang-cl + lld-link against the MSVC STL and the Windows SDK. See `build.sh` and `tools/setup_toolchain.sh`. The source list and flags are in `build.sh`: an x64 DLL linked with `src/loader/version.def` and the static OpenXR loader in `third_party/openxr/lib`. It has only been built this way so far. A Visual Studio project is not included.

```
TC=/path/to/toolchain ./build.sh      # -> dist/version.dll
```

Layout:
- `src/main.cpp`: configuration and hooks.
- `src/vr/`: stereo rendering, the OpenXR output, foveation (VRS) and the desktop mirror.
- `src/unity/`: the IL2CPP and Unity layer.
- `src/game/`: game-specific helpers.
- `vrkit/`: the reusable in-headset settings menu (Dear ImGui) and the pad input.
- `tools/`: Python analysis helpers used while porting (they disassemble GameAssembly.dll functions using a type dump).

## Third-party code

- [MinHook](https://github.com/TsudaKageyu/minhook): BSD 2-Clause (`third_party/minhook/LICENSE.txt`)
- [OpenXR SDK](https://github.com/KhronosGroup/OpenXR-SDK) headers and loader: Apache 2.0 (`third_party/openxr/LICENSE`)
- [Dear ImGui](https://github.com/ocornut/imgui): MIT (`vrkit/third_party/imgui/LICENSE.txt`)

Edge of Eternity is © Midgar Studio / Dear Villagers. This is an unofficial fan mod and is not affiliated with the developers or publisher.
