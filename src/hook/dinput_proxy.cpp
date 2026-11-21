// dinput.dll export forwarding.
//
// The game imports dinput.dll statically, so a local proxy is loaded during
// process startup - before MW4Mercs.exe parses its command line. This gives
// the hook a chance to append -window before the display path is chosen.
//
// The DirectInputCreate* exports are wrapped (not just forwarded): device
// enumeration and opening are filtered and guarded (issue #34, below), and
// the test automation ([debug] automation=1) filters
// IDirectInputDevice::GetDeviceState.
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

// Gamepad devices (issue #42). DirectInput shares one device vtable across
// all devices, so the gamepad flag has to be kept per device object.
struct DeviceObject {
    void* object;
    bool gamepad;
};
DeviceObject g_objects[8] = {};

void RememberDevice(void* object, bool gamepad) {
    if (!object) {
        return;
    }
    for (DeviceObject& entry : g_objects) {
        if (entry.object == object) {
            entry.gamepad = gamepad;
            return;
        }
    }
    for (DeviceObject& entry : g_objects) {
        if (!entry.object) {
            entry.object = object;
            entry.gamepad = gamepad;
            return;
        }
    }
}

bool IsGamepadObject(void* object) {
    for (const DeviceObject& entry : g_objects) {
        if (entry.object == object) {
            return entry.gamepad;
        }
    }
    return false;
}

volatile LONG g_gamepad_throttle = 0;

void PatchDevice(void* device, bool gamepad);

HRESULT WINAPI Hook_GetDeviceState(void* self, DWORD size, void* data) {
    auto original = reinterpret_cast<GetDeviceState_t>(
        FindOriginal(g_state_tables, VtableOf(self)));
    HRESULT result = original(self, size, data);
    if (mw4hook::GetConfig().input_gamepad_layout && IsGamepadObject(self)) {
        if (SUCCEEDED(result) && data && (size == 80 || size == 272)) {
            // The game reads the torso from Y (pitch) and Z (torso twist).
            // An Xbox-style pad's right stick is Rx/Ry; put it where the
            // game expects it, and use the left stick Y for the throttle.
            LONG* axes = static_cast<LONG*>(data);
            mw4hook::SetGamepadThrottle(static_cast<int>(axes[1]));
            axes[1] = axes[4];  // right stick Y -> pitch
            axes[2] = axes[3];  // right stick X -> torso twist
        } else if (FAILED(result)) {
            mw4hook::SetGamepadThrottle(0);
        }
    }
    mw4hook::AutomationFilterDeviceState(size, data, &result);
    return result;
}

HRESULT WINAPI Hook_DeviceQueryInterface(void* self, REFIID iid, void** out) {
    auto original = reinterpret_cast<QueryInterface_t>(
        FindOriginal(g_qi_tables, VtableOf(self)));
    HRESULT result = original(self, iid, out);
    if (SUCCEEDED(result) && out && *out) {
        PatchDevice(*out, IsGamepadObject(self));
    }
    return result;
}

void PatchDevice(void* device, bool gamepad) {
    if (!device) {
        return;
    }
    RememberDevice(device, gamepad);
    if (gamepad) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            mw4hook::Log("dinput proxy: gamepad axis layout (left stick throttle/turn, right "
                         "stick torso/pitch)\n");
        }
    }
    void** vtable = VtableOf(device);
    if (PatchSlot(g_state_tables, vtable, kGetDeviceState,
                  reinterpret_cast<void*>(&Hook_GetDeviceState))) {
        mw4hook::Log("dinput proxy: device vtable %p patched\n", vtable);
    }
    PatchSlot(g_qi_tables, vtable, kQueryInterface,
              reinterpret_cast<void*>(&Hook_DeviceQueryInterface));
}

// Input devices (issue #34). At startup the game enumerates every attached
// device (IDirectInput::EnumDevices, all types, attached only) and opens
// each one that is not a mouse or keyboard with CreateDeviceEx; a device it
// cannot open is skipped. Opening a HID device makes DirectInput build its
// object list (CHid_InitObjects / CHid_InitAxisClass), and with some
// devices Windows' DirectInput writes past the end of that list and crashes
// the game before the menus (seen in dinput.dll 10.0.26100.8115 at
// CHid_InitAxisClass+0x63E). The stock game does the same.
//
// The game only uses joysticks, gamepads and wheels (DIDEVTYPE_JOYSTICK).
// Devices DirectInput lists as DIDEVTYPE_DEVICE (lighting controllers,
// headset and media keys, the extra collections of gaming keyboards and
// mice) are not offered to the game unless [input] other_devices=1, and
// [input] controllers=0 leaves only the keyboard and mouse. Every device is
// logged with its type. Opening a controller is guarded: if DirectInput
// faults inside it, the device is logged and skipped (the open fails, as
// for an unplugged device) instead of the game closing.
//
// The devices are listed one type at a time (keyboard, mouse, controllers)
// rather than in one EnumDevices(0) pass (#40): a headset or receiver whose
// HID objects Windows' DirectInput mishandles can crash that pass, which
// used to leave the player with the keyboard and mouse only. If the probe
// cannot list a type safely, that type is left out (input_probe.cpp).
// Enumeration is still filtered, so the game never sees the other HID
// devices unless [input] other_devices=1.
constexpr int kEnumDevices = 4;  // IDirectInput*
constexpr BYTE kTypeDevice = 1;    // DIDEVTYPE_DEVICE
constexpr BYTE kTypeMouse = 2;     // DIDEVTYPE_MOUSE
constexpr BYTE kTypeKeyboard = 3;  // DIDEVTYPE_KEYBOARD
constexpr BYTE kTypeJoystick = 4;  // DIDEVTYPE_JOYSTICK
constexpr HRESULT kDeviceNotRegistered = static_cast<HRESULT>(0x80040154L);  // DIERR_DEVICENOTREG

// GUID_SysMouse / GUID_SysKeyboard
const GUID kSysMouse = {0x6F1D2B60, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
const GUID kSysKeyboard = {0x6F1D2B61, 0xD5A0, 0x11CF,
                           {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};

using EnumCallback_t = BOOL(CALLBACK*)(const void* instance, void* ref);
using EnumDevices_t = HRESULT(WINAPI*)(void*, DWORD, EnumCallback_t, void*, DWORD);

PatchedTable g_enum_tables[8] = {};
void** g_wide_vtables[8] = {};  // IDirectInput*W vtables (names are WCHAR)

bool IsWide(void** vtable) {
    for (void** v : g_wide_vtables) {
        if (v && v == vtable) {
            return true;
        }
    }
    return false;
}

// Names of the enumerated devices, for the open log.
struct KnownDevice {
    GUID instance;
    BYTE type;
    BYTE subtype;  // DIDEVTYPEJOYSTICK_* (4 = gamepad, issue #42)
    char name[80];
};
KnownDevice g_devices[32] = {};
int g_device_count = 0;

const KnownDevice* FindDevice(REFGUID instance) {
    for (int i = 0; i < g_device_count; ++i) {
        if (IsEqualGUID(g_devices[i].instance, instance)) {
            return &g_devices[i];
        }
    }
    return nullptr;
}

const char* TypeName(BYTE type) {
    switch (type) {
        case kTypeDevice: return "other device";
        case kTypeMouse: return "mouse";
        case kTypeKeyboard: return "keyboard";
        case kTypeJoystick: return "controller";
        default: return "unknown type";
    }
}

struct EnumContext {
    EnumCallback_t callback;
    void* ref;
    bool wide;
};

// DIDEVICEINSTANCEA / W: dwSize, guidInstance (+4), guidProduct (+0x14),
// dwDevType (+0x24), tszInstanceName (+0x28), tszProductName.
BOOL CALLBACK EnumFilter(const void* instance, void* ref) {
    const EnumContext* context = static_cast<const EnumContext*>(ref);
    const unsigned char* bytes = static_cast<const unsigned char*>(instance);
    GUID guid;
    memcpy(&guid, bytes + 4, sizeof(guid));
    DWORD dev_type = 0;
    memcpy(&dev_type, bytes + 0x24, sizeof(dev_type));
    const BYTE type = static_cast<BYTE>(dev_type & 0xFF);
    const mw4hook::Config& config = mw4hook::GetConfig();
    const bool system = type == kTypeMouse || type == kTypeKeyboard;
    const char* blocked = system ? nullptr : mw4hook::InputDeviceBlocked(guid);
    const bool pass = system || (config.input_controllers && !blocked &&
                                 (type == kTypeJoystick || config.input_other_devices));
    if (!FindDevice(guid) && g_device_count < 32) {
        KnownDevice& device = g_devices[g_device_count++];
        device.instance = guid;
        device.type = type;
        device.subtype = static_cast<BYTE>((dev_type >> 8) & 0xFF);
        if (context->wide) {
            WideCharToMultiByte(CP_UTF8, 0, reinterpret_cast<const wchar_t*>(bytes + 0x28), -1,
                                device.name, sizeof(device.name), nullptr, nullptr);
        } else {
            strncpy_s(device.name, reinterpret_cast<const char*>(bytes + 0x28), _TRUNCATE);
        }
        mw4hook::Log("dinput: device \"%s\" (%s, type %08lX) %s%s%s\n", device.name,
                     TypeName(type), dev_type,
                     pass ? "offered to the game" : "not offered to the game",
                     blocked ? ": " : "", blocked ? blocked : "");
    }
    if (!pass) {
        return TRUE;  // DIENUM_CONTINUE
    }
    return context->callback(instance, context->ref);
}

HRESULT WINAPI Hook_EnumDevices(void* self, DWORD type, EnumCallback_t callback, void* ref,
                                DWORD flags) {
    auto original =
        reinterpret_cast<EnumDevices_t>(FindOriginal(g_enum_tables, VtableOf(self)));
    if (!callback) {
        return original(self, type, callback, ref, flags);
    }
    mw4hook::InputSafetyInit();  // probe and crash memory, once, before the first listing
    EnumContext context = {callback, ref, IsWide(VtableOf(self))};
    const mw4hook::Config& config = mw4hook::GetConfig();
    if (type != 0) {
        if (!mw4hook::InputTypeSafe(static_cast<BYTE>(type))) {
            mw4hook::Log("dinput: not listing device type %lu: DirectInput cannot list it "
                         "safely\n",
                         static_cast<unsigned long>(type));
            return 0;  // DI_OK, nothing listed
        }
        return original(self, type, &EnumFilter, &context, flags);
    }
    // [input] other_devices=1 keeps the stock listing of every device when
    // the probe could list them. Otherwise list one device type at a time:
    // Windows' DirectInput can crash while walking the HID devices for a
    // headset or receiver, and a full listing would then cost the player the
    // joystick too (#40). Listing DIDEVTYPE_JOYSTICK does not offer the game
    // the other HID devices, which it never used.
    if (config.input_controllers && config.input_other_devices &&
        !mw4hook::InputEnumerationUnsafe()) {
        return original(self, 0, &EnumFilter, &context, flags);
    }
    BYTE types[4] = {kTypeMouse, kTypeKeyboard, 0, 0};
    int count = 2;
    bool controllers = false;
    bool others = false;
    if (config.input_controllers && mw4hook::InputTypeSafe(kTypeJoystick)) {
        types[count++] = kTypeJoystick;
        controllers = true;
    }
    if (config.input_other_devices && mw4hook::InputTypeSafe(kTypeDevice)) {
        types[count++] = kTypeDevice;
        others = true;
    }
    static bool logged = false;
    if (!logged) {
        logged = true;
        mw4hook::Log("dinput: listing the devices one type at a time (keyboard, mouse%s%s)\n",
                     controllers ? ", controllers" : "", others ? ", other devices" : "");
    }
    HRESULT result = 0;
    for (int i = 0; i < count; ++i) {
        const HRESULT listed = original(self, types[i], &EnumFilter, &context, flags);
        if (FAILED(listed)) {
            result = listed;
        }
    }
    return result;
}

bool IsSystemDevice(REFGUID guid) {
    return IsEqualGUID(guid, kSysMouse) || IsEqualGUID(guid, kSysKeyboard);
}

// DIDEVTYPEJOYSTICK_GAMEPAD = 4. The Logitech Extreme 3D is subtype 1
// (unknown) and keeps the traditional joystick layout; the Xbox 360 pad is
// subtype 4. Issue #42.
bool IsGamepadGuid(REFGUID guid) {
    const KnownDevice* device = FindDevice(guid);
    return device && device->type == kTypeJoystick && device->subtype == 4;
}

int CaptureFault(EXCEPTION_POINTERS* info, DWORD* code, void** where) {
    *code = info->ExceptionRecord->ExceptionCode;
    *where = info->ExceptionRecord->ExceptionAddress;
    return *code == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER
                                               : EXCEPTION_CONTINUE_SEARCH;
}

// No C++ objects in these two (SEH).
HRESULT GuardedCreate(CreateDevice_t fn, void* self, REFGUID guid, void** out, IUnknown* outer,
                      DWORD* code, void** where) {
    __try {
        return fn(self, guid, out, outer);
    } __except (CaptureFault(GetExceptionInformation(), code, where)) {
        return kDeviceNotRegistered;
    }
}

HRESULT GuardedCreateEx(CreateDeviceEx_t fn, void* self, REFGUID guid, REFIID iid, void** out,
                        IUnknown* outer, DWORD* code, void** where) {
    __try {
        return fn(self, guid, iid, out, outer);
    } __except (CaptureFault(GetExceptionInformation(), code, where)) {
        return kDeviceNotRegistered;
    }
}

void LogOpen(REFGUID guid, HRESULT result, DWORD code, void* where) {
    const KnownDevice* device = FindDevice(guid);
    const char* name = device ? device->name : "(not enumerated)";
    if (code) {
        HMODULE module = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCWSTR>(where), &module);
        mw4hook::Log("dinput: opening \"%s\" crashed DirectInput (exception %08lX at %p, module "
                     "%p + 0x%X); the device is skipped. If the game is unstable, unplug it "
                     "or set [input] controllers=0 in mw4hook.ini\n",
                     name, code, where, module,
                     module ? static_cast<unsigned>(static_cast<char*>(where) -
                                                    reinterpret_cast<char*>(module))
                            : 0u);
        return;
    }
    mw4hook::Log("dinput: opened \"%s\" -> 0x%08lX\n", name, static_cast<unsigned long>(result));
}

// Before the game opens a device that is not the system mouse or keyboard:
// refuse it when it is kept from the game (the probe or the crash memory
// found it unsafe, or [input] controllers=0), otherwise log it and note it in
// the crash memory. True when refused (the open then fails like an unplugged
// device).
bool RefuseOpen(REFGUID guid, void** out) {
    const KnownDevice* device = FindDevice(guid);
    const char* name = device ? device->name : "(not enumerated)";
    const char* blocked = mw4hook::InputDeviceBlocked(guid);
    if (blocked || !mw4hook::GetConfig().input_controllers) {
        mw4hook::Log("dinput: not opening \"%s\": %s\n", name,
                     blocked ? blocked : "[input] controllers=0");
        if (out) {
            *out = nullptr;
        }
        return true;
    }
    mw4hook::Log("dinput: opening \"%s\" ...\n", name);
    mw4hook::InputOpenBegin(guid, name);
    return false;
}

HRESULT WINAPI Hook_CreateDevice(void* self, REFGUID guid, void** out, IUnknown* outer) {
    auto original =
        reinterpret_cast<CreateDevice_t>(FindOriginal(g_create_tables, VtableOf(self)));
    HRESULT result;
    if (IsSystemDevice(guid)) {
        result = original(self, guid, out, outer);
    } else {
        if (RefuseOpen(guid, out)) {
            return kDeviceNotRegistered;
        }
        DWORD code = 0;
        void* where = nullptr;
        result = GuardedCreate(original, self, guid, out, outer, &code, &where);
        if (code && out) {
            *out = nullptr;
        }
        LogOpen(guid, result, code, where);
        if (!code) {
            mw4hook::InputOpenEnd(guid);  // a fault leaves it in [opening]: skipped next time
        }
    }
    if (SUCCEEDED(result) && out &&
        (mw4hook::GetConfig().debug_automation || mw4hook::GetConfig().input_gamepad_layout)) {
        PatchDevice(*out, IsGamepadGuid(guid));
    }
    return result;
}

HRESULT WINAPI Hook_CreateDeviceEx(void* self, REFGUID guid, REFIID iid, void** out,
                                   IUnknown* outer) {
    auto original =
        reinterpret_cast<CreateDeviceEx_t>(FindOriginal(g_createex_tables, VtableOf(self)));
    HRESULT result;
    if (IsSystemDevice(guid)) {
        result = original(self, guid, iid, out, outer);
    } else {
        if (RefuseOpen(guid, out)) {
            return kDeviceNotRegistered;
        }
        DWORD code = 0;
        void* where = nullptr;
        result = GuardedCreateEx(original, self, guid, iid, out, outer, &code, &where);
        if (code && out) {
            *out = nullptr;
        }
        LogOpen(guid, result, code, where);
        if (!code) {
            mw4hook::InputOpenEnd(guid);  // a fault leaves it in [opening]: skipped next time
        }
    }
    if (SUCCEEDED(result) && out &&
        (mw4hook::GetConfig().debug_automation || mw4hook::GetConfig().input_gamepad_layout)) {
        PatchDevice(*out, IsGamepadGuid(guid));
    }
    return result;
}

// IID_IDirectInput7A / IID_IDirectInput7W
const GUID kIidDirectInput7A = {0x9A4CB684, 0x236D, 0x11D3,
                                {0x8E, 0x9D, 0x00, 0xC0, 0x4F, 0x68, 0x44, 0xAE}};
const GUID kIidDirectInput7W = {0x9A4CB685, 0x236D, 0x11D3,
                                {0x8E, 0x9D, 0x00, 0xC0, 0x4F, 0x68, 0x44, 0xAE}};
// IID_IDirectInput2W / IID_IDirectInputW (the A forms are the others)
const GUID kIidDirectInput2W = {0x5944E683, 0xC92E, 0x11CF,
                                {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
const GUID kIidDirectInputW = {0x89521361, 0xAA8A, 0x11CF,
                               {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};

void PatchDirectInput(void* dinput, bool version7, bool wide) {
    if (!dinput) {
        return;
    }
    void** vtable = VtableOf(dinput);
    if (wide) {
        for (void**& v : g_wide_vtables) {
            if (!v || v == vtable) {
                v = vtable;
                break;
            }
        }
    }
    if (PatchSlot(g_enum_tables, vtable, kEnumDevices,
                  reinterpret_cast<void*>(&Hook_EnumDevices))) {
        mw4hook::Log("dinput proxy: device enumeration wrapped (controllers %d, other devices "
                     "%d)\n",
                     mw4hook::GetConfig().input_controllers ? 1 : 0,
                     mw4hook::GetConfig().input_other_devices ? 1 : 0);
    }
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
        PatchDirectInput(*out, false, false);
    }
    return result;
}

extern "C" HRESULT WINAPI Proxy_DirectInputCreateW(HINSTANCE instance, DWORD version,
                                                   void** out, IUnknown* outer) {
    auto fn = reinterpret_cast<DirectInputCreate_t>(g_pfn_DirectInputCreateW);
    HRESULT result = fn ? fn(instance, version, out, outer) : E_FAIL;
    if (SUCCEEDED(result) && out) {
        PatchDirectInput(*out, false, true);
    }
    return result;
}

extern "C" HRESULT WINAPI Proxy_DirectInputCreateEx(HINSTANCE instance, DWORD version,
                                                    REFIID iid, void** out,
                                                    IUnknown* outer) {
    auto fn = reinterpret_cast<DirectInputCreateEx_t>(g_pfn_DirectInputCreateEx);
    HRESULT result = fn ? fn(instance, version, iid, out, outer) : E_FAIL;
    if (SUCCEEDED(result) && out) {
        PatchDirectInput(*out,
                         IsEqualGUID(iid, kIidDirectInput7A) ||
                             IsEqualGUID(iid, kIidDirectInput7W),
                         IsEqualGUID(iid, kIidDirectInput7W) ||
                             IsEqualGUID(iid, kIidDirectInput2W) ||
                             IsEqualGUID(iid, kIidDirectInputW));
    }
    return result;
}

#define MW4_RESOLVE(name) \
    g_pfn_##name = reinterpret_cast<void*>(Resolve(#name))

namespace mw4hook {

void SetGamepadThrottle(int value) {
    InterlockedExchange(&g_gamepad_throttle, value);
}

int GamepadThrottle() {
    return static_cast<int>(InterlockedCompareExchange(&g_gamepad_throttle, 0, 0));
}

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
