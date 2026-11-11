// mw4hook - dinput.dll proxy entry point.
//
// The game imports dinput.dll statically, so this module is loaded during
// startup. If the process lacks -window, the proxy relaunches it with the
// switch, then the relaunched process installs the window sizing hooks.
#include "hook.h"

namespace {

// Opt out of DPI virtualization before the game creates its window.
//
// On a display scaled above 100%, an unaware process gets a 1024x768 logical
// client area that is really 1280x960 physical pixels at 125%. DirectDraw then
// draws a 1024x768 surface into the top-left of the larger window, leaving
// black bands on the right and bottom, and the virtualized mouse coordinates
// no longer match the game's render surface. System DPI awareness makes one
// game pixel one physical pixel on the primary monitor.
void EnableDpiAwareness() {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32) {
        return;
    }

    // DPI_AWARENESS_CONTEXT_SYSTEM_AWARE, defined locally so the hook does not
    // depend on a recent SDK.
    using SetProcessDpiAwarenessContext_t = BOOL(WINAPI*)(HANDLE);
    auto set_context = reinterpret_cast<SetProcessDpiAwarenessContext_t>(
        GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
    if (set_context) {
        HANDLE system_aware = reinterpret_cast<HANDLE>(-2);
        if (set_context(system_aware)) {
            mw4hook::Log("dpi: system aware\n");
            return;
        }
        mw4hook::Log("dpi: SetProcessDpiAwarenessContext failed (%lu)\n",
                     GetLastError());
    }

    using SetProcessDPIAware_t = BOOL(WINAPI*)();
    auto set_aware = reinterpret_cast<SetProcessDPIAware_t>(
        GetProcAddress(user32, "SetProcessDPIAware"));
    if (set_aware && set_aware()) {
        mw4hook::Log("dpi: SetProcessDPIAware\n");
        return;
    }
    mw4hook::Log("dpi: awareness not set (%lu)\n", GetLastError());
}

DWORD WINAPI HookThread(LPVOID) {
    Sleep(100);
    for (int i = 0; i < 300; ++i) {
        mw4hook::ApplyResolutionDefaults();
        mw4hook::InstallHooks();
        Sleep(10);
    }
    for (int i = 0; i < 240; ++i) {
        mw4hook::InstallHooks();
        Sleep(250);
    }
    return 0;
}

}  // namespace

namespace mw4hook {

void InstallHooks() {
    if (!GetConfig().hooks_enabled) {
        return;
    }
    static int reported = -1;
    int count = InstallWindowHooks();
    if (count != reported) {
        reported = count;
        Log("hooks: %d user32 import(s) patched\n", count);
    }
}

void ResolveDinputExports();

}  // namespace mw4hook

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        mw4hook::LogInit(instance);
        mw4hook::Log("dllmain: process attach\n");
        EnableDpiAwareness();
        mw4hook::ResolveDinputExports();
        mw4hook::RelaunchWindowed();
        mw4hook::Log("dllmain: attach complete\n");
        HANDLE thread = CreateThread(nullptr, 0, HookThread, nullptr, 0, nullptr);
        if (thread) {
            CloseHandle(thread);
        } else {
            mw4hook::Log("dllmain: CreateThread failed\n");
        }
    }
    return TRUE;
}
