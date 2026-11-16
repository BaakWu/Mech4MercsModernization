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
//   - CreateWindowExA: create the main window at the selected client size and
//     subclass it to catch Alt+Enter.
//   - SetWindowPos: when GameOS sizes the window for windowed mode, restore
//     the framed style if the window is still the fullscreen popup, keep the
//     client area at the selected resolution (the shell itself renders at its
//     native 800x600; the ddraw proxy scales it into the window), and center
//     it on the primary monitor's work area when its size changes.
//
// Alt+Enter: GameOS has its own toggle that sets its go-windowed or
// go-fullscreen flag directly, bypassing gos_SetScreenMode. The window
// procedure is subclassed so Alt+Enter flips the Windowed setting instead
// (same path as the checkbox) and GameOS' own toggle never runs.
#include "hook.h"

#include <intrin.h>
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
WNDPROC g_game_wndproc = nullptr;
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

LRESULT CALLBACK MainWindowProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    const bool alt_enter = (msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP ||
                            msg == WM_SYSCHAR) &&
                           wparam == VK_RETURN && (lparam & (1 << 29));
    if (alt_enter && GetConfig().display_enabled) {
        const bool first_press = msg == WM_SYSKEYDOWN && !(lparam & (1 << 30));
        if (first_press) {
            Log("Alt+Enter: Windowed -> %d\n", DesiredWindowed() ? 0 : 1);
            SetWindowedMode(!DesiredWindowed());
        }
        return 0;  // GameOS' own toggle must not run
    }
    return CallWindowProcA(g_game_wndproc, hwnd, msg, wparam, lparam);
}

// Where the main window goes for the current setting.
//   Windowed on:  framed window, client = selected resolution, centered on the
//                 primary monitor (kept where the player moved it unless its
//                 size or frame changes).
//   Windowed off: borderless window covering the whole primary monitor at its
//                 native resolution. The display mode is never changed; the
//                 ddraw proxy scales the picture to fit.
struct Layout {
    DWORD style;
    RECT rect;  // outer window rectangle, screen coordinates
};

RECT PrimaryMonitorRect() {
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    const POINT origin = {0, 0};
    if (GetMonitorInfoW(MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY), &info)) {
        return info.rcMonitor;
    }
    return {0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
}

Layout ComputeLayout(HWND hwnd) {
    Layout layout{};
    if (!DesiredWindowed()) {
        layout.style = WS_POPUP | WS_VISIBLE;
        layout.rect = PrimaryMonitorRect();
        return layout;
    }
    int client_w = 0;
    int client_h = 0;
    SelectedSize(&client_w, &client_h);
    layout.style = g_framed_style | WS_VISIBLE;
    SIZE outer = OuterSize(layout.style, client_w, client_h);
    POINT origin = CenteredOrigin(outer);
    if (hwnd) {
        // Keep a window the player moved where it is, unless the frame or the
        // size changes.
        const DWORD current_style = static_cast<DWORD>(GetWindowLongA(hwnd, GWL_STYLE));
        RECT client{};
        RECT window{};
        GetClientRect(hwnd, &client);
        GetWindowRect(hwnd, &window);
        if (!(current_style & WS_POPUP) && client.right == client_w &&
            client.bottom == client_h) {
            origin = {window.left, window.top};
        }
    }
    layout.rect = {origin.x, origin.y, origin.x + outer.cx, origin.y + outer.cy};
    return layout;
}

void ApplyLayout(HWND hwnd, HWND after, UINT flags) {
    const Layout layout = ComputeLayout(hwnd);
    const DWORD style = static_cast<DWORD>(GetWindowLongA(hwnd, GWL_STYLE));
    if ((style & ~WS_VISIBLE) != (layout.style & ~WS_VISIBLE)) {
        g_SetWindowLongA(hwnd, GWL_STYLE,
                         static_cast<LONG>(layout.style | (style & WS_VISIBLE)));
        flags |= SWP_FRAMECHANGED;
    }
    flags &= ~(SWP_NOMOVE | SWP_NOSIZE);
    g_SetWindowPos(hwnd, after, layout.rect.left, layout.rect.top,
                   layout.rect.right - layout.rect.left,
                   layout.rect.bottom - layout.rect.top, flags);
    // Log only changes; GameOS re-applies its window size often.
    static RECT logged = {};
    static DWORD logged_style = 0;
    if (!EqualRect(&logged, &layout.rect) || logged_style != layout.style) {
        logged = layout.rect;
        logged_style = layout.style;
        LogRect(DesiredWindowed() ? "main window (windowed)" : "main window (borderless)", hwnd);
    }
}

HWND WINAPI Hook_CreateWindowExA(DWORD ex_style, LPCSTR class_name,
                                 LPCSTR window_name, DWORD style, int x, int y,
                                 int width, int height, HWND parent, HMENU menu,
                                 HINSTANCE instance, LPVOID param) {
    const bool main = parent == nullptr && IsGameWindowClass(class_name) && !g_main_window;
    if (main && !(style & WS_POPUP)) {
        g_framed_style = style & ~WS_VISIBLE;
    }
    if (main && GetConfig().window_enabled && g_windowed_launch) {
        const Layout layout = ComputeLayout(nullptr);
        style = (style & WS_VISIBLE) | (layout.style & ~WS_VISIBLE);
        Log("CreateWindowExA main %dx%d -> %ldx%ld at %ld,%ld (%s)\n", width, height,
            layout.rect.right - layout.rect.left, layout.rect.bottom - layout.rect.top,
            layout.rect.left, layout.rect.top, DesiredWindowed() ? "windowed" : "borderless");
        x = layout.rect.left;
        y = layout.rect.top;
        width = layout.rect.right - layout.rect.left;
        height = layout.rect.bottom - layout.rect.top;
    }
    HWND hwnd = g_CreateWindowExA(ex_style, class_name, window_name, style, x, y,
                                  width, height, parent, menu, instance, param);
    if (main && hwnd) {
        g_main_window = hwnd;
        g_game_wndproc = reinterpret_cast<WNDPROC>(SetWindowLongPtrA(
            hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&MainWindowProc)));
        LogRect("main window created", hwnd);
    }
    return hwnd;
}

BOOL WINAPI Hook_SetWindowPos(HWND hwnd, HWND after, int x, int y, int cx, int cy,
                              UINT flags) {
    if (hwnd == g_main_window) {
        static void* callers[8] = {};
        static unsigned counts[8] = {};
        void* caller = _ReturnAddress();
        for (int i = 0; i < 8; ++i) {
            if (!callers[i] || callers[i] == caller) {
                callers[i] = caller;
                const unsigned n = ++counts[i];
                // GameOS calls this from its display setup; a count that keeps
                // doubling means the setup is being re-run (see Clear fix in
                // ddraw_proxy.cpp).
                if ((n & (n - 1)) == 0) {
                    Log("SetWindowPos(main) from %p: %d,%d %dx%d flags=0x%X (call %u)\n", caller,
                        x, y, cx, cy, flags, n);
                }
                break;
            }
        }
    }
    if (hwnd == g_main_window && GetConfig().window_enabled && !GameFullscreen() &&
        !(flags & SWP_NOSIZE) && cx > 0 && cy > 0) {
        // GameOS' windowed setup sizes the window to its render size (800x600
        // for the shell). The window follows the Windowed setting instead.
        ApplyLayout(hwnd, after, flags);
        return TRUE;
    }
    return g_SetWindowPos(hwnd, after, x, y, cx, cy, flags);
}

LONG WINAPI Hook_SetWindowLongA(HWND hwnd, int index, LONG value) {
    if (hwnd == g_main_window && index == GWL_STYLE) {
        Log("SetWindowLongA main style=0x%08lX\n", value);
    }
    return g_SetWindowLongA(hwnd, index, value);
}

}  // namespace

HWND MainWindow() { return g_main_window; }

// Re-applies the window layout after the Windowed setting or the selected
// resolution changed. Runs on the game thread (script callback or the window
// procedure).
void ApplyWindowLayout() {
    if (g_main_window && GetConfig().window_enabled && !GameFullscreen()) {
        ApplyLayout(g_main_window, nullptr, SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
}

void EnsureLaunchMode() {
    std::wstring command_line = GetCommandLineW();
    const size_t at = FindWindowSwitch(command_line);
    const bool has_switch = at != std::wstring::npos;
    // Both settings run on GameOS' windowed path: Windowed off is a borderless
    // window over the whole monitor, not exclusive fullscreen (which changes
    // the display mode and is unreliable on current Windows/drivers).
    const bool want = GetConfig().display_enabled ? true : DesiredWindowed();
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
