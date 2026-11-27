// mw4hook - dinput.dll proxy entry point.
//
// The game imports dinput.dll statically, so this module is loaded during
// startup. If the -window switch does not match the Windowed setting in
// options.ini, the proxy relaunches the game once with the switch added or
// removed; the (re)launched process then installs the display, window and
// shell-script hooks.
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

// Development aid: write the unpacked in-memory image of MW4Mercs.exe to
// mw4hook-image-<pid>.bin (file offset == RVA). The 3.1c executable is
// PECompact packed and AntiHack denies external PROCESS_VM_READ, so this is
// the reliable way to get a static-analysis copy. Off unless
// [debug] dump_image=1.
void DumpImage() {
    auto* base = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    const DWORD size = nt->OptionalHeader.SizeOfImage;

    wchar_t path[MAX_PATH];
    DWORD len = GetModuleFileNameW(nullptr, path, MAX_PATH);
    wchar_t* slash = len ? wcsrchr(path, L'\\') : nullptr;
    if (!slash) {
        return;
    }
    wsprintfW(slash + 1, L"mw4hook-image-%lu.bin", GetCurrentProcessId());
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    static unsigned char zero[0x1000];
    for (DWORD off = 0; off < size; off += 0x1000) {
        MEMORY_BASIC_INFORMATION info{};
        const unsigned char* page = zero;
        if (VirtualQuery(base + off, &info, sizeof(info)) == sizeof(info) &&
            info.State == MEM_COMMIT &&
            !(info.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
            page = base + off;
        }
        DWORD written = 0;
        WriteFile(file, page, 0x1000, &written, nullptr);
    }
    CloseHandle(file);
    mw4hook::Log("debug: dumped %lu bytes of image to mw4hook-image\n", size);
}

DWORD WINAPI HookThread(LPVOID) {
    const mw4hook::Config& config = mw4hook::GetConfig();
    bool display_done = !config.hooks_enabled || !config.display_enabled;
    bool script_done = !config.hooks_enabled || !config.checkbox_enabled;
    bool controls_done = !config.hooks_enabled;
    bool aim_done = !config.hooks_enabled;
    bool camo_done = !config.hooks_enabled;
    bool campaign_done = !config.hooks_enabled;
    bool gameos_done = !config.hooks_enabled;
    bool abl_done = !config.hooks_enabled;
    // Before GameOS loads dsound.dll and creates its buffers.
    if (config.hooks_enabled) {
        mw4hook::InstallSoundHooks();
    }
    // The packed image is unpacked by the time imports are resolved, but
    // retry briefly in case a pattern is not readable yet.
    for (int i = 0; i < 300; ++i) {
        if (!display_done) {
            display_done = mw4hook::InstallDisplayHooks();
        }
        if (!script_done) {
            script_done = mw4hook::InstallShellScriptHook();
        }
        if (!controls_done) {
            controls_done = mw4hook::InstallControlsHooks();
        }
        if (!aim_done) {
            aim_done = mw4hook::InstallAimHooks();
        }
        if (!camo_done) {
            camo_done = mw4hook::InstallCamoHooks();
        }
        if (!campaign_done) {
            campaign_done = mw4hook::InstallCampaignHooks();
        }
        if (!gameos_done) {
            gameos_done = mw4hook::InstallGameOsHooks();
        }
        if (!abl_done) {
            abl_done = mw4hook::InstallAblCrashReport();
        }
        mw4hook::InstallHooks();
        Sleep(10);
    }
    if (mw4hook::GetConfig().debug_dump_image) {
        DumpImage();
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
        // The input probe (input_probe.cpp) runs this DLL in rundll32: only
        // the real DirectInput is needed there; no hooks, log or relaunch.
        wchar_t host[MAX_PATH];
        const DWORD len = GetModuleFileNameW(nullptr, host, MAX_PATH);
        const wchar_t* name = len ? wcsrchr(host, L'\\') : nullptr;
        if (name && _wcsicmp(name + 1, L"rundll32.exe") == 0) {
            mw4hook::ResolveDinputExports();
            return TRUE;
        }
        mw4hook::LogInit(instance);
        mw4hook::Log("dllmain: process attach\n");
        mw4hook::InstallImagehlpRedirect();
        EnableDpiAwareness();
        mw4hook::ResolveDinputExports();
        mw4hook::DisplayInit();
        mw4hook::EnsureLaunchMode();
        mw4hook::AutomationInit();
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
