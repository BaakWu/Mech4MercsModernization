// GameOS error reporting (issue #47).
//
// 1. Null current heap. `operator new` in 3.1c (0x0040F3E0) is
//    gos_Malloc(size, -1): -1 means "the current heap", the top of the heap
//    stack that gos_PushCurrentHeap / gos_PopCurrentHeap (0x006FB3C0 /
//    0x006FB400) maintain. The bottom of that stack is null, and gos_Malloc
//    (0x006FA790) checks a 0 heap argument (it substitutes "Client Default")
//    but not the -1 substitution:
//
//      006FAA11  mov eax,[ebp+0C]          ; heap argument
//      006FAA14  cmp eax,-1
//      006FAA17  jne 006FAA24
//      006FAA19  mov edx,[0123123C]        ; current heap (may be null)
//      006FAA1F  mov [ebp+0C],edx
//      006FAA22  jmp 006FAA30              ; <- no null check
//      006FAA24  cmp eax,edi               ; edi = 0
//      006FAA26  jne 006FAA30
//      006FAA28  mov eax,[01231240]        ; "Client Default" heap
//      006FAA2D  mov [ebp+0C],eax
//      006FAA30  ...                       ; 006FABAA: mov cl,[heap+94h]
//
//    A `new` while no heap is pushed then faults at 0x006FABAA reading
//    [0+94h]. GameOS' error handler does exactly that when an error is
//    raised outside a pushed scope: its "GetProcessorDetails" step allocates
//    a 12-byte object, faults, and the player sees "Nested exception! -
//    Cause: 'GetProcessorDetails' ... at 0x6fabaa" instead of the real error.
//    The patch loads the current heap into eax and falls through to the
//    existing heap == 0 test, so a null current heap uses the default heap
//    (bytes 8B 15 <cur> 89 55 0C EB 0C -> A1 <cur> 89 45 0C 90 90 90). Only
//    a call that would have faulted changes; a block's header records its
//    heap, so freeing it is unchanged. [gameos] heap_fallback=0 turns it off.
//
// 2. The first error is kept. GameOS' error handler (0x006FF600,
//    __stdcall (flags, text)) receives the message of every STOP / assert /
//    exception. The hook writes it to mw4hook-<pid>.log before the dialog,
//    so the original error survives even if the handler itself fails.
#include "hook.h"

#include <cstring>

namespace mw4hook {
namespace {

using ErrorHandler_t = int(__stdcall*)(int flags, const char* text);
ErrorHandler_t g_error_handler = nullptr;  // trampoline
volatile LONG g_errors = 0;

// GameOS sends its non-fatal warnings through the same handler (flags 0x182,
// the same as a STOP from a failed DirectX call; DirectInput warnings repeat
// every frame), so each distinct text is logged once.
constexpr int kSeen = 256;
DWORD g_seen[kSeen];
volatile LONG g_seen_count = 0;

bool FirstTime(const char* text) {
    DWORD hash = 2166136261u;  // FNV-1a
    __try {
        for (const char* p = text; p && *p; ++p) {
            hash = (hash ^ static_cast<unsigned char>(*p)) * 16777619u;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    const LONG count = g_seen_count < kSeen ? g_seen_count : kSeen;
    for (LONG i = 0; i < count; ++i) {
        if (g_seen[i] == hash) {
            return false;
        }
    }
    const LONG slot = InterlockedIncrement(&g_seen_count) - 1;
    if (slot < kSeen) {
        g_seen[slot] = hash;
    }
    return true;
}

int __stdcall Hook_ErrorHandler(int flags, const char* text) {
    const LONG n = InterlockedIncrement(&g_errors);
    if (!FirstTime(text)) {
        return g_error_handler(flags, text);
    }
    char line[1200];
    size_t len = 0;
    __try {
        for (const char* p = text; p && *p && len + 1 < sizeof(line); ++p) {
            char c = *p;
            if (c == '\r') {
                continue;
            }
            if (c == '\n') {
                if (len + 3 >= sizeof(line)) {
                    break;
                }
                line[len++] = ' ';
                line[len++] = '|';
                c = ' ';
            }
            line[len++] = c;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    line[len] = '\0';
    Log("gameos: error #%ld (flags 0x%X): %s\n", n, static_cast<unsigned>(flags), line);
    return g_error_handler(flags, text);
}

bool PatchNullCurrentHeap() {
    unsigned char* site = FindUnique(
        "gos_malloc_current_heap",
        "8B 45 0C 83 F8 FF 75 0B 8B 15 ?? ?? ?? ?? 89 55 0C EB 0C 3B C7 75 08 A1 ?? ?? ?? ?? "
        "89 45 0C");
    if (!site) {
        return false;
    }
    unsigned char code[11] = {0xA1, 0, 0, 0, 0, 0x89, 0x45, 0x0C, 0x90, 0x90, 0x90};
    memcpy(code + 1, site + 10, 4);  // address of the current-heap global
    return WriteCode(site + 8, code, sizeof(code));
}

bool g_installed = false;

}  // namespace

bool InstallGameOsHooks() {
    if (g_installed) {
        return true;
    }
    unsigned char* handler = FindUnique(
        "gos_error_handler",
        "55 8B EC 81 EC E0 01 00 00 A0 ?? ?? ?? ?? 53 56 57 84 C0 C7 45 D8 00 00 00 00 0F 85");
    if (!handler) {
        return false;  // not unpacked yet (or not this build); retry
    }
    g_installed = true;
    // push ebp; mov ebp,esp; sub esp,1E0h: 9 bytes, position independent.
    InstallDetour(handler, 9, reinterpret_cast<void*>(&Hook_ErrorHandler),
                  reinterpret_cast<void**>(&g_error_handler));
    Log("gameos: error text logging %s\n", g_error_handler ? "ok" : "FAILED");
    if (GetConfig().gameos_heap_fallback) {
        const bool ok = PatchNullCurrentHeap();
        Log("gameos: null current heap -> default heap %s\n", ok ? "ok" : "not patched");
    } else {
        Log("gameos: heap fallback off ([gameos] heap_fallback=0)\n");
    }
    return true;
}

}  // namespace mw4hook
