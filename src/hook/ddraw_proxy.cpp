// ddraw.dll export forwarding.
//
// The game's renderer loads ddraw.dll from the application directory, so this
// proxy is loaded before the main window exists. Each export is a jump thunk
// to the real System32\ddraw.dll entry point, preserving the original calling
// convention and stack cleanup.
#include "hook.h"

extern "C" {

void* g_pfn_AcquireDDThreadLock = nullptr;
void* g_pfn_CompleteCreateSysmemSurface = nullptr;
void* g_pfn_D3DParseUnknownCommand = nullptr;
void* g_pfn_DDGetAttachedSurfaceLcl = nullptr;
void* g_pfn_DDInternalLock = nullptr;
void* g_pfn_DDInternalUnlock = nullptr;
void* g_pfn_DSoundHelp = nullptr;
void* g_pfn_DirectDrawCreate = nullptr;
void* g_pfn_DirectDrawCreateClipper = nullptr;
void* g_pfn_DirectDrawCreateEx = nullptr;
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
MW4_DDRAW_THUNK(DirectDrawCreate)
MW4_DDRAW_THUNK(DirectDrawCreateClipper)
MW4_DDRAW_THUNK(DirectDrawCreateEx)
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

namespace {

HMODULE g_real_ddraw = nullptr;

FARPROC Resolve(const char* name) {
    if (!g_real_ddraw) {
        wchar_t path[MAX_PATH];
        UINT len = GetSystemDirectoryW(path, MAX_PATH);
        if (len == 0 || len + 16 >= MAX_PATH) {
            return nullptr;
        }
        wcscat_s(path, L"\\ddraw.dll");
        g_real_ddraw = LoadLibraryW(path);
        mw4hook::Log("ddraw proxy: real module %p\n", g_real_ddraw);
    }
    if (!g_real_ddraw) {
        return nullptr;
    }
    return GetProcAddress(g_real_ddraw, name);
}

}  // namespace

#define MW4_RESOLVE(name) \
    g_pfn_##name = reinterpret_cast<void*>(Resolve(#name))

namespace mw4hook {

void ResolveDdrawExports() {
    MW4_RESOLVE(AcquireDDThreadLock);
    MW4_RESOLVE(CompleteCreateSysmemSurface);
    MW4_RESOLVE(D3DParseUnknownCommand);
    MW4_RESOLVE(DDGetAttachedSurfaceLcl);
    MW4_RESOLVE(DDInternalLock);
    MW4_RESOLVE(DDInternalUnlock);
    MW4_RESOLVE(DSoundHelp);
    MW4_RESOLVE(DirectDrawCreate);
    MW4_RESOLVE(DirectDrawCreateClipper);
    MW4_RESOLVE(DirectDrawCreateEx);
    MW4_RESOLVE(DirectDrawEnumerateA);
    MW4_RESOLVE(DirectDrawEnumerateExA);
    MW4_RESOLVE(DirectDrawEnumerateExW);
    MW4_RESOLVE(DirectDrawEnumerateW);
    MW4_RESOLVE(DllCanUnloadNow);
    MW4_RESOLVE(DllGetClassObject);
    MW4_RESOLVE(GetDDSurfaceLocal);
    MW4_RESOLVE(GetOLEThunkData);
    MW4_RESOLVE(GetSurfaceFromDC);
    MW4_RESOLVE(RegisterSpecialCase);
    MW4_RESOLVE(ReleaseDDThreadLock);
    MW4_RESOLVE(SetAppCompatData);

    const void* all[] = {g_pfn_AcquireDDThreadLock,
                         g_pfn_CompleteCreateSysmemSurface,
                         g_pfn_D3DParseUnknownCommand,
                         g_pfn_DDGetAttachedSurfaceLcl,
                         g_pfn_DDInternalLock,
                         g_pfn_DDInternalUnlock,
                         g_pfn_DSoundHelp,
                         g_pfn_DirectDrawCreate,
                         g_pfn_DirectDrawCreateClipper,
                         g_pfn_DirectDrawCreateEx,
                         g_pfn_DirectDrawEnumerateA,
                         g_pfn_DirectDrawEnumerateExA,
                         g_pfn_DirectDrawEnumerateExW,
                         g_pfn_DirectDrawEnumerateW,
                         g_pfn_DllCanUnloadNow,
                         g_pfn_DllGetClassObject,
                         g_pfn_GetDDSurfaceLocal,
                         g_pfn_GetOLEThunkData,
                         g_pfn_GetSurfaceFromDC,
                         g_pfn_RegisterSpecialCase,
                         g_pfn_ReleaseDDThreadLock,
                         g_pfn_SetAppCompatData};
    int resolved = 0;
    for (const void* p : all) {
        if (p) {
            ++resolved;
        }
    }
    Log("ddraw proxy: resolved %d/%d exports\n", resolved,
        static_cast<int>(sizeof(all) / sizeof(all[0])));
}

}  // namespace mw4hook
