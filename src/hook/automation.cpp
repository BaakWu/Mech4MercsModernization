// Test automation (development only, [debug] automation=1).
//
// Drives the game from inside the process so tests do not need the real
// cursor, keyboard focus, or a screen capture of the desktop:
//
// - cursor.cpp reads a virtual cursor from here instead of GetCursorPos,
// - DirectInput mouse/keyboard GetDeviceState results get injected buttons
//   and keys,
// - commands are read from mw4hook-cmd.txt in the game directory.
//
// Commands (one per line):
//   move  <x> <y>       client coordinates (physical pixels)
//   smove <x> <y>       800x600 shell coordinates, scaled to the client
//   click               left button down for ~150 ms, then up
//   mouse <dx> <dy> [ms] relative mouse motion (mickeys) spread over ms
//   key   <vk>          press a virtual key (decimal or 0x hex)
//   activate            post WM_ACTIVATEAPP/WM_ACTIVATE to the game window
//   setwindowed <0|1>   call the Windowed backend directly (diagnostics)
//   altenter            post Alt+Enter to the window procedure
//   movewin <x> <y>     move the window (screen coordinates), e.g. to another monitor
//   wait  <ms>
//   shot  <file.bmp>    ask the ddraw proxy to save the next presented frame
//   log   <text>
//
// Nothing here is active unless automation is enabled in mw4hook.ini.
#include "hook.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace mw4hook {
namespace {

CRITICAL_SECTION g_lock;
bool g_active = false;          // a command has been received
POINT g_cursor = {0, 0};        // virtual cursor, client coordinates
volatile LONG g_button_until = 0;
volatile LONG g_key_until = 0;
volatile LONG g_key_code = 0;

// Relative mouse motion ("mouse dx dy ms"), spread evenly over the duration
// and handed out through the DirectInput mouse state.
struct MouseMotion {
    DWORD start = 0;
    DWORD duration = 0;
    LONG total_x = 0;
    LONG total_y = 0;
    LONG sent_x = 0;
    LONG sent_y = 0;
} g_motion;

std::wstring GameDirFile(const wchar_t* name) {
    wchar_t path[MAX_PATH];
    DWORD len = GetModuleFileNameW(nullptr, path, MAX_PATH);
    wchar_t* slash = len ? wcsrchr(path, L'\\') : nullptr;
    if (!slash) {
        return L"";
    }
    slash[1] = L'\0';
    return std::wstring(path) + name;
}

bool ClientOrigin(POINT* origin, SIZE* size) {
    HWND hwnd = MainWindow();
    if (!hwnd) {
        return false;
    }
    RECT client{};
    GetClientRect(hwnd, &client);
    POINT p = {0, 0};
    ClientToScreen(hwnd, &p);
    *origin = p;
    size->cx = client.right;
    size->cy = client.bottom;
    return true;
}

bool Due(volatile LONG* until) {
    LONG value = *until;
    return value != 0 && static_cast<LONG>(GetTickCount() - value) < 0;
}

void KeepAppActive();

void Execute(const std::string& line) {
    char verb[16] = {};
    char rest[MAX_PATH] = {};
    if (sscanf_s(line.c_str(), "%15s %259[^\r\n]", verb,
                 static_cast<unsigned>(sizeof(verb)), rest,
                 static_cast<unsigned>(sizeof(rest))) < 1) {
        return;
    }
    g_active = true;
    KeepAppActive();
    Log("auto: %s %s\n", verb, rest);
    if (_stricmp(verb, "move") == 0 || _stricmp(verb, "smove") == 0) {
        int x = 0;
        int y = 0;
        sscanf_s(rest, "%d %d", &x, &y);
        if (_stricmp(verb, "smove") == 0) {
            POINT mapped{};
            POINT origin{};
            SIZE size{};
            if (ShellToClient(x, y, &mapped)) {
                x = mapped.x;
                y = mapped.y;
            } else if (ClientOrigin(&origin, &size)) {
                x = MulDiv(x, size.cx, 800);
                y = MulDiv(y, size.cy, 600);
            }
        }
        EnterCriticalSection(&g_lock);
        g_cursor.x = x;
        g_cursor.y = y;
        LeaveCriticalSection(&g_lock);
    } else if (_stricmp(verb, "click") == 0) {
        // The game keeps mouse buttons in its key bitmap (VK_LBUTTON), filled
        // by the window procedure; DirectInput is only used for motion.
        // Optional argument: hold time in ms (default 150; list arrows
        // auto-repeat while held).
        int hold = atoi(rest);
        if (hold <= 0) {
            hold = 150;
        }
        HWND hwnd = MainWindow();
        EnterCriticalSection(&g_lock);
        LPARAM where = MAKELPARAM(g_cursor.x, g_cursor.y);
        LeaveCriticalSection(&g_lock);
        InterlockedExchange(&g_button_until, static_cast<LONG>(GetTickCount() + hold));
        if (hwnd) {
            PostMessageA(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, where);
        }
        Sleep(static_cast<DWORD>(hold));
        if (hwnd) {
            PostMessageA(hwnd, WM_LBUTTONUP, 0, where);
        }
        Sleep(150);
    } else if (_stricmp(verb, "key") == 0) {
        // Virtual-key code; posted as window messages and injected into the
        // DirectInput keyboard state (scan code) for code paths that poll it.
        // Optional second argument: hold time in ms (default 150); while held,
        // auto-repeat WM_KEYDOWN messages are posted like a real keyboard.
        char* next = nullptr;
        const UINT vk = static_cast<UINT>(strtol(rest, &next, 0));
        int hold = next ? atoi(next) : 0;
        if (hold <= 0) {
            hold = 150;
        }
        const UINT scan = MapVirtualKeyA(vk, MAPVK_VK_TO_VSC);
        HWND hwnd = MainWindow();
        InterlockedExchange(&g_key_code, static_cast<LONG>(scan));
        InterlockedExchange(&g_key_until, static_cast<LONG>(GetTickCount() + hold));
        if (hwnd) {
            PostMessageA(hwnd, WM_KEYDOWN, vk, 1 | (scan << 16));
        }
        for (int held = 0; held < hold; held += 50) {
            Sleep(50);
            if (hwnd && held >= 500) {
                PostMessageA(hwnd, WM_KEYDOWN, vk, 1 | (scan << 16) | (1 << 30));
            }
        }
        if (hwnd) {
            PostMessageA(hwnd, WM_KEYUP, vk, 1 | (scan << 16) | 0xC0000000);
        }
        Sleep(150);
    } else if (_stricmp(verb, "mouse") == 0) {
        // Relative mouse motion in mickeys, spread over <ms> (default 0 =
        // the next poll). Returns once it has been delivered.
        int dx = 0;
        int dy = 0;
        int ms = 0;
        sscanf_s(rest, "%d %d %d", &dx, &dy, &ms);
        EnterCriticalSection(&g_lock);
        g_motion.start = GetTickCount();
        g_motion.duration = static_cast<DWORD>(ms > 0 ? ms : 0);
        g_motion.total_x = dx;
        g_motion.total_y = dy;
        g_motion.sent_x = 0;
        g_motion.sent_y = 0;
        LeaveCriticalSection(&g_lock);
        Sleep(static_cast<DWORD>(ms > 0 ? ms : 0) + 100);
    } else if (_stricmp(verb, "keydown") == 0 || _stricmp(verb, "keyup") == 0) {
        // Hold or release a key (e.g. Shift) around other commands.
        const UINT vk = static_cast<UINT>(strtol(rest, nullptr, 0));
        const UINT scan = MapVirtualKeyA(vk, MAPVK_VK_TO_VSC);
        HWND hwnd = MainWindow();
        if (hwnd) {
            const bool down = _stricmp(verb, "keydown") == 0;
            PostMessageA(hwnd, down ? WM_KEYDOWN : WM_KEYUP, vk,
                         1 | (scan << 16) | (down ? 0 : 0xC0000000));
        }
        Sleep(100);
    } else if (_stricmp(verb, "activate") == 0) {
        // Tell GameOS it is the active app (exclusive fullscreen ignores
        // input otherwise). Test-only; the real window does not get focus.
        HWND hwnd = MainWindow();
        if (hwnd) {
            PostMessageA(hwnd, WM_ACTIVATEAPP, TRUE, 0);
            PostMessageA(hwnd, WM_ACTIVATE, WA_ACTIVE, 0);
        }
        Sleep(300);
    } else if (_stricmp(verb, "altenter") == 0) {
        // Alt+Enter as the window procedure sees it (context bit 29 = Alt).
        HWND hwnd = MainWindow();
        if (hwnd) {
            const LPARAM down = 1 | (0x1C << 16) | (1 << 29);
            PostMessageA(hwnd, WM_SYSKEYDOWN, VK_RETURN, down);
            PostMessageA(hwnd, WM_SYSKEYUP, VK_RETURN, down | 0xC0000000);
        }
        Sleep(300);
    } else if (_stricmp(verb, "movewin") == 0) {
        // Move the window's top-left corner to screen x,y (e.g. onto another
        // monitor), as a drag would.
        int x = 0;
        int y = 0;
        sscanf_s(rest, "%d %d", &x, &y);
        HWND hwnd = MainWindow();
        if (hwnd) {
            SetWindowPos(hwnd, HWND_TOP, x, y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
        }
        Sleep(300);
    } else if (_stricmp(verb, "setwindowed") == 0) {
        // Calls the same backend as the Video checkbox (diagnostics only).
        SetWindowedMode(atoi(rest) != 0);
    } else if (_stricmp(verb, "wait") == 0) {
        Sleep(static_cast<DWORD>(atoi(rest)));
    } else if (_stricmp(verb, "shot") == 0) {
        std::wstring req = GameDirFile(L"mw4hook-shot.req");
        HANDLE file = CreateFileW(req.c_str(), GENERIC_WRITE, 0, nullptr,
                                  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteFile(file, rest, static_cast<DWORD>(strlen(rest)), &written, nullptr);
            CloseHandle(file);
        }
        // Wait for the ddraw proxy to take the frame.
        for (int i = 0; i < 40 && GetFileAttributesW(req.c_str()) != INVALID_FILE_ATTRIBUTES; ++i) {
            Sleep(50);
        }
    }
}

// GameOS skips its mouse update while the app is inactive (window not in the
// foreground). While automation is driving, keep both activity flags set.
void KeepAppActive() {
    static volatile BYTE* flags[2] = {};
    static bool searched = false;
    if (!searched) {
        searched = true;
        unsigned char* p = FindUnique(
            "mouse_active",
            "A0 ?? ?? ?? ?? 83 EC 1C 84 C0 53 55 56 0F 84 ?? ?? ?? ?? A0 ?? ?? ?? ?? 84 C0 0F 84");
        if (p) {
            flags[0] = reinterpret_cast<BYTE*>(*reinterpret_cast<DWORD*>(p + 1));
            flags[1] = reinterpret_cast<BYTE*>(*reinterpret_cast<DWORD*>(p + 0x14));
        }
    }
    for (volatile BYTE* flag : flags) {
        if (flag) {
            *flag = 1;
        }
    }
}

DWORD WINAPI AutomationThread(LPVOID) {
    const std::wstring cmd = GameDirFile(L"mw4hook-cmd.txt");
    const std::wstring busy = GameDirFile(L"mw4hook-cmd.busy");
    for (;;) {
        Sleep(50);
        if (g_active) {
            KeepAppActive();
        }
        // Rename first so a writer can never race with the reader.
        if (!MoveFileExW(cmd.c_str(), busy.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            continue;
        }
        std::string text;
        HANDLE file = CreateFileW(busy.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            char buffer[4096];
            DWORD read = 0;
            while (ReadFile(file, buffer, sizeof(buffer), &read, nullptr) && read) {
                text.append(buffer, read);
            }
            CloseHandle(file);
        }
        DeleteFileW(busy.c_str());
        size_t start = 0;
        while (start < text.size()) {
            size_t end = text.find('\n', start);
            if (end == std::string::npos) {
                end = text.size();
            }
            Execute(text.substr(start, end - start));
            start = end + 1;
        }
        Log("auto: batch done\n");
    }
}

}  // namespace

void AutomationInit() {
    if (!GetConfig().debug_automation) {
        return;
    }
    InitializeCriticalSection(&g_lock);
    HANDLE thread = CreateThread(nullptr, 0, AutomationThread, nullptr, 0, nullptr);
    if (thread) {
        CloseHandle(thread);
    }
    Log("auto: automation enabled\n");
}

bool AutomationCursor(POINT* client_pos) {
    if (!g_active) {
        return false;
    }
    EnterCriticalSection(&g_lock);
    *client_pos = g_cursor;
    LeaveCriticalSection(&g_lock);
    return true;
}

bool AutomationSetCursor(POINT client_pos) {
    if (!g_active) {
        return false;
    }
    EnterCriticalSection(&g_lock);
    g_cursor = client_pos;
    LeaveCriticalSection(&g_lock);
    return true;
}

// Called by the DirectInput device wrapper after the real GetDeviceState.
void AutomationFilterDeviceState(DWORD size, void* data, HRESULT* result) {
    if (!g_active || !data) {
        return;
    }
    if (size == 16 || size == 20) {  // DIMOUSESTATE / DIMOUSESTATE2
        if (FAILED(*result)) {
            memset(data, 0, size);
            *result = S_OK;
        }
        auto* buttons = static_cast<unsigned char*>(data) + 12;
        buttons[0] = Due(&g_button_until) ? 0x80 : 0x00;
        // The virtual cursor is absolute; real relative motion is suppressed
        // and replaced by the scripted "mouse" motion, if any.
        LONG x = 0;
        LONG y = 0;
        EnterCriticalSection(&g_lock);
        if (g_motion.sent_x != g_motion.total_x || g_motion.sent_y != g_motion.total_y) {
            const DWORD elapsed = GetTickCount() - g_motion.start;
            const double f = g_motion.duration == 0 || elapsed >= g_motion.duration
                                 ? 1.0
                                 : static_cast<double>(elapsed) / g_motion.duration;
            const LONG want_x = static_cast<LONG>(g_motion.total_x * f);
            const LONG want_y = static_cast<LONG>(g_motion.total_y * f);
            x = want_x - g_motion.sent_x;
            y = want_y - g_motion.sent_y;
            g_motion.sent_x = want_x;
            g_motion.sent_y = want_y;
        }
        LeaveCriticalSection(&g_lock);
        static_cast<LONG*>(data)[0] = x;
        static_cast<LONG*>(data)[1] = y;
    } else if (size == 256) {
        if (FAILED(*result)) {
            memset(data, 0, size);
            *result = S_OK;
        }
        LONG code = g_key_code;
        if (code > 0 && code < 256 && Due(&g_key_until)) {
            static_cast<unsigned char*>(data)[code] = 0x80;
        }
    }
}

}  // namespace mw4hook
