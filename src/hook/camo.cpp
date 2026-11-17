// Default camouflage for new pilots (issue #19).
//
// Every 'Mech a pilot gets (the campaign's starting 'Mechs, purchases,
// salvage) takes the pilot's own skin setting, MekTek's per-pilot default
// camouflage (Options > Multiplayer > Skin). A pilot created in the Roster
// starts from MekTek's template profile, whose skin is '7' ("7-Ops - Pink"),
// so all of a new unit's 'Mechs came out pink although the Multiplayer
// screen's own default is "None".
//
// The pilot's skin is only loaded together with its campaign, in the same
// step that creates the starting 'Mechs, so it cannot be changed beforehand.
// Instead the two places that copy it into a new 'Mech are detoured: for a
// pilot just created in the Roster (marked by display.cpp on its first
// selection), the first time one of its 'Mechs is created the pilot's skin is
// set to [mechlab] default_camo (default '_' = None) just before it is read.
// From then on that pilot's 'Mechs, and its saved setting, use it; the player
// can choose another default on Options > Multiplayer. Selecting any other
// pilot first cancels it. Existing pilots are never changed.
//
// Game side (3.1c, located by unique patterns):
//   0x00625E36 / 0x00625964  mov ecx,[eax+24h] / mov ecx,[edi+24h]
//                            mov al,[ecx+30h]      <- detour (6 bytes:
//                            lea ecx,[esi+2Ch]        both position independent)
//                            mov [esi+278h],al     ; the new 'Mech's skin code
//   The skin codes are the first characters of MekTek's camouflage names in
//   MechLab (chassis.script): '0'..'9', 'a'..'z', '!', '^', ... and '_' = None.
#include "hook.h"

namespace mw4hook {
namespace {

volatile LONG g_pending = 0;  // the current pilot is new: set its camouflage
void* g_trampoline[2] = {};

// Called from the stubs with the pilot's skin holder (ecx); all registers
// are preserved around it.
void __cdecl OnSkinRead(unsigned char* holder) {
    if (!g_pending || !holder) {
        return;
    }
    InterlockedExchange(&g_pending, 0);
    const char code = GetConfig().default_camo;
    __try {
        const unsigned char before = holder[0x30];
        holder[0x30] = static_cast<unsigned char>(code);
        Log("camo: new pilot's default camouflage '%c' (was '%c')\n", code, before);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

__declspec(naked) void Stub0() {
    __asm {
        pushad
        pushfd
        push ecx
        call OnSkinRead
        add esp, 4
        popfd
        popad
        jmp dword ptr [g_trampoline]
    }
}

__declspec(naked) void Stub1() {
    __asm {
        pushad
        pushfd
        push ecx
        call OnSkinRead
        add esp, 4
        popfd
        popad
        jmp dword ptr [g_trampoline + 4]
    }
}

bool g_installed = false;

}  // namespace

// The current pilot was just created: give it the default camouflage when
// its first 'Mech is created.
void SetNewPilotCamo() {
    if (!GetConfig().default_camo) {
        Log("camo: default_camo is empty; new pilot keeps the template's skin\n");
        return;
    }
    InterlockedExchange(&g_pending, 1);
}

// Any pilot selection: a pending default belongs to the pilot that was new.
void CancelNewPilotCamo() { InterlockedExchange(&g_pending, 0); }

bool InstallCamoHooks() {
    if (g_installed) {
        return true;
    }
    unsigned char* a = FindUnique("pilot_skin_a", "8B 48 24 8A 41 30 8D 4E 2C 88 86 78 02 00 00");
    unsigned char* b = FindUnique("pilot_skin_b", "8B 4F 24 8A 41 30 8D 4E 2C 88 86 78 02 00 00");
    if (!a && !b) {
        return false;  // not unpacked yet (or not this build); retry
    }
    g_installed = true;
    // mov al,[ecx+30h]; lea ecx,[esi+2Ch] = 6 bytes, position independent.
    if (a) {
        InstallDetour(a + 3, 6, reinterpret_cast<void*>(&Stub0), &g_trampoline[0]);
    }
    if (b) {
        InstallDetour(b + 3, 6, reinterpret_cast<void*>(&Stub1), &g_trampoline[1]);
    }
    Log("camo: new-'Mech skin hooks %s / %s (new pilots get '%c')\n",
        g_trampoline[0] ? "ok" : "missing", g_trampoline[1] ? "ok" : "missing",
        GetConfig().default_camo ? GetConfig().default_camo : '-');
    return true;
}

}  // namespace mw4hook
