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

// MekTek's camouflage list as MechLab shows it (chassis.script). A skin's
// code is the first character of its name; "None" is '_'.
const char* const kCamoNames[kCamoCount] = {
    "0-Ops - Blue", "1-Ops - Red", "2-Ops - Gold", "3-Ops - Green", "4-Ops - Orange",
    "5-Ops - Cyan", "6-Ops - Purple", "7-Ops - Pink", "8-Snow - WinterWar",
    "9-Snow - SnowBlind", "a-Snow - SnowBall", "b-Snow - Arctic",
    "c-Alpine - Temperate Winter", "d-Alpine - Snow Leopard", "e-Alpine - Taiga Winter",
    "f-Alpine - Ozark Winter", "g-Swamp - Great Dismal", "h-Swamp - Everglades",
    "i-Swamp - Pripyat", "j-Swamp - Bayou", "k-Woodland - Hunter Woods",
    "l-Woodland - Schwarzwald", "m-Woodland - Olympic Forest", "n-Woodland - Gecko",
    "!-Grassland - Summer Tundra", "^-Grassland - Rhodesian Fall",
    "#-Grassland - Steppe Spring", "$-Grassland - Great Plains", "%-Sand - Empty Quarter",
    "o-Sand - Negev", "p-Sand - Sahara", "q-Sand - Mojave", "r-Rock - Desert Fox",
    "s-Rock - Blood Stone", "t-Rock - Quattra Depression", "u-Rock - Savannah Tiger",
    "v-Urban - Concrete", "w-Urban - Spiral Maze", "x-Urban - Headstone", "y-Urban - Skyline",
    "z-Night Ops - Charred Steel", "~-Night Ops - Scorched Earth", "--Night Ops - Nightmare",
    "&-Night Ops - Urban Assault", "+-Parade - Capellan Confederation",
    ".-Parade - Draconis Combine", "(-Parade - Federated Suns", "'-Parade - Free Worlds League",
    ")-Parade - Lyran Alliance", "]-Parade - Blue Solaris", "[-Parade - Red Solaris", "None",
};

char CamoCode(int index) {
    if (index < 0 || index >= kCamoCount) {
        return '\0';
    }
    return index == kCamoNone ? '_' : kCamoNames[index][0];
}

int CamoIndex(char code) {
    for (int i = 0; i < kCamoCount; ++i) {
        if (CamoCode(i) == code) {
            return i;
        }
    }
    return kCamoNone;
}

namespace {

volatile LONG g_pending = 0;  // the current pilot is new: set its camouflage
volatile LONG g_chosen = 0;   // camouflage picked in Join Regiment (code), 0 = none
volatile LONG g_code = 0;     // the code to give the pending pilot
void* g_trampoline[2] = {};

// Called from the stubs with the pilot's skin holder (ecx); all registers
// are preserved around it.
void __cdecl OnSkinRead(unsigned char* holder) {
    if (!g_pending || !holder) {
        return;
    }
    InterlockedExchange(&g_pending, 0);
    const char code = static_cast<char>(g_code);
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

// Join Regiment's Camouflage list (option id kMwoNewPilotCamo, set just
// before the pilot is created).
void ChooseNewPilotCamo(int index) {
    const char code = CamoCode(index);
    InterlockedExchange(&g_chosen, static_cast<unsigned char>(code));
    Log("camo: Join Regiment camouflage %d '%s'\n", index,
        code ? kCamoNames[index] : "?");
}

// The current pilot was just created: give it its camouflage (the one picked
// in Join Regiment, else [mechlab] default_camo) when its first 'Mech is
// created.
void SetNewPilotCamo() {
    const char chosen = static_cast<char>(InterlockedExchange(&g_chosen, 0));
    const char code = chosen ? chosen : GetConfig().default_camo;
    if (!code) {
        Log("camo: default_camo is empty; new pilot keeps the template's skin\n");
        return;
    }
    InterlockedExchange(&g_code, static_cast<unsigned char>(code));
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
    Log("camo: new-'Mech skin hooks %s / %s (new pilots default to '%c')\n",
        g_trampoline[0] ? "ok" : "missing", g_trampoline[1] ? "ok" : "missing",
        GetConfig().default_camo ? GetConfig().default_camo : '-');
    return true;
}

}  // namespace mw4hook
