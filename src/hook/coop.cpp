// Campaign co-op recon (issue #16). Development only, off unless [debug]
// coop_trace=1 or coop_mission=<name> is set; nothing here ships enabled.
//
// Every mission start (single player, campaign, network) ends in
//   game->SetMissionPath(path)   3.1c 0x0053DC50, thiscall, 1 argument
//   game->Start()                3.1c 0x005306E0
// SetMissionPath only copies the path into the game object (+0x20ACB8);
// Start() loads it. The network start (0x005289E0) builds the path from the
// hosted scenario ("Resource\Missions\<name>.mw4").
//
// coop_trace logs every SetMissionPath call with its caller.
// coop_mission=<instance> replaces the mission of a *network* start with
// Missions\<instance>\<instance>.instance (the network start passes the
// hosted map's, e.g. Missions\DefianceHolds\DefianceHolds.instance), to test
// whether a campaign mission (eaton01's is eaton01_mercalley) loads and runs
// its scripts in a network game.
#include "hook.h"

#include <intrin.h>
#include <tlhelp32.h>
#include <cstdio>
#include <cstring>

namespace mw4hook {
namespace {

using SetMissionPath_t = void(__thiscall*)(void* game, const char* path);
SetMissionPath_t g_set_path = nullptr;  // trampoline
unsigned char* g_net_return = nullptr;  // return address of the network start's call
bool g_installed = false;
volatile bool g_own_script = false;  // the current network mission keeps its own script
void* g_script_continue = nullptr;   // the network branch of the script choice
void* g_script_load = nullptr;       // the single-player branch (load the mission's script)
char g_mission[64] = {};
char g_path[MAX_PATH] = {};

void* Campaign();

void __fastcall Hook_SetMissionPath(void* game, void* /*edx*/, const char* path) {
    void* ret = _ReturnAddress();
    const bool network = ret == g_net_return;
    const char* use = path;
    if (network && g_mission[0] && path) {
        // The network start passes the mission's instance inside its archive,
        // "Missions\<name>\<name>.instance"; use the other mission's.
        sprintf_s(g_path, "Missions\\%s\\%s.instance", g_mission, g_mission);
        use = g_path;
    }
    g_own_script = use != path;  // the next mission load keeps the mission's own script
    Log("coop: SetMissionPath \"%s\" from %p (%s)%s%s\n", path ? path : "(null)", ret,
        network ? "network start" : "other", use != path ? " -> " : "", use != path ? use : "");
    Log("coop: campaign %p, shell mode %d\n", Campaign(),
        *reinterpret_cast<unsigned char**>(0x0103D850)
            ? *reinterpret_cast<int*>(*reinterpret_cast<unsigned char**>(0x0103D850) + 0x24)
            : -99);
    g_set_path(game, use);
}

// Mission load (3.1c 0x006099B0) picks the mission's controller script. The
// script named by the mission (e.g. Eaton01_MercAlley.abl: story, dialogue,
// objectives) is used unless the game has network settings (game+0x50); then
// it is replaced by the game type's (<map>_<type>.abl or Stock_<type>.abl):
//   mov esi,[edx+50h]; test esi,esi; je <load script>; ...
// The stub takes the single-player branch while g_own_script is set.
__declspec(naked) void StubScriptChoice() {
    __asm {
        cmp byte ptr [g_own_script], 0
        jne own
        test esi, esi
        je own
        jmp dword ptr [g_script_continue]
    own:
        jmp dword ptr [g_script_load]
    }
}

void InstallScriptChoice() {
    // mov esi,[edx+50h]; test esi,esi; je rel32; mov eax,[eax+0Ch];
    // xor eax,85739531h; mov cl,[eax+17h]
    unsigned char* site = FindUnique(
        "mission_script_choice",
        "8B 72 50 85 F6 0F 84 ?? ?? ?? ?? 8B 40 0C 35 31 95 73 85 8A 48 17");
    if (!site) {
        return;
    }
    unsigned char* test = site + 3;  // test esi,esi (2) + je rel32 (6)
    g_script_continue = test + 8;
    g_script_load = test + 8 + *reinterpret_cast<int*>(test + 4);
    unsigned char code[8] = {0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90};
    const int rel = static_cast<int>(reinterpret_cast<unsigned char*>(&StubScriptChoice) -
                                     (test + 5));
    memcpy(code + 1, &rel, 4);
    const bool ok = WriteCode(test, code, sizeof(code));
    Log("coop: mission script choice %s (load %p)\n", ok ? "patched" : "FAILED", g_script_load);
}

// A 'Mech's AI (vehicle+0x2D4; PlayerAI for the player's) is created at
// mission start (3.1c 0x00554ED0) only when the game is not networked
// (game+0xC -> +0x17 == 0):
//   mov edx,[eax+0Ch]; xor edx,85739531h; cmp [edx+17h],bl; jne <skip>;
//   cmp [ebp+5B8h],bl
// A campaign story script hands the player's 'Mech to its AI (e.g. for the
// intro), and VehicleInterface::ExecutionStateEngine state 6 then calls
// vehicle+0x2D4 -> null. The stub creates the AI while g_own_script is set.
void* g_ai_continue = nullptr;
void* g_ai_skip = nullptr;

__declspec(naked) void StubAiGate() {
    __asm {
        cmp byte ptr [g_own_script], 0
        jne create
        cmp byte ptr [edx+17h], bl
        jne skip
    create:
        jmp dword ptr [g_ai_continue]
    skip:
        jmp dword ptr [g_ai_skip]
    }
}

// MW4Shell::InitMPScreen (3.1c 0x00507FE0), run when the network hosting
// screens open, deletes the loaded campaign (globals+0x44):
//   mov [esi+409A0h],eax; mov eax,[..]; mov edx,[..]; mov ecx,[eax]; xor ecx,edx;
//   mov eax,[ecx+44h]; mov edx,eax; xor edx,85739531h; je <keep>; ... delete
// A co-op host keeps its campaign (the mission result goes to it at the end),
// so with coop_mission set the je becomes jmp.
void InstallKeepCampaign() {
    unsigned char* site = FindUnique(
        "mp_screen_campaign_delete",
        "89 86 A0 09 04 00 A1 ?? ?? ?? ?? 8B 15 ?? ?? ?? ?? 8B 08 33 CA 8B 41 44 8B D0 81 F2 31 "
        "95 73 85 74");
    if (!site) {
        return;
    }
    const unsigned char jmp = 0xEB;
    const bool ok = WriteCode(site + 32, &jmp, 1);
    Log("coop: network screens keep the campaign: %s\n", ok ? "patched" : "FAILED");
}

void InstallAiGate() {
    unsigned char* site = FindUnique(
        "mission_ai_gate",
        "8B 50 0C 81 F2 31 95 73 85 38 5A 17 0F 85 ?? ?? ?? ?? 38 9D B8 05 00 00");
    if (!site) {
        return;
    }
    unsigned char* cmp = site + 9;  // cmp [edx+17h],bl (3) + jne rel32 (6)
    g_ai_continue = cmp + 9;
    g_ai_skip = cmp + 9 + *reinterpret_cast<int*>(cmp + 5);
    unsigned char code[9] = {0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90};
    const int rel = static_cast<int>(reinterpret_cast<unsigned char*>(&StubAiGate) - (cmp + 5));
    memcpy(code + 1, &rel, 4);
    const bool ok = WriteCode(cmp, code, sizeof(code));
    Log("coop: mission AI gate %s (skip %p)\n", ok ? "patched" : "FAILED", g_ai_skip);
}

// Mission end trace (coop_trace). The script's EndMission(won, delay) and the
// engine's own rules end a mission through the mission object:
//   SetEndDelay(float seconds)  3.1c 0x0060F370
//   SetResult(bool won)         3.1c 0x0060EBD0 (mission+0x2254)
using SetEndDelay_t = void(__thiscall*)(void* mission, float seconds);
using SetResult_t = void(__thiscall*)(void* mission, bool won);
SetEndDelay_t g_set_end_delay = nullptr;
SetResult_t g_set_result = nullptr;

void __fastcall Hook_SetEndDelay(void* mission, void*, float seconds) {
    Log("coop: mission end delay %.1f s from %p\n", seconds, _ReturnAddress());
    g_set_end_delay(mission, seconds);
}

// The loaded campaign (null outside a campaign). The game's globals are
// reached through [[0x103B0BC]] ^ [0x103B0B8]; their +0x44 holds the campaign
// XOR 0x85739531. At a mission's end 0x0060B263 hands the result to it
// (0x00623FD0: pay, salvage, ratings) when it is there.
void* Campaign() {
    const uintptr_t holder = *reinterpret_cast<uintptr_t*>(0x0103B0BC);
    if (!holder) {
        return nullptr;
    }
    const uintptr_t globals =
        *reinterpret_cast<uintptr_t*>(holder) ^ *reinterpret_cast<uintptr_t*>(0x0103B0B8);
    return reinterpret_cast<void*>(*reinterpret_cast<uintptr_t*>(globals + 0x44) ^ 0x85739531u);
}

volatile DWORD g_result_thread = 0;  // thread that set the mission result (the game's)
volatile DWORD g_result_tick = 0;

void __fastcall Hook_SetResult(void* mission, void*, bool won) {
    g_result_thread = GetCurrentThreadId();
    g_result_tick = GetTickCount();
    Log("coop: mission result %s from %p (campaign %p)\n", won ? "won" : "lost",
        _ReturnAddress(), Campaign());
    g_set_result(mission, won);
}

// MWApplication::EnterStoppingGameState (vtable slot 20) and
// EnterRecyclingGameState (slot 22) leave a mission for the menus. Log the
// code addresses on the stack to find which rule ended it.
using AppState_t = void(__thiscall*)(void* app, int arg);  // all ret 4
AppState_t g_enter_stopping = nullptr;
AppState_t g_enter_recycling = nullptr;

void LogStack(const char* what) {
    void** sp = reinterpret_cast<void**>(_AddressOfReturnAddress());
    char line[512];
    int n = sprintf_s(line, "coop: %s, stack:", what);
    int found = 0;
    for (int i = 0; i < 400 && found < 14; ++i) {
        const uintptr_t v = reinterpret_cast<uintptr_t>(sp[i]);
        if (v >= 0x401000 && v < 0x76A000) {
            n += sprintf_s(line + n, sizeof(line) - n, " %08X", static_cast<unsigned>(v));
            ++found;
        }
    }
    Log("%s\n", line);
}

void __fastcall Hook_EnterStopping(void* app, void*, int arg) {
    LogStack("EnterStoppingGameState");
    g_enter_stopping(app, arg);
}

void __fastcall Hook_EnterRecycling(void* app, void*, int arg) {
    LogStack("EnterRecyclingGameState");
    g_enter_recycling(app, arg);
}

// Slot 11 (0x00524E80) sets app+0x15, the stop request the mission loop
// (0x004C7630) acts on.
AppState_t g_request_stop = nullptr;

// MWMission's update (0x0060AA90) recycles a network game that has had fewer
// than 2 players for 60 s (call at 0x0060ACC2). A co-op host plays alone
// until friends drop in, so the co-op mission skips it.
void __fastcall Hook_RequestStop(void* app, void*, int arg) {
    void* ret = _ReturnAddress();
    if (g_own_script && reinterpret_cast<uintptr_t>(ret) == 0x0060ACC5) {
        static int skipped = 0;
        if (skipped++ < 3) {
            Log("coop: idle recycle skipped\n");
        }
        return;
    }
    LogStack("RequestStop");
    if (g_own_script && reinterpret_cast<uintptr_t>(ret) == 0x0060B2AB) {
        // The mission's own result ended it (0x0060B2A8, after the campaign
        // got the result). The end-game handler (slot 6, 0x0052F260) recycles
        // a network game to the scoreboard unless the stop argument
        // (app+0xCE) is set; then it stops like a single-player game. The
        // menus then open GetStartScreen's pick (0x00502900): with the shell's
        // mode (shell+0x24; 0 campaign, 2 Instant Action, 3 network) set to
        // campaign, that is the post-mission screen.
        unsigned char* shell = *reinterpret_cast<unsigned char**>(0x0103D850);
        if (shell) {
            Log("coop: co-op mission end: stop (not recycle), shell mode %d -> 0\n",
                *reinterpret_cast<int*>(shell + 0x24));
            *reinterpret_cast<int*>(shell + 0x24) = 0;
        }
        g_request_stop(app, 1);
        return;
    }
    g_request_stop(app, arg);
}

void InstallStateTrace() {
    // Recon only: MWApplication's 3.1c vtable is at 0x00792AC0 (slots
    // 0x00526140/0x00526340); checked before use.
    void** vt = reinterpret_cast<void**>(0x00792AC0);
    if (reinterpret_cast<uintptr_t>(vt[11]) != 0x00524E80 ||
        reinterpret_cast<uintptr_t>(vt[20]) != 0x00526140 ||
        reinterpret_cast<uintptr_t>(vt[22]) != 0x00526340) {
        Log("coop: state trace skipped (vtable mismatch)\n");
        return;
    }
    g_enter_stopping = reinterpret_cast<AppState_t>(vt[20]);
    g_enter_recycling = reinterpret_cast<AppState_t>(vt[22]);
    void* a = reinterpret_cast<void*>(&Hook_EnterStopping);
    void* b = reinterpret_cast<void*>(&Hook_EnterRecycling);
    g_request_stop = reinterpret_cast<AppState_t>(vt[11]);
    void* c = reinterpret_cast<void*>(&Hook_RequestStop);
    const bool ok = WriteCode(&vt[20], &a, sizeof(a)) && WriteCode(&vt[22], &b, sizeof(b)) &&
                    WriteCode(&vt[11], &c, sizeof(c));
    Log("coop: state trace %s\n", ok ? "ok" : "FAILED");
}

// Exit trace: first-chance exceptions (logged, then passed on) and the
// process exit. A global's destructor runs in DLL_PROCESS_DETACH on the thread
// that called ExitProcess, so its stack shows the caller; TerminateProcess
// skips it.
void LogStackFrom(const char* what, void** sp) {
    char line[512];
    int n = sprintf_s(line, "coop: %s, stack:", what);
    int found = 0;
    for (int i = 0; i < 600 && found < 16; ++i) {
        if (IsBadReadPtr(sp + i, sizeof(void*))) {
            break;
        }
        const uintptr_t v = reinterpret_cast<uintptr_t>(sp[i]);
        if (v >= 0x401000 && v < 0x76A000) {
            n += sprintf_s(line + n, sizeof(line) - n, " %08X", static_cast<unsigned>(v));
            ++found;
        }
    }
    Log("%s\n", line);
}

volatile LONG g_watch_hits = 0;

LONG CALLBACK CoopExceptionLog(EXCEPTION_POINTERS* info) {
    const DWORD code = info->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_SINGLE_STEP && (info->ContextRecord->Dr6 & 1)) {
        // The campaign slot's data breakpoint (CampaignWatch): EIP is the
        // instruction after the write.
        info->ContextRecord->Dr6 = 0;
        if (InterlockedIncrement(&g_watch_hits) <= 10) {
            char what[96];
            sprintf_s(what, "campaign slot written before %08lX -> %p",
                      static_cast<unsigned long>(info->ContextRecord->Eip), Campaign());
            LogStackFrom(what, reinterpret_cast<void**>(info->ContextRecord->Esp));
        }
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (code < 0xC0000000 || code == 0xE06D7363) {
        return EXCEPTION_CONTINUE_SEARCH;  // only errors (access violation, ...)
    }
    static volatile LONG logged = 0;
    if (InterlockedIncrement(&logged) <= 20) {
        char what[96];
        sprintf_s(what, "exception %08lX at %p (thread %lu)", code,
                  info->ExceptionRecord->ExceptionAddress, GetCurrentThreadId());
        LogStackFrom(what, reinterpret_cast<void**>(info->ContextRecord->Esp));
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

struct ExitLog {
    bool armed = false;
    ~ExitLog() {
        if (armed) {
            char what[64];
            sprintf_s(what, "process exit (thread %lu)", GetCurrentThreadId());
            LogStackFrom(what, reinterpret_cast<void**>(_AddressOfReturnAddress()));
        }
    }
} g_exit_log;

// Logs when the campaign is loaded/unloaded, the shell's mode changes, or the
// game loop switches (0x01240974: 0x0052B940 menus, 0x0052EBA0 mission).
// A write data breakpoint (DR0) on the campaign slot in every other thread, so
// the exception log names the code that replaces or clears it.
bool ArmCampaignBreakpoint() {
    const uintptr_t holder = *reinterpret_cast<uintptr_t*>(0x0103B0BC);
    if (!holder) {
        return false;
    }
    const uintptr_t slot = (*reinterpret_cast<uintptr_t*>(holder) ^
                            *reinterpret_cast<uintptr_t*>(0x0103B0B8)) + 0x44;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        return false;
    }
    int armed = 0;
    THREADENTRY32 te{sizeof(te)};
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != GetCurrentProcessId() ||
            te.th32ThreadID == GetCurrentThreadId()) {
            continue;
        }
        HANDLE t = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME,
                              FALSE, te.th32ThreadID);
        if (!t) {
            continue;
        }
        if (SuspendThread(t) != static_cast<DWORD>(-1)) {
            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(t, &ctx)) {
                ctx.Dr0 = slot;
                ctx.Dr7 = (ctx.Dr7 & ~0x000F0003u) | 1u | (1u << 16) | (3u << 18);  // L0, write, 4 bytes
                if (SetThreadContext(t, &ctx)) {
                    ++armed;
                }
            }
            ResumeThread(t);
        }
        CloseHandle(t);
    }
    CloseHandle(snap);
    Log("coop: campaign slot %08X watched in %d threads\n", static_cast<unsigned>(slot), armed);
    return armed > 0;
}

DWORD WINAPI CampaignWatch(LPVOID) {
    bool armed = false;
    void* last_campaign = reinterpret_cast<void*>(1);
    int last_mode = -100;
    uintptr_t last_loop = 1;
    int samples = 0;
    DWORD next_sample = 0;
    for (;;) {
        Sleep(200);
        // After a mission result, sample the game thread's position a few
        // times (a freeze after the player's death showed no exception).
        if (g_result_thread && samples < 8 && GetTickCount() - g_result_tick > 3000 &&
            static_cast<LONG>(GetTickCount() - next_sample) >= 0) {
            next_sample = GetTickCount() + 2000;
            ++samples;
            if (HANDLE t = OpenThread(THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE,
                                      g_result_thread)) {
                if (SuspendThread(t) != static_cast<DWORD>(-1)) {
                    CONTEXT ctx{};
                    ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
                    if (GetThreadContext(t, &ctx)) {
                        char what[96];
                        sprintf_s(what, "game thread sample %d at %08lX", samples,
                                  static_cast<unsigned long>(ctx.Eip));
                        LogStackFrom(what, reinterpret_cast<void**>(ctx.Esp));
                        // The EBP chain (frames that keep a frame pointer).
                        char line[512];
                        int n = sprintf_s(line, "coop:   ebp chain:");
                        uintptr_t* fp = reinterpret_cast<uintptr_t*>(ctx.Ebp);
                        for (int k = 0; k < 24 && fp && !IsBadReadPtr(fp, 8); ++k) {
                            n += sprintf_s(line + n, sizeof(line) - n, " %08X",
                                           static_cast<unsigned>(fp[1]));
                            uintptr_t* up = reinterpret_cast<uintptr_t*>(fp[0]);
                            if (up <= fp) {
                                break;
                            }
                            fp = up;
                        }
                        Log("%s\n", line);
                    }
                    ResumeThread(t);
                }
                CloseHandle(t);
            }
        }
        void* campaign = Campaign();
        unsigned char* shell = *reinterpret_cast<unsigned char**>(0x0103D850);
        const int mode = shell ? *reinterpret_cast<int*>(shell + 0x24) : -99;
        const uintptr_t loop = *reinterpret_cast<uintptr_t*>(0x01240974);
        if (campaign && !armed) {
            armed = ArmCampaignBreakpoint();
        }
        if (campaign != last_campaign || mode != last_mode || loop != last_loop) {
            Log("coop: watch campaign %p, shell mode %d, loop %08X\n", campaign, mode,
                static_cast<unsigned>(loop));
            last_campaign = campaign;
            last_mode = mode;
            last_loop = loop;
        }
    }
}

void InstallEndTrace() {
    if (HANDLE watch = CreateThread(nullptr, 0, CampaignWatch, nullptr, 0, nullptr)) {
        CloseHandle(watch);
    }
    AddVectoredExceptionHandler(1, CoopExceptionLog);
    g_exit_log.armed = true;
    InstallStateTrace();
    // push esi; mov esi,ecx; mov al,[esi+21E8h]; test al,al; jne; lea ecx,[esi+21BCh]
    unsigned char* delay = FindUnique("mission_end_delay",
                                      "56 8B F1 8A 86 E8 21 00 00 84 C0 75 0B 8D 8E BC 21 00 00");
    // mov al,[esp+4]; mov [ecx+2254h],al; ret 4
    unsigned char* result =
        FindUnique("mission_result", "8A 44 24 04 88 81 54 22 00 00 C2 04 00");
    if (delay) {
        InstallDetour(delay, 9, reinterpret_cast<void*>(&Hook_SetEndDelay),
                      reinterpret_cast<void**>(&g_set_end_delay));
    }
    if (result) {
        InstallDetour(result, 10, reinterpret_cast<void*>(&Hook_SetResult),
                      reinterpret_cast<void**>(&g_set_result));
    }
    Log("coop: mission end trace %s/%s\n", g_set_end_delay ? "ok" : "FAILED",
        g_set_result ? "ok" : "FAILED");
}

}  // namespace

bool InstallCoopHooks() {
    if (g_installed) {
        return true;
    }
    const Config& config = GetConfig();
    const wchar_t* mission = config.debug_coop_mission;
    for (int i = 0; i < 63 && mission[i]; ++i) {
        const wchar_t c = mission[i];
        if (!((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') ||
              c == L'_')) {
            g_mission[0] = 0;
            break;
        }
        g_mission[i] = static_cast<char>(c);
    }
    if (!config.debug_coop_trace && !g_mission[0]) {
        g_installed = true;
        return true;
    }
    // push ebx; mov ebx,[esp+8]; push esi; mov esi,ecx; push edi; mov edi,ebx;
    // mov byte [esi+352Ah],1; mov eax,[...]
    unsigned char* target = FindUnique(
        "set_mission_path", "53 8B 5C 24 08 56 8B F1 57 8B FB C6 86 2A 35 00 00 01 A1");
    if (!target) {
        return false;  // not unpacked yet; retry
    }
    // The network start: mov edx,[esp+10h]; mov eax,[..]; push edx; ...;
    // xor ecx,85739531h; call SetMissionPath
    unsigned char* site = FindUnique(
        "network_start_set_path",
        "8B 54 24 10 A1 ?? ?? ?? ?? 52 8B 15 ?? ?? ?? ?? 8B 08 33 CA 8B 49 0C 81 F1 31 95 73 85 "
        "E8");
    g_installed = true;
    g_net_return = site ? site + 29 + 5 : nullptr;
    // push ebx; mov ebx,[esp+8]; push esi (6 bytes) are position independent.
    InstallDetour(target, 6, reinterpret_cast<void*>(&Hook_SetMissionPath),
                  reinterpret_cast<void**>(&g_set_path));
    Log("coop: recon hook %s (network site %s, mission override \"%s\")\n",
        g_set_path ? "ok" : "FAILED", site ? "found" : "missing", g_mission);
    if (config.debug_coop_trace) {
        InstallEndTrace();
    }
    if (g_mission[0]) {
        InstallScriptChoice();
        InstallAiGate();
        InstallKeepCampaign();
    }
    return true;
}

}  // namespace mw4hook
