# Edge of Eternity VR - findings

## Game facts
- Edge of Eternity (Midgar Studio), Steam app **269190**, `D:\SteamLibrary\steamapps\common\Edge Of Eternity\EdgeOfEternity.exe`.
- Unity **2021.3.27f1**, **IL2CPP** x64 (GameAssembly.dll), D3D11, **built-in render pipeline**, Linear colour space.
  No protection. Steam launch works on the test PC (`Start-EOE`).
- Proxy: `version.dll` (UnityPlayer.dll imports VERSION.dll) - same as NEO VR / ONINAKI VR.
- Middleware: Post Processing Stack v2 (com.unity.postprocessing), Cinemachine (CameraMgr + CinemachineBrain),
  Rewired (input), Aura2, FogVolume, Enviro (TOD_Camera / SkySystems), SSRT, own DLSS plugin
  (`Orion.Graphics.DLSS.DLSS` component on the world camera), MagicaCloth, GPUInstancer, TextMeshPro, DOTween.
- Test save: the game goes straight from boot to gameplay with no key presses (~50 s after launch, Astryan Continent
  village next to the inn). Enter presses during gameplay are harmless.
- Window 1920x1080 on the test PC.

## Type dump (our "decompiler")
- `dump_types = 1` -> `eoevr_types.txt` at startup (src/unity/typedump.cpp): 126 assemblies, 13322 classes, 91719
  methods, every field with offset and every method with its GameAssembly RVA. Copy kept in agent\captures\002.

## Cameras (probe, gameplay in AstryanContinent)
| camera | depth | mask | target | role |
|---|---|---|---|---|
| Minimap Camera | 0 | 0x400 | RT 250x250 (ortho) | minimap |
| RawCam_v2 | 0 | 0xbfdffbff | screen | **world camera** (fov 65, clip 0.4..60000) |
| UICam | 2 | 0x40000000 (layer 30) | screen, clear depth | 3D UI over the world, same pose as RawCam_v2 |
| LoadingCam | 99 | 0x40000020 | screen | loading screen (ScreenSpaceCamera canvas) |
| Reflection Probe Cam | | | | realtime probes, occasionally |
Boot: 'Disclaimer' scene -> 'TEMPORARY_SCENE' (TEMP CAMERA, DISABLED DEFERRED) -> 'AstryanContinent'.
During loading RawCam_v2 renders 640x360 into 'Source Input Texture'.

## UI
- Almost the whole HUD is **Screen Space Overlay** canvases (CompassUI, CountersCanvas, QuestUI, MinimapSmall, UIInteract,
  DamageInterface, OverlayUI, DialogUI_AutoCut, ...) on layers 5 and 0. These draw directly into the back buffer.
- The mod switches each root overlay canvas to ScreenSpaceCamera on its own camera `EoeVR_HUDCamera` (far below the
  world, culling mask = the canvases' layers, depth 100, target = HUD texture), plane distance 1. Re-scanned every second.
- HUD alpha: black/white matte (ONINAKI technique): Unity renders the HUD camera over black, the mod renders it again
  over white; xrout's copy shader computes alpha = 1 - (white - black).

## Post-processing / AA
- PostProcessLayer.OnPreCull resets the projection (TAA) and TAA builds its jittered matrix from fieldOfView/aspect.
  The mod hooks the managed `Camera.set_projectionMatrix` / `Camera.ResetProjectionMatrix`: during an eye render of
  the camera being rendered, the eye projection is re-applied (a jitter in [0,2]/[1,2] is carried over).
- Game's PPv2 AA on the test PC: none. VR default `game_aa = smaa`.
- DLSS (Orion.Graphics.DLSS.DLSS on RawCam_v2, native plugin OrionGraphics_DLSS.dll + nvngx_dlss.dll):
  * Game setting: PlayerPrefs `GRAPHICS_NVDLSS_h2305810975` (+ _TEMP), stored as a STRING (REG_BINARY "2\0").
    UIGraphicOptionController.ApplySettingsToCamera: value > 0 enables the component; 1 = MaxQuality, 2 = Balanced,
    3 = MaxPerf, 4 = UltraPerformance (writes DLSS.Quality @0x20). Needs DLSS.ExternalIsDLSSAvailable().
    The test PC had it at "0" until test 022 (now "2" = Balanced).
  * Single window design: the native API is global (SetColorBuffer/SetOutputBuffer/SetMotionVectors/SetDepthBuffer,
    ResetDLSS - no view or context handle -> one history). DLSS.OnPreCull sets camera.targetTexture to its low-res
    m_colorTexture on EVERY render (would override our eye targets); the upscaled image reaches the window through an
    imposter camera. DLSS.OnDisable does not restore the camera target.
  * Test 022: with DLSS on in the game, the mod had 0 stereo frames (world camera "renders into a texture" -> menu
    mode). The game re-enables the component whenever it applies its graphics settings.
  * Fix (test 023, `dlss_in_vr = 0` default): EnforceDlss() every frame keeps the component disabled and sets the camera
    target back to the screen when it is DLSS's m_colorTexture. 600/600 stereo frames, 60 fps, 0 exceptions with the
    game's DLSS = Balanced.
  * DLSS PER EYE (tests 025-027, `dlss_in_vr = 1`, default from 0.1 build 05) - the game's own DLSS, no own NGX code:
    - DLSS.CheckDirty re-initialises when m_prevWidth/Height != Screen.width/height; Initialize -> RegenBuffers(srcW,
      srcH, targW, targH). Hook on RegenBuffers: target = eye size, source = eye size x the game's ratio (Balanced
      1280x720 -> 1920x1080 = 0.667). DlssEnsureEyeSize() sets m_prevWidth = -1 when the target is not the eye size.
    - AddRenderCommandBuffer creates a child "imposter" camera (DLSS.m_imposterCamera @0xd8) that carries the evaluate
      command buffer; ImposterCameraDLSSOutput.OnRenderImage blits the result (outDLSS) to its destination.
    - Per eye: camera target = DLSS m_colorTexture ('Source Input Texture') BEFORE Camera.Render (setting the eye RT and
      letting DLSS.OnPreCull switch it mid-render gave "Dimensions of color surface does not match dimensions of depth
      surface" x30000 and a fog-only image, test 025/026), render RawCam_v2, then render the imposter camera with the
      eye RT as its target. DLSS's previous-frame matrices (m_previousJitteredProjectionMatrix @0x188,
      m_previousCameraMatrix @0x1c8) are saved/restored per eye. The imposter camera is skipped in Unity's own loop.
    - Jitter: DLSS.StartJitterFrame sets a jittered symmetric projection; the projection hook keeps the eye projection
      and carries the jitter over.
    - ONE HISTORY PER EYE (src/game/dlsseyes.cpp, `dlss_eye_history = 1`): with the game's single NGX feature each
      eye was blended with the other eye's history -> doubled/ghosted characters, eyes not lining up (user test build
      05 on the VR PC, 4K desktop, 3980x3424 per eye; also visible as a see-through NPC in test 027/029 dumps).
      OrionGraphics_DLSS.dll links NGX (rel 1.6) statically and EXPORTS NVSDK_NGX_D3D11_CreateFeature /
      EvaluateFeature(_C) / ReleaseFeature: MinHook on those. Create -> a second SuperSampling feature with the same
      parameters; Evaluate -> the right eye uses the second handle; Release frees both. The eye is known on the render
      thread from GL.IssuePluginEvent(dlsseyes::EyeEvent, eye) before each eye's render (0 after the imposter render).
      Test 029: 600 left + 600 right evaluations / 10 s, ghosting gone; static content not smeared (the camera motion
      vectors come from DLSS's own previous-camera matrices, which are swapped per eye).
    - Test 027 (native 2244x2352, Balanced -> 1496x1568 per eye, + VRS): gpu stereo 10.5 ms (no DLSS: 14.1 ms).
    - The game loads EdgeOfEternity_Data\Plugins\x86_64\nvngx_dlss.dll (2.2.11 shipped), NOT the copy in the game root.
      DLSS 4 (310.6) installed there on the test PC (backup nvngx_dlss.dll.v2_2_11.bak).

## Game anti-aliasing: TAA must be off
- With the game's Anti-aliasing option on TAA (and DLSS off) the eyes show the game's flat view, doubled and stuck to
  the head (user test on the VR PC, build 03/04). Off, FXAA and DLSS work. Not investigated further (documented).

## DLSS / TAA: eyes rendered with the game's projection (FIXED in build 07, test 032)
- User test build 06: with DLSS (and the game's TAA) the whole scene sways with head motion, everything doubled,
  stereo wrong. Log: "proj intercepts 0" for the whole session.
- Cause: IL2CPP inlined the tiny managed wrappers Camera.set_projectionMatrix / ResetProjectionMatrix, so
  DLSS.StartJitterFrame (and PPv2) call the native icalls set_projectionMatrix_Injected / ResetProjectionMatrix
  directly; the mod's managed hooks never ran. Each eye rendered with the game's symmetric FOV projection (+ jitter)
  and our eye view matrix: wrong FOV for the lenses -> the world swims with head rotation, wrong stereo. On the test
  PC's motionless null headset it only showed as a more zoomed image (027 vs 023 dumps) - missed.
- Fix: MinHook on the four native icalls (set/Reset projection, set/Reset worldToCamera); unity::I keeps the
  trampolines for the mod's own calls. During an eye render: projection = eye projection + the script's jitter
  ([0,2]/[1,2]); DLSS.m_currJitteredProjectionMatrix (@0x148, stored by StartJitterFrame before it sets the
  projection) is overwritten with the eye's jittered projection; DLSS.GetProjectionExtents(cam, jx, jy) (Playdead
  formula: tan(fov/2)*aspect, tan(fov/2), jitter in tangent units) hooked -> eye frustum half extents, off-centre
  part + jitter in zw. DLSS.OnPreCull builds its motion-vector matrices from GPU(m_curr) x worldToCamera and
  GPU(m_prevJit) x m_prevCam (both per eye now).
- Test 032: projection intercepts 2 per eye render, DLSS-on framing identical to DLSS-off, `debug_head_sway = 8`
  (oscillating head yaw on a still headset) with DLSS: sharp, no doubling. Game TAA path not re-tested (PPv2 TAA
  via game_aa = taa made no projection calls; the game's own TAA option is to be checked by the user).

## Release 1.0.0 (2026-10-29)
- Version 1.0.0 = build 12 + version string. Code and notes on GitHub: https://github.com/mark-mon/edge-of-eternity-vr
  (main; agent scripts not included). Release files in builds\v1.0.0 (version.dll, zip with README, release notes).
- The Claude session cannot push tags or create GitHub releases (proxy/API policy): the user creates the v1.0.0
  release on GitHub and attaches the files.

## Build 08: the game's DLSS, FSR and TAA kept off in VR (user decision after build 07)
- User test build 07: with the eye projection fixed, per-eye DLSS showed a cross-eyed, tiny world (like world scale
  15), and DLSS at Performance was SLOWER than DLSS off. Decision: ignore the game's upscalers in VR.
  (Untested idea for later: the DLSS imposter/output path or RegenBuffers sizing may scale the eye image.)
- AMD_CAS (the game's FSR option, FSRQuality enum, CameraTarget + m_imposterCamera like DLSS) -> black image in VR.
  TAA2 (the game's TAA: m_currJitteredProjectionMatrix/m_previousCameraMatrix/m_HistoryTextures, one history per
  camera) and CTAA_PC are also on RawCam_v2. Component list on RawCam_v2 also has MLUpscaler, MadGoatSSAA, HPFSharpen.
- `vr_disable_components = DLSS,AMD_CAS,TAA2,CTAA_PC` (default): scanned when the world camera is configured;
  every frame each is kept disabled, the camera target is reset when it is the component's m_colorTexture /
  CameraTarget, and its m_imposterCamera is disabled. `dlss_in_vr = 0` default (1 skips DLSS in that list and runs
  the experimental per-eye DLSS). Build 08 was not run on the test PC (lent to another project) - user tests first.

## Build 09: defaults = the user's tested config
- User test build 08: everything works (DLSS / FSR / TAA settings harmless in VR). User's config differed from the
  defaults only in foveation = fixed and foveation_strength = high -> now the defaults (config text + code fallbacks).

## Distant shadows swim with head motion -> NGSS_FrustumShadows ("Block Dancing Shadows", build 12)
- User found it with build 11's effect test: NGSS_FrustumShadows (screen-space contact shadows on RawCam_v2).
  Off = slightly blander look but no dancing. Option `block_dancing_shadows` (default 1, menu Display, live) keeps
  the component disabled; switching it off re-enables it.
- Not fixed properly yet. Fields: m_Temporal/isTemporal @0x50/0x51, m_Jittering @0x58, m_SampleIndex @0x5c (Halton
  jitter), RenderScale + m_scalingCameraReference / GetReferenceCamera (reduced-resolution reference camera),
  OnPreRender sets worldToCameraMatrix on its material. Ideas to test on the test PC with debug_head_sway: temporal
  off (m_Temporal = false), per-eye temporal history, or the reference camera's projection.
- Earlier steps:
- Build 10 result: shadow_fit made no difference; the log shows the game already uses StableFit (cascades 4, distance
  600) and QualitySettings.set_shadowProjection is stripped. Option removed (its menu callback also called il2cpp from
  the XR thread - menu callbacks must only store values; the main thread applies them).
- Build 11: Debug > "Switch off effect" (debug_effect_off) keeps one world-camera effect disabled, live, for the user
  to find the culprit; the camera scan logs which components the game has disabled. Suspect: screen-space AO
  (HBAO / AmplifyOcclusion / NNAO / SSRT) or another effect building its frustum from camera.fieldOfView/aspect.
- Build 10 notes:
- User report (build 09): distant shadows on rocks / vegetation sway when moving the head left/right, the same in
  both eyes; the game's screen-space shadow option made no difference. NGSS_FrustumShadows (worldToCameraMatrix) and
  SEScreenSpaceShadows (projectionMatrix) use the camera's real per-eye matrices - not the cause.
- Candidate: Unity directional shadow cascades refit per camera render; ShadowProjection.CloseFit moves the cascade
  texels with every view change. `shadow_fit = stable` (default) sets QualitySettings.shadowProjection = StableFit,
  re-applied on every camera scan; menu Display > Shadow stability (Stable / Close fit / Game), live. Logs the game's
  shadow projection, cascades, distance, resolution once. Not run yet (test PC lent out) - user tests first.

## Game menus on the menu panel (status / Y menu)
- The status menu (gamepad Y, keyboard I: StatusUIv3_Bestiary(Clone)) is a Screen Space Camera canvas of the game's
  UICam (layer 30). UICam renders per eye with the full eye projection, so the menu filled the whole view, too close
  and too big (user test build 05). Now: ScreenSpaceCamera canvases of an overlay camera are moved to our HUD camera
  (the game re-assigns UICam on every open; the canvas scan converts it again) and remembered as game menus, plus
  overlay canvases whose name contains a `menu_canvases` entry (PauseMenu, StatusUI, MinimapFullUI, MultiOptionsUI,
  ParametricSettingsUI). While one is active, FRAME_HUD_MENU_PANEL: the HUD quad uses menu_distance / menu_size,
  world-locked at eye level (3D world still rendered). `menu_panel = 0` keeps them on the HUD panel. Test 031.
- Other game canvases (WorldSpace: StaminaBarV2 on UICam, WorldMarker, OpenWorldMapSystem on 'Minimap Camera',
  LoadingMenu_Experimental on LoadingCam) are left as they are and logged once ("game canvas ...").

## Game upscaler FSR: black image in VR (user test, build 05). Not investigated - documented: use DLSS or none.

## Foveated rendering (VRS) - src/vr/vrs.cpp (port of Divine Force / GranblueVR vrs.cpp)
- NVAPI VRS on the game's immediate context: OMSetRenderTargets / OMSetRenderTargetsAndUnorderedAccessViews hooked
  (vtable 33/34). The main thread brackets each eye's Camera.Render of RawCam_v2 with GL.IssuePluginEvent markers carrying
  a parameter-ring slot (eye, scene size, eye FOV, gaze). Scene pass = RT0 at the eye's scene size (DLSS source size
  with DLSS per eye) with a DSV -> pattern bound; everything else unbound. Pattern rows flipped (Unity RTs upside down).
- Modes: foveation = off / fixed / eye (eye = gaze when available, else fixed); strength low/medium/high; rings
  foveation_inner / foveation_outer. Gaze: XR_EXT_eye_gaze_interaction added to xrout (CreateGaze/UpdateGaze),
  debug_fake_gaze for testing. Test 027: ~3300 scene binds/s, pattern follows the (fake) gaze.

## Controller in the settings menu (Steam Input)
- User report: the menu stops reacting to the pad in game (keyboard still works). Steam Input hides the pad from the
  game's XInput. vrkit_input now reads the pad first through a private copy of System32 xinput1_4.dll
  (%TEMP%\vrkit_xinput1_4.dll, XInputGetStateEx ordinal 100), then the game's XInput. Logs where the pad was found.
  Xbox-type pads only. Test PC has no pad: verify on the VR PC.

## Vegetation only in the first render of a frame (fixed: draw replay)
- Symptom: tall grass / foliage in the first eye rendered only (right eye first -> right eye has it), never on the monitor.
- Not GPU Instancer (no active managers), not TerrainMaster culling switches (EnableGPUOcclusionCulling is already 0;
  DrawCallCuller / depth prepass made no difference), not OrionGraphicDriver.DRAW_TO_ALL_CAMERAS.
- Vegetation path: TerrainMaster.DoUpdate -> AssetSpawnDefinition.Update -> Draw -> Draw_V2 ->
  OrionGraphicDriver.DrawAssetIndirectCommand -> Graphics.DrawMeshInstancedIndirect (+ a per-frame CommandBuffer).
- Unity consumes queued Graphics.DrawMeshInstancedIndirect draws in the first camera render after they were queued.
- Fix (src/game/drawreplay.cpp, `draw_replay = 1`): MinHook on the native icall
  `Graphics::Internal_DrawMeshInstancedIndirect_Injected`; main-thread calls are recorded per frame (~480 per frame in the
  village) and queued again before the second eye and before the game's own render. No measurable cost.
  Also covers GPUFlock / Swarm effects (same API).
- Culling of these draws is still done with the game camera's frustum (65 deg, 16:9): grass at the outer edges of a
  wide headset FOV may pop. To check in the headset.

## Crash in the first VR run (fixed)
- Render thread AV in nvwgf2umx via xrout::SubmitRenderThread: the HUD RenderTexture's GPU texture was released and
  recreated by Unity during loading ("HUD native texture changed" in the log) while our ring still held the old pointer.
- Fix: native pointers re-read every frame; every texture queued to the render thread is AddRef'd and Released after
  the copy.

## Desktop view
- `desktop_view = eye` (default): after the eye renders, RawCam_v2 / UICam are disabled for the rest of the frame (the
  game's own full-resolution render with the whole post stack is skipped) and re-enabled in our HUD camera's
  OnPostRender (the last camera of the frame, before the next frame's scripts - Camera.main keeps working). The window
  shows the left eye, centre-cropped (Graphics.Blit to the back buffer).
- `desktop_view = game`: the game renders its own view as well (overlay UI is on the HUD panel, not on the monitor).

## World camera components (RawCam_v2)
Transform Camera FlareLayer AudioListener SSRT GFGridRenderCamera AudioSource BlurEffect CameraShake TOD_Camera
GlobalFog OverlayTransitionFX ScreenshotTake PkTransferTex GlobalMaterialIDRenderer StylisticFog SSR2 HBAO
AmplifyOcclusionEffect PostProcessLayer NVIDIA.Ansel FantasyImageEffect CameraMarker NNAO SEScreenSpaceShadows
NGSS_FrustumShadows TenkokuSkyFog HxVolumetricCamera FogVolumeRenderer CinemachineBrain HxVolumetricImageEffectOpaque
SSMSGlobalFog SSMS ScreenOverlay CinematicBorders CTAA_PC ReversedWorld GammaCorrectionEffect Rigidbody BoxCollider
GhostCameraController MadGoatSSAA AuraCamera Orion VRS HPFSharpen Orion DLSS AMD_CAS MLUpscaler SharedOnRenderImage
GPUOcclusionCullingManager MotionBlur TAA2
(many are disabled depending on the graphics settings; CTAA_PC / TAA2 / MotionBlur would need per-eye history if on)

## Tools
- tools/calls.py <rva> [len]: disassemble a GameAssembly function, name every call target from the type dump.
- tools/xref.py <rva...>: every direct call to an RVA (both executable sections: .text and il2cpp).
- tools/eyes.py <dir>: eye dump stats + side-by-side previews.
- Linker maps of deployed builds are kept to decode crash RVAs (VERSION.dll+0x...).

## Test log
- 001 status: agent OK, Unity 2021.3.27f1, RTX 3080 Ti, SteamVR null driver configured.
- 002 probe (VR off): mod loads, hooks fine, type dump, camera/canvas layout above.
- 003 first VR run: crash ~10 s in (render thread, stale HUD texture). 004: Player.log + watchdog stacks.
- 005 stable: ~50 fps game / 60 fps headset, 2244x2352 per eye, gpu stereo 15 ms. Grass only in the left eye.
- 006 grass in the eye rendered first (eye order swap). 007 DRAW_TO_ALL_CAMERAS no effect. 008 TerrainMaster culling
  switches no effect. 009 stale DLL (same staging path re-committed!). 010 draw replay: grass in both eyes + monitor.
- 011/012 desktop_view = eye first try: no gain. Unity renders texture cameras (our HUD camera) BEFORE screen cameras,
  so re-enabling in the HUD camera's OnPostRender put RawCam_v2 back before its own render.
- 013 dedicated EoeVR_MirrorCamera (screen, depth 1000, mask 0, clear Nothing = last camera of the frame; its
  OnPostRender re-enables the stereo cameras): game 50 -> 60 fps (locked to the 60 Hz null headset), gpu frame 20 -> 16.7 ms.
  Graphics.Blit(eye, null) from the camera callback did not reach the window.
- 014 window mirror via an IDXGISwapChain::Present/Present1 hook (src/vr/mirror.cpp): centre-cropped, upright left eye
  drawn into the back buffer (1920x1080, format 28) before Present. 60 fps, 0 exceptions in Player.log.
- 015 settings menu: Insert opens, PageDown/Down/Right changed game_aa, Backspace closes and saves config.txt.
- 016 Ultra preset (3142x3292 per eye, 2x the pixels): game 41 fps, gpu stereo 21.6 ms (native: 14.1 ms), stereo CPU
  10.3 ms (two Camera.Render calls with the full post stack). Roughly 6-7 ms fixed + ~7.5 ms per null-headset-sized
  pixel count on a 3080 Ti.
- 019 DLSS state logging: component behaviour 0 in gameplay = DLSS was off in the game settings all along.
- 020 PlayerPrefs read (GRAPHICS_NVDLSS = "0"). 021 wrote it as a DWORD -> ignored (string pref).
- 022 DLSS = "2" (Balanced, 1280x720 -> 1920x1080): VR broken (0 stereo frames), for both dlss_in_vr values.
- 023 EnforceDlss every frame: stereo 600/600, 60 fps, 0 exceptions with the game's DLSS on.
- User test (VR PC, build 04): DLSS on = perfect and crisp but slow (no upscaling); DLSS off + game TAA = broken.
- 025 DLSS per eye first try: fog-only image, 37 fps at Ultra. 026 debug views: the DLSS INPUT was already wrong;
  Player.log "Dimensions of color surface does not match dimensions of depth surface" x30000.
- 027 camera target = DLSS colour texture before the render: correct images in both eyes, gpu stereo 10.5 ms
  (native, Balanced, + VRS with fake gaze), 0 exceptions. Private xinput1_4 copy loads.
- 028 perf at Ultra (140% = 3142x3292 per eye, DLSS Balanced, AstryanContinent, test PC null headset):
    C no DLSS, no VRS: 41 fps, gpu stereo 21.4 ms | B DLSS per eye: 54 fps, 15.9 ms | A DLSS + fixed VRS: 60 fps
    (vsync cap), 13.4 ms. DLSS -26 %, VRS a further -16 % of the stereo GPU time.
- User test build 05 (RTX 5090, 3980x3424 per eye): eye-tracked VRS 65 -> 90 fps at 92 % GPU; pad menu works;
  DLSS doubled (shared history) -> fixed in 029; game FSR = black.
- 029 dlss_eye_history A/B: separate histories clean, shared history ghosts the NPC.
