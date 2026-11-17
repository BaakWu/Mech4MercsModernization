// Display controller: windowed/fullscreen setting, frontend resolution, and
// the Video Options "Windowed" checkbox backend.
//
// GameOS keeps every display change behind one entry point,
// gos_SetScreenMode (12 stdcall arguments). It only records the request and
// sets a pending flag; the main loop applies it on the next frame through the
// windowed or the fullscreen setup routine. Argument 8 requests fullscreen and
// argument 10 requests windowed. The game never sets either flag itself.
//
// The game always runs on GameOS' windowed path. The Windowed setting only
// decides the window (window.cpp): a framed window at the selected
// resolution, or a borderless window over the whole monitor ("fullscreen"
// without changing the display mode). Exclusive fullscreen is never used: on
// current Windows/drivers its 800x600/640x480 mode switches fail and GameOS
// falls back to a window anyway.
//
// The hook wraps gos_SetScreenMode and:
//   - leaves the shell's 800x600 requests alone, so the frontend, MechLab and
//     movies render exactly as designed (the ddraw proxy scales the picture
//     into the window),
//   - replaces mission requests with the selected resolution (3.1c's own
//     index-to-size map disagrees with its dropdown for some entries),
//   - raises 16-bit requests to 32-bit,
//   - never requests exclusive fullscreen.
//
// Only the player changes the Windowed setting (checkbox, or Alt+Enter, which
// window.cpp routes here).
//
// The Shell_CallbackHandler is wrapped to answer a new option id
// (kMwoWindowed) for ShellGetOptionValue/ShellSetOptionValue, to apply a
// Resolution change immediately, and to persist both on ShellSaveOptions.
//
// options.ini [graphics options]:
//   windowed=true|false      new key; missing means true
//   screenwidth/screenheight the selected resolution (existing keys)
#include "hook.h"

#include <cstdio>
#include <cstring>

namespace mw4hook {
namespace {

constexpr int kMwoScreenResolution = 7;
constexpr int kMwoUseExtendedVisibility = 34;
constexpr int kShellSetOptionValue = 49;
constexpr int kShellGetOptionValue = 50;
constexpr int kShellSaveOptions = 51;
constexpr int kShellCreateNewPilot = 18;
constexpr int kShellSelectPilot = 19;
constexpr int kResolutionCount = 29;

using SetScreenMode_t = void(__stdcall*)(DWORD, DWORD, DWORD, DWORD, DWORD, DWORD,
                                         DWORD, DWORD, DWORD, DWORD, DWORD, DWORD);
using ShellCallback_t = int(__stdcall*)(DWORD, DWORD, int*);

SetScreenMode_t g_set_screen_mode = nullptr;   // trampoline
ShellCallback_t g_shell_callback = nullptr;    // trampoline

// Game globals (located from the gos_SetScreenMode and mode-handler code).
volatile DWORD* g_bpp = nullptr;
volatile DWORD* g_device = nullptr;
volatile BYTE* g_arg5 = nullptr;
volatile BYTE* g_arg6 = nullptr;
volatile BYTE* g_arg7 = nullptr;
volatile BYTE* g_fullscreen = nullptr;
volatile DWORD* g_mission_width = nullptr;    // the game's own selected size
volatile DWORD* g_mission_height = nullptr;
const DWORD* g_resolutions = nullptr;         // 29 (width, height) pairs

bool g_windowed = true;
int g_sel_width = 1024;
int g_sel_height = 768;

// The size the game last asked for. 800x600 means the shell (frontend or the
// in-mission options overlay); anything else is the mission.
DWORD g_req_width = 800;
DWORD g_req_height = 600;
wchar_t g_options_path[MAX_PATH] = {};
bool g_installed = false;

bool SaneSize(int w, int h) { return w >= 640 && h >= 480 && w <= 16384 && h <= 16384; }

void OptionsPath() {
    DWORD len = GetModuleFileNameW(nullptr, g_options_path, MAX_PATH);
    wchar_t* slash = len ? wcsrchr(g_options_path, L'\\') : nullptr;
    if (!slash) {
        g_options_path[0] = L'\0';
        return;
    }
    slash[1] = L'\0';
    wcscat_s(g_options_path, L"options.ini");
}

void PersistWindowed() {
    if (g_options_path[0]) {
        WritePrivateProfileStringW(L"graphics options", L"windowed",
                                   g_windowed ? L"true" : L"false", g_options_path);
    }
}

void PersistResolution() {
    if (!g_options_path[0]) {
        return;
    }
    wchar_t value[16];
    swprintf_s(value, L"%d", g_sel_width);
    WritePrivateProfileStringW(L"graphics options", L"screenwidth", value, g_options_path);
    swprintf_s(value, L"%d", g_sel_height);
    WritePrivateProfileStringW(L"graphics options", L"screenheight", value, g_options_path);
}

int ResolutionIndex(int w, int h) {
    if (!g_resolutions) {
        return -1;
    }
    for (int i = 0; i < kResolutionCount; ++i) {
        if (static_cast<int>(g_resolutions[i * 2]) == w &&
            static_cast<int>(g_resolutions[i * 2 + 1]) == h) {
            return i;
        }
    }
    return -1;
}

void SyncGameSelectedSize() {
    if (g_mission_width && g_mission_height) {
        *g_mission_width = static_cast<DWORD>(g_sel_width);
        *g_mission_height = static_cast<DWORD>(g_sel_height);
    }
}

void __stdcall Hook_SetScreenMode(DWORD width, DWORD height, DWORD bpp, DWORD device,
                                  DWORD a5, DWORD a6, DWORD a7, DWORD goto_full,
                                  DWORD a9, DWORD goto_window, DWORD a11,
                                  DWORD renderer) {
    const DWORD req_w = width;
    const DWORD req_h = height;
    g_req_width = req_w;
    g_req_height = req_h;
    SyncGameSelectedSize();
    const bool shell = req_w == 800 && req_h == 600;
    if (!shell) {
        width = static_cast<DWORD>(g_sel_width);
        height = static_cast<DWORD>(g_sel_height);
    }
    if (bpp == 16) {
        bpp = 32;
    }
    // Never GameOS' exclusive fullscreen: Windowed off is a borderless window
    // (window.cpp). If GameOS is somehow in exclusive mode, bring it back.
    const bool fullscreen_now = g_fullscreen && *g_fullscreen != 0;
    goto_full = 0;
    goto_window = fullscreen_now ? 1 : 0;
    Log("gos_SetScreenMode %lux%lu -> %lux%lux%lu %s%s%s\n", req_w, req_h, width,
        height, bpp, fullscreen_now ? "[fullscreen]" : "[windowed]",
        goto_full ? " goto-fullscreen" : "", goto_window ? " goto-windowed" : "");
    g_set_screen_mode(width, height, bpp, device, a5, a6, a7, goto_full, a9,
                      goto_window, a11, renderer);
}

// Re-issue the current mode with the hook's size/flags. Runs on the game
// thread (from a script callback); the switch itself happens next frame.
void ApplyDisplay() {
    if (!g_set_screen_mode || !g_bpp) {
        return;
    }
    SyncGameSelectedSize();
    // Re-issue the game's last request so shell/mission state is preserved.
    Hook_SetScreenMode(g_req_width, g_req_height, *g_bpp, *g_device, *g_arg5, *g_arg6,
                       *g_arg7, 0, 0, 0, 0, 0);
}

// A new pilot whose controls still need a real layout (see below).
char g_new_pilot[64] = {};
// A new pilot whose default camouflage is still pending (camo.cpp).
char g_camo_pilot[64] = {};

// A script string argument: a pointer to the characters, or to a pointer to
// them. Returns nullptr if neither reads as a short printable string.
const char* ScriptString(int arg) {
    auto printable = [](const char* s) {
        if (!s || IsBadReadPtr(s, 1)) {
            return false;
        }
        for (int i = 0; i < 64; ++i) {
            if (IsBadReadPtr(s + i, 1)) {
                return false;
            }
            if (s[i] == '\0') {
                return i > 0;
            }
            if (static_cast<unsigned char>(s[i]) < 0x20) {
                return false;
            }
        }
        return false;
    };
    const char* direct = reinterpret_cast<const char*>(arg);
    if (printable(direct)) {
        return direct;
    }
    if (arg > 0x10000 && !IsBadReadPtr(direct, 4)) {
        const char* indirect = *reinterpret_cast<const char* const*>(direct);
        if (printable(indirect)) {
            return indirect;
        }
    }
    return nullptr;
}

int CallOriginal(DWORD a1, DWORD a2, int cmd, int option, int* value) {
    int args[3] = {cmd, option, reinterpret_cast<int>(value)};
    return g_shell_callback(a1, a2, args);
}

int __stdcall Hook_ShellCallback(DWORD a1, DWORD a2, int* args) {
    const int cmd = args ? args[0] : -1;
    if (GetConfig().debug_automation && (cmd == 100 || cmd == 101)) {
        // Test aid: how the Controls screen passes a key binding.
        auto show = [](int v) {
            static char text[4][48];
            static int slot = 0;
            char* out = text[slot++ & 3];
            if (v > 0x10000 && !IsBadReadPtr(reinterpret_cast<void*>(v), 4)) {
                sprintf_s(out, 48, "%08X->%d", v, *reinterpret_cast<int*>(v));
            } else {
                sprintf_s(out, 48, "%d", v);
            }
            return out;
        };
        Log("controls: callback %d args %s %s %s %s\n", cmd, show(args[1]), show(args[2]),
            show(args[3]), show(args[4]));
    }
    if ((cmd == kShellGetOptionValue || cmd == kShellSetOptionValue) && args[2]) {
        const int option = args[1];
        int* value = reinterpret_cast<int*>(args[2]);
        if (option == kMwoThrottleDecay) {
            if (cmd == kShellGetOptionValue) {
                *value = ThrottleDecayEnabled() ? 1 : 0;
            } else {
                SetThrottleDecay(*value != 0);
            }
            return 1;
        }
        if (option == kMwoGyroAim) {
            if (cmd == kShellGetOptionValue) {
                *value = GyroAimEnabled() ? 1 : 0;
            } else {
                SetGyroAim(*value != 0);
            }
            return 1;
        }
        if (option == kMwoWindowed) {
            if (cmd == kShellGetOptionValue) {
                *value = g_windowed ? 1 : 0;
            } else {
                Log("display: script sets Windowed = %d\n", *value);
                SetWindowedMode(*value != 0);
            }
            return 1;
        }
        if (option == kMwoScreenResolution && g_resolutions) {
            if (cmd == kShellGetOptionValue) {
                int result = g_shell_callback(a1, a2, args);
                int index = ResolutionIndex(g_sel_width, g_sel_height);
                if (index >= 0) {
                    *value = index;
                }
                return result;
            }
            // 3.1c's SetOptionValue(7) falls through into the
            // UseExtendedVisibility case and stores the resolution index
            // there too. Preserve that option across the call.
            int extended = 0;
            CallOriginal(a1, a2, kShellGetOptionValue, kMwoUseExtendedVisibility, &extended);
            int result = g_shell_callback(a1, a2, args);
            CallOriginal(a1, a2, kShellSetOptionValue, kMwoUseExtendedVisibility, &extended);
            const int index = *value;
            Log("display: script sets Resolution = index %d\n", index);
            if (index >= 0 && index < kResolutionCount) {
                const int w = static_cast<int>(g_resolutions[index * 2]);
                const int h = static_cast<int>(g_resolutions[index * 2 + 1]);
                if (SaneSize(w, h) && (w != g_sel_width || h != g_sel_height)) {
                    g_sel_width = w;
                    g_sel_height = h;
                    Log("display: resolution -> %dx%d (index %d)\n", w, h, index);
                    ApplyWindowLayout();  // the window size, now
                    ApplyDisplay();       // the mission size, if in a mission
                }
            }
            return result;
        }
    }
    int result = g_shell_callback(a1, a2, args);
    if (cmd == 100) {
        // ShellControlSetDefault (Controls screen DEFAULT): MW4's layout has
        // just been restored; with [controls] mw5=1 apply the MW5 layout.
        ApplyMw5Layout(g_shell_callback, a1, a2);
    }
    if (cmd == kShellSaveOptions) {
        // The game rewrites screenwidth/screenheight from its own index map,
        // which disagrees with the dropdown for some entries. Write ours last.
        PersistResolution();
        PersistWindowed();
    }
    if (cmd == kShellCreateNewPilot && result >= 0) {
        // A new pilot starts from MekTek's template profile, whose control
        // list shows a WASD layout that is not what loads for play (the
        // mission uses MW4's factory keys). Remember the pilot; when it is
        // selected (START / the roster list), give it a real layout below.
        // Doing it here, while the pilot is being created, hangs the game.
        const char* name = ScriptString(args[1]);
        strncpy_s(g_new_pilot, name ? name : "", _TRUNCATE);
        Log("controls: new pilot '%s' created\n", g_new_pilot);
    }
    if (cmd == kShellSelectPilot && g_camo_pilot[0]) {
        // A pending default camouflage belongs to the new pilot only.
        const char* name = ScriptString(args[1]);
        if (!name || _stricmp(name, g_camo_pilot) != 0) {
            CancelNewPilotCamo();
            g_camo_pilot[0] = '\0';
        }
    }
    if (cmd == kShellSelectPilot && g_new_pilot[0]) {
        const char* name = ScriptString(args[1]);
        Log("controls: pilot '%s' selected\n", name ? name : "?");
        if (name && _stricmp(name, g_new_pilot) == 0) {
            // As DEFAULT + OK in Options > Controls: MW4's layout, or with
            // [controls] mw5=1 the MW5 layout with the mouse on the torso;
            // then save the pilot's options.
            g_new_pilot[0] = '\0';
            int set_default[1] = {100};
            g_shell_callback(a1, a2, set_default);
            ApplyMw5Layout(g_shell_callback, a1, a2, false);
            SetNewPilotCamo();  // the template's skin is Pink; see camo.cpp
            strncpy_s(g_camo_pilot, name, _TRUNCATE);
            int save[1] = {kShellSaveOptions};
            Hook_ShellCallback(a1, a2, save);
            Log("controls: new pilot '%s': %s layout applied and saved\n", name,
                GetConfig().controls_mw5 ? "MW5" : "MW4");
        }
    }
    return result;
}

void Patch16BitFallbacks() {
    static const unsigned char k32[4] = {0x20, 0, 0, 0};
    unsigned char* renderer = FindUnique(
        "bpp16_renderer",
        "7D 08 39 1D ?? ?? ?? ?? 75 0A C7 05 ?? ?? ?? ?? 10 00 00 00 8B 15");
    if (renderer) {
        WriteCode(renderer + 16, k32, 4);
    }
    unsigned char* fallback = FindUnique(
        "bpp16_fallback", "53 53 6A 10 51 52 50 C7 05 ?? ?? ?? ?? 10 00 00 00 C6 05");
    if (fallback) {
        static const unsigned char push32 = 0x20;
        WriteCode(fallback + 3, &push32, 1);
        WriteCode(fallback + 13, k32, 4);
    }
    Log("display: 16-bit fallbacks raised to 32-bit (%s/%s)\n",
        renderer ? "ok" : "missing", fallback ? "ok" : "missing");
}

}  // namespace

void DisplayInit() {
    OptionsPath();
    if (!g_options_path[0]) {
        return;
    }
    wchar_t value[32] = {};
    GetPrivateProfileStringW(L"graphics options", L"windowed", L"true", value, 32,
                             g_options_path);
    g_windowed = !(_wcsicmp(value, L"false") == 0 || wcscmp(value, L"0") == 0);
    int w = static_cast<int>(GetPrivateProfileIntW(L"graphics options", L"screenwidth",
                                                   1024, g_options_path));
    int h = static_cast<int>(GetPrivateProfileIntW(L"graphics options", L"screenheight",
                                                   768, g_options_path));
    if (SaneSize(w, h)) {
        g_sel_width = w;
        g_sel_height = h;
    }
    Log("display: windowed=%d selected=%dx%d\n", g_windowed ? 1 : 0, g_sel_width,
        g_sel_height);
}

bool DesiredWindowed() { return g_windowed; }

int ShellMode() {
    if (!g_set_screen_mode) {
        return -1;  // display hooks not active: unknown
    }
    return g_req_width == 800 && g_req_height == 600 ? 1 : 0;
}

void SelectedSize(int* width, int* height) {
    *width = g_sel_width;
    *height = g_sel_height;
}

bool GameFullscreen() { return g_fullscreen && *g_fullscreen != 0; }

void SetWindowedMode(bool windowed) {
    if (windowed == g_windowed) {
        return;
    }
    g_windowed = windowed;
    Log("display: windowed -> %d (%s)\n", windowed ? 1 : 0,
        windowed ? "framed window" : "borderless fullscreen");
    PersistWindowed();
    ApplyWindowLayout();
}

// Installs the display hooks once the unpacked code is present. Returns true
// when done (successfully or not) so the caller stops retrying.
bool InstallDisplayHooks() {
    if (g_installed) {
        return true;
    }
    unsigned char* ssm = FindUnique(
        "gos_SetScreenMode",
        "8B 4C 24 04 A1 ?? ?? ?? ?? 8B 54 24 20 53 55 8B 6C 24 14 56 8B 74 24 3C "
        "57 8B 7C 24 18 3B C8 8B 44 24 28");
    if (!ssm) {
        return false;  // not unpacked yet; retry
    }
    g_installed = true;
    unsigned char* handler = FindUnique(
        "mode_handler",
        "A0 ?? ?? ?? ?? 84 C0 74 0C A0 ?? ?? ?? ?? 84 C0 75 03 32 C0 C3 A1 ?? ?? ?? ?? 50 FF 15");
    unsigned char* sel = FindUnique("selected_res_switch",
                                    "8B 46 30 83 F8 1C 0F 87 ?? ?? ?? ?? FF 24 85 ?? ?? ?? ??");
    unsigned char* table = FindUnique(
        "resolution_table", "BF ?? ?? ?? ?? 8B F1 2B F9 2B F3 89 44 24 14 BD ?? ?? ?? ??");
    unsigned char* callback = FindUnique("shell_callback",
                                         "8B 44 24 0C 8B 08 41 81 F9 E8 00 00 00 0F 87");
    if (!handler) {
        Log("display: mode handler not found; display hooks disabled\n");
        return true;
    }

    auto addr = [](unsigned char* p) { return *reinterpret_cast<DWORD*>(p); };
    g_bpp = reinterpret_cast<DWORD*>(addr(ssm + 0x2F));
    g_device = reinterpret_cast<DWORD*>(addr(ssm + 0x43));
    g_arg5 = reinterpret_cast<BYTE*>(addr(ssm + 0x4F));
    g_arg6 = reinterpret_cast<BYTE*>(addr(ssm + 0x57));
    g_arg7 = reinterpret_cast<BYTE*>(addr(ssm + 0x63));
    g_fullscreen = reinterpret_cast<BYTE*>(addr(handler + 1));

    if (sel) {
        // First case: C7 05 <width global> <w> C7 05 <height global> <h>
        auto* cases = reinterpret_cast<DWORD*>(addr(sel + 0x0F));
        auto* first = reinterpret_cast<unsigned char*>(cases[0]);
        if (first[0] == 0xC7 && first[1] == 0x05 && first[10] == 0xC7 && first[11] == 0x05) {
            g_mission_width = reinterpret_cast<DWORD*>(addr(first + 2));
            g_mission_height = reinterpret_cast<DWORD*>(addr(first + 12));
        }
    }
    if (table) {
        auto* entries = reinterpret_cast<const DWORD*>(addr(table + 0x10));
        bool ok = true;
        for (int i = 0; i < kResolutionCount; ++i) {
            ok = ok && SaneSize(static_cast<int>(entries[i * 2]),
                                static_cast<int>(entries[i * 2 + 1]));
        }
        g_resolutions = ok ? entries : nullptr;
    }
    Log("display: globals bpp=%p fullscreen=%p mission=%p/%p table=%p\n", g_bpp,
        g_fullscreen, g_mission_width, g_mission_height, g_resolutions);

    SyncGameSelectedSize();
    Patch16BitFallbacks();

    InstallDetour(ssm, 9, reinterpret_cast<void*>(&Hook_SetScreenMode),
                  reinterpret_cast<void**>(&g_set_screen_mode));
    if (callback) {
        InstallDetour(callback, 6, reinterpret_cast<void*>(&Hook_ShellCallback),
                      reinterpret_cast<void**>(&g_shell_callback));
    }
    Log("display: hooks %s / %s\n", g_set_screen_mode ? "ok" : "failed",
        g_shell_callback ? "ok" : "failed");
    return true;
}

}  // namespace mw4hook

// For the ddraw proxy: 1 while the game shows its 800x600 shell (frontend,
// loading screens, in-mission options overlay), 0 in a mission, -1 unknown.
extern "C" int WINAPI Mw4ShellMode() { return mw4hook::ShellMode(); }