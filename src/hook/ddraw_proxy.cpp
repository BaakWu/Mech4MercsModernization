// ddraw.dll proxy: scale the windowed picture to the window size.
//
// In windowed mode the window's client area is the selected resolution
// (window.cpp), while the game renders at its own size: 800x600 for the 2002
// shell (menus, MechLab, movies, the in-mission options overlay) and the
// selected size in missions. This proxy patches the real DirectDraw surface
// vtable; when the present (a Blt onto the primary surface) does not match
// the client area, the picture is drawn scaled into the client with its
// aspect ratio kept and black bars where it does not fill. Mission frames
// already match and pass through unchanged.
//
// A window that is not entirely on the main monitor (the only area the
// primary surface covers) is presented through the window's DC instead.
//
// Direct3D 7 fixes (see docs/HOOK.md): a failing device Clear made GameOS
// rebuild its whole display setup every frame; a 32-bit depth-buffer request
// in a layout current drivers reject made it fall back to 16-bit depth.
//
// The scaled viewport is exported (Mw4FrontendViewport) while the shell is
// shown, so the dinput proxy can map the cursor to match.
//
// Development only ([debug] automation=1): mw4hook-shot.req captures the
// presented frame to a BMP.
//
// This is a runtime hook only. The game executable on disk is not modified.
#include <windows.h>

#include <ddraw.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace {

HMODULE g_real_ddraw = nullptr;
HANDLE g_log = INVALID_HANDLE_VALUE;
bool g_stretch_enabled = true;
bool g_capture_enabled = false;
bool g_clear_fix = true;
bool g_zbuffer_fix = true;

void Log(const char* fmt, ...) {
    if (g_log == INVALID_HANDLE_VALUE) {
        return;
    }
    char buffer[1024];
    va_list args;
    va_start(args, fmt);
    int n = _vsnprintf_s(buffer, sizeof(buffer), _TRUNCATE, fmt, args);
    va_end(args);
    if (n <= 0) {
        return;
    }
    DWORD written = 0;
    WriteFile(g_log, buffer, static_cast<DWORD>(n), &written, nullptr);
}

void LoadConfig() {
    wchar_t path[MAX_PATH];
    DWORD len = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        return;
    }
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash) {
        return;
    }
    slash[1] = L'\0';
    wcscat_s(path, MAX_PATH, L"mw4hook.ini");
    g_stretch_enabled =
        GetPrivateProfileIntW(L"frontend", L"stretch", 1, path) != 0;
    g_capture_enabled =
        GetPrivateProfileIntW(L"debug", L"automation", 0, path) != 0;
    g_clear_fix = GetPrivateProfileIntW(L"display", L"clear_fix", 1, path) != 0;
    g_zbuffer_fix = GetPrivateProfileIntW(L"display", L"zbuffer_fix", 1, path) != 0;
    if (GetPrivateProfileIntW(L"log", L"enabled", 1, path) == 0) {
        return;
    }

    len = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        return;
    }
    slash = wcsrchr(path, L'\\');
    if (!slash) {
        return;
    }
    slash[1] = L'\0';
    wchar_t name[64];
    wsprintfW(name, L"mw4hook-ddraw-%lu.log", GetCurrentProcessId());
    wcscat_s(path, name);
    g_log = CreateFileW(path, FILE_APPEND_DATA,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

FARPROC Resolve(const char* name) {
    if (!g_real_ddraw) {
        wchar_t path[MAX_PATH];
        UINT len = GetSystemDirectoryW(path, MAX_PATH);
        if (len == 0 || len + 16 >= MAX_PATH) {
            return nullptr;
        }
        wcscat_s(path, L"\\ddraw.dll");
        g_real_ddraw = LoadLibraryW(path);
        Log("real ddraw: %p\n", g_real_ddraw);
    }
    if (!g_real_ddraw) {
        return nullptr;
    }
    return GetProcAddress(g_real_ddraw, name);
}

using Blt_t = HRESULT(WINAPI*)(IDirectDrawSurface7*, LPRECT,
                               IDirectDrawSurface7*, LPRECT, DWORD,
                               LPDDBLTFX);
using CreateSurface_t = HRESULT(WINAPI*)(IDirectDraw7*, DDSURFACEDESC2*,
                                         IDirectDrawSurface7**, IUnknown*);
using GetGDISurface_t = HRESULT(WINAPI*)(IDirectDraw7*,
                                         IDirectDrawSurface7**);
using DirectDrawCreate_t = HRESULT(WINAPI*)(GUID*, IDirectDraw**, IUnknown*);
using DirectDrawCreateEx_t = HRESULT(WINAPI*)(GUID*, void**, REFIID, IUnknown*);
using QueryInterface_t = HRESULT(WINAPI*)(void*, REFIID, void**);
using GetAttachedSurface_t = HRESULT(WINAPI*)(IDirectDrawSurface7*, DDSCAPS2*,
                                              IDirectDrawSurface7**);

Blt_t g_original_blt = nullptr;
CreateSurface_t g_original_create_surface = nullptr;
GetGDISurface_t g_original_get_gdi_surface = nullptr;
QueryInterface_t g_original_surface_qi = nullptr;
GetAttachedSurface_t g_original_get_attached = nullptr;

void* g_original_dd_vtable[30] = {};
void** g_surface_vtables[8] = {};
int g_surface_vtable_count = 0;

HRESULT WINAPI Hook_Blt(IDirectDrawSurface7* self, LPRECT dest_rect,
                        IDirectDrawSurface7* src, LPRECT src_rect, DWORD flags,
                        LPDDBLTFX fx);
HRESULT WINAPI Hook_BltInner(IDirectDrawSurface7* self, LPRECT dest_rect,
                             IDirectDrawSurface7* src, LPRECT src_rect, DWORD flags,
                             LPDDBLTFX fx);
using Flip_t = HRESULT(WINAPI*)(IDirectDrawSurface7*, IDirectDrawSurface7*, DWORD);
Flip_t g_original_flip = nullptr;
HRESULT WINAPI Hook_Flip(IDirectDrawSurface7* self, IDirectDrawSurface7* target, DWORD flags);
bool g_last_blt_stretched = false;
RECT g_last_stretch_src = {};
RECT g_last_stretch_view = {};

// Largest rectangle with the content's aspect ratio centered in width x
// height (the whole area when the aspect already matches within a pixel or
// two).
RECT FitAspect(int width, int height, int content_w, int content_h) {
    int view_w = width;
    int view_h = height;
    if (static_cast<long long>(width) * content_h > static_cast<long long>(height) * content_w) {
        view_w = MulDiv(height, content_w, content_h);
    } else {
        view_h = MulDiv(width, content_h, content_w);
    }
    if (width - view_w <= 2) {
        view_w = width;
    }
    if (height - view_h <= 2) {
        view_h = height;
    }
    const int x = (width - view_w) / 2;
    const int y = (height - view_h) / 2;
    return {x, y, x + view_w, y + view_h};
}

// The dinput proxy knows whether the game is showing its 800x600 shell (it
// sees every gos_SetScreenMode request). 1 = shell, 0 = mission, -1 = unknown
// (then the black-border heuristic decides).
int ShellModeFromHook() {
    using ShellMode_t = int(WINAPI*)();
    static ShellMode_t fn = nullptr;
    static bool searched = false;
    if (!searched) {
        wchar_t path[MAX_PATH];
        DWORD len = GetModuleFileNameW(nullptr, path, MAX_PATH);
        wchar_t* slash = len ? wcsrchr(path, L'\\') : nullptr;
        if (slash) {
            wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - path), L"dinput.dll");
            HMODULE hook = GetModuleHandleW(path);
            if (hook) {
                searched = true;
                fn = reinterpret_cast<ShellMode_t>(GetProcAddress(hook, "Mw4ShellMode"));
            }
        }
    }
    return fn ? fn() : -1;
}


// Shared with the dinput proxy (cursor mapping) through Mw4FrontendViewport.
CRITICAL_SECTION g_view_lock;
RECT g_view = {};
SIZE g_view_content = {};  // size of the scaled picture (render pixels)
DWORD g_view_time = 0;

void PublishFrontendView(const RECT& view, int content_w, int content_h) {
    EnterCriticalSection(&g_view_lock);
    g_view = view;
    g_view_content = {content_w, content_h};
    g_view_time = GetTickCount();
    LeaveCriticalSection(&g_view_lock);
}
HRESULT WINAPI Hook_SurfaceQueryInterface(void* self, REFIID iid, void** out);
HRESULT WINAPI Hook_GetAttachedSurface(IDirectDrawSurface7* self,
                                       DDSCAPS2* caps,
                                       IDirectDrawSurface7** out);



void PatchSurfaceVtable(IDirectDrawSurface7* surface) {
    if (!surface) {
        return;
    }
    auto* vtable = *reinterpret_cast<void***>(surface);
    if (!vtable) {
        return;
    }
    for (int i = 0; i < g_surface_vtable_count; ++i) {
        if (g_surface_vtables[i] == vtable) {
            return;
        }
    }
    if (g_surface_vtable_count >= 8) {
        return;
    }

    void* original[44] = {};
    memcpy(original, vtable, sizeof(original));
    if (!g_original_blt) {
        g_original_blt = reinterpret_cast<Blt_t>(original[5]);
    }
    if (!g_original_surface_qi) {
        g_original_surface_qi = reinterpret_cast<QueryInterface_t>(original[0]);
    }
    if (!g_original_flip) {
        g_original_flip = reinterpret_cast<Flip_t>(original[11]);
    }
    if (!g_original_get_attached) {
        g_original_get_attached =
            reinterpret_cast<GetAttachedSurface_t>(original[12]);
    }

    void* patched[44] = {};
    memcpy(patched, original, sizeof(patched));
    patched[0] = reinterpret_cast<void*>(&Hook_SurfaceQueryInterface);
    patched[5] = reinterpret_cast<void*>(&Hook_Blt);
    patched[11] = reinterpret_cast<void*>(&Hook_Flip);
    patched[12] = reinterpret_cast<void*>(&Hook_GetAttachedSurface);

    DWORD old_protect = 0;
    if (!VirtualProtect(vtable, sizeof(patched), PAGE_READWRITE, &old_protect)) {
        return;
    }
    memcpy(vtable, patched, sizeof(patched));
    VirtualProtect(vtable, sizeof(patched), old_protect, &old_protect);
    g_surface_vtables[g_surface_vtable_count++] = vtable;
}

HRESULT WINAPI Hook_SurfaceQueryInterface(void* self, REFIID iid, void** out) {
    HRESULT result = g_original_surface_qi(self, iid, out);
    if (SUCCEEDED(result) && out && *out) {
        PatchSurfaceVtable(reinterpret_cast<IDirectDrawSurface7*>(*out));
    }
    return result;
}

HRESULT WINAPI Hook_GetAttachedSurface(IDirectDrawSurface7* self,
                                       DDSCAPS2* caps,
                                       IDirectDrawSurface7** out) {
    HRESULT result = g_original_get_attached(self, caps, out);
    if (SUCCEEDED(result) && out && *out) {
        PatchSurfaceVtable(*out);
    }
    return result;
}

// ---------------------------------------------------------------------------
// Development-only frame capture. The test automation writes the output file
// name into mw4hook-shot.req; the next presented frame is saved as a BMP of
// exactly what reaches the window (after the frontend scaler). Polled at most
// every 200 ms, so it costs nothing measurable when unused.
// ---------------------------------------------------------------------------

bool GameDirPath(const wchar_t* name, wchar_t* out, size_t cch) {
    DWORD len = GetModuleFileNameW(nullptr, out, static_cast<DWORD>(cch));
    wchar_t* slash = len ? wcsrchr(out, L'\\') : nullptr;
    if (!slash) {
        return false;
    }
    slash[1] = L'\0';
    return wcscat_s(out, cch, name) == 0;
}

bool TakeShotRequest(wchar_t* out_path, size_t cch) {
    static DWORD last_check = 0;
    DWORD now = GetTickCount();
    if (now - last_check < 200) {
        return false;
    }
    last_check = now;
    wchar_t req[MAX_PATH];
    if (!GameDirPath(L"mw4hook-shot.req", req, MAX_PATH)) {
        return false;
    }
    HANDLE file = CreateFileW(req, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    char name[MAX_PATH] = {};
    DWORD read = 0;
    ReadFile(file, name, MAX_PATH - 1, &read, nullptr);
    CloseHandle(file);
    DeleteFileW(req);
    name[read] = '\0';
    for (DWORD i = 0; i < read; ++i) {
        if (name[i] == '\r' || name[i] == '\n') {
            name[i] = '\0';
        }
    }
    wchar_t wide[MAX_PATH];
    MultiByteToWideChar(CP_ACP, 0, name, -1, wide, MAX_PATH);
    if (wcschr(wide, L'\\') || wcschr(wide, L'/')) {
        wcscpy_s(out_path, cch, wide);
        return true;
    }
    return GameDirPath(wide, out_path, cch);
}

// Writes what the window shows: a client-sized black canvas with |src_rect|
// of the presented surface drawn at |dest| (client coordinates). Anything the
// game does not cover stays black, exactly as on screen.
void WriteShot(const wchar_t* path, HDC src_dc, int width, int height,
               const RECT& src_rect, const RECT& dest) {
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = height;  // bottom-up, as BMP expects
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 24;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC mem = CreateCompatibleDC(src_dc);
    HBITMAP dib = CreateDIBSection(mem, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib) {
        DeleteDC(mem);
        return;
    }
    HGDIOBJ old = SelectObject(mem, dib);
    RECT all = {0, 0, width, height};
    FillRect(mem, &all, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
    SetStretchBltMode(mem, HALFTONE);
    StretchBlt(mem, dest.left, dest.top, dest.right - dest.left, dest.bottom - dest.top,
               src_dc, src_rect.left, src_rect.top,
               src_rect.right - src_rect.left, src_rect.bottom - src_rect.top, SRCCOPY);
    GdiFlush();
    const DWORD stride = ((width * 3 + 3) & ~3u);
    const DWORD image = stride * height;
    BITMAPFILEHEADER header{};
    header.bfType = 0x4D42;
    header.bfOffBits = sizeof(header) + sizeof(info.bmiHeader);
    header.bfSize = header.bfOffBits + image;
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(file, &header, sizeof(header), &written, nullptr);
        WriteFile(file, &info.bmiHeader, sizeof(info.bmiHeader), &written, nullptr);
        WriteFile(file, bits, image, &written, nullptr);
        CloseHandle(file);
        Log("shot: %ls (%dx%d)\n", path, width, height);
    }
    SelectObject(mem, old);
    DeleteObject(dib);
    DeleteDC(mem);
}

void MaybeCaptureFrame(IDirectDrawSurface7* self, IDirectDrawSurface7* src,
                       LPRECT src_rect, LPRECT dest_rect,
                       const RECT* stretched_from) {
    static wchar_t pending[MAX_PATH] = {};
    if (!src || !dest_rect) {
        return;
    }
    if (!pending[0] && !TakeShotRequest(pending, MAX_PATH)) {
        return;
    }
    // Only the present (a blit onto the primary surface) is the frame.
    DDSCAPS2 caps{};
    if (FAILED(self->GetCaps(&caps)) || !(caps.dwCaps & DDSCAPS_PRIMARYSURFACE)) {
        return;
    }
    wchar_t path[MAX_PATH];
    wcscpy_s(path, pending);
    pending[0] = L'\0';
    HDC dc = nullptr;
    if (FAILED(src->GetDC(&dc))) {
        Log("shot: GetDC failed\n");
        return;
    }
    RECT from{};
    if (stretched_from) {
        from = *stretched_from;
    } else if (src_rect) {
        from = *src_rect;
    } else {
        DDSURFACEDESC2 desc{};
        desc.dwSize = sizeof(desc);
        src->GetSurfaceDesc(&desc);
        from = {0, 0, static_cast<LONG>(desc.dwWidth), static_cast<LONG>(desc.dwHeight)};
    }
    // Map the present rectangle (screen coordinates) into the window's client
    // area via the primary surface's clipper window.
    int width = dest_rect->right - dest_rect->left;
    int height = dest_rect->bottom - dest_rect->top;
    RECT dest = {0, 0, width, height};
    IDirectDrawClipper* clipper = nullptr;
    HWND hwnd = nullptr;
    if (SUCCEEDED(self->GetClipper(&clipper)) && clipper) {
        clipper->GetHWnd(&hwnd);
        clipper->Release();
    }
    if (hwnd) {
        RECT client{};
        GetClientRect(hwnd, &client);
        POINT origin = {0, 0};
        ClientToScreen(hwnd, &origin);
        width = client.right;
        height = client.bottom;
        dest = {dest_rect->left - origin.x, dest_rect->top - origin.y,
                dest_rect->right - origin.x, dest_rect->bottom - origin.y};
    }
    if (stretched_from) {
        // The scaler drew the picture into this rectangle of the client area.
        dest = g_last_stretch_view;
    }
    Log("shot: client %dx%d, present %ld,%ld-%ld,%ld\n", width, height, dest.left,
        dest.top, dest.right, dest.bottom);
    WriteShot(path, dc, width, height, from, dest);
    src->ReleaseDC(dc);
}

// Present statistics: frames per second, time spent in the present blit and
// the time between presents, logged every 5 seconds while frames are shown.
void CountPresent(LONGLONG blt_ticks, bool scaled) {
    static LARGE_INTEGER freq = {};
    static LONGLONG window_start = 0;
    static LONGLONG last = 0;
    static unsigned frames = 0;
    static unsigned scaled_frames = 0;
    static LONGLONG blt_total = 0;
    static LONGLONG blt_max = 0;
    static LONGLONG gap_max = 0;
    if (!freq.QuadPart) {
        QueryPerformanceFrequency(&freq);
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!window_start || now.QuadPart - last > freq.QuadPart * 2) {
        window_start = now.QuadPart;  // first frame or after a long pause
        frames = scaled_frames = 0;
        blt_total = blt_max = gap_max = 0;
    } else if (now.QuadPart - last > gap_max) {
        gap_max = now.QuadPart - last;
    }
    last = now.QuadPart;
    ++frames;
    scaled_frames += scaled ? 1 : 0;
    blt_total += blt_ticks;
    blt_max = blt_ticks > blt_max ? blt_ticks : blt_max;
    const LONGLONG span = now.QuadPart - window_start;
    if (span >= freq.QuadPart * 5) {
        const double ms = 1000.0 / static_cast<double>(freq.QuadPart);
        Log("present: %.1f fps, blit avg %.2f ms max %.2f ms, longest gap %.1f ms, "
            "%u/%u scaled, mode %d\n",
            frames * static_cast<double>(freq.QuadPart) / static_cast<double>(span),
            blt_total * ms / frames, blt_max * ms, gap_max * ms, scaled_frames, frames,
            ShellModeFromHook());
        window_start = now.QuadPart;
        frames = scaled_frames = 0;
        blt_total = blt_max = gap_max = 0;
    }
}

HRESULT WINAPI Hook_Blt(IDirectDrawSurface7* self, LPRECT dest_rect,
                        IDirectDrawSurface7* src, LPRECT src_rect, DWORD flags,
                        LPDDBLTFX fx) {
    LARGE_INTEGER start;
    QueryPerformanceCounter(&start);
    HRESULT result = Hook_BltInner(self, dest_rect, src, src_rect, flags, fx);
    if (src && dest_rect) {
        DDSCAPS2 caps{};
        if (SUCCEEDED(self->GetCaps(&caps)) && (caps.dwCaps & DDSCAPS_PRIMARYSURFACE)) {
            LARGE_INTEGER end;
            QueryPerformanceCounter(&end);
            CountPresent(end.QuadPart - start.QuadPart, g_last_blt_stretched);
        }
    }
    if (g_capture_enabled && src && dest_rect) {
        MaybeCaptureFrame(self, src, src_rect, dest_rect,
                          g_last_blt_stretched ? &g_last_stretch_src : nullptr);
    }
    return result;
}

// The window the primary surface presents into (its clipper's window).
HWND PresentWindow(IDirectDrawSurface7* primary) {
    IDirectDrawClipper* clipper = nullptr;
    HWND hwnd = nullptr;
    if (SUCCEEDED(primary->GetClipper(&clipper)) && clipper) {
        clipper->GetHWnd(&hwnd);
        clipper->Release();
    }
    return hwnd;
}

// Windowed present. GameOS blits its render surface onto the primary surface
// at the window's client origin, sized to its render size (800x600 for the
// shell). The window client is the selected resolution, so when the two
// differ the picture is drawn scaled into the client area instead: aspect
// kept, black bars where it does not fill. Mission frames already match the
// client and pass through untouched.
HRESULT WINAPI Hook_BltInner(IDirectDrawSurface7* self, LPRECT dest_rect,
                             IDirectDrawSurface7* src, LPRECT src_rect, DWORD flags,
                             LPDDBLTFX fx) {
    g_last_blt_stretched = false;
    if (!g_stretch_enabled || !src || !dest_rect) {
        return g_original_blt(self, dest_rect, src, src_rect, flags, fx);
    }
    DDSCAPS2 caps{};
    if (FAILED(self->GetCaps(&caps)) || !(caps.dwCaps & DDSCAPS_PRIMARYSURFACE)) {
        return g_original_blt(self, dest_rect, src, src_rect, flags, fx);
    }
    HWND hwnd = PresentWindow(self);
    if (!hwnd) {
        return g_original_blt(self, dest_rect, src, src_rect, flags, fx);
    }
    RECT client{};
    GetClientRect(hwnd, &client);
    POINT origin = {0, 0};
    ClientToScreen(hwnd, &origin);
    const int client_w = client.right;
    const int client_h = client.bottom;

    RECT content{};
    if (src_rect) {
        content = *src_rect;
    } else {
        DDSURFACEDESC2 desc{};
        desc.dwSize = sizeof(desc);
        if (FAILED(src->GetSurfaceDesc(&desc))) {
            return g_original_blt(self, dest_rect, src, src_rect, flags, fx);
        }
        content = {0, 0, static_cast<LONG>(desc.dwWidth), static_cast<LONG>(desc.dwHeight)};
    }
    const int content_w = content.right - content.left;
    const int content_h = content.bottom - content.top;
    if (client_w <= 0 || client_h <= 0 || content_w <= 0 || content_h <= 0) {
        return g_original_blt(self, dest_rect, src, src_rect, flags, fx);
    }
    // The primary surface only covers the main monitor. A window dragged
    // (even partly) onto another monitor gets nothing from a primary-surface
    // blit, and the picture there froze; present through the window's own DC
    // instead.
    bool on_primary = false;
    {
        DDSURFACEDESC2 primary_desc{};
        primary_desc.dwSize = sizeof(primary_desc);
        if (SUCCEEDED(self->GetSurfaceDesc(&primary_desc))) {
            on_primary = origin.x >= 0 && origin.y >= 0 &&
                         origin.x + client_w <= static_cast<int>(primary_desc.dwWidth) &&
                         origin.y + client_h <= static_cast<int>(primary_desc.dwHeight);
        }
    }
    const bool same_size = content_w == client_w && content_h == client_h;
    if (same_size && on_primary) {
        return g_original_blt(self, dest_rect, src, src_rect, flags, fx);
    }

    RECT view = FitAspect(client_w, client_h, content_w, content_h);
    RECT bars[2] = {};
    if (view.left > 0) {
        bars[0] = {0, 0, view.left, client_h};
        bars[1] = {view.right, 0, client_w, client_h};
    } else if (view.top > 0) {
        bars[0] = {0, 0, client_w, view.top};
        bars[1] = {0, view.bottom, client_w, client_h};
    }

    if (ShellModeFromHook() == 0 && on_primary) {
        // Mission frames run at full frame rate: let DirectDraw do the stretch
        // (hardware) and fill the bars with a color-fill blit. The clipper
        // keeps both inside the window.
        DDBLTFX fill{};
        fill.dwSize = sizeof(fill);
        fill.dwFillColor = 0;
        for (RECT bar : bars) {
            if (bar.right > bar.left && bar.bottom > bar.top) {
                OffsetRect(&bar, origin.x, origin.y);
                g_original_blt(self, &bar, nullptr, nullptr, DDBLT_COLORFILL | DDBLT_WAIT, &fill);
            }
        }
        RECT target = view;
        OffsetRect(&target, origin.x, origin.y);
        HRESULT hr = g_original_blt(self, &target, src, &content, flags, fx);
        if (SUCCEEDED(hr)) {
            g_last_blt_stretched = true;
            g_last_stretch_src = content;
            g_last_stretch_view = view;
            PublishFrontendView(view, content_w, content_h);
        }
        return hr;
    }

    HDC src_dc = nullptr;
    if (FAILED(src->GetDC(&src_dc))) {
        return g_original_blt(self, dest_rect, src, src_rect, flags, fx);
    }
    // Draw into the window's client area (works on every monitor).
    HDC dest_dc = GetDC(hwnd);
    if (!dest_dc) {
        src->ReleaseDC(src_dc);
        Log("present scale: GetDC(window) failed (%lu)\n", GetLastError());
        return g_original_blt(self, dest_rect, src, src_rect, flags, fx);
    }
    for (const RECT& bar : bars) {
        if (bar.right > bar.left && bar.bottom > bar.top) {
            FillRect(dest_dc, &bar, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        }
    }
    BOOL ok = FALSE;
    if (same_size) {
        ok = BitBlt(dest_dc, 0, 0, client_w, client_h, src_dc, content.left, content.top, SRCCOPY);
    } else {
        SetStretchBltMode(dest_dc, HALFTONE);
        SetBrushOrgEx(dest_dc, 0, 0, nullptr);
        ok = StretchBlt(dest_dc, view.left, view.top, view.right - view.left,
                        view.bottom - view.top, src_dc, content.left, content.top, content_w,
                        content_h, SRCCOPY);
    }
    ReleaseDC(hwnd, dest_dc);
    src->ReleaseDC(src_dc);
    if (!ok) {
        Log("present scale: StretchBlt failed (%lu)\n", GetLastError());
        return g_original_blt(self, dest_rect, src, src_rect, flags, fx);
    }

    g_last_blt_stretched = true;
    g_last_stretch_src = content;
    g_last_stretch_view = view;
    PublishFrontendView(view, content_w, content_h);
    static RECT reported = {};
    if (!EqualRect(&reported, &view)) {
        reported = view;
        Log("present scale: %dx%d -> %ldx%ld at %ld,%ld in %dx%d client\n", content_w,
            content_h, view.right - view.left, view.bottom - view.top, view.left, view.top,
            client_w, client_h);
    }
    return DD_OK;
}

HRESULT WINAPI Hook_CreateSurface(IDirectDraw7* self, DDSURFACEDESC2* desc,
                                  IDirectDrawSurface7** surface,
                                  IUnknown* outer) {
    HRESULT result = g_original_create_surface(self, desc, surface, outer);
    const bool zbuffer = desc && (desc->ddsCaps.dwCaps & DDSCAPS_ZBUFFER) &&
                         (desc->dwFlags & DDSD_PIXELFORMAT);
    if (zbuffer && FAILED(result) && g_zbuffer_fix && desc->ddpfPixelFormat.dwZBufferBitDepth == 32) {
        // GameOS asks for 32-bit depth with the Z bits in the high 24 bits
        // (Z mask 0xFFFFFF00, stencil in the low byte). Current drivers only
        // expose the standard layouts (Z in the low 24 bits), so the request
        // fails with E_NOTIMPL and GameOS falls back to a 16-bit depth buffer,
        // which is far too coarse for mission terrain. Ask for the standard
        // layouts instead.
        struct Candidate {
            DWORD flags;
            DWORD stencil_bits;
            DWORD z_mask;
            DWORD stencil_mask;
        };
        const bool wants_stencil = (desc->ddpfPixelFormat.dwFlags & DDPF_STENCILBUFFER) != 0;
        const Candidate candidates[] = {
            {DDPF_ZBUFFER | DDPF_STENCILBUFFER, 8, 0x00FFFFFF, 0xFF000000},  // D24S8
            {DDPF_ZBUFFER, 0, 0x00FFFFFF, 0},                                // D24X8
        };
        for (const Candidate& c : candidates) {
            if (c.stencil_bits && !wants_stencil) {
                continue;
            }
            DDSURFACEDESC2 retry = *desc;
            retry.ddpfPixelFormat.dwFlags = c.flags;
            retry.ddpfPixelFormat.dwZBufferBitDepth = 32;
            retry.ddpfPixelFormat.dwStencilBitDepth = c.stencil_bits;
            retry.ddpfPixelFormat.dwZBitMask = c.z_mask;
            retry.ddpfPixelFormat.dwStencilBitMask = c.stencil_mask;
            const HRESULT again = g_original_create_surface(self, &retry, surface, outer);
            Log("z-buffer %lux%lu: 32-bit Z mask 0x%08lX stencil %lu -> 0x%08lX\n", desc->dwWidth,
                desc->dwHeight, c.z_mask, c.stencil_bits, again);
            if (SUCCEEDED(again)) {
                // Report the format that was created, so GameOS' later checks
                // (stencil available or not) match the real surface.
                desc->ddpfPixelFormat = retry.ddpfPixelFormat;
                result = again;
                break;
            }
        }
    }
    if (SUCCEEDED(result) && surface && *surface) {
        PatchSurfaceVtable(*surface);
    }
    if (zbuffer) {
        Log("z-buffer %lux%lu: %lu-bit Z mask 0x%08lX, stencil %lu bits -> 0x%08lX\n",
            desc->dwWidth, desc->dwHeight, desc->ddpfPixelFormat.dwZBufferBitDepth,
            desc->ddpfPixelFormat.dwZBitMask, desc->ddpfPixelFormat.dwStencilBitDepth, result);
    }
    return result;
}

HRESULT WINAPI Hook_GetGDISurface(IDirectDraw7* self,
                                  IDirectDrawSurface7** surface) {
    HRESULT result = g_original_get_gdi_surface(self, surface);
    if (SUCCEEDED(result) && surface && *surface) {
        PatchSurfaceVtable(*surface);
    }
    return result;
}

// ---------------------------------------------------------------------------
// Direct3D 7 device Clear.
//
// GameOS clears the render target through a wrapper (3.1c 0x00742EF0) and,
// when Clear fails with anything but DDERR_SURFACELOST, re-runs its whole
// windowed DirectDraw/Direct3D setup (call at 0x00716355). On current
// Windows/drivers that failure repeats every frame in missions and some 3D
// menus, so the display was rebuilt every frame. The device is reached via
// IDirectDraw7::QueryInterface(IID_IDirect3D7) -> IDirect3D7::CreateDevice.
// ---------------------------------------------------------------------------

const GUID kIID_IDirect3D7 = {0xf5049e77, 0x4861, 0x11d2,
                              {0xa4, 0x07, 0x00, 0xa0, 0xc9, 0x06, 0x29, 0xa8}};
constexpr DWORD kClearTarget = 1;
constexpr DWORD kClearZBuffer = 2;
constexpr DWORD kClearStencil = 4;

using D3DCreateDevice_t = HRESULT(WINAPI*)(void*, REFCLSID, IDirectDrawSurface7*, void**);
using D3DClear_t = HRESULT(WINAPI*)(void*, DWORD, void*, DWORD, DWORD, float, DWORD);
QueryInterface_t g_original_dd_qi = nullptr;
D3DCreateDevice_t g_original_create_device = nullptr;
D3DClear_t g_original_clear = nullptr;

bool PatchSlot(void** vtable, int index, void* hook) {
    DWORD old_protect = 0;
    if (!VirtualProtect(&vtable[index], sizeof(void*), PAGE_READWRITE, &old_protect)) {
        return false;
    }
    vtable[index] = hook;
    VirtualProtect(&vtable[index], sizeof(void*), old_protect, &old_protect);
    return true;
}

HRESULT WINAPI Hook_D3DClear(void* device, DWORD count, void* rects, DWORD flags,
                             DWORD color, float z, DWORD stencil) {
    HRESULT result = g_original_clear(device, count, rects, flags, color, z, stencil);
    if (SUCCEEDED(result) || result == DDERR_SURFACELOST) {
        return result;
    }
    HRESULT retry = E_FAIL;
    DWORD retry_flags = flags;
    if (g_clear_fix) {
        // Clear what can be cleared: without stencil, then target only.
        if (flags & kClearStencil) {
            retry_flags = flags & ~kClearStencil;
            retry = g_original_clear(device, count, rects, retry_flags, color, z, stencil);
        }
        if (FAILED(retry) && (flags & kClearZBuffer)) {
            retry_flags = flags & kClearTarget;
            retry = retry_flags ? g_original_clear(device, count, rects, retry_flags, color, z,
                                                   stencil)
                                : S_OK;
        }
    }
    static unsigned failures = 0;
    const unsigned n = ++failures;
    if ((n & (n - 1)) == 0) {
        Log("Clear(count=%lu flags=0x%lX) failed 0x%08lX; retry flags 0x%lX -> 0x%08lX "
            "(failure %u)\n",
            count, flags, result, retry_flags, retry, n);
    }
    return SUCCEEDED(retry) ? retry : result;
}

HRESULT WINAPI Hook_D3DCreateDevice(void* d3d, REFCLSID clsid, IDirectDrawSurface7* target,
                                    void** device) {
    HRESULT result = g_original_create_device(d3d, clsid, target, device);
    if (SUCCEEDED(result) && device && *device) {
        void** vtable = *reinterpret_cast<void***>(*device);
        if (vtable[10] != reinterpret_cast<void*>(&Hook_D3DClear)) {
            if (!g_original_clear) {
                g_original_clear = reinterpret_cast<D3DClear_t>(vtable[10]);
            }
            const bool ok = PatchSlot(vtable, 10, reinterpret_cast<void*>(&Hook_D3DClear));
            Log("d3d device created: Clear hook %s\n", ok ? "ok" : "failed");
        }
    }
    return result;
}

HRESULT WINAPI Hook_DDQueryInterface(void* self, REFIID iid, void** out) {
    HRESULT result = g_original_dd_qi(self, iid, out);
    if (SUCCEEDED(result) && out && *out && IsEqualGUID(iid, kIID_IDirect3D7)) {
        void** vtable = *reinterpret_cast<void***>(*out);
        if (vtable[4] != reinterpret_cast<void*>(&Hook_D3DCreateDevice)) {
            if (!g_original_create_device) {
                g_original_create_device = reinterpret_cast<D3DCreateDevice_t>(vtable[4]);
            }
            PatchSlot(vtable, 4, reinterpret_cast<void*>(&Hook_D3DCreateDevice));
        }
    }
    return result;
}

void PatchDirectDrawVtable(IDirectDraw7* ddraw) {
    if (!ddraw || g_original_dd_vtable[6]) {
        return;
    }
    auto* vtable = *reinterpret_cast<void***>(ddraw);
    if (!vtable) {
        return;
    }
    memcpy(g_original_dd_vtable, vtable, sizeof(g_original_dd_vtable));
    g_original_create_surface =
        reinterpret_cast<CreateSurface_t>(g_original_dd_vtable[6]);
    g_original_get_gdi_surface =
        reinterpret_cast<GetGDISurface_t>(g_original_dd_vtable[14]);

    void* patched[30] = {};
    memcpy(patched, g_original_dd_vtable, sizeof(patched));
    patched[6] = reinterpret_cast<void*>(&Hook_CreateSurface);
    patched[14] = reinterpret_cast<void*>(&Hook_GetGDISurface);
    g_original_dd_qi = reinterpret_cast<QueryInterface_t>(g_original_dd_vtable[0]);
    patched[0] = reinterpret_cast<void*>(&Hook_DDQueryInterface);

    DWORD old_protect = 0;
    if (!VirtualProtect(vtable, sizeof(patched), PAGE_READWRITE, &old_protect)) {
        return;
    }
    memcpy(vtable, patched, sizeof(patched));
    VirtualProtect(vtable, sizeof(patched), old_protect, &old_protect);
}

}  // namespace

extern "C" {
void* g_pfn_AcquireDDThreadLock = nullptr;
void* g_pfn_CompleteCreateSysmemSurface = nullptr;
void* g_pfn_D3DParseUnknownCommand = nullptr;
void* g_pfn_DDGetAttachedSurfaceLcl = nullptr;
void* g_pfn_DDInternalLock = nullptr;
void* g_pfn_DDInternalUnlock = nullptr;
void* g_pfn_DSoundHelp = nullptr;
void* g_pfn_DirectDrawCreateClipper = nullptr;
void* g_pfn_DirectDrawEnumerateA = nullptr;
void* g_pfn_DirectDrawEnumerateExA = nullptr;
void* g_pfn_DirectDrawEnumerateExW = nullptr;
void* g_pfn_DirectDrawEnumerateW = nullptr;
void* g_pfn_DllCanUnloadNow = nullptr;
void* g_pfn_DllGetClassObject = nullptr;
void* g_pfn_GetDDSurfaceLocal = nullptr;
void* g_pfn_GetOLEThunkData = nullptr;
void* g_pfn_GetSurfaceFromDC = nullptr;
void* g_pfn_RegisterSpecialCase = nullptr;
void* g_pfn_ReleaseDDThreadLock = nullptr;
void* g_pfn_SetAppCompatData = nullptr;

#define MW4_DDRAW_THUNK(name)                 \
    __declspec(naked) void Proxy_##name() {   \
        __asm { jmp dword ptr[g_pfn_##name] } \
    }

MW4_DDRAW_THUNK(AcquireDDThreadLock)
MW4_DDRAW_THUNK(CompleteCreateSysmemSurface)
MW4_DDRAW_THUNK(D3DParseUnknownCommand)
MW4_DDRAW_THUNK(DDGetAttachedSurfaceLcl)
MW4_DDRAW_THUNK(DDInternalLock)
MW4_DDRAW_THUNK(DDInternalUnlock)
MW4_DDRAW_THUNK(DSoundHelp)
MW4_DDRAW_THUNK(DirectDrawCreateClipper)
MW4_DDRAW_THUNK(DirectDrawEnumerateA)
MW4_DDRAW_THUNK(DirectDrawEnumerateExA)
MW4_DDRAW_THUNK(DirectDrawEnumerateExW)
MW4_DDRAW_THUNK(DirectDrawEnumerateW)
MW4_DDRAW_THUNK(DllCanUnloadNow)
MW4_DDRAW_THUNK(DllGetClassObject)
MW4_DDRAW_THUNK(GetDDSurfaceLocal)
MW4_DDRAW_THUNK(GetOLEThunkData)
MW4_DDRAW_THUNK(GetSurfaceFromDC)
MW4_DDRAW_THUNK(RegisterSpecialCase)
MW4_DDRAW_THUNK(ReleaseDDThreadLock)
MW4_DDRAW_THUNK(SetAppCompatData)
}  // extern "C"

namespace {

// Fullscreen presents with Flip. The menus use the original 800x600 display
// mode there, so nothing is scaled; the hook only serves frame capture.
HRESULT WINAPI Hook_Flip(IDirectDrawSurface7* self, IDirectDrawSurface7* target,
                         DWORD flags) {
    if (g_capture_enabled) {
        wchar_t path[MAX_PATH];
        if (TakeShotRequest(path, MAX_PATH)) {
            IDirectDrawSurface7* back = target;
            bool release = false;
            if (!back) {
                DDSCAPS2 caps{};
                caps.dwCaps = DDSCAPS_BACKBUFFER;
                release = SUCCEEDED(self->GetAttachedSurface(&caps, &back)) && back;
            }
            DDSURFACEDESC2 desc{};
            desc.dwSize = sizeof(desc);
            HDC dc = nullptr;
            if (back && SUCCEEDED(back->GetSurfaceDesc(&desc)) && SUCCEEDED(back->GetDC(&dc))) {
                RECT all = {0, 0, static_cast<LONG>(desc.dwWidth), static_cast<LONG>(desc.dwHeight)};
                WriteShot(path, dc, all.right, all.bottom, all, all);
                back->ReleaseDC(dc);
            }
            if (release) {
                back->Release();
            }
        }
    }
    return g_original_flip(self, target, flags);
}

}  // namespace
// Reports where the picture was last drawn scaled (view, client coordinates)
// and its own size in render pixels. Returns FALSE when recent frames were not
// scaled, so callers leave the cursor untouched.
extern "C" BOOL WINAPI Mw4FrontendViewport(RECT* view, SIZE* content) {
    EnterCriticalSection(&g_view_lock);
    const bool recent = g_view_time != 0 && GetTickCount() - g_view_time < 500;
    if (recent) {
        *view = g_view;
        *content = g_view_content;
    }
    LeaveCriticalSection(&g_view_lock);
    return recent ? TRUE : FALSE;
}

extern "C" HRESULT WINAPI Proxy_DirectDrawCreate(GUID* guid, IDirectDraw** out,
                                                 IUnknown* outer) {
    auto fn = reinterpret_cast<DirectDrawCreate_t>(Resolve("DirectDrawCreate"));
    if (!fn) {
        return E_FAIL;
    }
    HRESULT result = fn(guid, out, outer);
    if (SUCCEEDED(result) && out && *out) {
        PatchDirectDrawVtable(reinterpret_cast<IDirectDraw7*>(*out));
    }
    return result;
}

extern "C" HRESULT WINAPI Proxy_DirectDrawCreateEx(GUID* guid, void** out,
                                                   REFIID iid, IUnknown* outer) {
    auto fn =
        reinterpret_cast<DirectDrawCreateEx_t>(Resolve("DirectDrawCreateEx"));
    if (!fn) {
        return E_FAIL;
    }
    HRESULT result = fn(guid, out, iid, outer);
    if (SUCCEEDED(result) && out && *out) {
        PatchDirectDrawVtable(reinterpret_cast<IDirectDraw7*>(*out));
    }
    return result;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        InitializeCriticalSection(&g_view_lock);
        LoadConfig();
        Log("ddraw proxy attached\n");
        Resolve("DirectDrawCreate");
        g_pfn_AcquireDDThreadLock = reinterpret_cast<void*>(Resolve("AcquireDDThreadLock"));
        g_pfn_CompleteCreateSysmemSurface = reinterpret_cast<void*>(Resolve("CompleteCreateSysmemSurface"));
        g_pfn_D3DParseUnknownCommand = reinterpret_cast<void*>(Resolve("D3DParseUnknownCommand"));
        g_pfn_DDGetAttachedSurfaceLcl = reinterpret_cast<void*>(Resolve("DDGetAttachedSurfaceLcl"));
        g_pfn_DDInternalLock = reinterpret_cast<void*>(Resolve("DDInternalLock"));
        g_pfn_DDInternalUnlock = reinterpret_cast<void*>(Resolve("DDInternalUnlock"));
        g_pfn_DSoundHelp = reinterpret_cast<void*>(Resolve("DSoundHelp"));
        g_pfn_DirectDrawCreateClipper = reinterpret_cast<void*>(Resolve("DirectDrawCreateClipper"));
        g_pfn_DirectDrawEnumerateA = reinterpret_cast<void*>(Resolve("DirectDrawEnumerateA"));
        g_pfn_DirectDrawEnumerateExA = reinterpret_cast<void*>(Resolve("DirectDrawEnumerateExA"));
        g_pfn_DirectDrawEnumerateExW = reinterpret_cast<void*>(Resolve("DirectDrawEnumerateExW"));
        g_pfn_DirectDrawEnumerateW = reinterpret_cast<void*>(Resolve("DirectDrawEnumerateW"));
        g_pfn_DllCanUnloadNow = reinterpret_cast<void*>(Resolve("DllCanUnloadNow"));
        g_pfn_DllGetClassObject = reinterpret_cast<void*>(Resolve("DllGetClassObject"));
        g_pfn_GetDDSurfaceLocal = reinterpret_cast<void*>(Resolve("GetDDSurfaceLocal"));
        g_pfn_GetOLEThunkData = reinterpret_cast<void*>(Resolve("GetOLEThunkData"));
        g_pfn_GetSurfaceFromDC = reinterpret_cast<void*>(Resolve("GetSurfaceFromDC"));
        g_pfn_RegisterSpecialCase = reinterpret_cast<void*>(Resolve("RegisterSpecialCase"));
        g_pfn_ReleaseDDThreadLock = reinterpret_cast<void*>(Resolve("ReleaseDDThreadLock"));
        g_pfn_SetAppCompatData = reinterpret_cast<void*>(Resolve("SetAppCompatData"));
    }
    return TRUE;
}