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
constexpr int kGamepadDeadzone = 250;      // stick, -1000..1000 (#42)
constexpr size_t kYawCmd = 0x2E4;          // torso twist rate command
constexpr size_t kPitchCmd = 0x2E0;        // torso pitch rate command

using Drive_t = void(__thiscall*)(void*, double);
Drive_t g_drive = nullptr;
// options.ini [special commands] autotorsocenter as the drive step reads it
// (0 off, 1 / 2 the automatic centering modes; 2 = after a delay). Read once
// per frame at the start of the step's centering block (3.1c 0x0054FCB5).
int* g_auto_center = nullptr;

constexpr size_t kMinForward = 0x64C;      // float, smallest forward throttle (>0)

double g_last_now = 0.0;
float g_last_written = 0.0f;
bool g_have_last = false;
volatile LONG g_decay = 0;      // Options > Game > Throttle Decay
bool g_decay_armed = false;     // last throttle change came from W/S

float Clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

void __fastcall Hook_Drive(char* self, void*, double now) {
    const float before = *reinterpret_cast<float*>(self + kThrottle);
    // Mouse aim point (issue #31): aiming must not turn the 'Mech. The
    // automatic centering turns the legs toward the torso, so the step sees
    // it off while aim mode is in use; the explicit Center commands
    // (controller +0x6D0) do not depend on it and keep working.
    int* const auto_center = g_auto_center && AimSuppressesAutoCenter(self) ? g_auto_center : nullptr;
    const int auto_center_mode = auto_center ? *auto_center : 0;
    if (auto_center) {
        *auto_center = 0;
    }
    // Gamepad (issue #42): the left stick's Y is the throttle. It sets the
    // game's own Accelerate/Decelerate held counters, so the throttle ramps
    // and holds like W/S (and Throttle Decay applies); the right stick is
    // remapped to pitch/torso in the dinput proxy. Centered leaves the
    // keyboard counters alone.
    if (GetConfig().input_gamepad_layout) {
        const int stick = GamepadThrottle();
        // DirectInput's Y axis is positive towards the player (the stock
        // game's Invert Y Axis); pushing the stick forward is negative.
        if (stick < -kGamepadDeadzone) {
            *reinterpret_cast<int*>(self + kForwardHeld) = 1;
            *reinterpret_cast<int*>(self + kBackwardHeld) = 0;
        } else if (stick > kGamepadDeadzone) {
            *reinterpret_cast<int*>(self + kBackwardHeld) = 1;
            *reinterpret_cast<int*>(self + kForwardHeld) = 0;
        }
    }
    g_drive(self, now);
    if (auto_center) {
        *auto_center = auto_center_mode;
    }

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

    // Gamepad torso (issue #42): drive the rate commands directly from the
    // right stick. The stock axis mapping is off for a gamepad (Z/Rx also
    // drive the throttle), so this is what makes right stick X twist and Y
    // pitch. The aim point follows through AimAfterDrive below.
    if (GetConfig().input_gamepad_layout) {
        const int right_x = GamepadRightX();
        const int right_y = GamepadRightY();
        if (right_x > kGamepadDeadzone || right_x < -kGamepadDeadzone) {
            *reinterpret_cast<float*>(self + kYawCmd) =
                -static_cast<float>(right_x) / 1000.0f;
        }
        if (right_y > kGamepadDeadzone || right_y < -kGamepadDeadzone) {
            // Right stick up looks up, down looks down (the stick's raw Y is
            // positive up on this pad; the yaw above stays as it is).
            *reinterpret_cast<float*>(self + kPitchCmd) =
                static_cast<float>(right_y) / 1000.0f;
        }
    }

    // Mouse aim point (aim.cpp): after the game's torso keys and centering.
    AimAfterDrive(self, now);
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

// The core of ShellControlAssignValue for a keyboard key, called directly.
// The script call also mails the Options > Controls screen to redraw the
// row; with that screen closed (a new pilot, in the Roster) the game tries
// to load it in the middle of the callback and hangs. Everything is taken
// from the handler's own instructions (3.1c 0x0051AFC8, "sar edx,8; cmp
// edx,1Bh; jz"):
//   +0x4A  mov eax,[ptr]   +0x4F mov esi,[key]   player = [[ptr]^[key]+2Ch]^K
//   +0x5F  xor ecx,K       +0x66 mov ecx,[controls]   +0x6C call core
// core(thiscall controls; player, row, vk, modifier flags).
using AssignCore_t = int(__thiscall*)(void*, int, int, int, int);
struct AssignSite {
    bool searched = false;
    AssignCore_t core = nullptr;
    void** controls = nullptr;
    DWORD** player_ptr = nullptr;
    DWORD* player_key = nullptr;
    DWORD player_offset = 0;
    DWORD player_xor = 0;
} g_assign;

bool FindAssignSite() {
    if (g_assign.searched) {
        return g_assign.core != nullptr;
    }
    g_assign.searched = true;
    unsigned char* p = FindUnique("assign_key", "C1 FA 08 83 FA 1B 0F 84");
    if (!p || p[0x4A] != 0xA1 || p[0x4F] != 0x8B || p[0x50] != 0x35 || p[0x5C] != 0x8B ||
        p[0x5D] != 0x48 || p[0x5F] != 0x81 || p[0x60] != 0xF1 || p[0x66] != 0x8B ||
        p[0x67] != 0x0D || p[0x6C] != 0xE8) {
        Log("controls: key assignment site not recognised\n");
        return false;
    }
    g_assign.player_ptr = *reinterpret_cast<DWORD***>(p + 0x4B);
    g_assign.player_key = *reinterpret_cast<DWORD**>(p + 0x51);
    g_assign.player_offset = p[0x5E];
    g_assign.player_xor = *reinterpret_cast<DWORD*>(p + 0x61);
    g_assign.controls = *reinterpret_cast<void***>(p + 0x68);
    g_assign.core = reinterpret_cast<AssignCore_t>(p + 0x71 + *reinterpret_cast<LONG*>(p + 0x6D));
    return true;
}

// Modifier key -> the game's flag (as the handler maps it).
int ModifierFlag(int vk) {
    switch (vk) {
        case VK_SHIFT: return 2;
        case VK_CONTROL: return 4;
        case VK_MENU: return 0x10;
        default: return 1;
    }
}

bool AssignKeyDirect(int row, int vk, int modifier) {
    if (!FindAssignSite()) {
        return false;
    }
    __try {
        const DWORD object = **g_assign.player_ptr ^ *g_assign.player_key;
        const int player = static_cast<int>(
            *reinterpret_cast<DWORD*>(object + g_assign.player_offset) ^ g_assign.player_xor);
        g_assign.core(*g_assign.controls, player, row, vk, ModifierFlag(modifier));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

}  // namespace

// Called by the options callback hook after ShellControlSetDefault (the
// Controls screen's DEFAULT button) with the same script context. Assigns the
// MW5-style layout through ShellControlAssignValue (101).
void ApplyMw5Layout(int(__stdcall* callback)(DWORD, DWORD, int*), DWORD a1, DWORD a2,
                    bool screen_open) {
    if (!GetConfig().controls_mw5 || !callback) {
        return;
    }
    int applied = 0;
    for (const Binding& b : kMw5Layout) {
        if (!screen_open) {
            // Outside Options > Controls: the game's assignment without the
            // screen redraw (see AssignKeyDirect).
            if (!AssignKeyDirect(b.row, b.vk, b.modifier)) {
                Log("controls: MW5 layout not applied (assignment failed at row %d)\n", b.row);
                return;
            }
            ++applied;
            continue;
        }
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
    // The centering block of the drive step: mov eax, [autotorsocenter];
    // cmp eax, 1; je ...; mov edx, [esi+6D0h]; cmp edx, 1.
    if (unsigned char* center = FindUnique(
            "auto_torso_center",
            "A1 ?? ?? ?? ?? 83 F8 01 0F 84 ?? ?? ?? ?? 8B 96 D0 06 00 00 83 FA 01")) {
        if (center > drive && center < drive + 0x1000) {
            g_auto_center = *reinterpret_cast<int**>(center + 1);
        }
    }
    // push ecx; fld qword [esp+8] = 5 bytes, position independent.
    InstallDetour(drive, 5, reinterpret_cast<void*>(&Hook_Drive),
                  reinterpret_cast<void**>(&g_drive));
    Log("controls: drive hook %s (mw5 %d, throttle decay %d, auto-center flag %s)\n",
        g_drive ? "ok" : "FAILED", GetConfig().controls_mw5 ? 1 : 0, static_cast<int>(g_decay),
        g_auto_center ? "found" : "not found");
    return true;
}

}  // namespace mw4hook
