// mw4hook - runtime compatibility layer for MW4 Mercenaries 3.1c
//
// ddraw.dll proxy. The game loads ddraw.dll from its application directory, so
// placing this file next to MW4Mercs.exe loads our code before the game
// creates its main window.
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace mw4hook {

// Logging
void LogInit(HINSTANCE self);
void Log(const char* fmt, ...);

// Install the Win32 hooks once the main module has usable import tables.
void InstallHooks();

// Install the window/display hooks (internal).
int InstallWindowHooks();

// Replace an imported function by matching its resolved address.
bool PatchIatByAddress(HMODULE module, const char* dll_name,
                       HMODULE target_module, void* target, void* detour,
                       void** original);

// Runtime configuration (mw4hook.ini next to the game executable).
struct Config {
    bool log_enabled = true;
    bool hooks_enabled = true;
    bool window_enabled = true;
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
