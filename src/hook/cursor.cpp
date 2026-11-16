// Cursor mapping for the scaled frontend.
//
// GameOS reads the cursor with GetCursorPos, subtracts the client origin and
// normalizes by the render size; the shell then hit-tests (and draws its
// cursor) in render pixels. The shell is laid out in the top-left 800x600 of
// the render surface, and the ddraw proxy scales that area into a 4:3
// viewport of the window. So while frontend frames are shown, the visible
// position is mapped from the viewport into the 800x600 shell space here;
// SetCursorPos gets the inverse. In missions the cursor is untouched.
#include "hook.h"

namespace mw4hook {
namespace {

using GetCursorPos_t = BOOL(WINAPI*)(LPPOINT);
using SetCursorPos_t = BOOL(WINAPI*)(int, int);
using FrontendViewport_t = BOOL(WINAPI*)(RECT*, SIZE*);

GetCursorPos_t g_GetCursorPos = nullptr;
SetCursorPos_t g_SetCursorPos = nullptr;
FrontendViewport_t g_frontend_viewport = nullptr;

FrontendViewport_t ResolveViewport() {
    static bool searched = false;
    if (!searched) {
        // The game directory's ddraw.dll is the proxy; the system ddraw.dll
        // shares the base name, so look it up by full path.
        wchar_t path[MAX_PATH];
        DWORD len = GetModuleFileNameW(nullptr, path, MAX_PATH);
        wchar_t* slash = len ? wcsrchr(path, L'\\') : nullptr;
        if (slash) {
            wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - path), L"ddraw.dll");
            HMODULE proxy = GetModuleHandleW(path);
            if (proxy) {
                searched = true;
                g_frontend_viewport = reinterpret_cast<FrontendViewport_t>(
                    GetProcAddress(proxy, "Mw4FrontendViewport"));
            }
        }
    }
    return g_frontend_viewport;
}

bool ClientOrigin(POINT* origin) {
    HWND hwnd = MainWindow();
    if (!hwnd) {
        return false;
    }
    POINT p = {0, 0};
    ClientToScreen(hwnd, &p);
    *origin = p;
    return true;
}

constexpr int kShellWidth = 800;
constexpr int kShellHeight = 600;

// True while the frontend is shown scaled (the viewport is in client pixels).
bool FrontendView(RECT* view) {
    FrontendViewport_t viewport = ResolveViewport();
    SIZE client{};
    if (ShellMode() == 0 || !viewport || !viewport(view, &client)) {
        return false;
    }
    return view->right > view->left && view->bottom > view->top;
}

LONG Clamp(LONG v, LONG lo, LONG hi) { return v < lo ? lo : (v > hi ? hi : v); }

BOOL WINAPI Hook_GetCursorPos(LPPOINT point) {
    if (!point) {
        return g_GetCursorPos(point);
    }
    POINT origin{};
    POINT client_pos{};
    const bool have_origin = ClientOrigin(&origin);
    if (have_origin && AutomationCursor(&client_pos)) {
        // virtual cursor (tests)
    } else {
        if (!g_GetCursorPos(point)) {
            return FALSE;
        }
        if (!have_origin) {
            return TRUE;
        }
        client_pos = {point->x - origin.x, point->y - origin.y};
    }
    RECT view{};
    if (FrontendView(&view)) {
        client_pos.x = MulDiv(Clamp(client_pos.x, view.left, view.right - 1) - view.left,
                              kShellWidth, view.right - view.left);
        client_pos.y = MulDiv(Clamp(client_pos.y, view.top, view.bottom - 1) - view.top,
                              kShellHeight, view.bottom - view.top);
    }
    point->x = origin.x + client_pos.x;
    point->y = origin.y + client_pos.y;
    return TRUE;
}

BOOL WINAPI Hook_SetCursorPos(int x, int y) {
    POINT origin{};
    if (!ClientOrigin(&origin)) {
        return g_SetCursorPos(x, y);
    }
    POINT client_pos = {x - origin.x, y - origin.y};
    RECT view{};
    if (FrontendView(&view)) {
        client_pos.x = view.left + MulDiv(client_pos.x, view.right - view.left, kShellWidth);
        client_pos.y = view.top + MulDiv(client_pos.y, view.bottom - view.top, kShellHeight);
    }
    if (AutomationSetCursor(client_pos)) {
        return TRUE;
    }
    return g_SetCursorPos(origin.x + client_pos.x, origin.y + client_pos.y);
}

}  // namespace

// Visible client position of a point in the 800x600 shell (tests).
bool ShellToClient(int x, int y, POINT* client_pos) {
    RECT view{};
    if (!FrontendView(&view)) {
        return false;
    }
    client_pos->x = view.left + MulDiv(x, view.right - view.left, kShellWidth);
    client_pos->y = view.top + MulDiv(y, view.bottom - view.top, kShellHeight);
    return true;
}

void InstallCursorHooks(HMODULE module) {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32) {
        return;
    }
    PatchIatByAddress(module, "USER32.dll", user32,
                      reinterpret_cast<void*>(GetProcAddress(user32, "GetCursorPos")),
                      reinterpret_cast<void*>(&Hook_GetCursorPos),
                      reinterpret_cast<void**>(&g_GetCursorPos));
    PatchIatByAddress(module, "USER32.dll", user32,
                      reinterpret_cast<void*>(GetProcAddress(user32, "SetCursorPos")),
                      reinterpret_cast<void*>(&Hook_SetCursorPos),
                      reinterpret_cast<void**>(&g_SetCursorPos));
}

}  // namespace mw4hook
