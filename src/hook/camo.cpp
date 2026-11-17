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

#include <cstdio>
#include <cstring>
#include <string>

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

char CamoCode(int index);

int CamoIndex(char code) {
    for (int i = 0; i < kCamoCount; ++i) {
        if (CamoCode(i) == code) {
            return i;
        }
    }
    return kCamoNone;
}

namespace {

volatile LONG g_pending = 0;  // the current pilot has a camouflage to apply
volatile LONG g_chosen = 0;   // camouflage picked in the dialog (code), 0 = none
volatile LONG g_code = 0;     // the code to give the current pilot
char g_current[64] = {};      // the selected pilot (ShellSelectPilot)
volatile LONG g_update_initial = 0;  // code the Update dialog's list opened with
void* g_trampoline[2] = {};

// mw4hook-pilots.ini next to the game (removed by uninstall):
//   [new_controls] <pilot>=1      layout still to apply (new pilot)
//   [camo_pending] <pilot>=<code> camouflage to apply at its next new 'Mech
//   [camo]         <pilot>=<code> the pilot's chosen camouflage (for Update)
// Codes are stored as numbers (several are ini punctuation: [ ] # ...).
const wchar_t* RecordsPath() {
    static wchar_t path[MAX_PATH] = {};
    if (!path[0]) {
        DWORD len = GetModuleFileNameW(nullptr, path, MAX_PATH);
        wchar_t* slash = len ? wcsrchr(path, L'\\') : nullptr;
        if (slash) {
            wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - path), L"mw4hook-pilots.ini");
        }
    }
    return path;
}

std::wstring Wide(const char* text) {
    wchar_t buffer[128] = {};
    MultiByteToWideChar(CP_ACP, 0, text, -1, buffer, 128);
    return buffer;
}

int ReadRecord(const wchar_t* section, const char* pilot) {
    return static_cast<int>(
        GetPrivateProfileIntW(section, Wide(pilot).c_str(), 0, RecordsPath()));
}

void WriteRecord(const wchar_t* section, const char* pilot, int value) {
    wchar_t text[16];
    swprintf_s(text, L"%d", value);
    WritePrivateProfileStringW(section, Wide(pilot).c_str(), value ? text : nullptr,
                               RecordsPath());
}

// Called from the stubs with the pilot's skin holder (ecx); all registers
// are preserved around it.
void __cdecl OnSkinRead(unsigned char* holder) {
    if (GetConfig().debug_automation) {
        static int logged = 0;
        if (logged < 40) {
            ++logged;
            Log("camo: skin read (pending %ld, holder %p, skin '%c')\n", g_pending, holder,
                holder && !IsBadReadPtr(holder + 0x30, 1) ? holder[0x30] : '?');
        }
    }
    if (!g_pending || !holder) {
        return;
    }
    InterlockedExchange(&g_pending, 0);
    const char code = static_cast<char>(g_code);
    __try {
        const unsigned char before = holder[0x30];
        holder[0x30] = static_cast<unsigned char>(code);
        Log("camo: %s's default camouflage '%c' (was '%c')\n", g_current, code, before);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    WriteRecord(L"camo_pending", g_current, 0);  // done
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

// The dialog's Camouflage list (option 204 new pilot / 205 Update), set just
// before the pilot is created or renamed.
void ChoosePilotCamo(int index) {
    const char code = CamoCode(index);
    InterlockedExchange(&g_chosen, static_cast<unsigned char>(code));
    Log("camo: dialog camouflage %d '%s'\n", index, code ? kCamoNames[index] : "?");
}

// A pilot was just created: its layout is applied on its next selection, its
// camouflage (the dialog's, else [mechlab] default_camo) on its next new
// 'Mech -- also after a restart.
void QueueNewPilot(const char* name) {
    const char chosen = static_cast<char>(InterlockedExchange(&g_chosen, 0));
    const char code = chosen ? chosen : GetConfig().default_camo;
    WriteRecord(L"new_controls", name, 1);
    if (code) {
        WriteRecord(L"camo_pending", name, static_cast<unsigned char>(code));
        WriteRecord(L"camo", name, static_cast<unsigned char>(code));
    }
}

// Roster > Update: the chosen camouflage becomes the pilot's default for the
// 'Mechs it gets from now on (its existing 'Mechs keep their paint).
void QueueUpdatedPilotCamo(const char* name) {
    const char chosen = static_cast<char>(InterlockedExchange(&g_chosen, 0));
    const char initial = static_cast<char>(InterlockedExchange(&g_update_initial, 0));
    if (!chosen || chosen == initial) {
        return;  // the list was not changed
    }
    WriteRecord(L"camo_pending", name, static_cast<unsigned char>(chosen));
    WriteRecord(L"camo", name, static_cast<unsigned char>(chosen));
    Log("camo: %s: camouflage '%c' for new 'Mechs\n", name, chosen);
    if (_stricmp(name, g_current) == 0) {
        SetCurrentPilot(name);  // re-arm for the selected pilot
    }
}

void RenamePilotRecords(const char* from, const char* to) {
    if (_stricmp(from, to) == 0) {
        return;
    }
    for (const wchar_t* section : {L"new_controls", L"camo_pending", L"camo"}) {
        const int value = ReadRecord(section, from);
        if (value) {
            WriteRecord(section, to, value);
            WriteRecord(section, from, 0);
        }
    }
    if (_stricmp(from, g_current) == 0) {
        strncpy_s(g_current, to, _TRUNCATE);
    }
}

bool TakeNewPilotControls(const char* name) {
    if (!ReadRecord(L"new_controls", name)) {
        return false;
    }
    WriteRecord(L"new_controls", name, 0);
    return true;
}

// The selected pilot: arm its pending camouflage, if any. Only this pilot's
// 'Mechs are created until another pilot is selected.
void SetCurrentPilot(const char* name) {
    strncpy_s(g_current, name, _TRUNCATE);
    const int code = name[0] ? ReadRecord(L"camo_pending", name) : 0;
    InterlockedExchange(&g_code, code);
    InterlockedExchange(&g_pending, code ? 1 : 0);
    if (code) {
        Log("camo: %s: camouflage '%c' pending for its next new 'Mech\n", name,
            static_cast<char>(code));
    }
}

int CurrentPilotCamoIndex() {
    const int code = g_current[0] ? ReadRecord(L"camo", g_current) : 0;
    const int index = CamoIndex(code ? static_cast<char>(code) : GetConfig().default_camo);
    // What the Update dialog opened with: leaving it alone changes nothing.
    InterlockedExchange(&g_update_initial, static_cast<unsigned char>(CamoCode(index)));
    return index;
}

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
