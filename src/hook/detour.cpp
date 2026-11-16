// Pattern scanning and small inline detours for the unpacked 3.1c image.
//
// Every patch site is located by a byte pattern that was verified to be
// unique in the supported 51.03.01.0030 build. If a pattern is missing or not
// unique, nothing is patched and the feature stays off.
#include "hook.h"

#include <cstdlib>
#include <cstring>

namespace mw4hook {
namespace {

bool ParsePattern(const char* text, unsigned char* bytes, bool* mask, size_t* len) {
    size_t n = 0;
    for (const char* p = text; *p;) {
        while (*p == ' ') {
            ++p;
        }
        if (!*p) {
            break;
        }
        if (n >= 128) {
            return false;
        }
        if (p[0] == '?' && p[1] == '?') {
            mask[n] = false;
            bytes[n++] = 0;
            p += 2;
            continue;
        }
        char hex[3] = {p[0], p[1], 0};
        bytes[n] = static_cast<unsigned char>(strtoul(hex, nullptr, 16));
        mask[n++] = true;
        p += 2;
    }
    *len = n;
    return n > 0;
}

bool TextSection(unsigned char** start, size_t* size) {
    auto* base = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    auto* section = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        if (section->Characteristics & IMAGE_SCN_MEM_EXECUTE) {
            *start = base + section->VirtualAddress;
            *size = section->Misc.VirtualSize;
            return true;
        }
    }
    return false;
}

}  // namespace

unsigned char* FindUnique(const char* name, const char* pattern) {
    unsigned char bytes[128];
    bool mask[128];
    size_t len = 0;
    unsigned char* start = nullptr;
    size_t size = 0;
    if (!ParsePattern(pattern, bytes, mask, &len) || !TextSection(&start, &size)) {
        return nullptr;
    }
    unsigned char* found = nullptr;
    int count = 0;
    __try {
        for (size_t i = 0; i + len <= size; ++i) {
            if (start[i] != bytes[0]) {
                continue;
            }
            size_t j = 1;
            while (j < len && (!mask[j] || start[i + j] == bytes[j])) {
                ++j;
            }
            if (j == len) {
                found = start + i;
                if (++count > 1) {
                    break;
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
    if (count != 1) {
        Log("pattern %s: %d match(es); feature disabled\n", name, count);
        return nullptr;
    }
    Log("pattern %s at %p\n", name, found);
    return found;
}

bool WriteCode(void* address, const void* data, size_t size) {
    DWORD old = 0;
    if (!VirtualProtect(address, size, PAGE_EXECUTE_READWRITE, &old)) {
        return false;
    }
    memcpy(address, data, size);
    VirtualProtect(address, size, old, &old);
    FlushInstructionCache(GetCurrentProcess(), address, size);
    return true;
}

// Replace the first |stolen| bytes of |target| with a jump to |hook|. The
// stolen instructions must be position independent (callers pass patterns
// that guarantee this). Returns a trampoline that runs them and continues at
// target + stolen. The jump is written with one 8-byte atomic exchange so a
// thread can never execute a half-written instruction.
void* InstallDetour(unsigned char* target, size_t stolen, void* hook, void** trampoline_out) {
    if (!target || stolen < 5 || stolen > 16) {
        return nullptr;
    }
    auto* trampoline = static_cast<unsigned char*>(
        VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!trampoline) {
        return nullptr;
    }
    memcpy(trampoline, target, stolen);
    trampoline[stolen] = 0xE9;
    *reinterpret_cast<LONG*>(trampoline + stolen + 1) =
        static_cast<LONG>((target + stolen) - (trampoline + stolen + 5));
    FlushInstructionCache(GetCurrentProcess(), trampoline, 64);
    // Publish the trampoline before the jump goes live.
    *reinterpret_cast<void* volatile*>(trampoline_out) = trampoline;
    MemoryBarrier();

    unsigned char patch[8];
    memcpy(patch, target, 8);
    patch[0] = 0xE9;
    *reinterpret_cast<LONG*>(patch + 1) =
        static_cast<LONG>(static_cast<unsigned char*>(hook) - (target + 5));

    DWORD old = 0;
    if (!VirtualProtect(target, 8, PAGE_EXECUTE_READWRITE, &old)) {
        *trampoline_out = nullptr;
        return nullptr;  // trampoline intentionally leaked: it may be published
    }
    LONGLONG original = *reinterpret_cast<volatile LONGLONG*>(target);
    LONGLONG replacement;
    memcpy(&replacement, patch, 8);
    InterlockedCompareExchange64(reinterpret_cast<volatile LONGLONG*>(target),
                                 replacement, original);
    VirtualProtect(target, 8, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, 8);
    return trampoline;
}

}  // namespace mw4hook
