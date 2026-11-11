// mw4hook - version.dll replacement for MW4 Mercenaries 3.1c.
//
// The game loads ddraw.dll from its application directory. This module acts as
// a ddraw proxy and installs the window/display compatibility hooks.
#include "hook.h"

namespace {

DWORD WINAPI HookThread(LPVOID) {
    // The 3.1c executable is PECompact-packed. The import tables and display
    // code become usable shortly after the process starts, so poll during the
    // first seconds and then settle into a slow heartbeat.
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

void ResolveDdrawExports();

}  // namespace mw4hook

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        mw4hook::LogInit(instance);
        mw4hook::Log("dllmain: process attach\n");
        mw4hook::ResolveDdrawExports();
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
