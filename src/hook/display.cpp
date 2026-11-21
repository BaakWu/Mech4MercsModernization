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

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace mw4hook {
namespace {

constexpr int kMwoScreenResolution = 7;
constexpr int kMwoUseExtendedVisibility = 34;
constexpr int kShellSetOptionValue = 49;
constexpr int kShellGetOptionValue = 50;
constexpr int kShellSaveOptions = 51;
constexpr int kShellCreateNewPilot = 18;
constexpr int kShellSelectPilot = 19;
constexpr int kShellRenamePilot = 21;
constexpr int kShellGetSupportedResolutions = 124;
constexpr int kResolutionCount = 29;
constexpr int kTextTypeNormal = 0;    // listboxes.script TEXT_TYPE_NORMAL (white)
constexpr int kTextTypeSpecial2 = 5;  // TEXT_TYPE_SPECIAL2 (red), the game's "unsupported"

// Options > Video > Screen Resolution (issue #38). The game fills the list
// from a fixed table of 29 sizes (0x0081DCC0) and the 29 labels built from it
// at startup. Six of them cannot be played; choosing one crashed or hung the
// game at the next mission start:
//   - 2560x1600, 3840x2400, 3072x768, 4320x900, 5040x1050: Direct3D 7's
//     CreateDevice (d3dim700.dll, Windows' own runtime) refuses a render
//     target wider or taller than 2048 (mov eax,800h; cmp; ja ->
//     DDERR_INVALIDOBJECT), whatever the card reports (16384 here); GameOS
//     then crashes.
//   - 2048x1536: the device is made, but the mission load stops on the
//     loading screen (cause not found).
// Nor can sizes outside the game's list be added: HUDNumberText::GetIndex
// (0x00563A70) only knows the screen heights 480, 600, 720, 768, 800, 854,
// 864, 900, 960, 1024, 1050, 1080, 1200, 1536, 1600 and 2400; any other
// (1152, 1440, 1800, 2160) raises a game error whose dialog never shows, and
// the mission load waits for it forever.
// So the list offers the other 23 sizes, in the game's order. For a large
// monitor, Windowed off (borderless) scales the mission to fill the screen.
constexpr int kListCount = 23;
constexpr DWORD kResolutions[kListCount * 2] = {
    640,  480,  800,  600,  960,  600,  1024, 768,  1152, 768,  1152, 864,
    1280, 720,  1280, 768,  1280, 800,  1280, 854,  1280, 960,  1280, 1024,
    1360, 768,  1366, 768,  1400, 1050, 1440, 900,  1440, 960,  1600, 900,
    1600, 1024, 1600, 1200, 1680, 1050, 1920, 1080, 1920, 1200,
};

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
// The (width, height) pairs behind the dropdown's rows: the game's table (29),
// or kResolutions (kListCount) once the hook has written the list
// (ListResolutions).
const DWORD* g_resolutions = nullptr;
int g_resolution_count = kResolutionCount;

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
    for (int i = 0; i < g_resolution_count; ++i) {
        if (static_cast<int>(g_resolutions[i * 2]) == w &&
            static_cast<int>(g_resolutions[i * 2 + 1]) == h) {
            return i;
        }
    }
    return -1;
}

bool Playable(int w, int h) {
    for (int i = 0; i < kListCount; ++i) {
        if (static_cast<int>(kResolutions[i * 2]) == w &&
            static_cast<int>(kResolutions[i * 2 + 1]) == h) {
            return true;
        }
    }
    return false;
}

// A size the game cannot play (one of the six removed from the list, from an
// older options.ini) is replaced by the listed size of the closest shape
// that fits inside it, the largest of those (2560x1600 -> 1920x1200,
// 3840x2160 -> 1920x1080, 2048x1536 -> 1600x1200, 5040x1050 -> 1600x900).
void PlayableSize(int* w, int* h) {
    if (Playable(*w, *h)) {
        return;
    }
    const double aspect = static_cast<double>(*w) / *h;
    int best = -1;
    double best_diff = 0.0;
    for (int i = 0; i < kListCount; ++i) {
        const int cw = static_cast<int>(kResolutions[i * 2]);
        const int ch = static_cast<int>(kResolutions[i * 2 + 1]);
        if (cw > *w || ch > *h) {
            continue;
        }
        const double diff = std::fabs(static_cast<double>(cw) / ch - aspect);
        const bool closer = best < 0 || diff < best_diff - 0.01;
        const bool same_but_larger =
            best >= 0 && std::fabs(diff - best_diff) <= 0.01 &&
            cw * ch > static_cast<int>(kResolutions[best * 2] * kResolutions[best * 2 + 1]);
        if (closer || same_but_larger) {
            best = i;
            best_diff = diff;
        }
    }
    if (best < 0) {
        best = 3;  // 1024x768
    }
    Log("display: %dx%d cannot be played (see display.cpp); using %lux%lu\n", *w, *h,
        kResolutions[best * 2], kResolutions[best * 2 + 1]);
    *w = static_cast<int>(kResolutions[best * 2]);
    *h = static_cast<int>(kResolutions[best * 2 + 1]);
}

// Runs after the game's ShellGetSupportedResolutions (0x0051CD50) has filled
// the Video tab's list: args[1] = list_item[] (one char* per row; the
// handler gives each row a private 128-byte string), args[2] =
// list_item_type[], args[3] = the row count (29; the script sets the list's
// length from it). Writes kResolutions' labels, in the game's "W x H" form,
// their types and their count into it. The game shows a row in red unless
// GameOS reports it as a fullscreen display mode, which under the patch
// failed for every row; the patch never changes the display mode, so a row
// is white when it fits on the primary monitor and red when the picture is
// scaled down to fit (any row can still be chosen, as before).
bool ListResolutions(const int* args) {
    __try {
        char** items = reinterpret_cast<char**>(args[1]);
        int* types = reinterpret_cast<int*>(args[2]);
        int* count = reinterpret_cast<int*>(args[3]);
        if (!items || !types || !count || *count != kResolutionCount) {
            return false;
        }
        for (int i = 0; i < kListCount; ++i) {
            if (!items[i] || IsBadWritePtr(items[i], 16)) {
                return false;
            }
        }
        const DWORD screen_w = static_cast<DWORD>(GetSystemMetrics(SM_CXSCREEN));
        const DWORD screen_h = static_cast<DWORD>(GetSystemMetrics(SM_CYSCREEN));
        int fit = 0;
        for (int i = 0; i < kListCount; ++i) {
            const DWORD w = kResolutions[i * 2];
            const DWORD h = kResolutions[i * 2 + 1];
            sprintf_s(items[i], 16, "%lu x %lu", w, h);
            const bool fits = w <= screen_w && h <= screen_h;
            types[i] = fits ? kTextTypeNormal : kTextTypeSpecial2;
            fit += fits ? 1 : 0;
        }
        *count = kListCount;
        static bool logged = false;
        if (!logged) {
            logged = true;
            Log("display: resolution list written (%d sizes, %d fit the %lux%lu monitor)\n",
                kListCount, fit, screen_w, screen_h);
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
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
    if (cmd == kShellGetSupportedResolutions && args) {
        // The tab asks for the list before it reads the selection, so the
        // index below already maps onto these rows.
        const int result = g_shell_callback(a1, a2, args);
        if (ListResolutions(args)) {
            g_resolutions = kResolutions;
            g_resolution_count = kListCount;
        } else {
            Log("display: resolution list not as expected; the game's list is kept\n");
        }
        return result;
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
        if (option == kMwoNewPilotCamo || option == kMwoPilotCamo) {
            // Join Regiment's Camouflage list: 204 = new pilot (starts at the
            // default), 205 = Update of the selected pilot (starts at its
            // last choice).
            const bool update = option == kMwoPilotCamo;
            if (cmd == kShellSetOptionValue) {
                ChoosePilotCamo(*value);
            } else {
                *value = update ? CurrentPilotCamoIndex() : CamoIndex(GetConfig().default_camo);
            }
            return 1;
        }
        if (option == kMwoMechClass) {
            // MechLab weight-class filter (shellscript.cpp), kept per session.
            static int s_class = 0;
            if (cmd == kShellGetOptionValue) {
                *value = s_class;
            } else {
                s_class = (*value >= 0 && *value <= 5) ? *value : 0;
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
            if (index >= 0 && index < g_resolution_count) {
                int w = static_cast<int>(g_resolutions[index * 2]);
                int h = static_cast<int>(g_resolutions[index * 2 + 1]);
                PlayableSize(&w, &h);  // the game's own list, if the hook's is not in use
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
        // A new pilot starts from MekTek's template profile: its control list
        // shows a WASD layout that is not what loads for play (the mission
        // uses MW4's factory keys), and its skin is Pink. Record the pilot
        // (mw4hook-pilots.ini, so it survives a restart); when it is selected
        // (START / the roster list) it gets a real layout below, and its
        // camouflage when its first 'Mech is created (camo.cpp). Applying the
        // layout here, while the pilot is being created, hangs the game.
        const char* name = ScriptString(args[1]);
        if (name) {
            QueueNewPilot(name);
            Log("controls: new pilot '%s' created\n", name);
        }
    }
    if (cmd == kShellRenamePilot && result >= 0) {
        // Roster > Update: the pilot may have a new name, and the
        // Camouflage list sets its default for 'Mechs it gets from now on.
        const char* old_name = ScriptString(args[1]);
        const char* new_name = ScriptString(args[2]);
        if (old_name && new_name) {
            std::string from = old_name;  // the script's buffers, read before use
            std::string to = new_name;
            RenamePilotRecords(from.c_str(), to.c_str());
            QueueUpdatedPilotCamo(to.c_str());
        }
    }
    if (cmd == kShellSelectPilot) {
        const char* name = ScriptString(args[1]);
        SetCurrentPilot(name ? name : "");
        if (name && TakeNewPilotControls(name)) {
            // As DEFAULT + OK in Options > Controls: MW4's layout, or with
            // [controls] mw5=1 the MW5 layout with the mouse on the torso;
            // then save the pilot's options.
            int set_default[1] = {100};
            g_shell_callback(a1, a2, set_default);
            ApplyMw5Layout(g_shell_callback, a1, a2, false);
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

// Texture memory test. GameOS' hardware device setup (0x00710790) rejects a
// card whose local + AGP video memory is below the game's minimum (0x1400
// KB, set at 0x0040B3BC) and the game then runs its software renderer and
// shows "Hardware Error":
//   mov ecx,[minimum]; cmp ecx,ebx; je skip; shl ecx,0Ah; cmp eax,ecx;
//   jb <reject>
// The total is a 32-bit byte count of DirectDraw's figures (+ doubled with
// VideoCard.txt AllowLowMemoryCard), which current cards report in
// gigabytes, so it can wrap below the minimum. Every current card has far
// more than 5 MB; the jb is removed.
void SkipTextureMemoryTest() {
    unsigned char* site = FindUnique(
        "texture_memory_test", "8B 0D ?? ?? ?? ?? 3B CB 74 0B C1 E1 0A 3B C1 0F 82");
    bool ok = false;
    if (site) {
        static const unsigned char kNops[6] = {0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
        ok = WriteCode(site + 15, kNops, sizeof(kNops));
    }
    Log("display: texture memory minimum %s\n",
        ok ? "skipped" : (site ? "patch FAILED" : "not found"));
}

// Device caps test. After the memory test the hardware setup rejects the
// card unless the VideoCard.txt LowEndCard flag is clear and the device's
// triangle caps (GetCaps into the D3DDEVICEDESC7 at 0x01242078, dpcTriCaps
// at +0x3C) have CULLNONE, SRCALPHA / INVSRCALPHA blending, perspective
// textures, NEAREST filtering, MODULATE in the DX5-era dwTextureBlendCaps
// and WRAP addressing (0x007111CF..0x0071123F). Every GPU of the last 25
// years does all of that, but a current driver does not have to advertise
// the legacy bits; an RTX 5080 got the Hardware Error with the device
// created (#35). The hook logs the caps and accepts the device:
//   test dh,8; jne <reject>   ->   jmp CapsAccepted (logs, then success)
const DWORD* g_device_desc = nullptr;  // the D3DDEVICEDESC7 (0x01242078)
void* g_caps_success = nullptr;        // call 0x007432A0; mov al,1; ... ret 8

void __cdecl LogDeviceCaps() {
    static int logged = 0;
    if (logged >= 4 || !g_device_desc) {
        return;
    }
    ++logged;
    const DWORD* tri = g_device_desc + 0x3C / 4;  // D3DPRIMCAPS: dwSize, dwMiscCaps, ...
    struct Check {
        const char* name;
        int field;  // dword index in D3DPRIMCAPS
        DWORD bit;
    };
    static const Check kChecks[] = {
        {"MiscCaps CULLNONE", 1, 0x10},          {"SrcBlendCaps SRCALPHA", 4, 0x10},
        {"DestBlendCaps INVSRCALPHA", 5, 0x20}, {"TextureCaps PERSPECTIVE", 8, 0x1},
        {"TextureFilterCaps NEAREST", 9, 0x1},  {"TextureBlendCaps MODULATE", 10, 0x2},
        {"TextureAddressCaps WRAP", 11, 0x1},
    };
    char missing[256] = {};
    for (const Check& c : kChecks) {
        if (!(tri[c.field] & c.bit)) {
            if (missing[0]) {
                strcat_s(missing, ", ");
            }
            strcat_s(missing, c.name);
        }
    }
    Log("renderer: hardware device caps: dev 0x%08lX misc 0x%08lX src 0x%08lX dest 0x%08lX tex "
        "0x%08lX filter 0x%08lX blend 0x%08lX address 0x%08lX%s%s\n",
        g_device_desc[0], tri[1], tri[4], tri[5], tri[8], tri[9], tri[10], tri[11],
        missing[0] ? "; the game would reject it for missing " : "; all the game checks",
        missing);
}

__declspec(naked) void CapsAccepted() {
    __asm {
        pushad
        pushfd
        call LogDeviceCaps
        popfd
        popad
        jmp dword ptr [g_caps_success]
    }
}

void AcceptDeviceCaps() {
    unsigned char* site = FindUnique(
        "device_caps_test",
        "F6 C6 08 75 7D 39 1D ?? ?? ?? ?? 74 0C A1 ?? ?? ?? ?? 83 E0 02 84 C0 75");
    // The success path, 0x72 bytes on: call rel32; pop edi; pop esi; mov al,1
    static const unsigned char kSuccess[] = {0x5F, 0x5E, 0xB0, 0x01};
    bool ok = false;
    if (site && site[0x72] == 0xE8 && memcmp(site + 0x77, kSuccess, sizeof(kSuccess)) == 0) {
        // The caps block: mov ecx,[0x012420D8] (the NEAREST check) at +0x19.
        const DWORD nearest = *reinterpret_cast<const DWORD*>(site + 0x1B);
        if (site[0x19] == 0x8B && site[0x1A] == 0x0D && nearest == 0x012420D8) {
            g_device_desc = reinterpret_cast<const DWORD*>(0x01242078);
            g_caps_success = site + 0x72;
            unsigned char code[5] = {0xE9};
            const int rel = static_cast<int>(reinterpret_cast<unsigned char*>(&CapsAccepted) -
                                             (site + 5));
            memcpy(code + 1, &rel, 4);
            ok = WriteCode(site, code, sizeof(code));  // test dh,8 (3) + jne rel8 (2)
        }
    }
    Log("display: device caps test %s\n",
        ok ? "logged and accepted" : (site ? "patch FAILED (bytes differ)" : "not found"));
}

// Renderer choice (diagnostics for the "Hardware Error" dialog). GameOS'
// renderer setup (3.1c 0x00711620, cdecl, no arguments) tries the hardware
// Direct3D device when the renderer option is 0, at least one enumerated
// device has hardware rasterization (D3DDEVCAPS_HWRASTERIZATION, checked
// by the IDirect3D7::EnumDevices callback 0x0073C710) and the mode is
// windowed or 16/32-bit; otherwise, or when the hardware device cannot be
// made, it falls back to the software renderer (3). Its device setup
// (0x00710790) sets the "hardware" flag 0x012423A4 only for the hardware
// renderer, and the startup check after it (0x0040AC4E,
// gos_GetMachineInformation 0x33) shows "Your current hardware or driver
// setup is not compatible with MechWarrior 4" when the flag is 0. The hook
// logs the inputs to that choice and the result after every setup.
using InitRenderer_t = int(__cdecl*)();
InitRenderer_t g_init_renderer = nullptr;  // trampoline
const DWORD* g_renderer_option = nullptr;  // options.ini renderer
const DWORD* g_hw_devices = nullptr;       // devices with hardware rasterization
const BYTE* g_renderer_fullscreen = nullptr;
const DWORD* g_renderer_bpp = nullptr;

int __cdecl Hook_InitRenderer() {
    const int result = g_init_renderer();
    static int logged = 0;
    if (logged >= 8) {
        return result;
    }
    ++logged;
    const DWORD hardware = *reinterpret_cast<const DWORD*>(0x012423A4);
    const DWORD devices = g_hw_devices[1];
    const DWORD selected = g_renderer_option[-1];
    Log("renderer: option %lu, %lu device(s), %lu with hardware 3D, selected %ld, %s, %lu-bit "
        "-> %s\n",
        *g_renderer_option, devices, *g_hw_devices, static_cast<long>(selected),
        *g_renderer_fullscreen ? "fullscreen" : "windowed", *g_renderer_bpp,
        hardware ? "hardware Direct3D"
                 : "software renderer (the game reports a Hardware Error)");
    // Device records: g_hw_devices + 0x10, 0x11D8 bytes each; the
    // D3DDEVICEDESC7 copy (dwDevCaps first) at +0x5BC.
    const unsigned char* records = reinterpret_cast<const unsigned char*>(g_hw_devices) + 0x10;
    for (DWORD i = 0; i < devices && i < 8; ++i) {
        const unsigned char* record = records + i * 0x11D8;
        if (IsBadReadPtr(record, 0x5C0)) {
            break;
        }
        const DWORD caps = *reinterpret_cast<const DWORD*>(record + 0x5BC);
        // The first readable name in the record (the DirectDraw description).
        char name[64] = {};
        for (int o = 0x10; o < 0x5BC && !name[0]; ++o) {
            int n = 0;
            while (o + n < 0x5BC && record[o + n] >= 0x20 && record[o + n] < 0x7F && n < 63) {
                ++n;
            }
            if (n >= 6 && record[o + n] == 0) {
                memcpy(name, record + o, n);
            }
            o += n;
        }
        Log("renderer:   device %lu \"%s\" devcaps 0x%08lX%s\n", i, name, caps,
            (caps & 0x80000) ? " (hardware rasterization)" : " (no hardware rasterization)");
    }
    return result;
}

void InstallRendererTrace() {
    // push ebp; mov ebp,esp; sub esp,30h; mov eax,[renderer]; push ebx;
    // xor ebx,ebx; push esi; cmp eax,ebx; jne +30h; cmp [hw devices],ebx;
    // je; mov cl,[fullscreen]; cmp cl,bl; jne; mov eax,[bpp]
    unsigned char* init = FindUnique(
        "renderer_init",
        "55 8B EC 83 EC 30 A1 ?? ?? ?? ?? 53 33 DB 56 3B C3 75 30 39 1D ?? ?? ?? ?? 74 28 8A "
        "0D ?? ?? ?? ?? 3A CB 75 0F A1");
    if (!init) {
        return;
    }
    auto addr = [](unsigned char* p) { return *reinterpret_cast<DWORD*>(p); };
    g_renderer_option = reinterpret_cast<const DWORD*>(addr(init + 7));
    g_hw_devices = reinterpret_cast<const DWORD*>(addr(init + 0x15));
    g_renderer_fullscreen = reinterpret_cast<const BYTE*>(addr(init + 0x1D));
    g_renderer_bpp = reinterpret_cast<const DWORD*>(addr(init + 0x26));
    // push ebp; mov ebp,esp; sub esp,30h (6 bytes) is position independent.
    InstallDetour(init, 6, reinterpret_cast<void*>(&Hook_InitRenderer),
                  reinterpret_cast<void**>(&g_init_renderer));
    Log("display: renderer trace %s\n", g_init_renderer ? "ok" : "failed");
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
        PlayableSize(&w, &h);  // saved on the next OK in Options
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
    SkipTextureMemoryTest();
    AcceptDeviceCaps();
    InstallRendererTrace();

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