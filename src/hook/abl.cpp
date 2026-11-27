// ABL script crash report (issue #44).
//
// The mission scripts run in ABL, the FASA script interpreter also used by
// MechCommander 2. In a session of 2026-11-24 the game crashed in a mission
// at 0x00667FA4: ABL's routine call (3.1c 0x00667D40; PR1 0x0064F060, the
// same code) had run the routine, whose exit (0x00664010) restored the
// caller's code pointer from the routine's stack frame ([frame+0Ch]), and
// reading the next token through that pointer faulted:
//
//   00667F98  mov eax,[codeSegmentPtr]
//   00667FA4  movsx edx,byte ptr [eax]   ; fault
//
// So the frame held a bad return address (a damaged ABL stack, or code that
// was freed while it ran). The code is unchanged from retail; the cause is
// unknown and there is no dump. This file does not change the game: a
// vectored exception handler sees a fault inside the routine call first and
// writes the script state to mw4hook-<pid>.log (module, routine, call depth,
// code pointer, stack frame), then lets GameOS handle the exception as
// before. If it happens again, the log names the script.
//
// Test aid: [debug] abl_crash=S damages the current ABL frame's return
// address S seconds after start, which reproduces the fault (the game then
// stops on GameOS' error dialog, as in the report).
#include "hook.h"

#include <cstring>

namespace mw4hook {
namespace {

// Addresses taken from the routine call's own instructions.
unsigned char* g_call = nullptr;    // routine call start
size_t g_call_size = 0x28B;         // through its ret
char** g_code_pointer = nullptr;    // codeSegmentPtr
int* g_depth = nullptr;             // routine call depth
unsigned char** g_module = nullptr; // current module (name at +4)
unsigned char** g_frame = nullptr;  // stackFrameBasePtr
volatile LONG g_reported = 0;

void CopyText(const char* text, char* out, size_t cch) {
    size_t n = 0;
    __try {
        for (; text && n + 1 < cch && text[n]; ++n) {
            const char c = text[n];
            out[n] = (c >= 0x20 && c < 0x7F) ? c : '?';
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    out[n] = '\0';
    if (n == 0) {
        strcpy_s(out, cch, "?");
    }
}

template <typename T>
T Read(const void* address) {
    T value{};
    __try {
        value = *static_cast<const T*>(address);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return value;
}

LONG CALLBACK AblExceptionHandler(EXCEPTION_POINTERS* info) {
    const EXCEPTION_RECORD* record = info->ExceptionRecord;
    if (record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const auto eip = reinterpret_cast<unsigned char*>(info->ContextRecord->Eip);
    if (eip < g_call || eip >= g_call + g_call_size) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (InterlockedIncrement(&g_reported) > 3) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    // In the routine call ebp holds the called routine's symbol node: name
    // pointer at +10h, its library module at +40h.
    const auto routine = reinterpret_cast<const unsigned char*>(info->ContextRecord->Ebp);
    char routine_name[64];
    char module_name[64];
    char library_name[64];
    CopyText(Read<const char*>(routine + 0x10), routine_name, sizeof(routine_name));
    const unsigned char* module = Read<unsigned char*>(g_module);
    CopyText(module ? reinterpret_cast<const char*>(module + 4) : nullptr, module_name,
             sizeof(module_name));
    const unsigned char* library = Read<unsigned char*>(routine + 0x40);
    CopyText(library ? reinterpret_cast<const char*>(library + 4) : nullptr, library_name,
             sizeof(library_name));
    const unsigned long address =
        record->NumberParameters >= 2 ? static_cast<unsigned long>(record->ExceptionInformation[1])
                                      : 0;
    Log("abl: access violation at %08lX (%s %08lX) returning from ABL routine '%s' "
        "(library %s) in module '%s'; call depth %d, code pointer %p, stack frame %p\n",
        static_cast<unsigned long>(info->ContextRecord->Eip),
        record->NumberParameters >= 1 && record->ExceptionInformation[0] ? "writing" : "reading",
        address, routine_name, library ? library_name : "none", module_name, Read<int>(g_depth),
        static_cast<void*>(Read<char*>(g_code_pointer)),
        static_cast<void*>(Read<unsigned char*>(g_frame)));
    // Test aid: lose the sound buffers now, as in the 2026-11-24 session
    // (GameOS then pauses its sounds for the dialog).
    if (GetConfig().debug_abl_crash > 0 && GetConfig().debug_sound_lost_play < 0) {
        LoseSoundBuffersForTest();
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

DWORD WINAPI CrashTestThread(LPVOID) {
    Sleep(static_cast<DWORD>(GetConfig().debug_abl_crash) * 1000);
    Log("abl: TEST: damaging the ABL frame's return address\n");
    for (int i = 0; i < 600000; ++i) {
        unsigned char* frame = Read<unsigned char*>(g_frame);
        if (frame && Read<int>(g_depth) > 0) {
            __try {
                *reinterpret_cast<unsigned long*>(frame + 0x0C) = 0x00000094;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
        }
        Sleep(0);
    }
    return 0;
}

template <typename T>
T* Operand(unsigned char* at) {
    return *reinterpret_cast<T**>(at);
}

bool g_installed = false;

}  // namespace

bool InstallAblCrashReport() {
    if (g_installed) {
        return true;
    }
    // sub esp,414h; push ebx; push ebp; mov ebp,[esp+420h]; xor eax,eax;
    // mov edx,[tos]; push esi; mov al,[ebp+44h]; mov ecx,[level]; push edi;
    // mov edi,[depth]; mov esi,eax
    unsigned char* call = FindUnique(
        "abl_routine_call",
        "81 EC 14 04 00 00 53 55 8B AC 24 20 04 00 00 33 C0 8B 15 ?? ?? ?? ?? 56 8A 45 44 "
        "8B 0D ?? ?? ?? ?? 57 8B 3D ?? ?? ?? ?? 8B F0");
    if (!call) {
        return false;  // not unpacked yet (or not this build); retry
    }
    g_installed = true;
    // mov edi,[depth] (+22h), mov eax,[codeSegmentPtr] (+62h),
    // mov [stackFrameBasePtr],edi (+A2h), mov ebx,[module] (+BCh),
    // movsx edx,byte ptr [eax] (+264h, the fault), ret (+28Ah)
    if (memcmp(call + 0x22, "\x8B\x3D", 2) != 0 || call[0x62] != 0xA1 ||
        memcmp(call + 0xA2, "\x89\x3D", 2) != 0 || memcmp(call + 0xBC, "\x8B\x1D", 2) != 0 ||
        memcmp(call + 0x264, "\x0F\xBE\x10", 3) != 0 || call[0x28A] != 0xC3) {
        Log("abl: routine call layout differs; crash report off\n");
        return true;
    }
    g_call = call;
    g_depth = Operand<int>(call + 0x24);
    g_code_pointer = Operand<char*>(call + 0x63);
    g_frame = Operand<unsigned char*>(call + 0xA4);
    g_module = Operand<unsigned char*>(call + 0xBE);
    const bool ok = AddVectoredExceptionHandler(1, &AblExceptionHandler) != nullptr;
    Log("abl: script crash report %s (routine call %p)\n", ok ? "ok" : "FAILED",
        static_cast<void*>(call));
    if (ok && GetConfig().debug_abl_crash > 0) {
        if (HANDLE thread = CreateThread(nullptr, 0, &CrashTestThread, nullptr, 0, nullptr)) {
            CloseHandle(thread);
        }
    }
    return true;
}

}  // namespace mw4hook
