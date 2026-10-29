#include "vrkit_input.h"
#include "vrkit_options.h"
#include <windows.h>
#include <xinput.h>
#include "MinHook.h"

namespace vrkit { namespace input {

namespace {
Settings g_set;
using GetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
struct Hooked { const wchar_t* dll; void* target; GetStateFn orig; GetStateFn origEx; void* targetEx; };
Hooked g_mods[3] = { { L"xinput1_4.dll" }, { L"xinput1_3.dll" }, { L"xinput9_1_0.dll" } };
GetStateFn g_read = nullptr;                // an original XInputGetState for our own polling
volatile bool g_block = false, g_chordHeld = false;
DWORD g_fakePacket[4] = {};

volatile bool g_untilRelease = false;   // after closing: keep blocking until the pad is released
DWORD Filter(DWORD r, DWORD user, XINPUT_STATE* st) {
    if (r == ERROR_SUCCESS && st && g_untilRelease && !g_block) {
        const XINPUT_GAMEPAD& p = st->Gamepad;
        if (!p.wButtons && p.bLeftTrigger < 30 && p.bRightTrigger < 30) g_untilRelease = false;
    }
    // while all chord buttons are down together, the game doesn't see them (no stray L3 / R3 actions)
    if (r == ERROR_SUCCESS && st && g_set.chordButtons && (st->Gamepad.wButtons & g_set.chordButtons) == g_set.chordButtons)
        st->Gamepad.wButtons &= ~g_set.chordButtons;
    if (r == ERROR_SUCCESS && st && (g_block || g_chordHeld || g_untilRelease)) {
        DWORD pkt = ++g_fakePacket[user & 3];
        ZeroMemory(st, sizeof(*st));
        st->dwPacketNumber = pkt;
    }
    return r;
}
// One hook function per DLL (each needs its own trampoline).
template <int N> DWORD WINAPI HkGet(DWORD u, XINPUT_STATE* s) { return Filter(g_mods[N].orig(u, s), u, s); }
template <int N> DWORD WINAPI HkGetEx(DWORD u, XINPUT_STATE* s) { return Filter(g_mods[N].origEx(u, s), u, s); }
void* const kHk[3] = { (void*)HkGet<0>, (void*)HkGet<1>, (void*)HkGet<2> };
void* const kHkEx[3] = { (void*)HkGetEx<0>, (void*)HkGetEx<1>, (void*)HkGetEx<2> };

// polling state
WORD g_prevButtons = 0; bool g_prevKeys[256] = {};
DWORD g_chordStart = 0; bool g_chordFired = false;
DWORD g_repeatAt[4] = {}; uint32_t g_repeatDir = 0;

// Steam Input can hide the pad from the game's own XInput (it remaps it to keyboard keys before the game sees it), so
// the menu reads the pad through a renamed private copy of System32's xinput1_4.dll that Steam does not hook. Works for
// Xbox-type pads (PlayStation / Switch pads are not XInput devices). Fallback: the game's XInput (originals).
GetStateFn g_private = nullptr;
bool g_privateTried = false;
void LoadPrivateXInput() {
    if (g_privateTried) return;
    g_privateTried = true;
    wchar_t sys[MAX_PATH], tmp[MAX_PATH], dst[MAX_PATH];
    if (!GetSystemDirectoryW(sys, MAX_PATH) || !GetTempPathW(MAX_PATH, tmp)) return;
    wcscat_s(sys, L"\\xinput1_4.dll");
    swprintf_s(dst, L"%svrkit_xinput1_4.dll", tmp);
    WIN32_FILE_ATTRIBUTE_DATA a{}, b{};
    bool same = GetFileAttributesExW(sys, GetFileExInfoStandard, &a) && GetFileAttributesExW(dst, GetFileExInfoStandard, &b) &&
                a.nFileSizeLow == b.nFileSizeLow && a.nFileSizeHigh == b.nFileSizeHigh;
    if (!same && !CopyFileW(sys, dst, FALSE)) { Log("vrkit: could not copy xinput1_4.dll to %ls (error %lu)", dst, GetLastError()); return; }
    HMODULE m = LoadLibraryW(dst);
    if (!m) { Log("vrkit: could not load %ls (error %lu)", dst, GetLastError()); return; }
    g_private = (GetStateFn)GetProcAddress(m, (LPCSTR)100);   // XInputGetStateEx (guide button too)
    if (!g_private) g_private = (GetStateFn)GetProcAddress(m, "XInputGetState");
    Log("vrkit: menu reads the pad through a private copy %ls (%s)", dst, g_private ? "ok" : "no XInputGetState");
}
int g_lastSource = -1;   // for logging where the pad was found

} // namespace

void Configure(const Settings& s) { g_set = s; }

bool InstallHooks() {
    bool any = false;
    for (int k = 0; k < 3; ++k) {
        Hooked& h = g_mods[k];
        HMODULE m = GetModuleHandleW(h.dll);
        if (!m) continue;
        if (!h.target) {
            void* p = (void*)GetProcAddress(m, "XInputGetState");
            if (p && MH_CreateHook(p, kHk[k], (void**)&h.orig) == MH_OK && MH_EnableHook(p) == MH_OK) { h.target = p; Log("vrkit: hooked XInputGetState in %ls", h.dll); }
            void* px = (void*)GetProcAddress(m, (LPCSTR)100);   // XInputGetStateEx (ordinal 100, includes the guide button)
            if (px && px != p && MH_CreateHook(px, kHkEx[k], (void**)&h.origEx) == MH_OK && MH_EnableHook(px) == MH_OK) h.targetEx = px;
        }
        if (h.orig && !g_read) g_read = h.orig;
        any = any || h.target;
    }
    if (!g_private) LoadPrivateXInput();
    if (!g_read) {   // the game hasn't loaded XInput (yet): read the pad ourselves
        HMODULE m = LoadLibraryW(L"xinput1_4.dll");
        if (!m) m = LoadLibraryW(L"xinput1_3.dll");
        if (m) g_read = (GetStateFn)GetProcAddress(m, "XInputGetState");
    }
    return any;
}

void SetBlocking(bool b) { if (g_block && !b) g_untilRelease = true; g_block = b; }

uint32_t Poll() {
    uint32_t out = 0;
    // --- gamepad (first connected)
    XINPUT_STATE st = {}; bool pad = false;
    int source = -1;
    // 1st: the private copy (not affected by Steam Input); 2nd: the game's XInput (original functions)
    GetStateFn readers[2] = { g_private, g_read };
    for (int r = 0; r < 2 && !pad; ++r) {
        if (!readers[r]) continue;
        for (DWORD u = 0; u < 4 && !pad; ++u) { ZeroMemory(&st, sizeof(st)); pad = readers[r](u, &st) == ERROR_SUCCESS; if (pad) source = r * 4 + (int)u; }
    }
    if (source != g_lastSource) {
        if (source < 0) Log("vrkit: no XInput pad found (private copy %s, game XInput %s)", g_private ? "loaded" : "missing", g_read ? "available" : "missing");
        else Log("vrkit: pad found on user %d via %s", source % 4, source < 4 ? "the private xinput1_4 copy" : "the game's XInput");
        g_lastSource = source;
    }
    WORD b = pad ? st.Gamepad.wButtons : 0;
    float lt = pad ? st.Gamepad.bLeftTrigger / 255.f : 0.f, rt = pad ? st.Gamepad.bRightTrigger / 255.f : 0.f;
    // stick as d-pad
    if (pad) {
        const SHORT dz = 16000;
        if (st.Gamepad.sThumbLY > dz) b |= XINPUT_GAMEPAD_DPAD_UP;
        if (st.Gamepad.sThumbLY < -dz) b |= XINPUT_GAMEPAD_DPAD_DOWN;
        if (st.Gamepad.sThumbLX > dz) b |= XINPUT_GAMEPAD_DPAD_RIGHT;
        if (st.Gamepad.sThumbLX < -dz) b |= XINPUT_GAMEPAD_DPAD_LEFT;
    }
    WORD pressed = b & ~g_prevButtons;
    g_prevButtons = b;
    // --- open / close chord (default L3 + R3)
    DWORD now = GetTickCount();
    bool chord = pad && (g_set.chordButtons || g_set.chordTriggers);
    if (g_set.chordButtons) chord = chord && (st.Gamepad.wButtons & g_set.chordButtons) == g_set.chordButtons;
    if (g_set.chordTriggers) chord = chord && lt > g_set.triggerThreshold && rt > g_set.triggerThreshold;
    if (chord) {
        if (!g_chordStart) g_chordStart = now;
        if (!g_chordFired && now - g_chordStart >= (DWORD)g_set.chordHoldMs) { g_chordFired = true; out |= Toggle; }
        g_chordHeld = g_chordFired;   // after it fired, keep the game from seeing the triggers until released
    } else { g_chordStart = 0; g_chordFired = false; g_chordHeld = false; }
    // --- keyboard (every key is queried each frame so its edge state stays right). Esc is not used: the
    // game would see it too (keyboard input is not blocked) and most games open their own menu on it.
    auto key = [&](int vk) { bool d = (GetAsyncKeyState(vk) & 0x8000) != 0; bool p = d && !g_prevKeys[vk]; g_prevKeys[vk] = d; return p; };
    auto held = [&](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
    bool kTog = g_set.keyboardToggleVk ? key(g_set.keyboardToggleVk) : false;
    bool kRet = key(VK_RETURN), kBack = key(VK_BACK), kPgUp = key(VK_PRIOR), kPgDn = key(VK_NEXT), kDel = key(VK_DELETE);
    if (kTog) out |= Toggle;
    if ((pressed & XINPUT_GAMEPAD_A) || kRet) out |= Accept;
    if ((pressed & XINPUT_GAMEPAD_B) || kBack) out |= Back;
    if ((pressed & XINPUT_GAMEPAD_LEFT_SHOULDER) || kPgUp) out |= PagePrev;
    if ((pressed & XINPUT_GAMEPAD_RIGHT_SHOULDER) || kPgDn) out |= PageNext;
    if ((pressed & XINPUT_GAMEPAD_Y) || kDel) out |= Reset;
    // directions with auto-repeat (first repeat after 400 ms, then every 90 ms)
    uint32_t dirs = 0;
    if (b & XINPUT_GAMEPAD_DPAD_UP || held(VK_UP)) dirs |= Up;
    if (b & XINPUT_GAMEPAD_DPAD_DOWN || held(VK_DOWN)) dirs |= Down;
    if (b & XINPUT_GAMEPAD_DPAD_LEFT || held(VK_LEFT)) dirs |= Left;
    if (b & XINPUT_GAMEPAD_DPAD_RIGHT || held(VK_RIGHT)) dirs |= Right;
    const uint32_t bits[4] = { Up, Down, Left, Right };
    for (int k = 0; k < 4; ++k) {
        if (!(dirs & bits[k])) { g_repeatAt[k] = 0; continue; }
        if (!g_repeatAt[k]) { out |= bits[k]; g_repeatAt[k] = now + 400; }
        else if ((int)(now - g_repeatAt[k]) >= 0) { out |= bits[k]; g_repeatAt[k] = now + 90; }
    }
    return out;
}

} } // namespace vrkit::input
