// mw4hook - runtime compatibility layer for MW4 Mercenaries 3.1c
//
// dinput.dll proxy. The game imports dinput.dll statically, so this proxy is
// loaded during startup, before the game parses its command line. The proxy
// makes the launch mode match the Windowed setting (relaunching once if the
// -window switch has to be added or removed) and installs the display, window
// and shell-script hooks.
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace mw4hook {

// Option id answered by the hook for ShellGetOptionValue/ShellSetOptionValue.
// The game's own MWO_* ids are 0..34.
constexpr int kMwoWindowed = 200;
// Option id for Options > Game > Throttle Decay (answered by the hook).
constexpr int kMwoThrottleDecay = 202;
// Option id for Options > Game > Gyro Aim (answered by the hook; issue #17).
constexpr int kMwoGyroAim = 203;
// Option id for Join Regiment > Camouflage (set only; issue #19).
constexpr int kMwoNewPilotCamo = 204;
// Option id for Roster > Update > Camouflage (the selected pilot's default).
constexpr int kMwoPilotCamo = 205;

// Logging
void LogInit(HINSTANCE self);
void Log(const char* fmt, ...);

// Install the Win32 window hooks once the main module has usable imports.
void InstallHooks();
int InstallWindowHooks();

// Relaunch the game when its -window switch does not match the Windowed
// setting. Returns only if no relaunch was needed (or it failed).
void EnsureLaunchMode();

// Replace an imported function by matching its resolved address.
bool PatchIatByAddress(HMODULE module, const char* dll_name,
                       HMODULE target_module, void* target, void* detour,
                       void** original);

// Code patching (detour.cpp).
unsigned char* FindUnique(const char* name, const char* pattern);
bool WriteCode(void* address, const void* data, size_t size);
void* InstallDetour(unsigned char* target, size_t stolen, void* hook, void** trampoline_out);

// Display controller (display.cpp).
void DisplayInit();
bool InstallDisplayHooks();
bool DesiredWindowed();
int ShellMode();
void SetWindowedMode(bool windowed);
void SelectedSize(int* width, int* height);
bool GameFullscreen();

// Options > Video "Windowed" checkbox script injection (shellscript.cpp).
bool InstallShellScriptHook();

// Runtime configuration (mw4hook.ini next to the game executable).
struct Config {
    bool log_enabled = true;
    bool hooks_enabled = true;
    bool window_enabled = true;
    bool relaunch = true;
    bool display_enabled = true;
    bool checkbox_enabled = true;
    bool debug_dump_image = false;
    bool debug_automation = false;
    bool controls_mw5 = false;  // [controls] mw5: MW5-style driving (opt-in)
    bool throttle_decay = false;  // [controls] decay: initial Throttle Decay setting
    bool aim_track_pointer = false;  // [aim] track_pointer: mouse moves an aim point (opt-in)
    float aim_sensitivity = 1.0f;    // [aim] sensitivity: multiplier on the game's slider
    bool aim_marker = true;          // [aim] marker: show where the torso is turning to
    bool aim_gyro = false;           // [aim] gyro: initial Gyro Aim setting (Options > Game)
    bool full_company = false;  // [campaign] full_company: 7 lancemates where 1-6 (#21)
    bool sound_background = true;  // [sound] background: keep playing when unfocused (#26)
    // [mechlab] default_camo: a new pilot's default camouflage code (the first
    // character of MekTek's camouflage name; '_' = None; empty = template's).
    char default_camo = '_';
};

// MW5-style keyboard driving (controls.cpp).
bool InstallControlsHooks();
bool ThrottleDecayEnabled();
void SetThrottleDecay(bool enabled);  // Options > Game; saved to mw4hook.ini
// screen_open: called from Options > Controls (DEFAULT); false elsewhere
// (a new pilot), where the key assignments bypass the screen redraw.
void ApplyMw5Layout(int(__stdcall* callback)(DWORD, DWORD, int*), DWORD a1, DWORD a2,
                    bool screen_open = true);

// Mouse aim point (aim.cpp, issue #15).
bool InstallAimHooks();
void AimAfterDrive(char* controller, double now);
bool GyroAimEnabled();

// Full company in campaign missions (campaign.cpp, issue #21).
bool InstallCampaignHooks();

// Audio keeps playing while the window is unfocused (sound.cpp, issue #26).
bool InstallSoundHooks();

// A new pilot's default camouflage (camo.cpp, issue #19).
// Per-pilot records in mw4hook-pilots.ini (survive restarts): a new pilot's
// pending controls layout and camouflage, and each pilot's chosen camouflage.
bool InstallCamoHooks();
void ChoosePilotCamo(int index);                // the dialog's Camouflage list
void QueueNewPilot(const char* name);            // after ShellCreateNewPilot
void QueueUpdatedPilotCamo(const char* name);    // after ShellRenamePilot (Update)
void RenamePilotRecords(const char* from, const char* to);
bool TakeNewPilotControls(const char* name);     // true once: apply the layout now
void SetCurrentPilot(const char* name);          // ShellSelectPilot
int CurrentPilotCamoIndex();
constexpr int kCamoCount = 52;       // MekTek's MechLab camouflage list
constexpr int kCamoNone = 51;        // "None" (code '_')
extern const char* const kCamoNames[kCamoCount];
int CamoIndex(char code);
void SetGyroAim(bool enabled);  // Options > Game; saved to mw4hook.ini

const Config& GetConfig();

// The game's top-level window once it has been created, else nullptr.
HWND MainWindow();

// Re-apply the main window layout for the Windowed setting / resolution.
void ApplyWindowLayout();

// Development-only test automation (see automation.cpp).
void AutomationInit();
bool AutomationCursor(POINT* client_pos);
bool AutomationSetCursor(POINT client_pos);

// Frontend cursor mapping (cursor.cpp).
void InstallCursorHooks(HMODULE module);
bool ShellToClient(int x, int y, POINT* client_pos);
void AutomationFilterDeviceState(DWORD size, void* data, HRESULT* result);

}  // namespace mw4hook
