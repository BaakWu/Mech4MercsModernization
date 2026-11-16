// Cursor mapping for the scaled picture.
//
// GameOS reads the cursor with GetCursorPos, subtracts the client origin and
// normalizes by its render size; the shell then hit-tests (and draws its
// cursor) in render pixels. When the ddraw proxy scales the picture into the
// window (the 800x600 menus in a larger window, or a mission in the
// borderless fullscreen window), the visible position is mapped from the
// scaled viewport into the picture's own pixels here; SetCursorPos gets the
// inverse. When the picture is not scaled the cursor is untouched.
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

// True while the picture is shown scaled: |view| is where (client pixels),
// |size| the picture's own size (render pixels).
bool ScaledView(RECT* view, SIZE* size) {
    FrontendViewport_t viewport = ResolveViewport();
    if (!viewport || !viewport(view, size)) {
        return false;
    }
    return view->right > view->left && view->bottom > view->top && size->cx > 0 &&
           size->cy > 0;
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
    SIZE size{};
    if (ScaledView(&view, &size)) {
        client_pos.x = MulDiv(Clamp(client_pos.x, view.left, view.right - 1) - view.left,
                              size.cx, view.right - view.left);
        client_pos.y = MulDiv(Clamp(client_pos.y, view.top, view.bottom - 1) - view.top,
                              size.cy, view.bottom - view.top);
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
    SIZE size{};
    if (ScaledView(&view, &size)) {
        client_pos.x = view.left + MulDiv(client_pos.x, view.right - view.left, size.cx);
        client_pos.y = view.top + MulDiv(client_pos.y, view.bottom - view.top, size.cy);
    }
    if (AutomationSetCursor(client_pos)) {
        return TRUE;
    }
    return g_SetCursorPos(origin.x + client_pos.x, origin.y + client_pos.y);
}

}  // namespace

// Visible client position of a point in the scaled picture (tests).
bool ShellToClient(int x, int y, POINT* client_pos) {
    RECT view{};
    SIZE size{};
    if (!ScaledView(&view, &size)) {
        return false;
    }
    client_pos->x = view.left + MulDiv(x, view.right - view.left, size.cx);
    client_pos->y = view.top + MulDiv(y, view.bottom - view.top, size.cy);
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
