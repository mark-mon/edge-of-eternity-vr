#!/bin/bash
# Cross-builds Edge of Eternity VR on Linux: clang-cl (zig 0.17 / clang 22) + lld-link against MSVC STL + Windows SDK 10.0.28000.
# Toolchain: toolchain64bit.zip from sharedtools extracted to $TC (default /home/claude/tc); see tools/setup_toolchain.sh.
#   dist/version.dll   the mod (x64). Copy next to EdgeOfEternity.exe.
set -e
cd "$(dirname "$0")"
B=${TC:-/home/claude/tc}/build.sh
mkdir -p build/x64 dist
CFLAGS="/O2 /Zi /W3 /Isrc /Ivrkit /Ivrkit/third_party/imgui /Ithird_party/minhook/include /Ithird_party/openxr/include -D_CRT_SECURE_NO_WARNINGS -DWIN32_LEAN_AND_MEAN -DNOMINMAX \
 -Wno-unused-function -Wno-missing-field-initializers -Wno-unknown-pragmas -Wno-sign-compare -Wno-unused-variable \
 -Wno-unused-command-line-argument"
SRC="src/main.cpp src/loader/version_proxy.cpp src/common/log.cpp src/common/config.cpp src/common/crash_log.cpp \
 src/unity/il2cpp.cpp src/unity/unity.cpp src/unity/probe.cpp src/unity/typedump.cpp \
 src/vr/stereo.cpp src/game/drawreplay.cpp src/game/dlsseyes.cpp src/vr/xrout.cpp src/vr/perf.cpp src/vr/mirror.cpp src/vr/vrs.cpp \
 vrkit/vrkit_options.cpp vrkit/vrkit_menu.cpp vrkit/vrkit_menu_core.cpp vrkit/vrkit_input.cpp \
 vrkit/third_party/imgui/imgui.cpp vrkit/third_party/imgui/imgui_draw.cpp vrkit/third_party/imgui/imgui_tables.cpp \
 vrkit/third_party/imgui/imgui_widgets.cpp vrkit/third_party/imgui/imgui_impl_dx11.cpp \
 third_party/minhook/src/buffer.c third_party/minhook/src/hook.c third_party/minhook/src/trampoline.c \
 third_party/minhook/src/hde/hde64.c"
$B x64 dist/version.dll $CFLAGS /LD /Fobuild/x64/ /Fdbuild/x64/version.pdb $SRC \
   /link /DEF:src/loader/version.def /MAP:build/x64/version.map /DEBUG /PDB:build/x64/version.pdb third_party/openxr/lib/openxr_loader_static.lib \
   user32.lib advapi32.lib d3d11.lib dxgi.lib dxguid.lib d3dcompiler.lib shlwapi.lib pathcch.lib ole32.lib shell32.lib
rm -f dist/version.lib dist/version.exp
ls -la dist
