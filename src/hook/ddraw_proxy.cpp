// ddraw.dll proxy: scale the fixed 800x600 frontend into a larger window.
//
// MW4 Mercenaries renders the in-mission scene at the configured resolution,
// but the 2002 shell (main menu, MechLab, roster, ...) is laid out for
// 800x600 and draws itself into the top-left of the render surface. The rest
// of the surface stays black, so a 1024x768 window shows an 800x600 picture
// with black bands on the right and bottom.
//
// The shell presents through IDirectDrawSurface7::Blt. This proxy patches the
// real DirectDraw surface vtable and, when the source is a 1024x768 surface
// whose 800x600 content is surrounded by the shell's black border, replaces
// the 1:1 Blt with a GDI StretchBlt of the 800x600 area. In-mission frames
// fill the surface, fail the border check, and pass through unchanged.
//
// This is a runtime hook only. The game executable on disk is not modified.
#include <windows.h>

#include <ddraw.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace {

HMODULE g_real_ddraw = nullptr;
HANDLE g_log = INVALID_HANDLE_VALUE;
bool g_stretch_enabled = true;

void Log(const char* fmt, ...) {
    if (g_log == INVALID_HANDLE_VALUE) {
        return;
    }
    char buffer[1024];
    va_list args;
    va_start(args, fmt);
    int n = _vsnprintf_s(buffer, sizeof(buffer), _TRUNCATE, fmt, args);
    va_end(args);
    if (n <= 0) {
        return;
    }
    DWORD written = 0;
    WriteFile(g_log, buffer, static_cast<DWORD>(n), &written, nullptr);
}

void LoadConfig() {
    wchar_t path[MAX_PATH];
    DWORD len = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        return;
    }
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash) {
        return;
    }
    slash[1] = L'\0';
    wcscat_s(path, MAX_PATH, L"mw4hook.ini");
    g_stretch_enabled =
        GetPrivateProfileIntW(L"frontend", L"stretch", 1, path) != 0;
    if (GetPrivateProfileIntW(L"log", L"enabled", 1, path) == 0) {
        return;
    }

    len = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        return;
    }
    slash = wcsrchr(path, L'\\');
    if (!slash) {
        return;
    }
    slash[1] = L'\0';
    wchar_t name[64];
    wsprintfW(name, L"mw4hook-ddraw-%lu.log", GetCurrentProcessId());
    wcscat_s(path, name);
    g_log = CreateFileW(path, FILE_APPEND_DATA,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

FARPROC Resolve(const char* name) {
    if (!g_real_ddraw) {
        wchar_t path[MAX_PATH];
        UINT len = GetSystemDirectoryW(path, MAX_PATH);
        if (len == 0 || len + 16 >= MAX_PATH) {
            return nullptr;
        }
        wcscat_s(path, L"\\ddraw.dll");
        g_real_ddraw = LoadLibraryW(path);
        Log("real ddraw: %p\n", g_real_ddraw);
    }
    if (!g_real_ddraw) {
        return nullptr;
    }
    return GetProcAddress(g_real_ddraw, name);
}

using Blt_t = HRESULT(WINAPI*)(IDirectDrawSurface7*, LPRECT,
                               IDirectDrawSurface7*, LPRECT, DWORD,
                               LPDDBLTFX);
using CreateSurface_t = HRESULT(WINAPI*)(IDirectDraw7*, DDSURFACEDESC2*,
                                         IDirectDrawSurface7**, IUnknown*);
using GetGDISurface_t = HRESULT(WINAPI*)(IDirectDraw7*,
                                         IDirectDrawSurface7**);
using DirectDrawCreate_t = HRESULT(WINAPI*)(GUID*, IDirectDraw**, IUnknown*);
using DirectDrawCreateEx_t = HRESULT(WINAPI*)(GUID*, void**, REFIID, IUnknown*);
using QueryInterface_t = HRESULT(WINAPI*)(void*, REFIID, void**);
using GetAttachedSurface_t = HRESULT(WINAPI*)(IDirectDrawSurface7*, DDSCAPS2*,
                                              IDirectDrawSurface7**);

Blt_t g_original_blt = nullptr;
CreateSurface_t g_original_create_surface = nullptr;
GetGDISurface_t g_original_get_gdi_surface = nullptr;
QueryInterface_t g_original_surface_qi = nullptr;
GetAttachedSurface_t g_original_get_attached = nullptr;

void* g_original_dd_vtable[30] = {};
void** g_surface_vtables[8] = {};
int g_surface_vtable_count = 0;

HRESULT WINAPI Hook_Blt(IDirectDrawSurface7* self, LPRECT dest_rect,
                        IDirectDrawSurface7* src, LPRECT src_rect, DWORD flags,
                        LPDDBLTFX fx);
HRESULT WINAPI Hook_SurfaceQueryInterface(void* self, REFIID iid, void** out);
HRESULT WINAPI Hook_GetAttachedSurface(IDirectDrawSurface7* self,
                                       DDSCAPS2* caps,
                                       IDirectDrawSurface7** out);

// The shell draws its 800x600 UI into the top-left of the 1024x768 render
// surface. A mission fills the surface. The shell's border can contain cursor
// trails and other sparse marks, so treat the frame as frontend content when
// the area outside 800x600 is mostly black rather than exactly black. A false
// negative only leaves the previous 1:1 behaviour; a false positive would
// crop a mission frame.
bool HasBlackFrontendBorder(HDC dc) {
    int sampled = 0;
    int lit = 0;
    auto sample = [&](int x, int y) {
        COLORREF color = GetPixel(dc, x, y);
        if (color == CLR_INVALID) {
            return false;
        }
        ++sampled;
        if ((GetRValue(color) | GetGValue(color) | GetBValue(color)) > 24) {
            ++lit;
        }
        return true;
    };
    for (int y = 10; y < 600; y += 24) {
        for (int x = 810; x < 1024; x += 32) {
            if (!sample(x, y)) {
                return false;
            }
        }
    }
    for (int y = 610; y < 768; y += 24) {
        for (int x = 10; x < 1024; x += 32) {
            if (!sample(x, y)) {
                return false;
            }
        }
    }
    // A mission fills the border; the shell leaves it black apart from a few
    // cursor marks.
    return sampled > 0 && lit * 4 < sampled;
}

void PatchSurfaceVtable(IDirectDrawSurface7* surface) {
    if (!surface) {
        return;
    }
    auto* vtable = *reinterpret_cast<void***>(surface);
    if (!vtable) {
        return;
    }
    for (int i = 0; i < g_surface_vtable_count; ++i) {
        if (g_surface_vtables[i] == vtable) {
            return;
        }
    }
    if (g_surface_vtable_count >= 8) {
        return;
    }

    void* original[44] = {};
    memcpy(original, vtable, sizeof(original));
    if (!g_original_blt) {
        g_original_blt = reinterpret_cast<Blt_t>(original[5]);
    }
    if (!g_original_surface_qi) {
        g_original_surface_qi = reinterpret_cast<QueryInterface_t>(original[0]);
    }
    if (!g_original_get_attached) {
        g_original_get_attached =
            reinterpret_cast<GetAttachedSurface_t>(original[12]);
    }

    void* patched[44] = {};
    memcpy(patched, original, sizeof(patched));
    patched[0] = reinterpret_cast<void*>(&Hook_SurfaceQueryInterface);
    patched[5] = reinterpret_cast<void*>(&Hook_Blt);
    patched[12] = reinterpret_cast<void*>(&Hook_GetAttachedSurface);

    DWORD old_protect = 0;
    if (!VirtualProtect(vtable, sizeof(patched), PAGE_READWRITE, &old_protect)) {
        return;
    }
    memcpy(vtable, patched, sizeof(patched));
    VirtualProtect(vtable, sizeof(patched), old_protect, &old_protect);
    g_surface_vtables[g_surface_vtable_count++] = vtable;
}

HRESULT WINAPI Hook_SurfaceQueryInterface(void* self, REFIID iid, void** out) {
    HRESULT result = g_original_surface_qi(self, iid, out);
    if (SUCCEEDED(result) && out && *out) {
        PatchSurfaceVtable(reinterpret_cast<IDirectDrawSurface7*>(*out));
    }
    return result;
}

HRESULT WINAPI Hook_GetAttachedSurface(IDirectDrawSurface7* self,
                                       DDSCAPS2* caps,
                                       IDirectDrawSurface7** out) {
    HRESULT result = g_original_get_attached(self, caps, out);
    if (SUCCEEDED(result) && out && *out) {
        PatchSurfaceVtable(*out);
    }
    return result;
}

HRESULT WINAPI Hook_Blt(IDirectDrawSurface7* self, LPRECT dest_rect,
                        IDirectDrawSurface7* src, LPRECT src_rect, DWORD flags,
                        LPDDBLTFX fx) {
    if (!g_stretch_enabled || !src || !dest_rect) {
        return g_original_blt(self, dest_rect, src, src_rect, flags, fx);
    }

    DDSURFACEDESC2 desc{};
    desc.dwSize = sizeof(desc);
    if (FAILED(src->GetSurfaceDesc(&desc))) {
        return g_original_blt(self, dest_rect, src, src_rect, flags, fx);
    }

    const int dest_w = dest_rect->right - dest_rect->left;
    const int dest_h = dest_rect->bottom - dest_rect->top;
    int content_w = 0;
    int content_h = 0;
    if (desc.dwWidth == 800 && desc.dwHeight == 600) {
        content_w = 800;
        content_h = 600;
    } else if (desc.dwWidth == 1024 && desc.dwHeight == 768) {
        content_w = 800;
        content_h = 600;
    }

    if (!content_w || dest_w <= content_w || dest_h <= content_h) {
        return g_original_blt(self, dest_rect, src, src_rect, flags, fx);
    }

    HDC src_dc = nullptr;
    HDC dest_dc = nullptr;
    HRESULT src_result = src->GetDC(&src_dc);
    if (FAILED(src_result)) {
        return g_original_blt(self, dest_rect, src, src_rect, flags, fx);
    }

    const bool is_frontend =
        desc.dwWidth != 1024 || HasBlackFrontendBorder(src_dc);
    if (!is_frontend) {
        src->ReleaseDC(src_dc);
        return g_original_blt(self, dest_rect, src, src_rect, flags, fx);
    }

    HRESULT dest_result = self->GetDC(&dest_dc);
    if (FAILED(dest_result)) {
        src->ReleaseDC(src_dc);
        Log("frontend stretch: GetDC(dest) failed 0x%08lX\n", dest_result);
        return g_original_blt(self, dest_rect, src, src_rect, flags, fx);
    }

    BOOL ok = StretchBlt(dest_dc, dest_rect->left, dest_rect->top, dest_w,
                         dest_h, src_dc, 0, 0, content_w, content_h, SRCCOPY);
    self->ReleaseDC(dest_dc);
    src->ReleaseDC(src_dc);
    if (!ok) {
        Log("frontend stretch: StretchBlt failed (%lu)\n", GetLastError());
        return g_original_blt(self, dest_rect, src, src_rect, flags, fx);
    }

    static bool reported = false;
    if (!reported) {
        reported = true;
        Log("frontend stretch: %dx%d -> %dx%d\n", content_w, content_h,
            dest_w, dest_h);
    }
    return DD_OK;
}

HRESULT WINAPI Hook_CreateSurface(IDirectDraw7* self, DDSURFACEDESC2* desc,
                                  IDirectDrawSurface7** surface,
                                  IUnknown* outer) {
    HRESULT result = g_original_create_surface(self, desc, surface, outer);
    if (SUCCEEDED(result) && surface && *surface) {
        PatchSurfaceVtable(*surface);
    }
    return result;
}

HRESULT WINAPI Hook_GetGDISurface(IDirectDraw7* self,
                                  IDirectDrawSurface7** surface) {
    HRESULT result = g_original_get_gdi_surface(self, surface);
    if (SUCCEEDED(result) && surface && *surface) {
        PatchSurfaceVtable(*surface);
    }
    return result;
}

void PatchDirectDrawVtable(IDirectDraw7* ddraw) {
    if (!ddraw || g_original_dd_vtable[6]) {
        return;
    }
    auto* vtable = *reinterpret_cast<void***>(ddraw);
    if (!vtable) {
        return;
    }
    memcpy(g_original_dd_vtable, vtable, sizeof(g_original_dd_vtable));
    g_original_create_surface =
        reinterpret_cast<CreateSurface_t>(g_original_dd_vtable[6]);
    g_original_get_gdi_surface =
        reinterpret_cast<GetGDISurface_t>(g_original_dd_vtable[14]);

    void* patched[30] = {};
    memcpy(patched, g_original_dd_vtable, sizeof(patched));
    patched[6] = reinterpret_cast<void*>(&Hook_CreateSurface);
    patched[14] = reinterpret_cast<void*>(&Hook_GetGDISurface);

    DWORD old_protect = 0;
    if (!VirtualProtect(vtable, sizeof(patched), PAGE_READWRITE, &old_protect)) {
        return;
    }
    memcpy(vtable, patched, sizeof(patched));
    VirtualProtect(vtable, sizeof(patched), old_protect, &old_protect);
}

}  // namespace

extern "C" {
void* g_pfn_AcquireDDThreadLock = nullptr;
void* g_pfn_CompleteCreateSysmemSurface = nullptr;
void* g_pfn_D3DParseUnknownCommand = nullptr;
void* g_pfn_DDGetAttachedSurfaceLcl = nullptr;
void* g_pfn_DDInternalLock = nullptr;
void* g_pfn_DDInternalUnlock = nullptr;
void* g_pfn_DSoundHelp = nullptr;
void* g_pfn_DirectDrawCreateClipper = nullptr;
void* g_pfn_DirectDrawEnumerateA = nullptr;
void* g_pfn_DirectDrawEnumerateExA = nullptr;
void* g_pfn_DirectDrawEnumerateExW = nullptr;
void* g_pfn_DirectDrawEnumerateW = nullptr;
void* g_pfn_DllCanUnloadNow = nullptr;
void* g_pfn_DllGetClassObject = nullptr;
void* g_pfn_GetDDSurfaceLocal = nullptr;
void* g_pfn_GetOLEThunkData = nullptr;
void* g_pfn_GetSurfaceFromDC = nullptr;
void* g_pfn_RegisterSpecialCase = nullptr;
void* g_pfn_ReleaseDDThreadLock = nullptr;
void* g_pfn_SetAppCompatData = nullptr;

#define MW4_DDRAW_THUNK(name)                 \
    __declspec(naked) void Proxy_##name() {   \
        __asm { jmp dword ptr[g_pfn_##name] } \
    }

MW4_DDRAW_THUNK(AcquireDDThreadLock)
MW4_DDRAW_THUNK(CompleteCreateSysmemSurface)
MW4_DDRAW_THUNK(D3DParseUnknownCommand)
MW4_DDRAW_THUNK(DDGetAttachedSurfaceLcl)
MW4_DDRAW_THUNK(DDInternalLock)
MW4_DDRAW_THUNK(DDInternalUnlock)
MW4_DDRAW_THUNK(DSoundHelp)
MW4_DDRAW_THUNK(DirectDrawCreateClipper)
MW4_DDRAW_THUNK(DirectDrawEnumerateA)
MW4_DDRAW_THUNK(DirectDrawEnumerateExA)
MW4_DDRAW_THUNK(DirectDrawEnumerateExW)
MW4_DDRAW_THUNK(DirectDrawEnumerateW)
MW4_DDRAW_THUNK(DllCanUnloadNow)
MW4_DDRAW_THUNK(DllGetClassObject)
MW4_DDRAW_THUNK(GetDDSurfaceLocal)
MW4_DDRAW_THUNK(GetOLEThunkData)
MW4_DDRAW_THUNK(GetSurfaceFromDC)
MW4_DDRAW_THUNK(RegisterSpecialCase)
MW4_DDRAW_THUNK(ReleaseDDThreadLock)
MW4_DDRAW_THUNK(SetAppCompatData)
}  // extern "C"

extern "C" HRESULT WINAPI Proxy_DirectDrawCreate(GUID* guid, IDirectDraw** out,
                                                 IUnknown* outer) {
    auto fn = reinterpret_cast<DirectDrawCreate_t>(Resolve("DirectDrawCreate"));
    if (!fn) {
        return E_FAIL;
    }
    HRESULT result = fn(guid, out, outer);
    if (SUCCEEDED(result) && out && *out) {
        PatchDirectDrawVtable(reinterpret_cast<IDirectDraw7*>(*out));
    }
    return result;
}

extern "C" HRESULT WINAPI Proxy_DirectDrawCreateEx(GUID* guid, void** out,
                                                   REFIID iid, IUnknown* outer) {
    auto fn =
        reinterpret_cast<DirectDrawCreateEx_t>(Resolve("DirectDrawCreateEx"));
    if (!fn) {
        return E_FAIL;
    }
    HRESULT result = fn(guid, out, iid, outer);
    if (SUCCEEDED(result) && out && *out) {
        PatchDirectDrawVtable(reinterpret_cast<IDirectDraw7*>(*out));
    }
    return result;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        LoadConfig();
        Log("ddraw proxy attached\n");
        Resolve("DirectDrawCreate");
        g_pfn_AcquireDDThreadLock = reinterpret_cast<void*>(Resolve("AcquireDDThreadLock"));
        g_pfn_CompleteCreateSysmemSurface = reinterpret_cast<void*>(Resolve("CompleteCreateSysmemSurface"));
        g_pfn_D3DParseUnknownCommand = reinterpret_cast<void*>(Resolve("D3DParseUnknownCommand"));
        g_pfn_DDGetAttachedSurfaceLcl = reinterpret_cast<void*>(Resolve("DDGetAttachedSurfaceLcl"));
        g_pfn_DDInternalLock = reinterpret_cast<void*>(Resolve("DDInternalLock"));
        g_pfn_DDInternalUnlock = reinterpret_cast<void*>(Resolve("DDInternalUnlock"));
        g_pfn_DSoundHelp = reinterpret_cast<void*>(Resolve("DSoundHelp"));
        g_pfn_DirectDrawCreateClipper = reinterpret_cast<void*>(Resolve("DirectDrawCreateClipper"));
        g_pfn_DirectDrawEnumerateA = reinterpret_cast<void*>(Resolve("DirectDrawEnumerateA"));
        g_pfn_DirectDrawEnumerateExA = reinterpret_cast<void*>(Resolve("DirectDrawEnumerateExA"));
        g_pfn_DirectDrawEnumerateExW = reinterpret_cast<void*>(Resolve("DirectDrawEnumerateExW"));
        g_pfn_DirectDrawEnumerateW = reinterpret_cast<void*>(Resolve("DirectDrawEnumerateW"));
        g_pfn_DllCanUnloadNow = reinterpret_cast<void*>(Resolve("DllCanUnloadNow"));
        g_pfn_DllGetClassObject = reinterpret_cast<void*>(Resolve("DllGetClassObject"));
        g_pfn_GetDDSurfaceLocal = reinterpret_cast<void*>(Resolve("GetDDSurfaceLocal"));
        g_pfn_GetOLEThunkData = reinterpret_cast<void*>(Resolve("GetOLEThunkData"));
        g_pfn_GetSurfaceFromDC = reinterpret_cast<void*>(Resolve("GetSurfaceFromDC"));
        g_pfn_RegisterSpecialCase = reinterpret_cast<void*>(Resolve("RegisterSpecialCase"));
        g_pfn_ReleaseDDThreadLock = reinterpret_cast<void*>(Resolve("ReleaseDDThreadLock"));
        g_pfn_SetAppCompatData = reinterpret_cast<void*>(Resolve("SetAppCompatData"));
    }
    return TRUE;
}
