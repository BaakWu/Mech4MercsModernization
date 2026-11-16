// Main window placement and launch-mode reconciliation.
//
// The game's -window switch selects GameOS' windowed path at startup. The hook
// makes the switch follow the Windowed setting: if the command line does not
// match, the process is relaunched once with -window added or removed (this
// happens before the game initializes; the first process exits). Changing the
// mode while the game runs is handled by display.cpp through
// gos_SetScreenMode.
//
// Window hooks (user32 imports of MW4Mercs.exe):
//   - CreateWindowExA: create the main window at the selected client size.
//   - SetWindowPos: when GameOS resizes the window for windowed mode, restore
//     the framed style if the window is still the fullscreen popup, keep the
//     client area at the requested size, and center it on the primary
//     monitor's work area.
#include "hook.h"

#include <tlhelp32.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace mw4hook {
namespace {

using CreateWindowExA_t = HWND(WINAPI*)(DWORD, LPCSTR, LPCSTR, DWORD, int, int,
                                        int, int, HWND, HMENU, HINSTANCE, LPVOID);
using SetWindowPos_t = BOOL(WINAPI*)(HWND, HWND, int, int, int, int, UINT);
using SetWindowLongA_t = LONG(WINAPI*)(HWND, int, LONG);

CreateWindowExA_t g_CreateWindowExA = nullptr;
SetWindowPos_t g_SetWindowPos = nullptr;
SetWindowLongA_t g_SetWindowLongA = nullptr;

HWND g_main_window = nullptr;
DWORD g_framed_style = WS_OVERLAPPEDWINDOW & ~(WS_MAXIMIZEBOX | WS_THICKFRAME);
bool g_windowed_launch = false;

constexpr char kGameClass[] = "MechWarrior Mercenaries";
constexpr wchar_t kRelaunchGuard[] = L"MW4HOOK_RELAUNCHED";

bool IsGameWindowClass(const char* class_name) {
    return class_name && !IS_INTRESOURCE(class_name) &&
           _stricmp(class_name, kGameClass) == 0;
}

// Finds a standalone "-window" token (case-insensitive). Returns npos if none.
size_t FindWindowSwitch(const std::wstring& command_line) {
    std::wstring lower = command_line;
    for (wchar_t& c : lower) {
        c = static_cast<wchar_t>(towlower(c));
    }
    size_t at = 0;
    while ((at = lower.find(L"-window", at)) != std::wstring::npos) {
        const bool start_ok = at == 0 || iswspace(lower[at - 1]);
        const size_t end = at + 7;
        const bool end_ok = end == lower.size() || iswspace(lower[end]);
        if (start_ok && end_ok) {
            return at;
        }
        at = end;
    }
    return std::wstring::npos;
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

SIZE OuterSize(DWORD style, int client_w, int client_h) {
    RECT rect = {0, 0, client_w, client_h};
    AdjustWindowRect(&rect, style, FALSE);
    return {rect.right - rect.left, rect.bottom - rect.top};
}

// Top-left that centers an outer window of |size| on the primary monitor's
// work area, clamped so the title bar stays reachable.
POINT CenteredOrigin(SIZE size) {
    RECT work{};
    if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0)) {
        work = {0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    }
    POINT p = {work.left + ((work.right - work.left) - size.cx) / 2,
               work.top + ((work.bottom - work.top) - size.cy) / 2};
    if (p.x < work.left) {
        p.x = work.left;
    }
    if (p.y < work.top) {
        p.y = work.top;
    }
    return p;
}

HWND WINAPI Hook_CreateWindowExA(DWORD ex_style, LPCSTR class_name,
                                 LPCSTR window_name, DWORD style, int x, int y,
                                 int width, int height, HWND parent, HMENU menu,
                                 HINSTANCE instance, LPVOID param) {
    const bool main = parent == nullptr && IsGameWindowClass(class_name) && !g_main_window;
    if (main && GetConfig().window_enabled && g_windowed_launch) {
        int sel_w = 0;
        int sel_h = 0;
        SelectedSize(&sel_w, &sel_h);
        SIZE outer = OuterSize(style, sel_w, sel_h);
        POINT origin = CenteredOrigin(outer);
        Log("CreateWindowExA main %dx%d -> %ldx%ld outer (%dx%d client) at %ld,%ld\n",
            width, height, outer.cx, outer.cy, sel_w, sel_h, origin.x, origin.y);
        width = outer.cx;
        height = outer.cy;
        x = origin.x;
        y = origin.y;
    }
    HWND hwnd = g_CreateWindowExA(ex_style, class_name, window_name, style, x, y,
                                  width, height, parent, menu, instance, param);
    if (main && hwnd) {
        g_main_window = hwnd;
        if (!(style & WS_POPUP)) {
            g_framed_style = style & ~WS_VISIBLE;
        }
        LogRect("main window created", hwnd);
    }
    return hwnd;
}

BOOL WINAPI Hook_SetWindowPos(HWND hwnd, HWND after, int x, int y, int cx, int cy,
                              UINT flags) {
    if (hwnd == g_main_window && !GameFullscreen() && !(flags & SWP_NOSIZE)) {
        // Only GameOS' windowed setup resizes the main window.
        NoteAppliedMode(true);
    }
    if (hwnd == g_main_window && GetConfig().window_enabled && !GameFullscreen() &&
        !(flags & SWP_NOSIZE) && cx > 0 && cy > 0) {
        // GameOS sized cx/cy with AdjustWindowRect for the current style.
        DWORD style = static_cast<DWORD>(GetWindowLongA(hwnd, GWL_STYLE));
        RECT probe = {0, 0, 0, 0};
        AdjustWindowRect(&probe, style, FALSE);
        int client_w = cx - (probe.right - probe.left);
        int client_h = cy - (probe.bottom - probe.top);
        bool restored = false;
        if (style & WS_POPUP) {
            // Returning from fullscreen: put the normal window frame back.
            DWORD framed = g_framed_style | (style & WS_VISIBLE);
            g_SetWindowLongA(hwnd, GWL_STYLE, static_cast<LONG>(framed));
            style = framed;
            flags |= SWP_FRAMECHANGED;
            restored = true;
            Log("SetWindowPos: restored framed style 0x%08lX\n", framed);
        }
        SIZE outer = OuterSize(style, client_w, client_h);
        RECT current{};
        GetClientRect(hwnd, &current);
        // Re-center only when the size or frame changes; GameOS passes
        // SWP_FRAMECHANGED on every mode set, and a window the player moved
        // should stay where it is.
        const bool resized =
            restored || current.right != client_w || current.bottom != client_h;
        if (resized) {
            POINT origin = CenteredOrigin(outer);
            x = origin.x;
            y = origin.y;
            flags &= ~SWP_NOMOVE;
        }
        Log("SetWindowPos main: client %dx%d outer %ldx%ld%s\n", client_w, client_h,
            outer.cx, outer.cy, resized ? " (centered)" : "");
        cx = outer.cx;
        cy = outer.cy;
        BOOL result = g_SetWindowPos(hwnd, after, x, y, cx, cy, flags);
        LogRect("main window", hwnd);
        return result;
    }
    return g_SetWindowPos(hwnd, after, x, y, cx, cy, flags);
}

LONG WINAPI Hook_SetWindowLongA(HWND hwnd, int index, LONG value) {
    if (hwnd == g_main_window && index == GWL_STYLE) {
        Log("SetWindowLongA main style=0x%08lX\n", value);
        if (static_cast<DWORD>(value) & WS_POPUP) {
            // Only GameOS' fullscreen setup makes the window a popup.
            NoteAppliedMode(false);
        }
    }
    return g_SetWindowLongA(hwnd, index, value);
}

}  // namespace

HWND MainWindow() { return g_main_window; }

void EnsureLaunchMode() {
    std::wstring command_line = GetCommandLineW();
    const size_t at = FindWindowSwitch(command_line);
    const bool has_switch = at != std::wstring::npos;
    const bool want = DesiredWindowed();
    g_windowed_launch = has_switch;
    Log("launch: -window %s, Windowed setting %s\n", has_switch ? "present" : "absent",
        want ? "on" : "off");
    if (!GetConfig().relaunch || has_switch == want) {
        return;
    }
    wchar_t guard[4] = {};
    if (GetEnvironmentVariableW(kRelaunchGuard, guard, 4)) {
        // Already relaunched once; never loop. display.cpp switches the mode
        // at runtime instead.
        Log("launch: relaunch guard set; continuing without relaunch\n");
        return;
    }
    if (want) {
        command_line += L" -window";
    } else {
        size_t erase_from = at;
        size_t erase_len = 7;
        if (erase_from > 0 && iswspace(command_line[erase_from - 1])) {
            --erase_from;
            ++erase_len;
        }
        command_line.erase(erase_from, erase_len);
    }
    SetEnvironmentVariableW(kRelaunchGuard, L"1");
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (CreateProcessW(nullptr, &command_line[0], nullptr, nullptr, FALSE, 0, nullptr,
                       nullptr, &startup, &process)) {
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        Log("launch: relaunched as: %ls\n", command_line.c_str());
        TerminateProcess(GetCurrentProcess(), 0);
        return;
    }
    Log("launch: relaunch failed (%lu)\n", GetLastError());
    SetEnvironmentVariableW(kRelaunchGuard, nullptr);
}

namespace {

int g_hooked_count = 0;
bool g_count_hooks = false;

void HookOne(HMODULE module, const char* name, void* detour, void** original) {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32) {
        return;
    }
    void* target = reinterpret_cast<void*>(GetProcAddress(user32, name));
    if (!target) {
        return;
    }
    void* previous = *original;
    PatchIatByAddress(module, "USER32.dll", user32, target, detour, original);
    if (g_count_hooks && previous == nullptr && *original != nullptr) {
        ++g_hooked_count;
    }
}

}  // namespace

int InstallWindowHooks() {
    HMODULE exe = GetModuleHandleW(nullptr);
    if (!exe) {
        return 0;
    }
    g_hooked_count = 0;
    g_count_hooks = true;
    HookOne(exe, "CreateWindowExA", reinterpret_cast<void*>(&Hook_CreateWindowExA),
            reinterpret_cast<void**>(&g_CreateWindowExA));
    HookOne(exe, "SetWindowPos", reinterpret_cast<void*>(&Hook_SetWindowPos),
            reinterpret_cast<void**>(&g_SetWindowPos));
    HookOne(exe, "SetWindowLongA", reinterpret_cast<void*>(&Hook_SetWindowLongA),
            reinterpret_cast<void**>(&g_SetWindowLongA));
    g_count_hooks = false;
    InstallCursorHooks(exe);
    return g_hooked_count;
}

}  // namespace mw4hook
