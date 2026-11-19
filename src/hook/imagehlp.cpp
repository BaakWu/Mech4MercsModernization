// The game's own imagehlp.dll (issue #35; AGENTS.md first-release item 9).
//
// GameOS loads <game>\assets\binaries\imagehlp.dll (2003) by full path with
// LoadLibraryA at startup (3.1c 0x00734ABE) and falls back to imagehlp.dll
// from a second folder (0x00734B24) when that fails. Once that old copy is in
// the process, every later load of "imagehlp.dll" binds to it. NVIDIA's
// display driver needs the system imagehlp.dll: with the old one, its
// DirectDraw/Direct3D hardware path does not come up, the game's device
// check fails with DDERR_NODIRECTDRAWHW and the game shows "Your current
// hardware or driver setup is not compatible" (RTX 3090 and RTX 5080; AMD is
// not affected). Renaming the file fixed it for a player; the patch changes
// no game file, so it refuses that one load instead and GameOS takes its
// fallback, Windows' imagehlp.dll, exactly as after the rename.
//
// The refusal has to be in place before GameOS gets there, and the packed
// executable fills its import table late, so KernelBase's LoadLibraryA is
// detoured from DllMain.
#include "hook.h"

#include <cstring>

namespace mw4hook {
namespace {

using LoadLibraryA_t = HMODULE(WINAPI*)(LPCSTR);
LoadLibraryA_t g_load_library_a = nullptr;  // trampoline

bool EndsWithNoCase(const char* text, const char* suffix) {
    const size_t n = strlen(text);
    const size_t m = strlen(suffix);
    return n >= m && _stricmp(text + n - m, suffix) == 0;
}

HMODULE WINAPI Hook_LoadLibraryA(LPCSTR name) {
    if (name && !IsBadStringPtrA(name, MAX_PATH) &&
        EndsWithNoCase(name, "\\assets\\binaries\\imagehlp.dll")) {
        Log("imagehlp: not loading the game's own imagehlp.dll; Windows' is used instead\n");
        SetLastError(ERROR_MOD_NOT_FOUND);
        return nullptr;
    }
    return g_load_library_a(name);
}

// kernel32!LoadLibraryA is a jmp [imp] stub into KernelBase on current
// Windows; follow it.
unsigned char* RealLoadLibraryA() {
    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    auto* p = kernel32 ? reinterpret_cast<unsigned char*>(GetProcAddress(kernel32, "LoadLibraryA"))
                       : nullptr;
    for (int hops = 0; p && hops < 3; ++hops) {
        if (p[0] == 0xFF && p[1] == 0x25) {  // jmp dword ptr [abs]
            p = **reinterpret_cast<unsigned char***>(p + 2);
        } else if (p[0] == 0x8B && p[1] == 0xFF && p[2] == 0xEB) {  // mov edi,edi; jmp short
            p = p + 4 + static_cast<signed char>(p[3]);
        } else if (p[0] == 0xE9) {  // jmp rel32
            p = p + 5 + *reinterpret_cast<int*>(p + 1);
        } else {
            break;
        }
    }
    return p;
}

}  // namespace

void InstallImagehlpRedirect() {
    unsigned char* target = RealLoadLibraryA();
    // mov edi,edi; push ebp; mov ebp,esp: 5 position-independent bytes.
    static const unsigned char kPrologue[] = {0x8B, 0xFF, 0x55, 0x8B, 0xEC};
    if (!target || memcmp(target, kPrologue, sizeof(kPrologue)) != 0) {
        Log("imagehlp: LoadLibraryA has an unexpected start (%p); the game's own imagehlp.dll "
            "may be used\n",
            target);
        return;
    }
    InstallDetour(target, sizeof(kPrologue), reinterpret_cast<void*>(&Hook_LoadLibraryA),
                  reinterpret_cast<void**>(&g_load_library_a));
    Log("imagehlp: %s\n", g_load_library_a ? "the game's own imagehlp.dll will not be loaded"
                                            : "detour FAILED");
}

}  // namespace mw4hook
