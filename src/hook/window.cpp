// Window sizing and startup command-line hooks.
//
// The retail 3.1c executable's -window switch always creates an 800x600 client
// area. These hooks force the main window to the configured size. The
// command-line hook appends -window before the game parses its arguments, so
// starting MW4Mercs.exe directly uses the game's normal windowed path instead
// of switching the monitor to a fullscreen mode.
#include "hook.h"

#include <tlhelp32.h>
#include <winternl.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace mw4hook {
namespace {

using CreateWindowExA_t = HWND(WINAPI*)(DWORD, LPCSTR, LPCSTR, DWORD, int, int,
                                        int, int, HWND, HMENU, HINSTANCE, LPVOID);
using CreateWindowExW_t = HWND(WINAPI*)(DWORD, LPCWSTR, LPCWSTR, DWORD, int, int,
                                        int, int, HWND, HMENU, HINSTANCE, LPVOID);
using SetWindowPos_t = BOOL(WINAPI*)(HWND, HWND, int, int, int, int, UINT);
using AdjustWindowRect_t = BOOL(WINAPI*)(LPRECT, DWORD, BOOL);
using AdjustWindowRectEx_t = BOOL(WINAPI*)(LPRECT, DWORD, BOOL, DWORD);
using ShowWindow_t = BOOL(WINAPI*)(HWND, int);
using SetWindowLongA_t = LONG(WINAPI*)(HWND, int, LONG);
using SetWindowLongW_t = LONG(WINAPI*)(HWND, int, LONG);

CreateWindowExA_t g_CreateWindowExA = nullptr;
CreateWindowExW_t g_CreateWindowExW = nullptr;
SetWindowPos_t g_SetWindowPos = nullptr;
AdjustWindowRect_t g_AdjustWindowRect = nullptr;
AdjustWindowRectEx_t g_AdjustWindowRectEx = nullptr;
ShowWindow_t g_ShowWindow = nullptr;
SetWindowLongA_t g_SetWindowLongA = nullptr;
SetWindowLongW_t g_SetWindowLongW = nullptr;

HWND g_main_window = nullptr;
bool g_forced_main_window = false;
bool g_override_next_setwindowpos = false;

bool IsGameWindowClass(const char* class_name) {
    return class_name && _stricmp(class_name, "MechWarrior Mercenaries") == 0;
}

bool IsGameWindowClassW(const wchar_t* class_name) {
    return class_name && _wcsicmp(class_name, L"MechWarrior Mercenaries") == 0;
}

void LogRect(const char* label, HWND hwnd) {
    RECT client{};
    RECT window{};
    GetClientRect(hwnd, &client);
    GetWindowRect(hwnd, &window);
    Log("%s: client=%dx%d window=%dx%d at %d,%d style=0x%08lX\n", label,
        client.right - client.left, client.bottom - client.top,
        window.right - window.left, window.bottom - window.top, window.left,
        window.top, GetWindowLongA(hwnd, GWL_STYLE));
}

void OuterSizeForClient(DWORD style, BOOL menu, int client_w, int client_h,
                        int* outer_w, int* outer_h) {
    RECT rect = {0, 0, client_w, client_h};
    if (g_AdjustWindowRect) {
        g_AdjustWindowRect(&rect, style, menu);
    }
    *outer_w = rect.right - rect.left;
    *outer_h = rect.bottom - rect.top;
}

void RegisterMainWindow(HWND hwnd, bool forced) {
    if (g_main_window || !hwnd) {
        return;
    }
    g_main_window = hwnd;
    if (forced) {
        g_forced_main_window = true;
        g_override_next_setwindowpos = true;
    }
    char title[256] = {};
    GetWindowTextA(hwnd, title, sizeof(title));
    Log("main window: hwnd=%p title=\"%s\"\n", hwnd, title);
    LogRect("main window after creation", hwnd);

    // The game's -window placement can land partly off-screen. Center the
    // outer window on the primary work area after creation.
    RECT work{};
    RECT window{};
    if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0) &&
        GetWindowRect(hwnd, &window)) {
        const int width = window.right - window.left;
        const int height = window.bottom - window.top;
        const int x = work.left + ((work.right - work.left) - width) / 2;
        const int y = work.top + ((work.bottom - work.top) - height) / 2;
        SetWindowPos(hwnd, nullptr, x, y, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        Log("main window: centered at %d,%d\n", x, y);
    }
}

HWND WINAPI Hook_CreateWindowExA(DWORD ex_style, LPCSTR class_name,
                                 LPCSTR window_name, DWORD style, int x, int y,
                                 int width, int height, HWND parent, HMENU menu,
                                 HINSTANCE instance, LPVOID param) {
    const Config& config = GetConfig();
    bool forcing = config.window_enabled && !g_forced_main_window &&
                   parent == nullptr && IsGameWindowClass(class_name);

    if (forcing) {
        if (config.force_windowed) {
            style &= ~static_cast<DWORD>(WS_POPUP);
            style |= WS_OVERLAPPEDWINDOW;
            Log("windowed: forced overlapped style 0x%08lX\n", style);
        }
        int outer_w = 0;
        int outer_h = 0;
        OuterSizeForClient(style, menu != nullptr, config.window_width,
                           config.window_height, &outer_w, &outer_h);
        Log("window override: %dx%d -> %dx%d outer (%dx%d client)\n", width,
            height, outer_w, outer_h, config.window_width, config.window_height);
        width = outer_w;
        height = outer_h;
    }

    Log("CreateWindowExA class=\"%s\" title=\"%s\" style=0x%08lX size=%dx%d "
        "pos=%d,%d\n",
        class_name ? class_name : "", window_name ? window_name : "", style,
        width, height, x, y);

    HWND hwnd = g_CreateWindowExA(ex_style, class_name, window_name, style, x, y,
                                  width, height, parent, menu, instance, param);
    if (parent == nullptr && (forcing || IsGameWindowClass(class_name))) {
        RegisterMainWindow(hwnd, forcing);
    }
    return hwnd;
}

HWND WINAPI Hook_CreateWindowExW(DWORD ex_style, LPCWSTR class_name,
                                 LPCWSTR window_name, DWORD style, int x, int y,
                                 int width, int height, HWND parent, HMENU menu,
                                 HINSTANCE instance, LPVOID param) {
    const Config& config = GetConfig();
    bool forcing = config.window_enabled && !g_forced_main_window &&
                   parent == nullptr && IsGameWindowClassW(class_name);

    if (forcing) {
        if (config.force_windowed) {
            style &= ~static_cast<DWORD>(WS_POPUP);
            style |= WS_OVERLAPPEDWINDOW;
            Log("windowed: forced overlapped style (W) 0x%08lX\n", style);
        }
        int outer_w = 0;
        int outer_h = 0;
        OuterSizeForClient(style, menu != nullptr, config.window_width,
                           config.window_height, &outer_w, &outer_h);
        Log("window override (W): %dx%d -> %dx%d outer (%dx%d client)\n", width,
            height, outer_w, outer_h, config.window_width, config.window_height);
        width = outer_w;
        height = outer_h;
    }

    HWND hwnd = g_CreateWindowExW(ex_style, class_name, window_name, style, x, y,
                                  width, height, parent, menu, instance, param);
    if (parent == nullptr && IsGameWindowClassW(class_name)) {
        RegisterMainWindow(hwnd, forcing);
    }
    return hwnd;
}

BOOL WINAPI Hook_SetWindowPos(HWND hwnd, HWND after, int x, int y, int cx, int cy,
                              UINT flags) {
    const Config& config = GetConfig();
    if (hwnd == g_main_window) {
        Log("SetWindowPos main=%p size=%dx%d flags=0x%04X\n", hwnd, cx, cy, flags);
    }

    if (hwnd == g_main_window && config.force_windowed) {
        int outer_w = 0;
        int outer_h = 0;
        OuterSizeForClient(GetWindowLongA(hwnd, GWL_STYLE), FALSE,
                           config.window_width, config.window_height, &outer_w,
                           &outer_h);
        const bool size_requested = !(flags & SWP_NOSIZE) && cx > 0 && cy > 0;
        if (!size_requested || cx != outer_w || cy != outer_h ||
            (flags & SWP_NOMOVE)) {
            RECT work{};
            SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
            x = work.left + ((work.right - work.left) - outer_w) / 2;
            y = work.top + ((work.bottom - work.top) - outer_h) / 2;
            cx = outer_w;
            cy = outer_h;
            flags &= ~static_cast<UINT>(SWP_NOSIZE | SWP_NOMOVE);
            Log("windowed: forced SetWindowPos %dx%d at %d,%d flags=0x%04X\n",
                cx, cy, x, y, flags);
        }
    } else if (hwnd == g_main_window && g_override_next_setwindowpos &&
               config.window_enabled && !(flags & SWP_NOSIZE) && cx > 0 &&
               cy > 0) {
        int outer_w = 0;
        int outer_h = 0;
        OuterSizeForClient(GetWindowLongA(hwnd, GWL_STYLE), FALSE,
                           config.window_width, config.window_height, &outer_w,
                           &outer_h);
        Log("SetWindowPos override: %dx%d -> %dx%d\n", cx, cy, outer_w, outer_h);
        cx = outer_w;
        cy = outer_h;
        g_override_next_setwindowpos = false;
    }

    BOOL result = g_SetWindowPos(hwnd, after, x, y, cx, cy, flags);
    if (hwnd == g_main_window) {
        LogRect("main window after SetWindowPos", hwnd);
    }
    return result;
}

BOOL WINAPI Hook_AdjustWindowRect(LPRECT rect, DWORD style, BOOL menu) {
    RECT before = *rect;
    BOOL result = g_AdjustWindowRect(rect, style, menu);
    Log("AdjustWindowRect in=%dx%d style=0x%08lX out=%dx%d\n",
        before.right - before.left, before.bottom - before.top, style,
        rect->right - rect->left, rect->bottom - rect->top);
    return result;
}

BOOL WINAPI Hook_AdjustWindowRectEx(LPRECT rect, DWORD style, BOOL menu,
                                    DWORD ex_style) {
    RECT before = *rect;
    BOOL result = g_AdjustWindowRectEx(rect, style, menu, ex_style);
    Log("AdjustWindowRectEx in=%dx%d style=0x%08lX ex=0x%08lX out=%dx%d\n",
        before.right - before.left, before.bottom - before.top, style, ex_style,
        rect->right - rect->left, rect->bottom - rect->top);
    return result;
}

BOOL WINAPI Hook_ShowWindow(HWND hwnd, int cmd) {
    Log("ShowWindow hwnd=%p cmd=%d\n", hwnd, cmd);
    return g_ShowWindow(hwnd, cmd);
}

LONG WINAPI Hook_SetWindowLongA(HWND hwnd, int index, LONG value) {
    const Config& config = GetConfig();
    if (config.force_windowed && hwnd == g_main_window && index == GWL_STYLE) {
        LONG forced = (value & ~static_cast<LONG>(WS_POPUP)) | WS_OVERLAPPEDWINDOW;
        Log("windowed: SetWindowLongA style 0x%08lX -> 0x%08lX\n", value, forced);
        value = forced;
    }
    if (hwnd == g_main_window || index == GWL_STYLE || index == GWL_EXSTYLE) {
        Log("SetWindowLongA hwnd=%p index=%d value=0x%08lX\n", hwnd, index, value);
    }
    return g_SetWindowLongA(hwnd, index, value);
}

LONG WINAPI Hook_SetWindowLongW(HWND hwnd, int index, LONG value) {
    const Config& config = GetConfig();
    if (config.force_windowed && hwnd == g_main_window && index == GWL_STYLE) {
        LONG forced = (value & ~static_cast<LONG>(WS_POPUP)) | WS_OVERLAPPEDWINDOW;
        Log("windowed: SetWindowLongW style 0x%08lX -> 0x%08lX\n", value, forced);
        value = forced;
    }
    if (hwnd == g_main_window || index == GWL_STYLE || index == GWL_EXSTYLE) {
        Log("SetWindowLongW hwnd=%p index=%d value=0x%08lX\n", hwnd, index, value);
    }
    return g_SetWindowLongW(hwnd, index, value);
}

}  // namespace

namespace {

int g_hooked_count = 0;
bool g_count_hooks = false;

void HookOne(HMODULE module, const char* dll_name, const wchar_t* module_name,
             const char* name, void* detour, void** original) {
    HMODULE target_module = GetModuleHandleW(module_name);
    if (!target_module) {
        return;
    }
    void* target = reinterpret_cast<void*>(GetProcAddress(target_module, name));
    if (!target) {
        return;
    }
    void* previous = *original;
    PatchIatByAddress(module, dll_name, target_module, target, detour, original);
    if (g_count_hooks && previous == nullptr && *original != nullptr) {
        ++g_hooked_count;
    }
}

void HookWindowModule(HMODULE module) {
    HookOne(module, "USER32.dll", L"user32.dll", "CreateWindowExA",
            reinterpret_cast<void*>(&Hook_CreateWindowExA),
            reinterpret_cast<void**>(&g_CreateWindowExA));
    HookOne(module, "USER32.dll", L"user32.dll", "CreateWindowExW",
            reinterpret_cast<void*>(&Hook_CreateWindowExW),
            reinterpret_cast<void**>(&g_CreateWindowExW));
    HookOne(module, "USER32.dll", L"user32.dll", "SetWindowPos",
            reinterpret_cast<void*>(&Hook_SetWindowPos),
            reinterpret_cast<void**>(&g_SetWindowPos));
    HookOne(module, "USER32.dll", L"user32.dll", "AdjustWindowRect",
            reinterpret_cast<void*>(&Hook_AdjustWindowRect),
            reinterpret_cast<void**>(&g_AdjustWindowRect));
    HookOne(module, "USER32.dll", L"user32.dll", "AdjustWindowRectEx",
            reinterpret_cast<void*>(&Hook_AdjustWindowRectEx),
            reinterpret_cast<void**>(&g_AdjustWindowRectEx));
    HookOne(module, "USER32.dll", L"user32.dll", "ShowWindow",
            reinterpret_cast<void*>(&Hook_ShowWindow),
            reinterpret_cast<void**>(&g_ShowWindow));
    HookOne(module, "USER32.dll", L"user32.dll", "SetWindowLongA",
            reinterpret_cast<void*>(&Hook_SetWindowLongA),
            reinterpret_cast<void**>(&g_SetWindowLongA));
    HookOne(module, "USER32.dll", L"user32.dll", "SetWindowLongW",
            reinterpret_cast<void*>(&Hook_SetWindowLongW),
            reinterpret_cast<void**>(&g_SetWindowLongW));
}

}  // namespace

void InstallCommandLineHooks() {
    if (!GetConfig().force_windowed) {
        return;
    }
    auto* peb = NtCurrentTeb()->ProcessEnvironmentBlock;
    if (!peb || !peb->ProcessParameters) {
        Log("command line: PEB unavailable\n");
        return;
    }
    UNICODE_STRING& command_line = peb->ProcessParameters->CommandLine;
    if (!command_line.Buffer || command_line.Length == 0) {
        Log("command line: empty\n");
        return;
    }

    std::wstring current(command_line.Buffer,
                         command_line.Length / sizeof(wchar_t));
    if (current.find(L"-window") != std::wstring::npos) {
        Log("command line: -window already present\n");
        return;
    }

    std::wstring updated = current + L" -window";
    USHORT new_length =
        static_cast<USHORT>(updated.size() * sizeof(wchar_t));
    USHORT new_max = static_cast<USHORT>(new_length + sizeof(wchar_t));
    if (new_max <= command_line.MaximumLength && command_line.Buffer) {
        memcpy(command_line.Buffer, updated.c_str(), new_max);
    } else {
        auto* buffer = static_cast<wchar_t*>(
            HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, new_max));
        if (!buffer) {
            Log("command line: allocation failed\n");
            return;
        }
        memcpy(buffer, updated.c_str(), new_max);
        command_line.Buffer = buffer;
        command_line.MaximumLength = new_max;
    }
    command_line.Length = new_length;
    Log("command line: appended -window: %ls\n", updated.c_str());
}

void MaintainWindow() {
    const Config& config = GetConfig();
    if (!config.force_windowed || !config.window_enabled || !g_main_window) {
        return;
    }
    HWND hwnd = g_main_window;

    LONG style = GetWindowLongA(hwnd, GWL_STYLE);
    bool style_changed = false;
    if (config.force_windowed && (style & WS_POPUP)) {
        style = (style & ~static_cast<LONG>(WS_POPUP)) | WS_OVERLAPPEDWINDOW;
        SetWindowLongA(hwnd, GWL_STYLE, style);
        style_changed = true;
    }

    RECT client{};
    GetClientRect(hwnd, &client);
    const int client_w = client.right - client.left;
    const int client_h = client.bottom - client.top;
    if (client_w == config.window_width && client_h == config.window_height &&
        !style_changed) {
        return;
    }

    int outer_w = 0;
    int outer_h = 0;
    OuterSizeForClient(style, FALSE, config.window_width, config.window_height,
                       &outer_w, &outer_h);
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int x = work.left + ((work.right - work.left) - outer_w) / 2;
    const int y = work.top + ((work.bottom - work.top) - outer_h) / 2;
    SetWindowPos(hwnd, nullptr, x, y, outer_w, outer_h,
                 SWP_NOZORDER | SWP_NOACTIVATE);

    static int restores = 0;
    if (restores < 5) {
        ++restores;
        Log("maintain: restored window to %dx%d client at %d,%d\n",
            config.window_width, config.window_height, x, y);
    }
}

int InstallWindowHooks() {
    HMODULE exe = GetModuleHandleW(nullptr);
    if (!exe) {
        return 0;
    }

    g_hooked_count = 0;
    g_count_hooks = true;
    HookWindowModule(exe);
    g_count_hooks = false;

    wchar_t gameDir[MAX_PATH] = {};
    if (GetModuleFileNameW(exe, gameDir, MAX_PATH)) {
        wchar_t* slash = wcsrchr(gameDir, L'\\');
        if (slash) {
            slash[1] = L'\0';
        }
    }

    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                       reinterpret_cast<LPCWSTR>(&HookWindowModule), &self);

    HANDLE snapshot =
        CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snapshot != INVALID_HANDLE_VALUE) {
        MODULEENTRY32 entry{};
        entry.dwSize = sizeof(entry);
        if (Module32First(snapshot, &entry)) {
            do {
                if (entry.hModule == exe || entry.hModule == self) {
                    continue;
                }
                if (gameDir[0] &&
                    _wcsnicmp(entry.szExePath, gameDir, wcslen(gameDir)) == 0) {
                    HookWindowModule(entry.hModule);
                }
            } while (Module32Next(snapshot, &entry));
        }
        CloseHandle(snapshot);
    }
    return g_hooked_count;
}

}  // namespace mw4hook
