// dinput.dll export forwarding.
//
// The game imports dinput.dll statically, so a local proxy is loaded during
// process startup - before MW4Mercs.exe parses its command line. This gives
// the hook a chance to append -window before the display path is chosen.
//
// The DirectInputCreate* exports are wrapped (not just forwarded) so the test
// automation can filter IDirectInputDevice::GetDeviceState. Without
// [debug] automation=1 the wrappers only forward.
#include "hook.h"

#include <unknwn.h>

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

// COM vtable slots.
constexpr int kQueryInterface = 0;
constexpr int kCreateDevice = 3;      // IDirectInput*
constexpr int kCreateDeviceEx = 9;    // IDirectInput7
constexpr int kGetDeviceState = 9;    // IDirectInputDevice*

using QueryInterface_t = HRESULT(WINAPI*)(void*, REFIID, void**);
using CreateDevice_t = HRESULT(WINAPI*)(void*, REFGUID, void**, IUnknown*);
using CreateDeviceEx_t = HRESULT(WINAPI*)(void*, REFGUID, REFIID, void**, IUnknown*);
using GetDeviceState_t = HRESULT(WINAPI*)(void*, DWORD, void*);

struct PatchedTable {
    void** vtable;
    void* original;
};

PatchedTable g_state_tables[8] = {};
PatchedTable g_qi_tables[8] = {};
PatchedTable g_create_tables[8] = {};
PatchedTable g_createex_tables[8] = {};

void* FindOriginal(PatchedTable* tables, void** vtable) {
    for (int i = 0; i < 8; ++i) {
        if (tables[i].vtable == vtable) {
            return tables[i].original;
        }
    }
    return nullptr;
}

bool PatchSlot(PatchedTable* tables, void** vtable, int slot, void* detour) {
    if (!vtable || FindOriginal(tables, vtable)) {
        return false;
    }
    for (int i = 0; i < 8; ++i) {
        if (!tables[i].vtable) {
            DWORD old = 0;
            if (!VirtualProtect(&vtable[slot], sizeof(void*), PAGE_READWRITE, &old)) {
                return false;
            }
            tables[i].vtable = vtable;
            tables[i].original = vtable[slot];
            vtable[slot] = detour;
            VirtualProtect(&vtable[slot], sizeof(void*), old, &old);
            return true;
        }
    }
    return false;
}

void** VtableOf(void* object) { return *reinterpret_cast<void***>(object); }

void PatchDevice(void* device);

HRESULT WINAPI Hook_GetDeviceState(void* self, DWORD size, void* data) {
    auto original = reinterpret_cast<GetDeviceState_t>(
        FindOriginal(g_state_tables, VtableOf(self)));
    HRESULT result = original(self, size, data);
    mw4hook::AutomationFilterDeviceState(size, data, &result);
    return result;
}

HRESULT WINAPI Hook_DeviceQueryInterface(void* self, REFIID iid, void** out) {
    auto original = reinterpret_cast<QueryInterface_t>(
        FindOriginal(g_qi_tables, VtableOf(self)));
    HRESULT result = original(self, iid, out);
    if (SUCCEEDED(result) && out && *out) {
        PatchDevice(*out);
    }
    return result;
}

void PatchDevice(void* device) {
    if (!device) {
        return;
    }
    void** vtable = VtableOf(device);
    if (PatchSlot(g_state_tables, vtable, kGetDeviceState,
                  reinterpret_cast<void*>(&Hook_GetDeviceState))) {
        mw4hook::Log("dinput proxy: device vtable %p patched\n", vtable);
    }
    PatchSlot(g_qi_tables, vtable, kQueryInterface,
              reinterpret_cast<void*>(&Hook_DeviceQueryInterface));
}

HRESULT WINAPI Hook_CreateDevice(void* self, REFGUID guid, void** out,
                                 IUnknown* outer) {
    auto original = reinterpret_cast<CreateDevice_t>(
        FindOriginal(g_create_tables, VtableOf(self)));
    HRESULT result = original(self, guid, out, outer);
    if (SUCCEEDED(result) && out) {
        PatchDevice(*out);
    }
    return result;
}

HRESULT WINAPI Hook_CreateDeviceEx(void* self, REFGUID guid, REFIID iid,
                                   void** out, IUnknown* outer) {
    auto original = reinterpret_cast<CreateDeviceEx_t>(
        FindOriginal(g_createex_tables, VtableOf(self)));
    HRESULT result = original(self, guid, iid, out, outer);
    if (SUCCEEDED(result) && out) {
        PatchDevice(*out);
    }
    return result;
}

// IID_IDirectInput7A / IID_IDirectInput7W
const GUID kIidDirectInput7A = {0x9A4CB684, 0x236D, 0x11D3,
                                {0x8E, 0x9D, 0x00, 0xC0, 0x4F, 0x68, 0x44, 0xAE}};
const GUID kIidDirectInput7W = {0x9A4CB685, 0x236D, 0x11D3,
                                {0x8E, 0x9D, 0x00, 0xC0, 0x4F, 0x68, 0x44, 0xAE}};

void PatchDirectInput(void* dinput, bool version7) {
    if (!dinput || !mw4hook::GetConfig().debug_automation) {
        return;
    }
    void** vtable = VtableOf(dinput);
    PatchSlot(g_create_tables, vtable, kCreateDevice,
              reinterpret_cast<void*>(&Hook_CreateDevice));
    if (version7) {
        PatchSlot(g_createex_tables, vtable, kCreateDeviceEx,
                  reinterpret_cast<void*>(&Hook_CreateDeviceEx));
    }
}

using DirectInputCreate_t = HRESULT(WINAPI*)(HINSTANCE, DWORD, void**, IUnknown*);
using DirectInputCreateEx_t = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, void**,
                                               IUnknown*);

}  // namespace

extern "C" HRESULT WINAPI Proxy_DirectInputCreateA(HINSTANCE instance, DWORD version,
                                                   void** out, IUnknown* outer) {
    auto fn = reinterpret_cast<DirectInputCreate_t>(g_pfn_DirectInputCreateA);
    HRESULT result = fn ? fn(instance, version, out, outer) : E_FAIL;
    if (SUCCEEDED(result) && out) {
        PatchDirectInput(*out, false);
    }
    return result;
}

extern "C" HRESULT WINAPI Proxy_DirectInputCreateW(HINSTANCE instance, DWORD version,
                                                   void** out, IUnknown* outer) {
    auto fn = reinterpret_cast<DirectInputCreate_t>(g_pfn_DirectInputCreateW);
    HRESULT result = fn ? fn(instance, version, out, outer) : E_FAIL;
    if (SUCCEEDED(result) && out) {
        PatchDirectInput(*out, false);
    }
    return result;
}

extern "C" HRESULT WINAPI Proxy_DirectInputCreateEx(HINSTANCE instance, DWORD version,
                                                    REFIID iid, void** out,
                                                    IUnknown* outer) {
    auto fn = reinterpret_cast<DirectInputCreateEx_t>(g_pfn_DirectInputCreateEx);
    HRESULT result = fn ? fn(instance, version, iid, out, outer) : E_FAIL;
    if (SUCCEEDED(result) && out) {
        PatchDirectInput(*out, IsEqualGUID(iid, kIidDirectInput7A) ||
                                   IsEqualGUID(iid, kIidDirectInput7W));
    }
    return result;
}

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
