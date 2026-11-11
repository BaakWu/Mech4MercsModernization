// Display-resolution defaults.
//
// MW4 Mercenaries 3.1c initializes its graphics globals to 800x600 before it
// reads options.ini, and its -window path pushes 800x600 again when it applies
// the display mode. The hook rewrites those constants in memory so the game's
// internal render size matches the forced 1024x768 window. The executable on
// disk is not modified.
#include "hook.h"

#include <cstring>

namespace mw4hook {
namespace {

constexpr unsigned char kStoreDword = 0xC7;
constexpr unsigned char kModRmAbsolute = 0x05;
constexpr unsigned char kWidth800[4] = {0x20, 0x03, 0x00, 0x00};
constexpr unsigned char kHeight600[4] = {0x58, 0x02, 0x00, 0x00};

constexpr size_t kScanLimit = 0x200000;
constexpr int kMaxScans = 400;

bool g_scan_done = false;
int g_scan_count = 0;

void WriteDword(unsigned char* target, int value) {
    unsigned char bytes[4] = {static_cast<unsigned char>(value & 0xFF),
                              static_cast<unsigned char>((value >> 8) & 0xFF),
                              static_cast<unsigned char>((value >> 16) & 0xFF),
                              static_cast<unsigned char>((value >> 24) & 0xFF)};
    DWORD old_protect = 0;
    if (VirtualProtect(target, 4, PAGE_READWRITE, &old_protect)) {
        memcpy(target, bytes, 4);
        VirtualProtect(target, 4, old_protect, &old_protect);
    }
}

bool PatchWindowedModePushes(const Config& config, unsigned char* start,
                             size_t size) {
    bool patched = false;
    __try {
        for (size_t offset = 0; offset + 11 < size; ++offset) {
            unsigned char* p = start + offset;
            if (p[0] != 0x68 || p[1] != kHeight600[0] || p[2] != kHeight600[1] ||
                p[3] != 0x00 || p[4] != 0x00) {
                continue;
            }
            if (p[5] != 0x68 || p[6] != kWidth800[0] || p[7] != kWidth800[1] ||
                p[8] != 0x00 || p[9] != 0x00) {
                continue;
            }
            if (p[10] != 0xE8) {
                continue;
            }
            WriteDword(p + 1, config.resolution_height);
            WriteDword(p + 6, config.resolution_width);
            patched = true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return patched;
    }
    return patched;
}

bool PatchStorePairs(const Config& config, unsigned char* start, size_t size,
                     int* candidate_count) {
    bool patched = false;
    __try {
        for (size_t offset = 0; offset + 30 < size; ++offset) {
            unsigned char* p = start + offset;
            if (p[0] != kStoreDword || p[1] != kModRmAbsolute) {
                continue;
            }
            if (memcmp(p + 6, kWidth800, 4) != 0) {
                continue;
            }
            for (size_t look = 0x0A; look < 0x30; ++look) {
                unsigned char* q = p + look;
                if (q[0] != kStoreDword || q[1] != kModRmAbsolute) {
                    continue;
                }
                if (memcmp(q + 6, kHeight600, 4) != 0) {
                    continue;
                }

                ++*candidate_count;
                WriteDword(q + 6, config.resolution_height);
                WriteDword(p + 6, config.resolution_width);
                patched = true;

                auto* width_global = reinterpret_cast<unsigned char*>(
                    static_cast<uintptr_t>(*reinterpret_cast<DWORD*>(p + 2)));
                auto* height_global = reinterpret_cast<unsigned char*>(
                    static_cast<uintptr_t>(*reinterpret_cast<DWORD*>(q + 2)));
                __try {
                    if (*reinterpret_cast<DWORD*>(width_global) == 800 &&
                        *reinterpret_cast<DWORD*>(height_global) == 600) {
                        WriteDword(height_global, config.resolution_height);
                        WriteDword(width_global, config.resolution_width);
                    }
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                }
                break;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return patched;
    }
    return patched;
}

}  // namespace

void ApplyResolutionDefaults() {
    const Config& config = GetConfig();
    if (!config.resolution_enabled || g_scan_done) {
        return;
    }
    if (config.resolution_width == 800 && config.resolution_height == 600) {
        g_scan_done = true;
        return;
    }
    if (++g_scan_count > kMaxScans) {
        g_scan_done = true;
        return;
    }

    HMODULE module = GetModuleHandleW(nullptr);
    if (!module) {
        return;
    }
    auto* base = reinterpret_cast<unsigned char*>(module);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    auto* section = IMAGE_FIRST_SECTION(nt);

    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        if ((section->Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0) {
            continue;
        }
        size_t size = section->Misc.VirtualSize;
        if (size > kScanLimit) {
            size = kScanLimit;
        }
        unsigned char* start = base + section->VirtualAddress;
        int candidates = 0;
        bool stores = PatchStorePairs(config, start, size, &candidates);
        bool pushes = PatchWindowedModePushes(config, start, size);
        if (stores || pushes) {
            Log("resolution: patched %s (%d store pair(s), pushes=%d)\n",
                section->Name, candidates, pushes ? 1 : 0);
        }
    }
}

}  // namespace mw4hook
