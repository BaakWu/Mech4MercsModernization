// mw4hook - dinput.dll proxy entry point.
//
// The game imports dinput.dll statically, so this module is loaded during
// startup. If the process lacks -window, the proxy relaunches it with the
// switch, then the relaunched process installs the window sizing hooks.
#include "hook.h"

namespace {

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
