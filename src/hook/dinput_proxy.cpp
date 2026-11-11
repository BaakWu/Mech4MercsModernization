// dinput.dll export forwarding.
//
// The game imports dinput.dll statically, so a local proxy is loaded during
// process startup - before MW4Mercs.exe parses its command line. This gives
// the hook a chance to append -window before the display path is chosen.
#include "hook.h"

extern "C" {

void* g_pfn_DirectInputCreateA = nullptr;
void* g_pfn_DirectInputCreateW = nullptr;
void* g_pfn_DirectInputCreateEx = nullptr;
void* g_pfn_DllCanUnloadNow = nullptr;
void* g_pfn_DllGetClassObject = nullptr;
void* g_pfn_DllRegisterServer = nullptr;
void* g_pfn_DllUnregisterServer = nullptr;

#define MW4_DINPUT_THUNK(name)                \
    __declspec(naked) void Proxy_##name() {   \
        __asm { jmp dword ptr[g_pfn_##name] } \
    }

MW4_DINPUT_THUNK(DirectInputCreateA)
MW4_DINPUT_THUNK(DirectInputCreateW)
MW4_DINPUT_THUNK(DirectInputCreateEx)
MW4_DINPUT_THUNK(DllCanUnloadNow)
MW4_DINPUT_THUNK(DllGetClassObject)
MW4_DINPUT_THUNK(DllRegisterServer)
MW4_DINPUT_THUNK(DllUnregisterServer)

}  // extern "C"

namespace {

HMODULE g_real_dinput = nullptr;

FARPROC Resolve(const char* name) {
    if (!g_real_dinput) {
        wchar_t path[MAX_PATH];
        UINT len = GetSystemDirectoryW(path, MAX_PATH);
        if (len == 0 || len + 16 >= MAX_PATH) {
            return nullptr;
        }
        wcscat_s(path, L"\\dinput.dll");
        g_real_dinput = LoadLibraryW(path);
        mw4hook::Log("dinput proxy: real module %p\n", g_real_dinput);
    }
    if (!g_real_dinput) {
        return nullptr;
    }
    return GetProcAddress(g_real_dinput, name);
}

}  // namespace

#define MW4_RESOLVE(name) \
    g_pfn_##name = reinterpret_cast<void*>(Resolve(#name))

namespace mw4hook {

void ResolveDinputExports() {
    MW4_RESOLVE(DirectInputCreateA);
    MW4_RESOLVE(DirectInputCreateW);
    MW4_RESOLVE(DirectInputCreateEx);
    MW4_RESOLVE(DllCanUnloadNow);
    MW4_RESOLVE(DllGetClassObject);
    MW4_RESOLVE(DllRegisterServer);
    MW4_RESOLVE(DllUnregisterServer);
    Log("dinput proxy: resolved exports\n");
}

}  // namespace mw4hook
