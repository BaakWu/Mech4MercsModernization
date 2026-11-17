// Keep the game's audio playing when its window is not in the foreground
// (issue #26).
//
// DirectSound mutes every secondary buffer whose description lacks
// DSBCAPS_GLOBALFOCUS while the window that set the cooperative level is not
// the foreground window. In windowed / borderless play that silences the game
// as soon as another window is clicked. GameOS loads dsound.dll itself
// (LoadLibrary + GetProcAddress "DirectSoundCreate"); the hook loads the same
// system DLL first, detours its DirectSoundCreate export (hot-patch prologue
// mov edi,edi; push ebp; mov ebp,esp) and, on the IDirectSound it returns,
// patches CreateSoundBuffer (vtable slot 3) to add DSBCAPS_GLOBALFOCUS to
// secondary buffers. Duplicated buffers inherit it. Input is unaffected:
// GameOS still ignores the mouse and keys while the window is inactive.
//
// [sound] background=0 turns it off.
#include "hook.h"

#include <mmreg.h>
#include <dsound.h>

namespace mw4hook {
namespace {

using DirectSoundCreate_t = HRESULT(WINAPI*)(LPCGUID, LPDIRECTSOUND*, LPUNKNOWN);
using CreateSoundBuffer_t = HRESULT(WINAPI*)(void*, LPCDSBUFFERDESC, LPDIRECTSOUNDBUFFER*,
                                             LPUNKNOWN);

DirectSoundCreate_t g_create = nullptr;  // trampoline
CreateSoundBuffer_t g_create_buffer = nullptr;
volatile LONG g_buffers = 0;

HRESULT WINAPI Hook_CreateSoundBuffer(void* self, LPCDSBUFFERDESC desc,
                                      LPDIRECTSOUNDBUFFER* buffer, LPUNKNOWN outer) {
    if (desc && desc->dwSize >= sizeof(DSBUFFERDESC1) &&
        !(desc->dwFlags & (DSBCAPS_PRIMARYBUFFER | DSBCAPS_GLOBALFOCUS))) {
        DSBUFFERDESC copy{};
        memcpy(&copy, desc, desc->dwSize < sizeof(copy) ? desc->dwSize : sizeof(copy));
        copy.dwFlags |= DSBCAPS_GLOBALFOCUS;
        const HRESULT result = g_create_buffer(self, &copy, buffer, outer);
        if (SUCCEEDED(result)) {
            if (InterlockedIncrement(&g_buffers) == 1) {
                Log("sound: buffers keep playing in the background (DSBCAPS_GLOBALFOCUS)\n");
            }
            return result;
        }
        // Not expected; fall back to the game's own description.
        Log("sound: CreateSoundBuffer with GLOBALFOCUS failed (%08lX); unchanged\n",
            static_cast<unsigned long>(result));
    }
    return g_create_buffer(self, desc, buffer, outer);
}

void PatchDirectSound(IDirectSound* ds) {
    void** vtable = *reinterpret_cast<void***>(ds);
    if (vtable[3] == reinterpret_cast<void*>(&Hook_CreateSoundBuffer)) {
        return;
    }
    DWORD old = 0;
    if (!VirtualProtect(&vtable[3], sizeof(void*), PAGE_READWRITE, &old)) {
        return;
    }
    if (!g_create_buffer) {
        g_create_buffer = reinterpret_cast<CreateSoundBuffer_t>(vtable[3]);
    }
    vtable[3] = reinterpret_cast<void*>(&Hook_CreateSoundBuffer);
    VirtualProtect(&vtable[3], sizeof(void*), old, &old);
}

HRESULT WINAPI Hook_DirectSoundCreate(LPCGUID device, LPDIRECTSOUND* ds, LPUNKNOWN outer) {
    const HRESULT result = g_create(device, ds, outer);
    if (SUCCEEDED(result) && ds && *ds) {
        PatchDirectSound(*ds);
    }
    return result;
}

bool g_installed = false;

}  // namespace

bool InstallSoundHooks() {
    if (g_installed) {
        return true;
    }
    g_installed = true;
    if (!GetConfig().sound_background) {
        return true;
    }
    wchar_t path[MAX_PATH];
    const UINT len = GetSystemDirectoryW(path, MAX_PATH);
    if (!len || len > MAX_PATH - 16) {
        return true;
    }
    wcscat_s(path, L"\\dsound.dll");
    HMODULE dsound = LoadLibraryW(path);
    auto* target = dsound ? reinterpret_cast<unsigned char*>(
                                GetProcAddress(dsound, "DirectSoundCreate"))
                          : nullptr;
    // mov edi,edi; push ebp; mov ebp,esp: 5 bytes, position independent.
    static const unsigned char kPrologue[5] = {0x8B, 0xFF, 0x55, 0x8B, 0xEC};
    if (!target || memcmp(target, kPrologue, sizeof(kPrologue)) != 0) {
        Log("sound: DirectSoundCreate not hookable; background audio unchanged\n");
        return true;
    }
    InstallDetour(target, 5, reinterpret_cast<void*>(&Hook_DirectSoundCreate),
                  reinterpret_cast<void**>(&g_create));
    Log("sound: DirectSoundCreate hook %s\n", g_create ? "ok" : "FAILED");
    return true;
}

}  // namespace mw4hook
