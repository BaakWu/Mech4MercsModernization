// MechWarrior 5-style controls, opt-in ([controls] mw5=1).
//
// Driving. MW4 already drives with W/S/A/D: holding W ("Accelerate") raises
// the throttle, S ("Decelerate") lowers it and runs on into reverse, and the
// throttle stays where it was set. On top of that:
//   - W/S move the throttle twice as fast (the game adds +-0.02 every 0.05 s,
//     2.5 s from 0 to full; this repeats each step once more),
//   - reverse decays: with no throttle key held, a reverse throttle eases
//     back to 0 (fast at first, then slower); a forward throttle stays.
//
// Key layout. With mw5=1 the DEFAULT button in Options > Controls applies an
// MW5-style layout (X full stop, P power, N night vision, R/T/Q targeting,
// fire groups on 1-6, throttle presets on the numpad, ...) through the
// game's own binding call, so the bindings behave and save like a manual
// remap; with mw5=0 DEFAULT restores MW4's layout as before.
//
// Game side (3.1c, located by unique patterns):
//   0x0054F2A0  per-frame keyboard drive step (thiscall, one double: time).
//               Reads the Accelerate/Decelerate "held" counters (+0x3E4 /
//               +0x3E8) and steps the working throttle +0x2D8 (-1..1); at its
//               end the throttle is stored in +0x564 for the next frame.
//   Shell_CallbackHandler (display.cpp): ShellControlSetDefault (100),
//               ShellControlAssignValue (101: row, column, key<<8, modifier<<8;
//               all arguments are pointers). Assigning a key clears it from
//               any other command.
#include "hook.h"

#include <cmath>

namespace mw4hook {
namespace {

constexpr size_t kThrottle = 0x2D8;        // float, working throttle (-1..1)
constexpr size_t kStoredThrottle = 0x564;  // float, kept between frames
constexpr size_t kForwardHeld = 0x3E4;     // int, Accelerate key held
constexpr size_t kBackwardHeld = 0x3E8;    // int, Decelerate key held
constexpr size_t kMinReverse = 0x650;      // float, smallest reverse throttle (<0)

constexpr float kRampFactor = 2.0f;        // W/S speed multiplier
constexpr double kDecayTau = 0.35;         // reverse decay time constant, s
constexpr float kSnap = 0.02f;             // below this the throttle is 0

using Drive_t = void(__thiscall*)(void*, double);
Drive_t g_drive = nullptr;

constexpr size_t kMinForward = 0x64C;      // float, smallest forward throttle (>0)

double g_last_now = 0.0;
float g_last_written = 0.0f;
bool g_have_last = false;
volatile LONG g_decay = 0;      // Options > Game > Throttle Decay
bool g_decay_armed = false;     // last throttle change came from W/S

float Clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

void __fastcall Hook_Drive(char* self, void*, double now) {
    const float before = *reinterpret_cast<float*>(self + kThrottle);
    g_drive(self, now);

    float t = *reinterpret_cast<float*>(self + kThrottle);
    const bool forward = *reinterpret_cast<int*>(self + kForwardHeld) > 0;
    const bool backward = *reinterpret_cast<int*>(self + kBackwardHeld) > 0;
    double dt = g_have_last ? now - g_last_now : 0.0;
    if (dt < 0.0 || dt > 0.25) {
        dt = 0.0;  // first frame, pause, or a time jump
    }
    g_last_now = now;

    // Faster W/S (mw5 only): repeat the game's own step (small changes only,
    // so the throttle presets and the minimum-speed jumps are left alone).
    const float delta = t - before;
    if (GetConfig().controls_mw5 && (forward || backward) && delta != 0.0f &&
        std::fabs(delta) <= 0.1f) {
        t += delta * (kRampFactor - 1.0f);
    }

    // Throttle decay applies to a throttle set with W/S. A throttle set any
    // other way (number-key presets and X inside the game's step, a joystick
    // before it) holds until W/S is used again.
    const bool untouched = !g_have_last || std::fabs(before - g_last_written) < 1e-6f;
    if (forward || backward) {
        g_decay_armed = true;
    } else if (!untouched || std::fabs(delta) > 1e-6f) {
        g_decay_armed = false;
    }
    if (g_decay && g_decay_armed && !forward && !backward && t != 0.0f && dt > 0.0) {
        // Ease toward 0 (fast at first, then slower); below the mech's
        // smallest forward/reverse throttle the throttle is 0.
        t *= static_cast<float>(std::exp(-dt / kDecayTau));
        float min_forward = *reinterpret_cast<float*>(self + kMinForward);
        float min_reverse = *reinterpret_cast<float*>(self + kMinReverse);
        min_forward = (min_forward > 0.0f && min_forward < 0.5f) ? min_forward : kSnap;
        min_reverse = (min_reverse < 0.0f && min_reverse > -0.5f) ? min_reverse : -kSnap;
        if ((t > 0.0f && t < min_forward) || (t < 0.0f && t > min_reverse) ||
            std::fabs(t) < kSnap) {
            t = 0.0f;
        }
    }

    t = Clamp(t, -1.0f, 1.0f);
    if (GetConfig().debug_automation) {
        // Test aid: log the throttle whenever it moves noticeably.
        static float logged = 0.0f;
        static int logged_keys = 0;
        const int keys = (forward ? 1 : 0) | (backward ? 2 : 0);
        if (std::fabs(t - logged) >= 0.05f || keys != logged_keys || (t == 0.0f && logged != 0.0f)) {
            Log("controls: t=%.3f throttle %+.2f keys %s%s\n", now, t, forward ? "W" : "-",
                backward ? "S" : "-");
            logged = t;
            logged_keys = keys;
        }
    }
    *reinterpret_cast<float*>(self + kThrottle) = t;
    *reinterpret_cast<float*>(self + kStoredThrottle) = t;
    g_last_written = t;
    g_have_last = true;
}

// One binding: a row of the Options > Controls list (3.1c order), a key
// (Windows virtual-key code) and a modifier key (0, VK_SHIFT or VK_CONTROL).
struct Binding {
    int row;
    int vk;
    int modifier;
    const char* what;
};

// Applied in this order, after DEFAULT restored MW4's factory layout (which
// is not WASD: = / - throttle, arrow keys turn, J jump jets, Space centres
// the torso, C crouches). Assigning a key takes it away from whatever had
// it, so every displaced command is given its new key further down the list.
const Binding kMw5Layout[] = {
    {0, 'A', 0, "Turn Left"},
    {1, 'D', 0, "Turn Right"},
    {6, 'W', 0, "Accelerate"},
    {7, 'S', 0, "Decelerate"},
    {9, VK_SPACE, 0, "Fire Jump Jets"},
    {21, 'C', 0, "Center Torso To Legs"},
    {65, 'C', VK_SHIFT, "Crouch"},
    {54, 'O', 0, "Override Auto Shutdown (heat override)"},
    {59, 'O', VK_SHIFT, "Show Mission Objectives"},
    {49, 'Z', 0, "Toggle Zoom Window"},
    {10, 'X', 0, "Set Throttle To 0% (full stop)"},
    {55, 'P', 0, "Shutdown/Startup 'Mech (power)"},
    {24, 'P', VK_CONTROL, "Select Next Weapon"},
    {64, VK_F12, 0, "Screen Snapshot"},
    {52, 'B', 0, "Toggle Radar/Map (battle grid)"},
    {57, 'P', VK_SHIFT, "Startup 'Mech"},
    {67, 'N', 0, "Toggle Light Amplification (night vision)"},
    {60, 'N', VK_SHIFT, "Select Next Nav Point"},
    {43, 'R', 0, "Target Object Under Reticule"},
    {51, 'R', VK_SHIFT, "Toggle Radar Range"},
    {40, 'T', 0, "Target Next Enemy"},
    {42, 'T', VK_SHIFT, "Target Previous Enemy"},
    {44, 'Q', 0, "Target Next Friendly"},
    {27, '1', 0, "Fire Group One"},
    {28, '2', 0, "Fire Group Two"},
    {29, '3', 0, "Fire Group Three"},
    {30, '4', 0, "Fire Group Four"},
    {31, '5', 0, "Fire Group Five"},
    {32, '6', 0, "Fire Group Six"},
    {11, VK_NUMPAD2, 0, "Set Throttle To 20%"},
    {12, VK_NUMPAD3, 0, "Set Throttle To 30%"},
    {13, VK_NUMPAD4, 0, "Set Throttle To 40%"},
    {14, VK_NUMPAD5, 0, "Set Throttle To 50%"},
    {15, VK_NUMPAD6, 0, "Set Throttle To 60%"},
    {16, VK_NUMPAD7, 0, "Set Throttle To 70%"},
    {17, VK_NUMPAD8, 0, "Set Throttle To 80%"},
    {18, VK_NUMPAD9, 0, "Set Throttle To 90%"},
    {19, VK_NUMPAD0, 0, "Set Throttle To 100%"},
    {2, VK_UP, 0, "Pitch Torso Up"},
    {3, VK_DOWN, 0, "Pitch Torso Down"},
    {4, VK_LEFT, 0, "Torso Twist Left"},
    {5, VK_RIGHT, 0, "Torso Twist Right"},
    {47, VK_LEFT, VK_SHIFT, "Look Left"},
    {48, VK_RIGHT, VK_SHIFT, "Look Right"},
    {39, VK_BACK, 0, "Toggle Group Firing Mode (chain fire)"},
    {8, VK_OEM_5, 0, "Toggle Forward/Reverse"},  // backslash
};

bool g_installed = false;

}  // namespace

// Called by the options callback hook after ShellControlSetDefault (the
// Controls screen's DEFAULT button) with the same script context. Assigns the
// MW5-style layout through ShellControlAssignValue (101).
void ApplyMw5Layout(int(__stdcall* callback)(DWORD, DWORD, int*), DWORD a1, DWORD a2) {
    if (!GetConfig().controls_mw5 || !callback) {
        return;
    }
    int applied = 0;
    for (const Binding& b : kMw5Layout) {
        int row = b.row;
        int column = 1;  // keyboard
        int key = b.vk << 8;
        int modifier = b.modifier << 8;
        int args[5] = {101, reinterpret_cast<int>(&row), reinterpret_cast<int>(&column),
                       reinterpret_cast<int>(&key), reinterpret_cast<int>(&modifier)};
        callback(a1, a2, args);
        ++applied;
    }
    // Mouse aims the torso, as in MW5: enable the mouse (SetMouse 213, 1 = on)
    // and put its X axis on the torso (SetMouseXAxis 225; the Controls
    // screen's Mouse X Axis list: 0 None, 1 Turning, 2 Torso). Arguments are
    // pointers.
    int on = 1;
    int torso = 2;
    int mouse_args[2] = {213, reinterpret_cast<int>(&on)};
    callback(a1, a2, mouse_args);
    int axis_args[2] = {225, reinterpret_cast<int>(&torso)};
    callback(a1, a2, axis_args);
    Log("controls: MW5 key layout applied (%d bindings, mouse on, mouse X = torso)\n", applied);
}

bool ThrottleDecayEnabled() { return g_decay != 0; }

void SetThrottleDecay(bool enabled) {
    InterlockedExchange(&g_decay, enabled ? 1 : 0);
    g_decay_armed = false;  // a throttle already set holds
    wchar_t path[MAX_PATH];
    DWORD len = GetModuleFileNameW(nullptr, path, MAX_PATH);
    wchar_t* slash = len ? wcsrchr(path, L'\\') : nullptr;
    if (slash) {
        wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - path), L"mw4hook.ini");
        WritePrivateProfileStringW(L"controls", L"decay", enabled ? L"1" : L"0", path);
    }
    Log("controls: throttle decay %s\n", enabled ? "on" : "off");
}

// Returns true when done so the caller stops retrying. The drive hook is
// installed whenever the hook layer is on, because Throttle Decay can be
// switched on in Options > Game at any time; with decay off and mw5=0 it
// changes nothing.
bool InstallControlsHooks() {
    if (g_installed) {
        return true;
    }
    InterlockedExchange(&g_decay, GetConfig().throttle_decay ? 1 : 0);
    unsigned char* drive = FindUnique(
        "keyboard_drive",
        "51 DD 44 24 08 53 55 56 8B F1 D9 C0 DC A6 E4 00 00 00 8B 86 DC 03 00 00 33 DB 3B C3 57");
    if (!drive) {
        return false;  // not unpacked yet; retry
    }
    g_installed = true;
    // push ecx; fld qword [esp+8] = 5 bytes, position independent.
    InstallDetour(drive, 5, reinterpret_cast<void*>(&Hook_Drive),
                  reinterpret_cast<void**>(&g_drive));
    Log("controls: drive hook %s (mw5 %d, throttle decay %d)\n", g_drive ? "ok" : "FAILED",
        GetConfig().controls_mw5 ? 1 : 0, static_cast<int>(g_decay));
    return true;
}

}  // namespace mw4hook
