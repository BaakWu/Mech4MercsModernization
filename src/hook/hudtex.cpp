// Sharper HUD text at high resolutions (issue #27).
//
// The mission HUD's small texts are not drawn with a font: the 'Mech names in
// the lance roster, the weapon-group digits, the damage-section labels
// (HD LT CT ...) and the HUD graphics are pre-drawn images in 256x256 texture
// atlases, textures\hud\*.tga (hud4/hud5/hud6..., num.tga). A name is about
// 6 texels tall and is drawn about 1.8 times larger at 1080p, so it looks
// blocky.
//
// When the game fetches one of those images from its archive (the member
// fetch hook in shellscript.cpp), the hook replaces it with a copy four times
// larger in each direction. The texts are white masks (the game tints them):
// their alpha is upscaled with Lanczos-3 and the edges firmed a little, so
// they stay crisp instead of soft. Every other texel is copied as it is
// (see Upscale), so the rest of the HUD looks exactly as before.
// Direct3D texture coordinates are 0..1, so the larger atlas draws in exactly
// the same places; only the texel density changes. The new image is
// allocated with the game's own allocator (the one the fetch uses), so the
// game frees it as usual. Only compressed members are replaced (their buffer
// belongs to the entry; an uncompressed member points into the archive).
//
// [hud] sharp=0 turns it off.
#include "hook.h"

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace mw4hook {
namespace {

using GameAlloc_t = void*(__cdecl*)(DWORD size, DWORD heap);
using GameFree_t = void(__cdecl*)(void* block);
GameAlloc_t g_alloc = nullptr;
GameFree_t g_free = nullptr;

constexpr int kMaxFactor = 4;
constexpr int kTaps = 6;          // Lanczos-3
constexpr float kFirm = 2.0f;     // alpha edge contrast (0 = plain Lanczos)
// Edge threshold: below 0.5 so thin, partly covered strokes (the diagonals of
// K, W, A) keep their weight instead of thinning out.
constexpr float kPivot = 0.35f;

// Archive entry layout (see shellscript.cpp).
constexpr size_t kEntryData = 0x08;
constexpr size_t kEntryStored = 0x18;
constexpr size_t kEntrySize = 0x1C;
constexpr size_t kEntryOffset = 0x20;
constexpr size_t kEntryOwned = 0x26;
constexpr size_t kEntryNameLen = 0x27;
constexpr size_t kEntryName = 0x28;
constexpr size_t kArchiveHeap = 0x24;

float Lanczos3(float t) {
    t = std::fabs(t);
    if (t < 1e-6f) {
        return 1.0f;
    }
    if (t >= 3.0f) {
        return 0.0f;
    }
    const float pi = 3.14159265f;
    return 3.0f * std::sin(pi * t) * std::sin(pi * t / 3.0f) / (pi * pi * t * t);
}

// Weights for output phase p (0..factor-1): source taps first..first+5.
struct Phase {
    int offset;  // first tap relative to the source index (dst / factor)
    float w[kTaps];
};

void BuildPhases(Phase phases[kMaxFactor], int factor) {
    for (int p = 0; p < factor; ++p) {
        const float center = (p + 0.5f) / factor - 0.5f;  // relative to source i
        const int first = static_cast<int>(std::floor(center)) - 2;
        float sum = 0.0f;
        for (int k = 0; k < kTaps; ++k) {
            phases[p].w[k] = Lanczos3(center - static_cast<float>(first + k));
            sum += phases[p].w[k];
        }
        for (float& w : phases[p].w) {
            w /= sum;
        }
        phases[p].offset = first;
    }
}

// A texel is part of a mask drawing when it is pure white (the game tints
// these at draw time) or fully transparent. The HUD's texts - 'Mech names,
// digits, labels - are such masks: white with an anti-aliased alpha on a
// transparent background.
inline bool MaskTexel(const unsigned char* p) {
    return p[3] == 0 || (p[0] == 255 && p[1] == 255 && p[2] == 255);
}

// BGRA bytes (w x h) -> BGRA bytes (w*4 x h*4). Where the filter's whole 7x7
// source neighbourhood is mask texels, the alpha is upscaled with Lanczos-3
// and its edges firmed (colour stays white): smooth, crisp text. Everywhere
// else the source texel is copied (nearest), so the other HUD graphics -
// translucent panels, coloured lines, tightly packed boxes - look exactly as
// before, with nothing bleeding between neighbouring drawings.
void Upscale(const unsigned char* src, int w, int h, int kFactor, unsigned char* dst) {
    Phase phases[kMaxFactor];
    BuildPhases(phases, kFactor);
    const int W = w * kFactor;
    const int H = h * kFactor;
    // ok[y][x]: every texel within 3 of (x, y) is a mask texel.
    std::vector<int> bad(static_cast<size_t>(w + 1) * (h + 1), 0);  // summed area
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const int v = MaskTexel(&src[(static_cast<size_t>(y) * w + x) * 4]) ? 0 : 1;
            bad[(y + 1) * (w + 1) + x + 1] = v + bad[y * (w + 1) + x + 1] +
                                             bad[(y + 1) * (w + 1) + x] - bad[y * (w + 1) + x];
        }
    }
    auto ok = [&](int x, int y) {
        const int x0 = x - 3 < 0 ? 0 : x - 3, y0 = y - 3 < 0 ? 0 : y - 3;
        const int x1 = x + 4 > w ? w : x + 4, y1 = y + 4 > h ? h : y + 4;
        return bad[y1 * (w + 1) + x1] - bad[y0 * (w + 1) + x1] - bad[y1 * (w + 1) + x0] +
                   bad[y0 * (w + 1) + x0] ==
               0;
    };
    // Alpha, horizontal: w x h -> W x h.
    std::vector<float> mid(static_cast<size_t>(W) * h);
    for (int y = 0; y < h; ++y) {
        const unsigned char* row = &src[static_cast<size_t>(y) * w * 4];
        for (int x = 0; x < W; ++x) {
            const Phase& ph = phases[x % kFactor];
            const int base = x / kFactor + ph.offset;
            float acc = 0.0f;
            for (int k = 0; k < kTaps; ++k) {
                int sx = base + k;
                sx = sx < 0 ? 0 : (sx >= w ? w - 1 : sx);
                acc += row[sx * 4 + 3] * ph.w[k];
            }
            mid[static_cast<size_t>(y) * W + x] = acc;
        }
    }
    // Vertical, then compose.
    for (int y = 0; y < H; ++y) {
        const Phase& ph = phases[y % kFactor];
        const int sy0 = y / kFactor;
        const int base = sy0 + ph.offset;
        for (int x = 0; x < W; ++x) {
            const int sx0 = x / kFactor;
            const unsigned char* s = &src[(static_cast<size_t>(sy0) * w + sx0) * 4];
            unsigned char* o = &dst[(static_cast<size_t>(y) * W + x) * 4];
            if (!ok(sx0, sy0)) {
                o[0] = s[0]; o[1] = s[1]; o[2] = s[2]; o[3] = s[3];
                continue;
            }
            float acc = 0.0f;
            for (int k = 0; k < kTaps; ++k) {
                int sy = base + k;
                sy = sy < 0 ? 0 : (sy >= h ? h - 1 : sy);
                acc += mid[static_cast<size_t>(sy) * W + x] * ph.w[k];
            }
            float a = acc / 255.0f;
            a = (a - kPivot) * (1.0f + kFirm) + 0.5f;
            a = a < 0.0f ? 0.0f : (a > 1.0f ? 1.0f : a);
            o[0] = o[1] = o[2] = 255;
            o[3] = static_cast<unsigned char>(a * 255.0f + 0.5f);
        }
    }
}

bool IsHudImage(const char* name, int len) {
    static const char kPrefix[] = "textures\\hud\\";
    const int prefix = sizeof(kPrefix) - 1;
    return len > prefix + 4 && _strnicmp(name, kPrefix, prefix) == 0 &&
           _strnicmp(name + len - 4, ".tga", 4) == 0;
}

int g_replaced = 0;

// ---------------------------------------------------------------------------
// 'Mech names in the lance roster, drawn from the game's own font.
//
// HUDComm (the lance roster) registers one name image per chassis index c
// (0..127) in hud5m / hud6a, 7 texels tall (3.1c 0x006A5801; the right edges
// are a float table read from that code):
//   c  0..21  hud5m  x 54  .. edge  y 8c+54
//   c 22..43  hud5m  x 143 .. edge  y 8(c-22)+54
//   c 44..73  hud6a  x 2   .. edge  y 8(c-44)+5
//   c 74..103 hud6a  x 85  .. edge  y 8(c-74)+5
//   c 104..   hud6a  x 161 .. edge  y 8(c-104)+5
// The letters fill the top 6 rows, from 2 texels in to the right edge. In the
// upscaled atlas each of these is redrawn from MicroStyle Bold Extended - the
// typeface of the original images, shipped with the game as GameOS .d3f
// bitmap fonts for the menus - at the atlas's new resolution, so the roster
// names are as sharp as the pilot names beside them. The text is the
// chassis's name from ScriptStrings_MERCS.dll (string 4500 + c), in capitals.

constexpr int kChassis = 128;
constexpr DWORD kD3fHeader = 1625;
constexpr DWORD kD3fSize = kD3fHeader + 256 * 256;

struct D3fFont {
    std::vector<unsigned char> data;  // the .d3f file
    int cell = 0;                     // glyph cell height (rows)
    int cap_top = 0;                  // rows of a capital in the cell
    int cap_bottom = 0;
};
std::vector<D3fFont> g_fonts;  // MicroStyle Bold Extended sizes seen so far

struct NameRect {
    bool hud6a;
    float x0, y0, x1;  // texels of the 256x256 atlas
};
NameRect g_rects[kChassis];
bool g_rects_ok = false;
std::string g_names[kChassis];
bool g_names_loaded = false;

const unsigned char* D3fTable(const D3fFont& f, int which) {
    return f.data.data() + 0x155 + 256 * which;  // x, y, width, before, after
}

void CaptureFont(const char* name, int len, const unsigned char* data, DWORD size) {
    if (len < 16 || size != kD3fSize || memcmp(data, "D3F4", 4) != 0 ||
        _strnicmp(name + len - 11, "boldext.d3f", 11) != 0) {
        return;
    }
    bool micro = false;
    for (int i = 0; i + 10 <= len; ++i) {
        micro = micro || _strnicmp(name + i, "microstyle", 10) == 0;
    }
    if (!micro) {
        return;
    }
    D3fFont font;
    font.data.assign(data, data + size);
    font.cell = *reinterpret_cast<const int*>(data + 0x50) >> 8;  // row spacing
    if (font.cell < 6 || font.cell > 64) {
        return;
    }
    // Capital height from 'H': rows with a mostly covered texel.
    const unsigned char* x = D3fTable(font, 0);
    const unsigned char* y = D3fTable(font, 1);
    const unsigned char* w = D3fTable(font, 2);
    const unsigned char* tex = font.data.data() + kD3fHeader;
    int top = -1, bottom = -1;
    for (int r = 0; r < font.cell; ++r) {
        for (int c = 0; c < w['H']; ++c) {
            const int ty = y['H'] + r, tx = x['H'] + c;
            if (ty < 256 && tx < 256 && tex[ty * 256 + tx] > 128) {
                if (top < 0) top = r;
                bottom = r + 1;
            }
        }
    }
    if (top < 0 || bottom - top < 4) {
        return;
    }
    font.cap_top = top;
    font.cap_bottom = bottom;
    for (const D3fFont& f : g_fonts) {
        if (f.cap_bottom - f.cap_top == bottom - top) {
            return;  // have this size
        }
    }
    g_fonts.push_back(std::move(font));
    Log("hud: font %.*s (capitals %d px)\n", len, name, bottom - top);
}

void FindNameRects() {
    if (g_rects_ok) {
        return;
    }
    // mov [esp+14h],-2Ch; mov eax,[esp+14h]; mov [esp+18h], <right-edge table>
    unsigned char* code =
        FindUnique("hud_name_table", "C7 44 24 14 D4 FF FF FF 8B 44 24 14 C7 44 24 18");
    if (!code) {
        return;
    }
    // The column starts, pushed as floats in the five branches.
    static const struct {
        int offset;
        DWORD value;
    } kColumns[] = {{0x61, 0x42580000}, {0xBE, 0x430F0000}, {0x111, 0x40000000},
                    {0x165, 0x42AA0000}, {0x1AD, 0x43210000}};
    for (const auto& c : kColumns) {
        if (code[c.offset] != 0x68 || *reinterpret_cast<DWORD*>(code + c.offset + 1) != c.value) {
            Log("hud: name table layout not recognised; names not redrawn\n");
            return;
        }
    }
    const float* edge = *reinterpret_cast<float**>(code + 16);
    if (IsBadReadPtr(edge, kChassis * sizeof(float))) {
        return;
    }
    for (int c = 0; c < kChassis; ++c) {
        NameRect& r = g_rects[c];
        const float right = edge[c] + 1.0f;
        if (c < 22) {
            r = {false, 54.0f, 8.0f * c + 54.0f, right};
        } else if (c < 44) {
            r = {false, 143.0f, 8.0f * (c - 22) + 54.0f, right};
        } else if (c < 74) {
            r = {true, 2.0f, 8.0f * (c - 44) + 5.0f, right};
        } else if (c < 104) {
            r = {true, 85.0f, 8.0f * (c - 74) + 5.0f, right};
        } else {
            r = {true, 161.0f, 8.0f * (c - 104) + 5.0f, right};
        }
    }
    g_rects_ok = true;
}

void LoadNames() {
    if (g_names_loaded) {
        return;
    }
    g_names_loaded = true;
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    wchar_t* slash = n ? wcsrchr(path, L'\\') : nullptr;
    if (!slash) {
        return;
    }
    wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - path), L"ScriptStrings_MERCS.dll");
    HMODULE strings = LoadLibraryExW(path, nullptr, LOAD_LIBRARY_AS_DATAFILE);
    if (!strings) {
        Log("hud: ScriptStrings_MERCS.dll not loaded (%lu)\n", GetLastError());
        return;
    }
    int loaded = 0;
    for (int c = 0; c < kChassis; ++c) {
        wchar_t text[64] = {};
        if (LoadStringW(strings, 4500 + c, text, 64) <= 0) {
            continue;
        }
        std::string name;
        for (const wchar_t* p = text; *p; ++p) {
            wchar_t ch = *p;
            if (ch == L'_' || ch == L'-') {
                ch = L' ';
            }
            if (ch >= L'a' && ch <= L'z') {
                ch = static_cast<wchar_t>(ch - L'a' + L'A');
            }
            if (ch < 0x20 || ch > 0x7E) {
                continue;
            }
            name += static_cast<char>(ch);
        }
        // As on the original images.
        const size_t mark = name.find("MARK II");
        if (mark != std::string::npos) {
            name.replace(mark, 7, "MKII");
        }
        g_names[c] = name;
        ++loaded;
    }
    FreeLibrary(strings);
    Log("hud: %d 'Mech names loaded\n", loaded);
}

// The font whose capitals are closest to the target height (a little larger
// is better than smaller: it is scaled down, not up).
const D3fFont* PickFont(float cap_px) {
    const D3fFont* best = nullptr;
    float best_score = 1e9f;
    for (const D3fFont& f : g_fonts) {
        const float ratio = (f.cap_bottom - f.cap_top) / cap_px;
        const float score = ratio >= 1.0f ? ratio - 1.0f : (1.0f / ratio - 1.0f) * 1.5f;
        if (score < best_score) {
            best_score = score;
            best = &f;
        }
    }
    return best;
}

// Bilinear sample of the font sheet, limited to one glyph's cell.
float SampleGlyph(const unsigned char* tex, int gx, int gy, int gw, int gh, float u, float v) {
    if (u < 0.0f || v < 0.0f || u >= gw || v >= gh) {
        return 0.0f;
    }
    const float fu = u - 0.5f, fv = v - 0.5f;
    const int u0 = static_cast<int>(std::floor(fu)), v0 = static_cast<int>(std::floor(fv));
    const float au = fu - u0, av = fv - v0;
    float acc = 0.0f;
    for (int dv = 0; dv < 2; ++dv) {
        for (int du = 0; du < 2; ++du) {
            const int cu = u0 + du, cv = v0 + dv;
            if (cu < 0 || cv < 0 || cu >= gw || cv >= gh || gy + cv > 255 || gx + cu > 255) {
                continue;
            }
            const float wgt = (du ? au : 1.0f - au) * (dv ? av : 1.0f - av);
            acc += tex[(gy + cv) * 256 + gx + cu] * wgt;
        }
    }
    return acc / 255.0f;
}

// Draws `text` into the BGRA image (W x H, rows top-down via `row`) so its
// capitals span [top, top + cap) and it runs from `left` to `right`.
template <typename RowFn>
void DrawName(const std::string& text, const D3fFont& font, float left, float right, float top,
              float cap, int W, int H, RowFn row) {
    const unsigned char* gx = D3fTable(font, 0);
    const unsigned char* gy = D3fTable(font, 1);
    const unsigned char* gw = D3fTable(font, 2);
    const unsigned char* pre = D3fTable(font, 3);
    const unsigned char* post = D3fTable(font, 4);
    const unsigned char* tex = font.data.data() + kD3fHeader;
    auto s8 = [](unsigned char v) { return v > 127 ? v - 256 : static_cast<int>(v); };
    const float scale = cap / (font.cap_bottom - font.cap_top);
    // Natural width, then spread or squeeze to fill the original's span.
    float natural = 0.0f;
    const int n = static_cast<int>(text.size());
    for (int i = 0; i < n; ++i) {
        const unsigned char ch = static_cast<unsigned char>(text[i]);
        natural += (s8(pre[ch]) + gw[ch] + (i + 1 < n ? s8(post[ch]) : 0)) * scale;
    }
    const float avail = right - left;
    float sx = 1.0f, gap = 0.0f;
    if (natural > avail) {
        sx = avail / natural;
    } else if (n > 1) {
        gap = (avail - natural) / (n - 1);
        if (gap > cap * 0.35f) {
            gap = cap * 0.35f;  // keep short names left-aligned, like the originals
        }
    }
    float pen = left;
    for (int i = 0; i < n; ++i) {
        const unsigned char ch = static_cast<unsigned char>(text[i]);
        pen += s8(pre[ch]) * scale * sx;
        const float gw_px = gw[ch] * scale * sx;
        const float cell_top = top - font.cap_top * scale;  // row 0 of the cell
        const int x0 = static_cast<int>(std::floor(pen)), x1 = static_cast<int>(std::ceil(pen + gw_px));
        const int y0 = static_cast<int>(std::floor(cell_top));
        const int y1 = static_cast<int>(std::ceil(cell_top + font.cell * scale));
        for (int y = y0 < 0 ? 0 : y0; y < y1 && y < H; ++y) {
            unsigned char* line = row(y);
            for (int x = x0 < 0 ? 0 : x0; x < x1 && x < W; ++x) {
                // 4x4 supersampled coverage of this output pixel.
                float cov = 0.0f;
                for (int sy = 0; sy < 4; ++sy) {
                    for (int sxs = 0; sxs < 4; ++sxs) {
                        const float px = x + (sxs + 0.5f) / 4.0f, py = y + (sy + 0.5f) / 4.0f;
                        cov += SampleGlyph(tex, gx[ch], gy[ch], gw[ch], font.cell,
                                           (px - pen) / (scale * sx), (py - cell_top) / scale);
                    }
                }
                cov /= 16.0f;
                unsigned char* o = line + x * 4;
                const int a = static_cast<int>(cov * 255.0f + 0.5f);
                if (a > o[3]) {
                    o[0] = o[1] = o[2] = 255;
                    o[3] = static_cast<unsigned char>(a > 255 ? 255 : a);
                }
            }
        }
        pen += gw_px + (i + 1 < n ? s8(post[ch]) * scale * sx : 0.0f) + gap;
    }
}

// Redraws the roster names in an upscaled hud5m / hud6a (BGRA, W x H).
void RedrawNames(bool hud6a, unsigned char* pixels, int W, int H, int factor, bool top_down) {
    FindNameRects();
    LoadNames();
    const float cap = 6.0f * factor;
    const D3fFont* font = PickFont(cap);
    if (!g_rects_ok || !font) {
        Log("hud: names not redrawn (table %d, fonts %d)\n", g_rects_ok ? 1 : 0,
            static_cast<int>(g_fonts.size()));
        return;
    }
    auto row = [&](int y) { return pixels + static_cast<size_t>(top_down ? y : H - 1 - y) * W * 4; };
    int drawn = 0;
    for (int c = 0; c < kChassis; ++c) {
        const NameRect& r = g_rects[c];
        if (r.hud6a != hud6a || g_names[c].empty() || r.x1 <= r.x0 + 4.0f) {
            continue;
        }
        // Clear the original image, then draw the name.
        const int cx0 = static_cast<int>(r.x0 * factor), cx1 = static_cast<int>(r.x1 * factor);
        const int cy0 = static_cast<int>(r.y0 * factor), cy1 = static_cast<int>((r.y0 + 7.0f) * factor);
        for (int y = cy0; y < cy1 && y < H; ++y) {
            unsigned char* line = row(y);
            for (int x = cx0; x < cx1 && x < W; ++x) {
                unsigned char* o = line + x * 4;
                o[0] = o[1] = o[2] = 255;
                o[3] = 0;
            }
        }
        DrawName(g_names[c], *font, (r.x0 + 2.0f) * factor, r.x1 * factor, r.y0 * factor, cap, W,
                 H, row);
        ++drawn;
    }
    Log("hud: %d 'Mech names redrawn in %s (font capitals %d px, target %.0f px)\n", drawn,
        hud6a ? "hud6a" : "hud5m", font->cap_bottom - font->cap_top, cap);
}

// Texels per original texel: the HUD scale (screen height / 600) rounded up,
// so the atlas is drawn close to 1:1 on screen.
int HudFactor() {
    int w = 0, h = 0;
    SelectedSize(&w, &h);
    int f = static_cast<int>(std::ceil(h / 600.0));
    return f < 2 ? 2 : (f > kMaxFactor ? kMaxFactor : f);
}

}  // namespace

void SetGameAllocator(void* alloc, void* free_fn) {
    g_alloc = reinterpret_cast<GameAlloc_t>(alloc);
    g_free = reinterpret_cast<GameFree_t>(free_fn);
}

// Called by the archive member fetch hook after the game filled the entry.
void SharpenHudImage(unsigned char* entry, void* archive) {
    if (!GetConfig().hud_sharp || !g_alloc || !g_free || !archive) {
        return;
    }
    const int len = entry[kEntryNameLen];
    const char* name = reinterpret_cast<const char*>(entry + kEntryName);
    {
        const unsigned char* fdata = *reinterpret_cast<unsigned char**>(entry + kEntryData);
        const DWORD fsize = *reinterpret_cast<DWORD*>(entry + kEntrySize);
        if (fdata && len > 4 && _strnicmp(name + len - 4, ".d3f", 4) == 0) {
            CaptureFont(name, len, fdata, fsize);
            return;
        }
    }
    if (!IsHudImage(name, len)) {
        return;
    }
    const DWORD stored = *reinterpret_cast<DWORD*>(entry + kEntryStored);
    const DWORD size = *reinterpret_cast<DWORD*>(entry + kEntrySize);
    unsigned char* data = *reinterpret_cast<unsigned char**>(entry + kEntryData);
    // Only a decompressed member owns its buffer (see above).
    if (!data || stored >= size || !entry[kEntryOwned] ||
        *reinterpret_cast<int*>(entry + kEntryOffset) == -1 || size < 18) {
        return;
    }
    // Uncompressed 32-bit true-colour TGA, no colour map, at most 512 wide.
    const int id_len = data[0];
    const int w = data[12] | (data[13] << 8);
    const int h = data[14] | (data[15] << 8);
    if (data[1] != 0 || data[2] != 2 || data[16] != 32 || w <= 0 || h <= 0 || w > 512 ||
        h > 512) {
        return;
    }
    const DWORD pixels = static_cast<DWORD>(w) * h * 4;
    if (18u + id_len + pixels > size) {
        return;
    }
    const int factor = HudFactor();
    const int W = w * factor;
    const int H = h * factor;
    const DWORD out_size = 18u + static_cast<DWORD>(W) * H * 4;
    const DWORD heap = *reinterpret_cast<DWORD*>(static_cast<char*>(archive) + kArchiveHeap);
    auto* out = static_cast<unsigned char*>(g_alloc(out_size, heap));
    if (!out) {
        return;
    }
    memset(out, 0, 18);
    out[2] = 2;
    out[12] = static_cast<unsigned char>(W & 0xFF);
    out[13] = static_cast<unsigned char>(W >> 8);
    out[14] = static_cast<unsigned char>(H & 0xFF);
    out[15] = static_cast<unsigned char>(H >> 8);
    out[16] = 32;
    out[17] = data[17];  // alpha bits and row order unchanged
    Upscale(data + 18 + id_len, w, h, factor, out + 18);
    if (GetConfig().hud_names && w == 256 && h == 256) {
        const bool hud5m = len >= 9 && _strnicmp(name + len - 9, "hud5m.tga", 9) == 0;
        const bool hud6a = len >= 9 && _strnicmp(name + len - 9, "hud6a.tga", 9) == 0;
        if (hud5m || hud6a) {
            RedrawNames(hud6a, out + 18, W, H, factor, (data[17] & 0x20) != 0);
        }
    }
    g_free(data);
    *reinterpret_cast<unsigned char**>(entry + kEntryData) = out;
    *reinterpret_cast<DWORD*>(entry + kEntrySize) = out_size;
    if (g_replaced++ < 40) {
        Log("hud: %.*s %dx%d -> %dx%d\n", len, name, w, h, W, H);
    }
}

}  // namespace mw4hook
