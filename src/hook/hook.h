// mw4hook - runtime compatibility layer for MW4 Mercenaries 3.1c
//
// dinput.dll proxy. The game imports dinput.dll statically, so this proxy is
// loaded during startup, before the game parses its command line and creates
// its main window.
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

// Append -window to the process command line before the game parses it.
void InstallCommandLineHooks();

// Keep the game's main window at the configured client size during startup.
void MaintainWindow();

// Patch the game's -window argument check so the normal windowed path is
// selected from startup.
bool ForceWindowedCommandLinePatch();
bool IsWindowedCommandLinePatched();

// Replace an imported function by matching its resolved address.
bool PatchIatByAddress(HMODULE module, const char* dll_name,
                       HMODULE target_module, void* target, void* detour,
                       void** original);

// Runtime configuration (mw4hook.ini next to the game executable).
struct Config {
    bool log_enabled = true;
    bool hooks_enabled = true;
    bool window_enabled = true;
    bool force_windowed = true;
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
