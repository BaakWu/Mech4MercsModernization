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
// coop_mission=campaign: the mission the host last selected in the campaign
// (the shell's selected mission name, shell+0x40970), captured by the watcher
// while the game is not networked (the lobby later sets it to its map).
char g_campaign_mission[64] = {};

void* Campaign();

const char* CoopMission() {
    if (_stricmp(g_mission, "campaign") == 0) {
        return g_campaign_mission[0] ? g_campaign_mission : nullptr;
    }
    return g_mission[0] ? g_mission : nullptr;
}

void __fastcall Hook_SetMissionPath(void* game, void* /*edx*/, const char* path) {
    void* ret = _ReturnAddress();
    const bool network = ret == g_net_return;
    const char* use = path;
    const char* mission = CoopMission();
    if (network && mission && path) {
        // The network start passes the mission's instance inside its archive,
        // "Missions\<name>\<name>.instance"; use the other mission's.
        sprintf_s(g_path, "Missions\\%s\\%s.instance", mission, mission);
        use = g_path;
    }
    g_own_script = use != path;  // the next mission load keeps the mission's own script
    Log("coop: SetMissionPath \"%s\" from %p (%s)%s%s\n", path ? path : "(null)", ret,
        network ? "network start" : "other", use != path ? " -> " : "", use != path ? use : "");
    g_set_path(game, use);
    // After the path is set (the setup below looks the mission up by it).
    if (g_own_script && Campaign()) {
        // A co-op host's campaign: run the campaign launch's mission setup
        // (shell method 0x00511260, called by the campaign start 0x0052B220
        // with 0): the campaign's begin-mission (0x00624CA0), the mission
        // record's data and the drop fee. Without it the mission end reads a
        // garbage size from the record ("Invalid allocation").
        static const unsigned char kPrologue[] = {0x6A, 0xFF, 0x68, 0x8F, 0x60, 0x76, 0x00,
                                                  0x64, 0xA1, 0x00, 0x00, 0x00, 0x00};
        void* setup = reinterpret_cast<void*>(0x00511260);
        void* shell = *reinterpret_cast<void**>(0x0103D850);
        if (shell && memcmp(setup, kPrologue, sizeof(kPrologue)) == 0) {
            using CampaignSetup_t = void(__thiscall*)(void* shell, int arg);
            // The setup looks the mission up by the shell's selected mission
            // name (shell+0x40970, a game string; the lobby set it to its map).
            // Give it the co-op mission's name for the call: a string built
            // by the game's own constructor (0x00417D90) and destroyed by its
            // destructor (0x004149D0).
            using StrCtor_t = void(__thiscall*)(void** self, const char* text);
            using StrDtor_t = void(__thiscall*)(void** self);
            void* name = nullptr;
            reinterpret_cast<StrCtor_t>(0x00417D90)(&name, mission);
            void** slot = reinterpret_cast<void**>(static_cast<unsigned char*>(shell) + 0x40970);
            void* saved = *slot;
            *slot = name;
            Log("coop: campaign mission setup (0x00511260) for \"%s\" ...\n", mission);
            reinterpret_cast<CampaignSetup_t>(setup)(shell, 0);
            Log("coop: campaign mission setup done\n");
            *slot = saved;
            reinterpret_cast<StrDtor_t>(0x004149D0)(&name);
            // The lancemates: the campaign start adds one vehicle per
            // lancemate in the campaign's lance (campaign+0x40) with
            // game->0x00529A60(); the network start only sends the player's.
            static const unsigned char kLance[] = {0x6A, 0xFF, 0x68, 0xA0, 0x76, 0x76, 0x00,
                                                   0x64, 0xA1, 0x00, 0x00, 0x00, 0x00};
            void* lance = reinterpret_cast<void*>(0x00529A60);
            if (GetConfig().debug_coop_lance && memcmp(lance, kLance, sizeof(kLance)) == 0) {
                using AddLance_t = void(__thiscall*)(void* game);
                // The local player's index (the campaign start reads it from
                // [0x0103B60C]+0x28 / [0x0103B608]+0x28); the lance add uses
                // roster positions 0.. as slot indices.
                const unsigned char* a = *reinterpret_cast<unsigned char**>(0x0103B60C);
                const unsigned char* b = *reinterpret_cast<unsigned char**>(0x0103B608);
                Log("coop: local player index %d / %d\n", a ? a[0x28] : -1, b ? b[0x28] : -1);
                for (int s = 0; s < 8; ++s) {
                    const unsigned char* slot = static_cast<unsigned char*>(game) + s * 0x22AC;
                    Log("coop:   slot %d: request %p connection %p flags %02X %02X\n", s,
                        *reinterpret_cast<void* const*>(slot + 0x20BCBC),
                        *reinterpret_cast<void* const*>(slot + 0x20BCC0), slot[0x20BECC],
                        slot[0x20BECD]);
                }
                // The lance add uses roster positions as slot indices (player
                // 0, lancemates 1..), but in a network game the host is a
                // connection slot too (index 1 when hosting). Shift lancemates
                // that land on or above the host's slot up by one, and give
                // the host's slot back its contents from before the add.
                const int host = a ? a[0x28] : 0;
                const size_t kSlot = 0x22AC;
                const int kSlots = 16;
                unsigned char* slots = static_cast<unsigned char*>(game) + 0x20BCBC;
                // Snapshot every slot, add the lance, then move each added
                // lancemate (slot+0x210 == 1) to the free slots after the
                // host's, and give the slots it used back their contents.
                static unsigned char before[16][0x22AC];
                static unsigned char moved[16][0x22AC];
                memcpy(before, slots, sizeof(before));
                Log("coop: adding the campaign lance (0x00529A60) ...\n");
                reinterpret_cast<AddLance_t>(lance)(game);
                Log("coop: campaign lance added\n");
                int count = 0;
                int from[16];
                for (int s = 0; s < kSlots; ++s) {
                    if (slots[s * kSlot + 0x210] == 1 && before[s][0x210] != 1) {
                        memcpy(moved[count], slots + s * kSlot, kSlot);
                        from[count++] = s;
                        memcpy(slots + s * kSlot, before[s], kSlot);
                    }
                }
                int next = host + 1;
                for (int k = 0; k < count; ++k) {
                    // The next slot that was empty (no request) before the add.
                    while (next < kSlots &&
                           *reinterpret_cast<void* const*>(before[next]) != nullptr) {
                        ++next;
                    }
                    if (next >= kSlots) {
                        Log("coop: no free slot for lancemate %d\n", k);
                        break;
                    }
                    memcpy(slots + next * kSlot, moved[k], kSlot);
                    Log("coop: lancemate from slot %d placed in slot %d (host slot %d)\n",
                        from[k], next, host);
                    ++next;
                }
                for (int s = 0; s < 8; ++s) {
                    Log("coop:   slot %d after: request %p flags %02X\n", s,
                        *reinterpret_cast<void* const*>(slots + s * kSlot),
                        slots[s * kSlot + 0x210]);
                }
            }
        } else {
            Log("coop: campaign mission setup skipped (no shell or bytes differ)\n");
        }
    }
    Log("coop: campaign %p, shell mode %d\n", Campaign(),
        *reinterpret_cast<unsigned char**>(0x0103D850)
            ? *reinterpret_cast<int*>(*reinterpret_cast<unsigned char**>(0x0103D850) + 0x24)
            : -99);
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
    // The teardown that follows deletes every single-player object: globals
    // +0x44 (campaign), +0x30, +0x3C, +0x34, +0x38 and the campaign's mission
    // state at 0x011D4840..0x011D484C. Jump from its start (site+6) to the
    // function's end: pop esi; mov eax,1; pop ebx; ret.
    unsigned char* start = site + 6;
    unsigned char* end = nullptr;
    static const unsigned char kEnd[] = {0x5E, 0xB8, 0x01, 0x00, 0x00, 0x00, 0x5B, 0xC3};
    for (unsigned char* p = start; p < start + 0x200; ++p) {
        if (memcmp(p, kEnd, sizeof(kEnd)) == 0) {
            end = p;
            break;
        }
    }
    if (!end) {
        Log("coop: network screens keep the campaign: end not found\n");
        return;
    }
    unsigned char code[5] = {0xE9};
    const int rel = static_cast<int>(end - (start + 5));
    memcpy(code + 1, &rel, 4);
    const bool ok = WriteCode(start, code, sizeof(code));
    Log("coop: network screens keep the single-player state (%p -> %p): %s\n", start, end,
        ok ? "patched" : "FAILED");
}

// MWMission's update (0x0060AA90) runs the host's network rules when the game
// is networked: cmp byte [edi+17h],1; jne <skip>; cmp [esi+60h],ebp; je ...
// They recycle a game with fewer than 2 players for 60 s, start the round when
// 2 or more players (lancemates count) have been in for 5 s (0x0060DAC0), and
// end the game on its time or frag limit. A co-op mission ends through its own
// script, so the stub skips them while g_own_script is set.
void* g_rules_continue = nullptr;
void* g_rules_skip = nullptr;

__declspec(naked) void StubNetworkRules() {
    __asm {
        cmp byte ptr [g_own_script], 0
        jne skip
        cmp byte ptr [edi+17h], 1
        jne skip
        jmp dword ptr [g_rules_continue]
    skip:
        jmp dword ptr [g_rules_skip]
    }
}

void InstallNetworkRulesGate() {
    unsigned char* site = FindUnique("mission_network_rules",
                                     "80 7F 17 01 0F 85 ?? ?? ?? ?? 39 6E 60 0F 84");
    if (!site) {
        return;
    }
    g_rules_continue = site + 10;
    g_rules_skip = site + 10 + *reinterpret_cast<int*>(site + 6);
    unsigned char code[10] = {0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90, 0x90};
    const int rel = static_cast<int>(reinterpret_cast<unsigned char*>(&StubNetworkRules) -
                                     (site + 5));
    memcpy(code + 1, &rel, 4);
    const bool ok = WriteCode(site, code, sizeof(code));
    Log("coop: mission network rules gate %s (skip %p)\n", ok ? "patched" : "FAILED",
        g_rules_skip);
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

// gos_Free (3.1c 0x006FAC90, stdcall, 1 argument): log frees of the
// campaign's mission state ([campaign+0x34]) and its current mission record
// ([that+0x48]) with the caller's stack.
using Free_t = void(__stdcall*)(void* p);
Free_t g_free = nullptr;
void* volatile g_watch_free[2] = {};

void __stdcall Hook_Free(void* p) {
    if (p && (p == g_watch_free[0] || p == g_watch_free[1])) {
        char what[96];
        sprintf_s(what, "freed %s %p", p == g_watch_free[0] ? "campaign+0x34 object"
                                                            : "mission record", p);
        LogStackFrom(what, reinterpret_cast<void**>(_AddressOfReturnAddress()));
    }
    g_free(p);
}

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
    if (code == EXCEPTION_SINGLE_STEP && (info->ContextRecord->Dr6 & 4)) {
        info->ContextRecord->Dr6 = 0;
        static volatile LONG hits = 0;
        if (InterlockedIncrement(&hits) <= 20) {
            char what[128];
            sprintf_s(what, "campaign+0x34 = %08X before %08lX",
                      *reinterpret_cast<const unsigned*>(info->ContextRecord->Dr2),
                      static_cast<unsigned long>(info->ContextRecord->Eip));
            LogStackFrom(what, reinterpret_cast<void**>(info->ContextRecord->Esp));
        }
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (code == EXCEPTION_SINGLE_STEP && (info->ContextRecord->Dr6 & 2)) {
        // DR1: the game's flag dword +0x14..+0x17 (+0x15 stop request,
        // +0x16, +0x17 networked).
        info->ContextRecord->Dr6 = 0;
        static volatile LONG hits = 0;
        if (InterlockedIncrement(&hits) <= 30) {
            const unsigned char* f = reinterpret_cast<const unsigned char*>(info->ContextRecord->Dr1);
            char what[128];
            sprintf_s(what, "game flags +14..17 = %02X %02X %02X %02X before %08lX", f[0], f[1],
                      f[2], f[3], static_cast<unsigned long>(info->ContextRecord->Eip));
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
        const CONTEXT* c = info->ContextRecord;
        Log("coop:   eax=%08lX ebx=%08lX ecx=%08lX edx=%08lX esi=%08lX edi=%08lX ebp=%08lX "
            "esp=%08lX\n",
            c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp);
        const DWORD* sp = reinterpret_cast<const DWORD*>(c->Esp);
        if (!IsBadReadPtr(sp, 32 * 4)) {
            char line[512];
            int n = sprintf_s(line, "coop:   stack:");
            for (int i = 0; i < 32; ++i) {
                n += sprintf_s(line + n, sizeof(line) - n, " %08lX", sp[i]);
            }
            Log("%s\n", line);
        }
        // The EBP chain.
        char line[512];
        int n = sprintf_s(line, "coop:   ebp chain:");
        const uintptr_t* fp = reinterpret_cast<const uintptr_t*>(c->Ebp);
        for (int k = 0; k < 20 && fp && !IsBadReadPtr(fp, 8); ++k) {
            n += sprintf_s(line + n, sizeof(line) - n, " %08X", static_cast<unsigned>(fp[1]));
            const uintptr_t* up = reinterpret_cast<const uintptr_t*>(fp[0]);
            if (up <= fp) {
                break;
            }
            fp = up;
        }
        Log("%s\n", line);
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
    const uintptr_t globals =
        *reinterpret_cast<uintptr_t*>(holder) ^ *reinterpret_cast<uintptr_t*>(0x0103B0B8);
    const uintptr_t slot = globals + 0x44;
    const uintptr_t game = *reinterpret_cast<uintptr_t*>(globals + 0x0C) ^ 0x85739531u;
    if (!game) {
        return false;
    }
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
                ctx.Dr1 = game + 0x14;
                // L0 + L1, both write, 4 bytes.
                ctx.Dr7 = (ctx.Dr7 & ~0x00FF000Fu) | 1u | 4u | (1u << 16) | (3u << 18) |
                          (1u << 20) | (3u << 22);
                if (SetThreadContext(t, &ctx)) {
                    ++armed;
                }
            }
            ResumeThread(t);
        }
        CloseHandle(t);
    }
    CloseHandle(snap);
    Log("coop: campaign slot %08X and game flags %08X watched in %d threads\n",
        static_cast<unsigned>(slot), static_cast<unsigned>(game + 0x14), armed);
    return armed > 0;
}

// DR2 on one more address (write, 4 bytes) in every other thread.
int ArmWatch2(uintptr_t addr) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        return 0;
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
                ctx.Dr2 = addr;
                ctx.Dr7 = (ctx.Dr7 & ~0x0F000030u) | 0x10u | (1u << 24) | (3u << 26);
                if (SetThreadContext(t, &ctx)) {
                    ++armed;
                }
            }
            ResumeThread(t);
        }
        CloseHandle(t);
    }
    CloseHandle(snap);
    return armed;
}

DWORD WINAPI CampaignWatch(LPVOID) {
    bool armed = false;
    uintptr_t watched_record = 0;
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
        // coop_mission=campaign: remember the campaign's selected mission
        // while the game is not networked (game+0x17 == 0).
        if (_stricmp(g_mission, "campaign") == 0 && campaign && shell) {
            const uintptr_t holder = *reinterpret_cast<uintptr_t*>(0x0103B0BC);
            const uintptr_t globals =
                holder ? *reinterpret_cast<uintptr_t*>(holder) ^
                             *reinterpret_cast<uintptr_t*>(0x0103B0B8)
                       : 0;
            const uintptr_t game =
                globals ? *reinterpret_cast<uintptr_t*>(globals + 0x0C) ^ 0x85739531u : 0;
            const char* name = *reinterpret_cast<const char**>(shell + 0x40970);
            if (game && !*reinterpret_cast<unsigned char*>(game + 0x17) && name &&
                !IsBadReadPtr(name, 1) && name[0] && strncmp(name, g_campaign_mission, 63) != 0) {
                bool ok = true;
                for (int i = 0; i < 63 && name[i]; ++i) {
                    const char c = name[i];
                    ok = ok && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                                (c >= '0' && c <= '9') || c == '_');
                }
                if (ok) {
                    strncpy_s(g_campaign_mission, name, 63);
                    Log("coop: campaign mission selected: %s\n", g_campaign_mission);
                }
            }
        }
        if (!armed && *reinterpret_cast<uintptr_t*>(0x01240974) == 0x0052B940) {  // menus up
            armed = ArmCampaignBreakpoint();
        }
        // The campaign's mission state pointer, campaign+0x34 (its +0x48 is
        // the current mission record).
        if (campaign) {
            const uintptr_t sub = reinterpret_cast<uintptr_t>(campaign) + 0x34;
            if (sub != watched_record) {
                watched_record = sub;
                const int n = ArmWatch2(watched_record);
                Log("coop: campaign+0x34 at %08X (now %08X) watched in %d threads\n",
                    static_cast<unsigned>(watched_record),
                    *reinterpret_cast<unsigned*>(watched_record), n);
            }
            const uintptr_t obj = *reinterpret_cast<uintptr_t*>(sub);
            if (obj && !IsBadReadPtr(reinterpret_cast<void*>(obj + 0x48), 4)) {
                void* record = *reinterpret_cast<void**>(obj + 0x48);
                if (g_watch_free[0] != reinterpret_cast<void*>(obj) ||
                    g_watch_free[1] != record) {
                    g_watch_free[0] = reinterpret_cast<void*>(obj);
                    g_watch_free[1] = record;
                    Log("coop: watching frees of %p and record %p\n", g_watch_free[0], record);
                }
            }
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

// Shell callback trace: the shell registers each $$Callback$$ with
// 0x006C2F00(name, impl, ...) (cdecl). The detour gives every callback a
// thunk that logs its name (the first 20 calls per name) before running it.
void* g_register_tramp = nullptr;
unsigned char* g_thunks = nullptr;
size_t g_thunk_used = 0;
const size_t kThunkPool = 64 * 1024;

struct CbCount {
    const char* name;
    int count;
};
CbCount g_cb_counts[1024];
int g_cb_used = 0;

void __cdecl LogCallback(const char* name) {
    int* count = nullptr;
    for (int i = 0; i < g_cb_used; ++i) {
        if (g_cb_counts[i].name == name) {
            count = &g_cb_counts[i].count;
            break;
        }
    }
    if (!count && g_cb_used < 1024) {
        g_cb_counts[g_cb_used] = {name, 0};
        count = &g_cb_counts[g_cb_used++].count;
    }
    if (count && (*count)++ < 20) {
        Log("cb: %s\n", name);
    }
}

void* __cdecl MakeCallbackThunk(const char* name, void* impl) {
    if (!g_thunks) {
        g_thunks = static_cast<unsigned char*>(
            VirtualAlloc(nullptr, kThunkPool, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    }
    if (!g_thunks || !name || !impl || g_thunk_used + 32 > kThunkPool) {
        return impl;
    }
    unsigned char* t = g_thunks + g_thunk_used;
    g_thunk_used += 32;
    unsigned char* p = t;
    *p++ = 0x60;  // pushad
    *p++ = 0x68;  // push name
    memcpy(p, &name, 4);
    p += 4;
    *p++ = 0xB8;  // mov eax, LogCallback
    void* log = reinterpret_cast<void*>(&LogCallback);
    memcpy(p, &log, 4);
    p += 4;
    *p++ = 0xFF;  // call eax
    *p++ = 0xD0;
    *p++ = 0x83;  // add esp, 4
    *p++ = 0xC4;
    *p++ = 0x04;
    *p++ = 0x61;  // popad
    *p++ = 0x68;  // push impl
    memcpy(p, &impl, 4);
    p += 4;
    *p++ = 0xC3;  // ret (jumps to impl with the caller's stack)
    return t;
}

__declspec(naked) void StubRegisterCallback() {
    __asm {
        mov eax, [esp + 4]   // name
        mov ecx, [esp + 8]   // impl
        push ecx
        push eax
        call MakeCallbackThunk
        add esp, 8
        mov [esp + 8], eax
        jmp dword ptr [g_register_tramp]
    }
}

void InstallCallbackTrace() {
    unsigned char* f = reinterpret_cast<unsigned char*>(0x006C2F00);
    // mov eax,[0x01210AB0]; push ebx; push ebp; mov ebp,[esp+0Ch]
    static const unsigned char kReg[] = {0xA1, 0xB0, 0x0A, 0x21, 0x01, 0x53, 0x55,
                                         0x8B, 0x6C, 0x24, 0x0C};
    if (memcmp(f, kReg, sizeof(kReg)) == 0) {
        InstallDetour(f, 5, reinterpret_cast<void*>(&StubRegisterCallback), &g_register_tramp);
    }
    Log("coop: callback trace %s\n", g_register_tramp ? "ok" : "skipped");
}

void InstallEndTrace() {
    InstallCallbackTrace();
    if (HANDLE watch = CreateThread(nullptr, 0, CampaignWatch, nullptr, 0, nullptr)) {
        CloseHandle(watch);
    }
    AddVectoredExceptionHandler(1, CoopExceptionLog);
    g_exit_log.armed = true;
    {
        // push ebp; mov ebp,esp; sub esp,8; push ebx; mov ebx,[ebp+8]; push esi; push edi;
        // test ebx,ebx; je
        unsigned char* f = reinterpret_cast<unsigned char*>(0x006FAC90);
        static const unsigned char kFree[] = {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x08, 0x53,
                                              0x8B, 0x5D, 0x08, 0x56, 0x57, 0x85, 0xDB};
        if (memcmp(f, kFree, sizeof(kFree)) == 0) {
            InstallDetour(f, 6, reinterpret_cast<void*>(&Hook_Free),
                          reinterpret_cast<void**>(&g_free));
        }
        Log("coop: free trace %s\n", g_free ? "ok" : "skipped");
    }
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
        InstallNetworkRulesGate();
    }
    return true;
}

}  // namespace mw4hook
