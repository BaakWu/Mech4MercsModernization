// Input device safety (issue #34).
//
// At startup the game enumerates every attached DirectInput device and opens
// each controller. Windows' own DirectInput (dinput.dll) has bugs in its HID
// parsing (CHid_InitAxisClass writes past its tables for some devices) that
// close the game before the menus, and which device triggers them is not
// predictable. So, instead of guessing by device type, the hook finds out:
//
// 1. Probe. Before the game's first enumeration, a throwaway process
//    (rundll32 running ProbeInputDevices from this DLL) enumerates the
//    devices and opens, configures and reads each controller the way a game
//    does. It writes each step to a file before taking it. If DirectInput
//    crashes or hangs on a device, only that process ends; the device it
//    was on is not offered to the game. If enumeration itself fails, the
//    game is given the keyboard and mouse only.
// 2. Crash memory. Before the game opens a controller, the device is written
//    to mw4hook-input.ini [opening] and removed when the open returns. An
//    entry left there means the game closed during that open: from the next
//    start the device is listed under [skip] and not offered (delete the
//    file to try it again). This works where the probe cannot run.
// 3. The guard around the open in dinput_proxy.cpp, and [input]
//    controllers=0 (keyboard and mouse only).
#include "hook.h"

#include <cstdio>
#include <cstring>

#define DIRECTINPUT_VERSION 0x0700
#include <dinput.h>

extern "C" void* g_pfn_DirectInputCreateEx;  // the real export (dinput_proxy.cpp)

namespace mw4hook {
namespace {

// Defined here so the hook does not link dxguid.lib.
const GUID kIidDirectInput7A = {0x9A4CB684, 0x236D, 0x11D3,
                                {0x8E, 0x9D, 0x00, 0xC0, 0x4F, 0x68, 0x44, 0xAE}};
const GUID kIidDirectInputDevice7A = {0x57D7C6BC, 0x2356, 0x11D3,
                                      {0x8E, 0x9D, 0x00, 0xC0, 0x4F, 0x68, 0x44, 0xAE}};
const GUID kXAxis = {0xA36D02E0, 0xC9F3, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0, 0}};
const GUID kYAxis = {0xA36D02E1, 0xC9F3, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0, 0}};
const GUID kZAxis = {0xA36D02E2, 0xC9F3, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0, 0}};
const GUID kRxAxis = {0xA36D02F4, 0xC9F3, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0, 0}};
const GUID kRyAxis = {0xA36D02F5, 0xC9F3, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0, 0}};
const GUID kRzAxis = {0xA36D02E3, 0xC9F3, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0, 0}};
const GUID kSlider = {0xA36D02E4, 0xC9F3, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0, 0}};
const GUID kPov = {0xA36D02F2, 0xC9F3, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0, 0}};

constexpr DWORD kDidftOptional = 0x80000000;  // DIDFT_OPTIONAL
constexpr BYTE kTypeMouse = 2;     // DIDEVTYPE_MOUSE
constexpr BYTE kTypeKeyboard = 3;  // DIDEVTYPE_KEYBOARD
constexpr BYTE kTypeJoystick = 4;  // DIDEVTYPE_JOYSTICK
constexpr DWORD kProbeTimeoutMs = 6000;  // per round

void FormatGuid(const GUID& g, char (&out)[40]) {
    sprintf_s(out, "%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X", g.Data1, g.Data2,
              g.Data3, g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5],
              g.Data4[6], g.Data4[7]);
}

// ---------------------------------------------------------------------------
// The probe (runs in rundll32)
// ---------------------------------------------------------------------------

HANDLE g_probe_out = INVALID_HANDLE_VALUE;

void ProbeWrite(const char* text) {
    if (g_probe_out == INVALID_HANDLE_VALUE) {
        return;
    }
    DWORD written = 0;
    WriteFile(g_probe_out, text, static_cast<DWORD>(strlen(text)), &written, nullptr);
    FlushFileBuffers(g_probe_out);
}

// A crash in the probe: record where, and end the process at once (no
// Windows Error Reporting dialog).
LONG WINAPI ProbeCrashFilter(EXCEPTION_POINTERS* info) {
    void* where = info->ExceptionRecord->ExceptionAddress;
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       static_cast<LPCWSTR>(where), &module);
    char name[MAX_PATH] = "?";
    if (module) {
        char path[MAX_PATH];
        if (GetModuleFileNameA(module, path, MAX_PATH)) {
            const char* slash = strrchr(path, '\\');
            strcpy_s(name, slash ? slash + 1 : path);
        }
    }
    char line[400];
    sprintf_s(line, "crash %08lX %s+0x%X\r\n", info->ExceptionRecord->ExceptionCode, name,
              module ? static_cast<unsigned>(static_cast<char*>(where) -
                                             reinterpret_cast<char*>(module))
                     : 0u);
    ProbeWrite(line);
    TerminateProcess(GetCurrentProcess(), 3);
    return EXCEPTION_EXECUTE_HANDLER;
}

struct ProbeList {
    DIDEVICEINSTANCEA devices[32];
    int count;
};

BOOL CALLBACK ProbeCollect(const DIDEVICEINSTANCEA* instance, void* ref) {
    ProbeList* list = static_cast<ProbeList*>(ref);
    if (list->count < 32) {
        list->devices[list->count++] = *instance;
    }
    return DIENUM_CONTINUE;
}

// The standard joystick format (DIJOYSTATE), as c_dfDIJoystick.
void JoystickFormat(DIDATAFORMAT& format, DIOBJECTDATAFORMAT (&objects)[44]) {
    const DWORD axis = DIDFT_AXIS | DIDFT_ANYINSTANCE | kDidftOptional;
    const GUID* axes[8] = {&kXAxis, &kYAxis, &kZAxis, &kRxAxis,
                           &kRyAxis, &kRzAxis, &kSlider, &kSlider};
    int n = 0;
    for (int i = 0; i < 8; ++i) {
        objects[n++] = {axes[i], static_cast<DWORD>(i * 4), axis, DIDOI_ASPECTPOSITION};
    }
    for (int i = 0; i < 4; ++i) {
        objects[n++] = {&kPov, static_cast<DWORD>(32 + i * 4),
                        DIDFT_POV | DIDFT_ANYINSTANCE | kDidftOptional, 0};
    }
    for (int i = 0; i < 32; ++i) {
        objects[n++] = {nullptr, static_cast<DWORD>(48 + i),
                        DIDFT_BUTTON | DIDFT_ANYINSTANCE | kDidftOptional, 0};
    }
    format = {sizeof(DIDATAFORMAT), sizeof(DIOBJECTDATAFORMAT), DIDF_ABSAXIS,
              sizeof(DIJOYSTATE), static_cast<DWORD>(n), objects};
}

// skip: "J" (controllers only) or "A" (every device the game may be given,
// [input] other_devices=1), then the instance GUIDs to leave out.
void RunProbe(const char* skip, const char* out_path) {
    const bool all_types = skip[0] == 'A';
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    SetUnhandledExceptionFilter(&ProbeCrashFilter);
    g_probe_out = CreateFileA(out_path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_probe_out == INVALID_HANDLE_VALUE) {
        return;
    }
    ProbeWrite("start\r\n");
    using Create_t = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, void**, IUnknown*);
    auto create = reinterpret_cast<Create_t>(g_pfn_DirectInputCreateEx);
    IDirectInput7A* dinput = nullptr;
    if (!create || FAILED(create(GetModuleHandleW(nullptr), 0x0700, kIidDirectInput7A,
                                 reinterpret_cast<void**>(&dinput), nullptr)) ||
        !dinput) {
        ProbeWrite("nodinput\r\n");
        return;
    }
    static ProbeList list = {};
    const HRESULT enumerated =
        dinput->EnumDevices(0, &ProbeCollect, &list, DIEDFL_ATTACHEDONLY);
    char line[200];
    sprintf_s(line, "enum %08lX %d\r\n", static_cast<unsigned long>(enumerated), list.count);
    ProbeWrite(line);
    HWND window = CreateWindowExA(0, "STATIC", "", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr,
                                  nullptr, nullptr);
    static DIOBJECTDATAFORMAT objects[44];
    DIDATAFORMAT format;
    JoystickFormat(format, objects);
    for (int i = 0; i < list.count; ++i) {
        const DIDEVICEINSTANCEA& device = list.devices[i];
        const BYTE type = static_cast<BYTE>(device.dwDevType & 0xFF);
        if (type == kTypeMouse || type == kTypeKeyboard ||
            (!all_types && type != kTypeJoystick)) {
            continue;  // the game is not given it
        }
        char guid[40];
        FormatGuid(device.guidInstance, guid);
        if (strstr(skip, guid)) {
            continue;  // failed in an earlier round
        }
        sprintf_s(line, "open %s\r\n", guid);
        ProbeWrite(line);
        IDirectInputDevice7A* dev = nullptr;
        const HRESULT opened = dinput->CreateDeviceEx(device.guidInstance, kIidDirectInputDevice7A,
                                                      reinterpret_cast<void**>(&dev), nullptr);
        if (SUCCEEDED(opened) && dev) {
            dev->SetDataFormat(&format);
            if (window) {
                dev->SetCooperativeLevel(window, DISCL_NONEXCLUSIVE | DISCL_BACKGROUND);
            }
            if (SUCCEEDED(dev->Acquire())) {
                DIJOYSTATE state = {};
                dev->Poll();
                dev->GetDeviceState(sizeof(state), &state);
                dev->Unacquire();
            }
            dev->Release();
        }
        sprintf_s(line, "ok %s %08lX\r\n", guid, static_cast<unsigned long>(opened));
        ProbeWrite(line);
    }
    if (window) {
        DestroyWindow(window);
    }
    dinput->Release();
    ProbeWrite("done\r\n");
    CloseHandle(g_probe_out);
    g_probe_out = INVALID_HANDLE_VALUE;
}

// ---------------------------------------------------------------------------
// The game side
// ---------------------------------------------------------------------------

struct Blocked {
    char guid[40];
    char reason[200];
};
Blocked g_blocked[32] = {};
int g_blocked_count = 0;
bool g_enumeration_unsafe = false;
bool g_initialized = false;
wchar_t g_memory_path[MAX_PATH] = {};  // mw4hook-input.ini next to the game

void Block(const char* guid, const char* reason) {
    for (int i = 0; i < g_blocked_count; ++i) {
        if (strcmp(g_blocked[i].guid, guid) == 0) {
            return;
        }
    }
    if (g_blocked_count < 32) {
        strcpy_s(g_blocked[g_blocked_count].guid, guid);
        strncpy_s(g_blocked[g_blocked_count].reason, reason, _TRUNCATE);
        ++g_blocked_count;
    }
}

void MemoryPath() {
    DWORD len = GetModuleFileNameW(nullptr, g_memory_path, MAX_PATH);
    wchar_t* slash = len && len < MAX_PATH ? wcsrchr(g_memory_path, L'\\') : nullptr;
    if (!slash || slash + 20 >= g_memory_path + MAX_PATH) {
        g_memory_path[0] = L'\0';
        return;
    }
    wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - g_memory_path), L"mw4hook-input.ini");
}

// [opening] entries left by a crash become [skip]; [skip] entries block.
void LoadCrashMemory() {
    if (!g_memory_path[0] || GetFileAttributesW(g_memory_path) == INVALID_FILE_ATTRIBUTES) {
        return;  // nothing remembered (and no file is created)
    }
    wchar_t keys[4096] = {};
    const bool opening = GetPrivateProfileStringW(L"opening", nullptr, L"", keys, 4096,
                                                  g_memory_path) != 0;
    for (const wchar_t* key = keys; *key; key += wcslen(key) + 1) {
        wchar_t name[128] = {};
        GetPrivateProfileStringW(L"opening", key, L"", name, 128, g_memory_path);
        WritePrivateProfileStringW(L"skip", key, name, g_memory_path);
        char text[160];
        WideCharToMultiByte(CP_UTF8, 0, name, -1, text, sizeof(text), nullptr, nullptr);
        Log("input: the game closed while opening \"%s\" last time; it is skipped from now on "
            "(delete mw4hook-input.ini to try it again)\n",
            text);
    }
    if (opening) {
        WritePrivateProfileStringW(L"opening", nullptr, nullptr, g_memory_path);
    }
    keys[0] = L'\0';
    GetPrivateProfileStringW(L"skip", nullptr, L"", keys, 4096, g_memory_path);
    for (const wchar_t* key = keys; *key; key += wcslen(key) + 1) {
        char guid[40];
        WideCharToMultiByte(CP_UTF8, 0, key, -1, guid, sizeof(guid), nullptr, nullptr);
        Block(guid, "the game closed while opening it before (mw4hook-input.ini [skip])");
    }
}

// One probe round; true when it stopped on a device (then another round
// runs without it, so the devices after it are checked too).
bool RunProbeRound(int round) {
    wchar_t dll[MAX_PATH];
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&RunProbeRound), &self);
    if (!self || !GetModuleFileNameW(self, dll, MAX_PATH)) {
        Log("input: probe not run (module path)\n");
        return false;
    }
    wchar_t system[MAX_PATH];
    UINT n = GetSystemWow64DirectoryW(system, MAX_PATH);  // 32-bit rundll32 on 64-bit Windows
    if (n == 0 || n >= MAX_PATH) {
        n = GetSystemDirectoryW(system, MAX_PATH);
    }
    wchar_t temp[MAX_PATH];
    if (n == 0 || n >= MAX_PATH || !GetTempPathW(MAX_PATH, temp)) {
        Log("input: probe not run (paths)\n");
        return false;
    }
    wchar_t out[MAX_PATH];
    swprintf_s(out, L"%smw4hook-input-%lu.txt", temp, GetCurrentProcessId());
    DeleteFileW(out);
    // Arguments: the devices that failed in earlier rounds, '|', the output.
    wchar_t skip[32 * 40 + 2] = {};
    wcscpy_s(skip, GetConfig().input_other_devices ? L"A" : L"J");
    for (int i = 0; i < g_blocked_count; ++i) {
        wchar_t guid[40];
        MultiByteToWideChar(CP_UTF8, 0, g_blocked[i].guid, -1, guid, 40);
        wcscat_s(skip, guid);
        wcscat_s(skip, L",");
    }
    wchar_t command[4 * MAX_PATH + 32 * 40];
    swprintf_s(command, L"\"%s\\rundll32.exe\" \"%s\",ProbeInputDevices %s|%s", system, dll,
               skip, out);
    wchar_t exe[MAX_PATH];
    swprintf_s(exe, L"%s\\rundll32.exe", system);
    STARTUPINFOW si = {sizeof(si)};
    PROCESS_INFORMATION pi = {};
    const DWORD start = GetTickCount();
    if (!CreateProcessW(exe, command, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                        nullptr, &si, &pi)) {
        Log("input: probe not run (CreateProcess %lu); relying on the crash memory\n",
            GetLastError());
        return false;
    }
    const DWORD waited = WaitForSingleObject(pi.hProcess, kProbeTimeoutMs);
    if (waited != WAIT_OBJECT_0) {
        TerminateProcess(pi.hProcess, 4);
        WaitForSingleObject(pi.hProcess, 1000);
    }
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    char data[8192] = {};
    HANDLE file = CreateFileW(out, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD got = 0;
    if (file != INVALID_HANDLE_VALUE) {
        ReadFile(file, data, sizeof(data) - 1, &got, nullptr);
        CloseHandle(file);
    }
    DeleteFileW(out);
    data[got] = '\0';
    const DWORD elapsed = GetTickCount() - start;
    if (!strstr(data, "start\r\n")) {
        Log("input: probe did not start (exit %lu); relying on the crash memory\n", code);
        return false;
    }
    if (strstr(data, "nodinput\r\n")) {
        Log("input: probe could not create DirectInput\n");
        return false;
    }
    // The last "open" without its "ok" is the device it stopped on.
    char pending[40] = {};
    int checked = 0;
    bool enumerated = false;
    char crash[200] = {};
    char* next = nullptr;
    for (char* line = strtok_s(data, "\r\n", &next); line;
         line = strtok_s(nullptr, "\r\n", &next)) {
        if (strncmp(line, "enum ", 5) == 0) {
            enumerated = true;
        } else if (strncmp(line, "open ", 5) == 0) {
            strncpy_s(pending, line + 5, _TRUNCATE);
        } else if (strncmp(line, "ok ", 3) == 0) {
            pending[0] = '\0';
            ++checked;
        } else if (strncmp(line, "crash ", 6) == 0) {
            strncpy_s(crash, line + 6, _TRUNCATE);
        }
    }
    const char* how = crash[0] ? crash : (waited != WAIT_OBJECT_0 ? "no response" : "exited");
    if (!enumerated) {
        g_enumeration_unsafe = true;
        Log("input: probe: DirectInput failed while listing the devices (%s); the game gets "
            "the keyboard and mouse only\n",
            how);
        return false;
    }
    if (pending[0]) {
        char reason[200];
        sprintf_s(reason, "DirectInput failed opening it in the probe (%s)", how);
        Block(pending, reason);
    }
    Log("input: probe round %d: %d controller(s) opened and read in %lu ms%s%s\n", round,
        checked, elapsed, pending[0] ? "; stopped on one: " : "", pending[0] ? how : "");
    return pending[0] != '\0';
}

}  // namespace

void InputSafetyInit() {
    if (g_initialized) {
        return;
    }
    g_initialized = true;
    MemoryPath();
    LoadCrashMemory();
    if (GetConfig().input_controllers) {
        for (int round = 1; round <= 4 && RunProbeRound(round); ++round) {
        }
    }
}

bool InputEnumerationUnsafe() { return g_enumeration_unsafe; }

const char* InputDeviceBlocked(const GUID& instance) {
    char guid[40];
    FormatGuid(instance, guid);
    for (int i = 0; i < g_blocked_count; ++i) {
        if (strcmp(g_blocked[i].guid, guid) == 0) {
            return g_blocked[i].reason;
        }
    }
    return nullptr;
}

void InputOpenBegin(const GUID& instance, const char* name) {
    if (!g_memory_path[0]) {
        return;
    }
    char guid[40];
    FormatGuid(instance, guid);
    wchar_t key[40];
    wchar_t value[128];
    MultiByteToWideChar(CP_UTF8, 0, guid, -1, key, 40);
    MultiByteToWideChar(CP_UTF8, 0, name ? name : "", -1, value, 128);
    WritePrivateProfileStringW(L"opening", key, value, g_memory_path);
}

void InputOpenEnd(const GUID& instance) {
    if (!g_memory_path[0]) {
        return;
    }
    char guid[40];
    FormatGuid(instance, guid);
    wchar_t key[40];
    MultiByteToWideChar(CP_UTF8, 0, guid, -1, key, 40);
    WritePrivateProfileStringW(L"opening", key, nullptr, g_memory_path);
    // Leave no empty section or file behind.
    wchar_t keys[8] = {};
    if (!GetPrivateProfileStringW(L"opening", nullptr, L"", keys, 8, g_memory_path)) {
        WritePrivateProfileStringW(L"opening", nullptr, nullptr, g_memory_path);
    }
    wchar_t probe[8] = {};
    if (!GetPrivateProfileStringW(nullptr, nullptr, L"", probe, 8, g_memory_path)) {
        DeleteFileW(g_memory_path);
    }
}

}  // namespace mw4hook

// rundll32 "<this dll>",ProbeInputDevices <output file>
extern "C" void CALLBACK ProbeInputDevices(HWND, HINSTANCE, LPSTR command, int) {
    char* bar = command ? strchr(command, '|') : nullptr;
    if (bar && bar[1]) {
        *bar = '\0';
        mw4hook::RunProbe(command, bar + 1);
    }
}
