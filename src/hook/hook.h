// mw4hook - runtime compatibility layer for MW4 Mercenaries 3.1c
//
// dinput.dll proxy. The game imports dinput.dll statically, so this proxy is
// loaded during startup. If the process was not started with -window, it
// relaunches itself with the switch; the relaunched process handles the
// normal window sizing hooks.
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace mw4hook {

// Logging
void LogInit(HINSTANCE self);
void Log(const char* fmt, ...);

// Install the Win32 window hooks once the main module has usable imports.
void InstallHooks();
int InstallWindowHooks();

// Relaunch the game with -window when the command line lacks it.
void RelaunchWindowed();

// Replace an imported function by matching its resolved address.
bool PatchIatByAddress(HMODULE module, const char* dll_name,
                       HMODULE target_module, void* target, void* detour,
                       void** original);

// Runtime configuration (mw4hook.ini next to the game executable).
struct Config {
    bool log_enabled = true;
    bool hooks_enabled = true;
    bool window_enabled = true;
    bool relaunch = true;
    bool resolution_enabled = true;
    int window_width = 1024;
    int window_height = 768;
    int resolution_width = 1024;
    int resolution_height = 768;
};

const Config& GetConfig();

// Rewrite the game's built-in 800x600 display defaults to the configured
// resolution. This affects the game's internal render size, not just the
// Win32 window.
void ApplyResolutionDefaults();

}  // namespace mw4hook
