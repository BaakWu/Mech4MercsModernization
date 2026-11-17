// Full company in campaign missions (issue #21, experimental, off by default).
//
// Each campaign mission has a record (built from the campaign definition's
// [Mission] keys) whose NumLancemates is at +0x40. Everything that limits a
// mission's lance reads it from there: the pre-mission screen's slot count
// (Shell command 30, ShellCampaignGetMissionRegionData, argument 7), the lance
// tonnage (slots 0-3 = first lance, 4-7 = second lance, walked up to
// NumLancemates) and the launch. The records are found with one lookup,
// CampaignMissionById (3.1c 0x0059A960: walk the campaign's mission list for
// record+0x38 == id). The hook lets the lookup run and raises a count of 1-6
// to 7 in the record it returns, so every reader sees the full company.
// Missions without lancemates (0) stay solo. Every campaign mission already
// has the second lance's drop zone (DropZones\SecondaryLanceStart).
#include "hook.h"

#include <intrin.h>
#include <cstring>

namespace mw4hook {
namespace {

constexpr int kRecordLancemates = 0x40;
constexpr int kFullCompany = 7;

using MissionById_t = unsigned char*(__thiscall*)(void* campaign, int id);
MissionById_t g_mission_by_id = nullptr;  // trampoline
bool g_installed = false;

unsigned char* __fastcall Hook_MissionById(void* campaign, void* /*edx*/, int id) {
    unsigned char* record = g_mission_by_id(campaign, id);
    if (record && !IsBadWritePtr(record + kRecordLancemates, sizeof(int))) {
        int& lancemates = *reinterpret_cast<int*>(record + kRecordLancemates);
        if (lancemates >= 1 && lancemates < kFullCompany) {
            static int logged = 0;
            if (logged < 40) {
                ++logged;
                Log("campaign: mission %d lancemates %d -> %d\n", id, lancemates, kFullCompany);
            }
            lancemates = kFullCompany;
        }
    }
    return record;
}

// Drop positions. A drop zone hands out positions from its markers (3.1c
// 0x004E61F0, DropZoneNextPosition: cycle the marker list at zone+0x174 and
// return the first spot that is clear). A zone without markers returns the
// zone object's own location instead. Stock missions only use the first lance,
// so SecondaryLanceStart is never used in the stock game, and some missions
// leave it empty (Tharkad - Checkmate's has no markers). With the full company
// the second lance then drops at that far-away spot: on the roster, nowhere
// near the player. For the
// player / lancemate spawn only (one call site, matched by return address),
// an empty zone's position is replaced by one beside the first lance: the
// positions the first lance got, shifted 40 m sideways per extra 'Mech.
using NextPosition_t = float*(__thiscall*)(void* zone, float* out);
NextPosition_t g_next_position = nullptr;  // trampoline
void* g_spawn_return = nullptr;            // return address of the spawn call

constexpr int kZoneMarkers = 0x174;
constexpr int kMaxRecorded = 8;
float g_recorded[kMaxRecorded][12];
int g_recorded_count = 0;
int g_filled = 0;
DWORD g_last_spawn = 0;

float* __fastcall Hook_NextPosition(void* zone, void* /*edx*/, float* out) {
    void* caller = _ReturnAddress();
    const bool spawn = caller == g_spawn_return;
    bool empty = false;
    if (spawn) {
        const DWORD now = GetTickCount();
        if (now - g_last_spawn > 20000) {  // a new mission
            g_recorded_count = 0;
            g_filled = 0;
        }
        g_last_spawn = now;
        empty = !IsBadReadPtr(static_cast<char*>(zone) + kZoneMarkers, sizeof(void*)) &&
                *reinterpret_cast<void**>(static_cast<char*>(zone) + kZoneMarkers) == nullptr;
    }
    float* result = g_next_position(zone, out);
    if (!spawn || !result) {
        return result;
    }
    if (!empty) {
        if (g_recorded_count < kMaxRecorded) {
            memcpy(g_recorded[g_recorded_count++], result, sizeof(g_recorded[0]));
        }
        return result;
    }
    if (g_recorded_count == 0) {
        Log("campaign: drop zone without positions and nothing to place beside\n");
        return result;
    }
    // Same facing as the first lance; position beside one of its spots.
    const int index = g_filled % g_recorded_count;
    const float distance = 40.0f * static_cast<float>(1 + g_filled / g_recorded_count);
    ++g_filled;
    const float* base = g_recorded[index];
    memcpy(result, base, sizeof(g_recorded[0]));
    // Row 2 of the rotation is horizontal (MW4 drop zones only turn about
    // the vertical axis).
    result[3] = base[3] + base[8] * distance;
    result[11] = base[11] + base[10] * distance;
    Log("campaign: drop zone has no positions; 'Mech placed %.0f m beside the first lance "
        "(%.0f, %.0f, %.0f)\n",
        distance, result[3], result[7], result[11]);
    return result;
}

}  // namespace

bool InstallCampaignHooks() {
    if (g_installed) {
        return true;
    }
    if (!GetConfig().full_company) {
        g_installed = true;
        return true;
    }
    // mov ecx,[ecx+10h]; mov edx,[esp+4]; test ecx,ecx; jz; mov eax,[ecx];
    // mov ecx,[ecx+4]; test eax,eax; jz; cmp [eax+38h],edx; jz; jmp; xor eax,eax;
    // ret 4
    unsigned char* target = FindUnique(
        "campaign_mission_by_id",
        "8B 49 10 8B 54 24 04 85 C9 74 10 8B 01 8B 49 04 85 C0 74 09 39 50 38 74 04 EB EC "
        "33 C0 C2 04 00");
    if (!target) {
        return false;  // not unpacked yet (or not this build); retry
    }
    g_installed = true;
    // The first two instructions (7 bytes) are position independent.
    InstallDetour(target, 7, reinterpret_cast<void*>(&Hook_MissionById),
                  reinterpret_cast<void**>(&g_mission_by_id));
    Log("campaign: full company (7 lancemates) hook %s\n", g_mission_by_id ? "ok" : "FAILED");

    // sub esp,0A0h; push ebx; push ebp; push esi; mov esi,ecx; push edi;
    // push 1; mov eax,[esi]; mov [esp+14h],esi; call [eax+48h]
    unsigned char* next = FindUnique(
        "drop_zone_next_position",
        "81 EC A0 00 00 00 53 55 56 8B F1 57 6A 01 8B 06 89 74 24 14 FF 50 48");
    // The player / lancemate spawn: lea ecx,[ebp-1A8h]; push ecx; mov ecx,eax;
    // call DropZoneNextPosition
    unsigned char* site = FindUnique("lancemate_spawn_position", "8D 8D 58 FE FF FF 51 8B C8 E8");
    if (next && site) {
        g_spawn_return = site + 9 + 5;
        // sub esp,0A0h (6 bytes) is position independent.
        InstallDetour(next, 6, reinterpret_cast<void*>(&Hook_NextPosition),
                      reinterpret_cast<void**>(&g_next_position));
    }
    Log("campaign: empty drop zone fallback %s\n", g_next_position ? "ok" : "missing");
    return true;
}

}  // namespace mw4hook
