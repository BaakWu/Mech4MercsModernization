// Adds the "Windowed" checkbox to Options > Video.
//
// The shell UI is gosScript source compiled at runtime. The Video tab is
// mercsshellscripts\OM_Video.script inside RESOURCE\props.mw4. MekTek already
// added a "Use Extended Visibility" checkbox to that script, backed by
// ShellGetOptionValue/ShellSetOptionValue; the Windowed checkbox copies that
// pattern exactly (same widget, bitmap, font and message flow) and uses the
// option id kMwoWindowed, which display.cpp answers.
//
// The hook wraps the #VBD archive member fetch. After the game decompresses
// OM_Video.script into its own buffer, the text is rewritten in place at the
// same length: full-line comments are dropped and alignment whitespace is
// collapsed, which frees more than enough room for the inserted lines. Buffer
// pointers and size fields are never changed, so the game's allocator and
// stream code see exactly what they expect. props.mw4 is not modified.
//
// If an anchor is missing or the result would not fit, the script is left
// untouched and the tab looks exactly as before.
#include "hook.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace mw4hook {
namespace {

using MemberFetch_t = void(__fastcall*)(unsigned char* entry, void* edx, void* archive);
MemberFetch_t g_member_fetch = nullptr;

constexpr char kTargetName[] = "mercsshellscripts\\OM_Video.script";
constexpr char kGameTarget[] = "mercsshellscripts\\OM_Game.script";
constexpr char kJoinTarget[] = "mercsshellscripts\\JoinRegimentModal.script";
constexpr char kMarker[] = "MW4HOOK_WINDOWED";

// Entry layout (3.1c, matches the retail oracle's FUN_00413480/00413830).
constexpr size_t kEntryData = 0x08;
constexpr size_t kEntryStored = 0x18;
constexpr size_t kEntrySize = 0x1C;
constexpr size_t kEntryNameLen = 0x27;
constexpr size_t kEntryName = 0x28;

// The blocks are kept short: the rewrite must fit in the space freed by
// compacting the original text. Lines end in CRLF like the original file.
std::string CreateBlock() {
    return "int nWin = 1\r\n"
           "int nWinNow\r\n"
           "int dWin\r\n"
           "object oWin = s_multistatepanetoggle\r\n"
           "oWin.location = oDRes.location.x + 175,oDRes.location.y,nZ_BUTTON\r\n"
           "oWin.file = WPATH \"checkbox_15x15_4state.tga\"\r\n"
           "oWin.total_states = 4\r\n"
           "oWin.text = \"\"\r\n"
           "object oWinT = s_Text\r\n"
           "oWinT.location = oWin.location.x + 25,oWin.location.y,nZ_TEXT\r\n"
           "oWinT.nFontID = IDS_F_OM_LABEL\r\n"
           "oWinT.szText = \"Windowed\"\r\n"
           "oWinT.cTextColor = cWhite\r\n"
           "oWinT.nLayoutWidth = 120\r\n"
           "oWinT.nLayoutHeight = TextH\r\n"
           "initialize(oWinT)\r\n";
}

std::string InitBlock() {
    char buffer[200];
    sprintf_s(buffer,
              "callback($$Shell_CallbackHandler$$,ShellGetOptionValue,%d,nWin)\r\n"
              "initialize(oWin)\r\n"
              "mail(nWin + 1,oWin)\r\n",
              kMwoWindowed);
    return buffer;
}

std::string MailboxBlock() {
    char buffer[240];
    sprintf_s(buffer,
              "if (sender == oWin)\r\n"
              "{\r\n"
              "nWin = !nWin\r\n"
              "mail(nWin + 1,oWin)\r\n"
              "callback($$Shell_CallbackHandler$$,ShellSetOptionValue,%d,nWin)\r\n"
              "}\r\n",
              kMwoWindowed);
    return buffer;
}

// The tab's Reset button restores the values from when the tab was opened
// (the script keeps them as dGamma, dRes, ...). dWin does the same for the
// Windowed setting: recorded next to dRes, restored next to it.
std::string ResetBlock() {
    char buffer[120];
    sprintf_s(buffer, "callback($$Shell_CallbackHandler$$,ShellSetOptionValue,%d,dWin)\r\n",
              kMwoWindowed);
    return buffer;
}

// Keeps the box in step when the mode changes without a click (Alt+Enter,
// or a failed fullscreen switch that falls back to a window).
std::string ExecuteBlock() {
    char buffer[240];
    sprintf_s(buffer,
              "GUI_EXECUTE\r\n"
              "{\r\n"
              "callback($$Shell_CallbackHandler$$,ShellGetOptionValue,%d,nWinNow)\r\n"
              "if (nWinNow != nWin)\r\n"
              "{\r\n"
              "nWin = nWinNow\r\n"
              "mail(nWin + 1,oWin)\r\n"
              "}\r\n"
              "}\r\n",
              kMwoWindowed);
    return buffer;
}

bool InsertAfterLine(std::string* text, const char* anchor, const std::string& block) {
    size_t at = text->find(anchor);
    if (at == std::string::npos) {
        return false;
    }
    size_t eol = text->find('\n', at);
    if (eol == std::string::npos) {
        return false;
    }
    text->insert(eol + 1, block);
    return true;
}

bool InsertBeforeLine(std::string* text, const char* anchor, size_t from,
                      const std::string& block) {
    size_t at = text->find(anchor, from);
    if (at == std::string::npos) {
        return false;
    }
    size_t bol = text->rfind('\n', at);
    bol = bol == std::string::npos ? 0 : bol + 1;
    text->insert(bol, block);
    return true;
}

// Drop full-line // comments, trim each line, collapse runs of spaces/tabs
// outside string literals. One statement per line, CRLF line endings (the
// original file's), and no newline after the last line (the original ends
// with "}" too).
std::string Compact(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) {
            end = text.size();
        }
        std::string line = text.substr(start, end - start);
        start = end + 1;
        size_t first = line.find_first_not_of(" \t\r");
        if (first == std::string::npos) {
            continue;
        }
        size_t last = line.find_last_not_of(" \t\r");
        line = line.substr(first, last - first + 1);
        if (line.compare(0, 2, "//") == 0) {
            continue;
        }
        if (!out.empty()) {
            out += "\r\n";
        }
        bool in_string = false;
        bool space = false;
        for (char c : line) {
            if (c == '"') {
                in_string = !in_string;
            }
            if (!in_string && (c == ' ' || c == '\t')) {
                space = true;
                continue;
            }
            if (space) {
                out.push_back(' ');
                space = false;
            }
            out.push_back(c);
        }
    }
    return out;
}

// Writes |text| back into the game's buffer at the same length: a marker
// comment padded with spaces, then the compacted script. Leaves the buffer
// untouched if it does not fit.
void WriteBack(char* data, size_t size, const std::string& text, const char* name,
               const char* what) {
    const std::string body = Compact(text);
    std::string out = std::string("//") + kMarker;
    if (out.size() + 2 + body.size() > size) {
        Log("script: %s rewrite needs %zu bytes, have %zu; skipped\n", name,
            out.size() + 2 + body.size(), size);
        return;
    }
    out.append(size - out.size() - 2 - body.size(), ' ');
    out += "\r\n";
    out += body;
    memcpy(data, out.data(), size);
    Log("script: %s added to %s (%zu bytes, %zu spare)\n", what, name, size,
        size - 2 - body.size() - sizeof(kMarker) - 1);
}

void InjectWindowedCheckbox(char* data, size_t size) {
    std::string text(data, size);
    if (text.compare(0, sizeof(kMarker) + 1, std::string("//") + kMarker) == 0) {
        return;  // already rewritten (cached entry)
    }
    bool ok = InsertAfterLine(&text, "int dUseExtendedVisibility", CreateBlock());
    size_t init = text.find("GUI_INIT");
    ok = ok && init != std::string::npos &&
         InsertBeforeLine(&text, "if (first_time)", init, InitBlock());
    ok = ok && InsertAfterLine(&text, "dRes = nRes", "dWin = nWin\r\n");
    ok = ok && InsertBeforeLine(&text,
                                "callback($$Shell_CallbackHandler$$,ShellSetOptionValue,"
                                "MWO_SCREENRESOLUTION,dRes)",
                                0, ResetBlock());
    ok = ok && InsertBeforeLine(&text, "if (sender == oUseExtendedVisibility)", 0,
                                MailboxBlock());
    // main's GUI_MAILBOX is the first one in the file.
    ok = ok && InsertBeforeLine(&text, "GUI_MAILBOX", 0, ExecuteBlock());
    if (!ok) {
        Log("script: OM_Video.script anchors not found; checkbox not added\n");
        return;
    }
    WriteBack(data, size, text, "OM_Video.script", "Windowed checkbox");
}

// One On/Off row on Options > Game, using the tab's own label and switch
// objects, placed one row below `above` (an existing s_OnOff object) and
// included in the tab's Reset. The value is read and written through the
// hook's option id. `id` names the script variables (n<id>, d<id>,
// o<id>T, o<id>OnOff).
bool AddGameSwitch(std::string* text, const char* id, const char* label, int row,
                   const char* above, int option) {
    char init[200];
    sprintf_s(init,
              "callback($$Shell_CallbackHandler$$,ShellGetOptionValue,%d,n%s)\r\n"
              "mail(nMSG_UPDATETOGGLE,n%s,o%sOnOff)\r\n",
              option, id, id, id);
    char set[120];
    sprintf_s(set, "callback($$Shell_CallbackHandler$$,ShellSetOptionValue,%d,", option);
    char create[900];
    sprintf_s(create,
              "int n%s\r\n"
              "int d%s\r\n"
              "object o%sT = s_Text\r\n"
              "o%sT.location = XIndent + TextX,YIndent + TextY + (%d * TextYOff),nZ_TEXT\r\n"
              "o%sT.nFontID = IDS_F_OM_LABEL\r\n"
              "o%sT.szText = \"%s\"\r\n"
              "o%sT.cTextColor = cWhite\r\n"
              "o%sT.nJustify = 0\r\n"
              "o%sT.nLayoutWidth = TextW\r\n"
              "o%sT.nLayoutHeight = TextH\r\n"
              "initialize(o%sT)\r\n"
              "object o%sOnOff = s_OnOff\r\n"
              "o%sOnOff.location = %s.location.x,%s.location.y + TextYOff,%s.location.z\r\n"
              "initialize(o%sOnOff)\r\n",
              id, id, id, id, row, id, id, label, id, id, id, id, id, id, id, above, above,
              above, id);
    char mailbox[400];
    sprintf_s(mailbox,
              "case o%sOnOff:\r\n{\r\nif (getmessage() == nMSG_OPTIONON)\r\n{\r\nn%s = 1\r\n}\r\n"
              "else\r\n{\r\nn%s = 0\r\n}\r\n%sn%s)\r\n}\r\nbreak\r\n",
              id, id, id, set, id);
    char record[60];
    sprintf_s(record, "d%s = n%s\r\n", id, id);
    char reset[160];
    sprintf_s(reset, "%sd%s)\r\n", set, id);
    char after_create[80];
    sprintf_s(after_create, "initialize(%s)", above);

    bool ok = InsertAfterLine(text, after_create, create);
    const size_t gui_init = text->find("GUI_INIT");
    ok = ok && gui_init != std::string::npos &&
         InsertBeforeLine(text, "if (first_time)", gui_init, init);
    ok = ok && InsertAfterLine(text, "dDiff = nDiff", record);
    ok = ok && InsertAfterLine(text,
                               "callback($$Shell_CallbackHandler$$,ShellOptionsSetToggleValue,"
                               "MWO_SPLASHDAMAGE,dSplashToggle)",
                               reset);
    ok = ok && InsertBeforeLine(text, "case oSplashOnOff:", 0, mailbox);
    return ok;
}

// Options > Game: "Throttle Decay:" below Splash Damage (option id 202), and
// with the mouse aim point installed "Gyro Aim:" below that (option id 203).
void InjectGameSwitches(char* data, size_t size) {
    std::string text(data, size);
    if (text.compare(0, sizeof(kMarker) + 1, std::string("//") + kMarker) == 0) {
        return;
    }
    bool ok = AddGameSwitch(&text, "Decay", "Throttle Decay:", 5, "oSplashOnOff",
                            kMwoThrottleDecay);
    const bool gyro = GetConfig().aim_track_pointer;
    if (ok && gyro) {
        ok = AddGameSwitch(&text, "Gyro", "Gyro Aim:", 6, "oDecayOnOff", kMwoGyroAim);
    }
    if (!ok) {
        Log("script: OM_Game.script anchors not found; switches not added\n");
        return;
    }
    WriteBack(data, size, text, "OM_Game.script",
              gyro ? "Throttle Decay and Gyro Aim switches" : "Throttle Decay switch");
}

// Roster > New > Join Regiment: a "Camouflage" list below Difficulty (the
// dialog's own droplist style), with MekTek's camouflage names, starting at
// [mechlab] default_camo. OK passes the choice to the hook (option id
// kMwoNewPilotCamo, ShellSetOptionValue = 49) just before the pilot is
// created; camo.cpp gives the new pilot's 'Mechs that camouflage. OK and
// Cancel move down 25 pixels to make room (still inside the frame). The
// list is hidden when the dialog is used to rename a pilot (Update).
void InjectCamoChoice(char* data, size_t size) {
    std::string text(data, size);
    if (text.compare(0, sizeof(kMarker) + 1, std::string("//") + kMarker) == 0) {
        return;
    }
    char head[200];
    sprintf_s(head,
              "int nCm\r\n"
              "callback($$Shell_CallbackHandler$$,50,%d,nCm)\r\n"
              "object oCm = s_droplistbox\r\n",
              kMwoNewPilotCamo);
    std::string create = head;
    create +=
        "oCm.location = 180, 395, 10\r\n"
        "oCm.boxFont3D = FPATH localize$(IDS_F_GEN_DROPDOWN)\r\n"
        "oCm.labelFont3D = FPATH localize$(IDS_F_GEN_LABEL)\r\n"
        "oCm.label = \"Camouflage:\"\r\n"
        "oCm.offsetLabel = 0, -15\r\n"
        "oCm.itemWidth = 300 - 16\r\n"
        "oCm.itemHeight = 20\r\n";
    char sizes[160];
    sprintf_s(sizes, "oCm.list_size = %d\r\noCm.max_displayed = 6\r\n", kCamoCount);
    create += sizes;
    for (int i = 0; i < kCamoCount; ++i) {
        char line[96];
        sprintf_s(line, "oCm.list_item[%d] = \"%s\"\r\n", i, kCamoNames[i]);
        create += line;
    }
    // Open the list with the default in view (6 rows shown).
    const int default_index = CamoIndex(GetConfig().default_camo);
    char select[120];
    sprintf_s(select, "oCm.nSelected = nCm\r\noCm.top_of_list = %d\r\ninitialize(oCm)\r\n",
              default_index > 5 ? default_index - 5 : 0);
    create += select;
    char set[160];
    sprintf_s(set,
              "nCm = oCm.nselected\r\n"
              "callback($$Shell_CallbackHandler$$,49,%d,nCm)\r\n",
              kMwoNewPilotCamo);

    bool ok = InsertAfterLine(&text, "initialize(DifficultyListBox)", create);
    ok = ok && InsertAfterLine(&text, "initialize( DifficultyListBox )",
                               "initialize(oCm)\r\n"
                               "if (useUpdateTitleString)\r\n{\r\ndeactivate(oCm)\r\n}\r\n");
    ok = ok && InsertAfterLine(&text, "deactivate( DifficultyListBox )", "deactivate(oCm)\r\n");
    ok = ok && InsertBeforeLine(&text, "int create_flag = callback(", 0, set);
    // OK / Cancel: 395 -> 420 (the frame ends at 450).
    for (const char* button : {"180, 395, 10", "480, 395, 10"}) {
        const size_t at = text.find(button);
        ok = ok && at != std::string::npos;
        if (at != std::string::npos) {
            text.replace(at + 5, 3, "420");
        }
    }
    if (!ok) {
        Log("script: JoinRegimentModal.script anchors not found; camouflage list not added\n");
        return;
    }
    WriteBack(data, size, text, "JoinRegimentModal.script", "Camouflage list");
}

// Development only ([debug] automation=1): save every shell script the game
// loads to mw4hook-scripts\ in the game directory, as fetched (before any
// rewrite), for reading the menus' logic.
void DumpScript(const unsigned char* entry) {
    const int len = entry[kEntryNameLen];
    const char* name = reinterpret_cast<const char*>(entry + kEntryName);
    const DWORD size = *reinterpret_cast<const DWORD*>(entry + kEntrySize);
    const char* data = *reinterpret_cast<char* const*>(entry + kEntryData);
    const bool script = len >= 8 && _strnicmp(name + len - 7, ".script", 7) == 0;
    const bool header = (len >= 3 && _strnicmp(name + len - 2, ".h", 2) == 0) ||
                        (len >= 5 && _strnicmp(name + len - 4, ".hpp", 4) == 0);
    if (!data || size == 0 || size > 1024 * 1024 || !(script || header)) {
        return;
    }
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
    char* slash = n ? strrchr(path, '\\') : nullptr;
    if (!slash) {
        return;
    }
    strcpy_s(slash + 1, MAX_PATH - (slash + 1 - path), "mw4hook-scripts");
    CreateDirectoryA(path, nullptr);
    const char* base = name;
    for (int i = 0; i < len; ++i) {
        if (name[i] == '\\' || name[i] == '/') {
            base = name + i + 1;
        }
    }
    char file[MAX_PATH];
    sprintf_s(file, "%s\\%.*s", path, static_cast<int>(name + len - base), base);
    HANDLE h = CreateFileA(file, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(h, data, size, &written, nullptr);
        CloseHandle(h);
    }
}

void __fastcall Hook_MemberFetch(unsigned char* entry, void* edx, void* archive) {
    g_member_fetch(entry, edx, archive);
    __try {
        if (GetConfig().debug_automation) {
            DumpScript(entry);
        }
        const char* name = reinterpret_cast<char*>(entry + kEntryName);
        const size_t len = entry[kEntryNameLen];
        const bool video = len == sizeof(kTargetName) - 1 && _stricmp(name, kTargetName) == 0;
        const bool game = len == sizeof(kGameTarget) - 1 && _stricmp(name, kGameTarget) == 0;
        const bool join = len == sizeof(kJoinTarget) - 1 && _stricmp(name, kJoinTarget) == 0;
        if (!video && !game && !join) {
            return;
        }
        const DWORD stored = *reinterpret_cast<DWORD*>(entry + kEntryStored);
        const DWORD size = *reinterpret_cast<DWORD*>(entry + kEntrySize);
        char* data = *reinterpret_cast<char**>(entry + kEntryData);
        // Only a decompressed member owns a private buffer that is safe to edit.
        if (!data || stored >= size || size > 1024 * 1024) {
            Log("script: %s not in a private buffer; skipped\n", name);
            return;
        }
        if (video) {
            InjectWindowedCheckbox(data, size);
        } else if (game) {
            InjectGameSwitches(data, size);
        } else {
            InjectCamoChoice(data, size);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("script: exception while inspecting archive entry\n");
    }
}

bool g_installed = false;

}  // namespace

bool InstallShellScriptHook() {
    if (g_installed) {
        return true;
    }
    unsigned char* fetch = FindUnique(
        "member_fetch", "55 56 57 8B 7C 24 10 8B F1 33 ED 8B 47 10 8B 0F 85 C0 74 39");
    if (!fetch) {
        return false;
    }
    g_installed = true;
    InstallDetour(fetch, 7, reinterpret_cast<void*>(&Hook_MemberFetch),
                  reinterpret_cast<void**>(&g_member_fetch));
    Log("script: archive member hook %s\n", g_member_fetch ? "ok" : "failed");
    return true;
}

}  // namespace mw4hook