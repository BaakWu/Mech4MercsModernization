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
constexpr char kControlsTarget[] = "mercsshellscripts\\OM_Controls.script";
constexpr char kGameTarget[] = "mercsshellscripts\\OM_Game.script";
constexpr char kJoinTarget[] = "mercsshellscripts\\JoinRegimentModal.script";
constexpr char kMainMenuTarget[] = "mercsshellscripts\\MainMenu.script";
constexpr char kListBoxSuffix[] = "\\cm_listboxes.script";
constexpr char kChassisSuffix[] = "\\chassis.script";
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
bool FitsBack(size_t size, const std::string& text) {
    return sizeof(kMarker) + 1 + 2 + Compact(text).size() <= size;
}

bool WriteBack(char* data, size_t size, const std::string& text, const char* name,
               const char* what) {
    const std::string body = Compact(text);
    std::string out = std::string("//") + kMarker;
    if (out.size() + 2 + body.size() > size) {
        Log("script: %s rewrite needs %zu bytes, have %zu; skipped\n", name,
            out.size() + 2 + body.size(), size);
        return false;
    }
    out.append(size - out.size() - 2 - body.size(), ' ');
    out += "\r\n";
    out += body;
    memcpy(data, out.data(), size);
    Log("script: %s added to %s (%zu bytes, %zu spare)\n", what, name, size,
        size - 2 - body.size() - sizeof(kMarker) - 1);
    return true;
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
// created; camo.cpp gives the new pilot's 'Mechs that camouflage. In the
// Update (rename) dialog the list starts at the pilot's last choice
// (kMwoPilotCamo) and a change becomes its default for new 'Mechs. OK and
// Cancel move down 25 pixels to make room (still inside the frame).
void InjectCamoChoice(char* data, size_t size) {
    std::string text(data, size);
    if (text.compare(0, sizeof(kMarker) + 1, std::string("//") + kMarker) == 0) {
        return;
    }
    // Written compactly (no optional spaces): the rewritten script must fit
    // the original buffer.
    char head[200];
    sprintf_s(head,
              "int nCm\r\n"
              "callback($$Shell_CallbackHandler$$,50,%d,nCm)\r\n"
              "object oCm=s_droplistbox\r\n",
              kMwoNewPilotCamo);
    std::string create = head;
    create +=
        "oCm.location=180,395,10\r\n"
        "oCm.boxFont3D=FPATH localize$(IDS_F_GEN_DROPDOWN)\r\n"
        "oCm.labelFont3D=FPATH localize$(IDS_F_GEN_LABEL)\r\n"
        "oCm.label=\"Camouflage:\"\r\n"
        "oCm.offsetLabel=0,-15\r\n"
        "oCm.itemWidth=284\r\n"
        "oCm.itemHeight=20\r\n";
    char sizes[160];
    sprintf_s(sizes, "oCm.list_size=%d\r\noCm.max_displayed=6\r\n", kCamoCount);
    create += sizes;
    for (int i = 0; i < kCamoCount; ++i) {
        char line[96];
        sprintf_s(line, "oCm.list_item[%d]=\"%s\"\r\n", i, kCamoNames[i]);
        create += line;
    }
    create += "oCm.nSelected=nCm\r\ninitialize(oCm)\r\n";
    // Update (rename) dialog: start at the selected pilot's camouflage.
    char update_init[200];
    sprintf_s(update_init,
              "if (useUpdateTitleString)\r\n{\r\n"
              "callback($$Shell_CallbackHandler$$,50,%d,nCm)\r\n"
              "oCm.nSelected=nCm\r\n}\r\n"
              "initialize(oCm)\r\n",
              kMwoPilotCamo);
    char set_new[120];
    sprintf_s(set_new, "nCm=oCm.nselected\r\ncallback($$Shell_CallbackHandler$$,49,%d,nCm)\r\n",
              kMwoNewPilotCamo);
    char set_update[120];
    sprintf_s(set_update, "nCm=oCm.nselected\r\ncallback($$Shell_CallbackHandler$$,49,%d,nCm)\r\n",
              kMwoPilotCamo);

    bool ok = InsertAfterLine(&text, "initialize(DifficultyListBox)", create);
    ok = ok && InsertAfterLine(&text, "initialize( DifficultyListBox )", update_init);
    ok = ok && InsertAfterLine(&text, "deactivate( DifficultyListBox )", "deactivate(oCm)\r\n");
    ok = ok && InsertBeforeLine(&text, "int create_flag = callback(", 0, set_new);
    ok = ok && InsertBeforeLine(&text, "int rename_flag = callback(", 0, set_update);
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
    if (script) {
        // Test aid: which screen scripts load, in order (the regression test
        // waits for these as screen checkpoints).
        Log("script: load %.*s\n", static_cast<int>(name + len - base), base);
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

// The installed patch version from the install record next to the game
// (mw4patch-install.ini, tool_version=...); empty for a development install.
std::string InstalledVersion() {
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
    char* slash = n ? strrchr(path, '\\') : nullptr;
    if (!slash) {
        return {};
    }
    strcpy_s(slash + 1, MAX_PATH - (slash + 1 - path), "mw4patch-install.ini");
    HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return {};
    }
    char buffer[8192] = {};
    DWORD read = 0;
    ReadFile(file, buffer, sizeof(buffer) - 1, &read, nullptr);
    CloseHandle(file);
    const char* key = strstr(buffer, "tool_version=");
    if (!key) {
        return {};
    }
    std::string version;
    for (const char* p = key + 13; *p && version.size() < 16; ++p) {
        if ((*p >= '0' && *p <= '9') || *p == '.') {
            version += *p;
        } else {
            break;
        }
    }
    return version;
}

// Main Menu (issue #24): a "MODERNIZED / PATCH vX.Y.Z" badge in the empty
// recessed plate at the bottom left of the frame, opposite MekTek's badge -
// two s_Text labels in the menus' own MicroStyle fonts (font 55 =
// microstyle8bold, the smallest), over the straight part of the plate, in front of the frame
// (depth 100; the plate is mt_unk_button at 10,475; higher depth is in front).
void InjectMainMenuBadge(char* data, size_t size) {
    std::string text(data, size);
    if (text.compare(0, sizeof(kMarker) + 1, std::string("//") + kMarker) == 0) {
        return;
    }
    const std::string version = InstalledVersion();
    const std::string second = version.empty() ? "PATCH" : "PATCH v" + version;
    const std::string block =
        "object oMwmA = s_Text\r\n"
        "oMwmA.location = 27,514,120\r\n"
        "oMwmA.nFontID = 55\r\n"
        "oMwmA.szText = \"MODERNIZED\"\r\n"
        "oMwmA.cTextColor = 0xffd8a424\r\n"
        "oMwmA.nJustify = 1\r\n"
        "oMwmA.nLayoutWidth = 90\r\n"
        "oMwmA.nLayoutHeight = 12\r\n"
        "initialize(oMwmA)\r\n"
        "object oMwmB = s_Text\r\n"
        "oMwmB.location = 27,526,120\r\n"
        "oMwmB.nFontID = 55\r\n"
        "oMwmB.szText = \"" + second + "\"\r\n"
        "oMwmB.cTextColor = 0xffc4c4c4\r\n"
        "oMwmB.nJustify = 1\r\n"
        "oMwmB.nLayoutWidth = 90\r\n"
        "oMwmB.nLayoutHeight = 12\r\n"
        "initialize(oMwmB)\r\n";
    // s_Text lives in c_Text.script, which the Main Menu does not include.
    if (!InsertAfterLine(&text, "initialize(mt_unk_button)", block) ||
        !InsertAfterLine(&text, "mektek_version.script\"",
                         "#include \"Content\\\\MercsShellScripts\\\\c_Text.script\"\r\n")) {
        Log("script: MainMenu.script anchor not found; badge not added\n");
        return;
    }
    WriteBack(data, size, text, "MainMenu.script", "Modernization badge");
}

// ---------------------------------------------------------------------------
// MechLab weight-class filter (issue #28).
//
// The MechLab's chassis list (Instant Action / multiplayer: "Type", one row
// per chassis; campaign: the owned 'Mechs) is cm_mechlistbox, a list widget
// in cm_listboxes.script. The rest of the MechLab works with the widget's row
// numbers (nSelected, list_item[]) in about 40 places, so the filter lives in
// the widget: it keeps a list of visible rows, vis[0..vis_count), and scrolls,
// highlights and draws those; a pick still sets nSelected to the original
// row. With filter_class < 0 (the default, and in every other screen that
// uses the widget) vis is the identity and vis_count = list_size, so the
// widget behaves exactly as before.
//
// A row's class: its name is matched against the chassis names, string
// 4500 + id, both without spaces, '_' and '-' (the list writes "Black Heart"
// and "-Elemental" where the strings say "Blackheart" and "Elemental"); the
// longest prefix wins ("Mad Cat Mark II" over "Mad Cat"). It is looked up in
// MekTek's own
// mechWeightClassArray[id] (MechWeight.script, 0 Light .. 3 Assault) on the
// MechBay's mech_info object. Rows with no match (e.g. a renamed 'Mech) stay
// visible.
//
// chassis.script narrows the list to two thirds of its row and puts a
// "Class:" droplist (All / Light / Medium / Heavy / Assault / Elemental -
// battle armour '-' and infantry '~' - only the classes present) in the last
// third; choosing a class loads that class's first 'Mech (the widget picks
// its first visible row when its owner mails it -77). The choice is kept by
// the hook for the session (option kMwoMechClass).

// Inserts `block` after the line holding the first '{' that follows `anchor`.
bool InsertInsideBlock(std::string* text, const char* anchor, const std::string& block,
                       size_t from = 0) {
    size_t at = text->find(anchor, from);
    if (at == std::string::npos) {
        return false;
    }
    size_t brace = text->find('{', at);
    if (brace == std::string::npos) {
        return false;
    }
    size_t eol = text->find('\n', brace);
    if (eol == std::string::npos) {
        return false;
    }
    text->insert(eol + 1, block);
    return true;
}

int ReplaceAll(std::string* text, const std::string& from, const std::string& to) {
    int n = 0;
    for (size_t at = text->find(from); at != std::string::npos;
         at = text->find(from, at + to.size())) {
        text->replace(at, from.size(), to);
        ++n;
    }
    return n;
}

// Options > Controls > Joystick > None: add a "None" item after the device
// names (the device indices stay the same; None is the count). The remap
// poller must not poll an out-of-range index for None; its two guards are
// narrowed to the real devices.
void InjectJoystickNone(char* data, size_t size) {
    std::string text(data, size);
    if (text.compare(0, sizeof(kMarker) + 1, std::string("//") + kMarker) == 0) {
        return;
    }
    const char* anchor =
        "callback($$Shell_CallbackHandler$$,GetJoyStickNames,nJoysticks,szJoyNames[])";
    const std::string block =
        "if(nJoysticks > 0 && nJoysticks < 10)\r\n"
        "{\r\n"
        "szJoyNames[nJoysticks] = \"None\"\r\n"
        "nJoysticks = nJoysticks + 1\r\n"
        "}\r\n";
    if (!InsertAfterLine(&text, anchor, block)) {
        Log("script: OM_Controls.script anchor not found; None option not added\n");
        return;
    }
    ReplaceAll(&text, "if (parent.nJoysticks != 0)",
              "if (parent.nJoysticks > 1 && parent.nJoySelected < parent.nJoysticks - 1)");
    ReplaceAll(&text, "if(parent.nJoysticks != 0)",
              "if(parent.nJoysticks > 1 && parent.nJoySelected < parent.nJoysticks - 1)");
    WriteBack(data, size, text, "OM_Controls.script", "Joystick None option");
}

// Script lines that set mwt to `var` without spaces, '_' and '-'.
#define MW_SQUEEZE(var)                                                          \
    "mwt = \"\"\r\n"                                                             \
    "for (mwj = 0; mwj < length$(" var "); mwj++)\r\n{\r\n"                     \
    "mwc = mid$(" var ", mwj, mwj + 1)\r\n"                                      \
    "if (equal$(mwc, \" \") != true)\r\n{\r\n"                                   \
    "if (equal$(mwc, \"_\") != true)\r\n{\r\n"                                   \
    "if (equal$(mwc, \"-\") != true)\r\n{\r\n"                                   \
    "mwt = mwt mwc\r\n}\r\n}\r\n}\r\n}\r\n"

// Appended to the filter's GUI_INIT block (see InjectListFilter).
#define MW_LIST_TRACE                                                             \
    "if (mwdbg == 1)\r\n{\r\n"                                                    \
    "callback($$Shell_CallbackHandler$$, 49, 208, filter_class, vis_count)\r\n"    \
    "for (mwi = 0; mwi < vis_count; mwi++)\r\n{\r\n"                               \
    "callback($$Shell_CallbackHandler$$, 49, 207, list_item[vis[mwi]], "           \
    "row_class[vis[mwi]])\r\n}\r\n}\r\n"
const char kListTraceMarker[] = "if (mwdbg == 1)";
const std::string kListTrace = MW_LIST_TRACE;

void InjectListFilter(char* data, size_t size) {
    std::string text(data, size);
    if (text.compare(0, sizeof(kMarker) + 1, std::string("//") + kMarker) == 0) {
        return;
    }
    if (text.find("cm_mechlistbox") == std::string::npos) {
        return;
    }
    // Navigation and drawing work in visible rows.
    const int sizes = ReplaceAll(&text, "list_size", "vis_count");
    // ...except the declaration (the first one), which the owners fill.
    const size_t decl = text.find("vis_count");
    if (decl == std::string::npos) {
        return;
    }
    text.replace(decl, 9, "list_size");
    const int status = ReplaceAll(&text, "mech_status [i]", "mech_status [vis[i]]");
    const int sel = ReplaceAll(&text, "nSelected == i", "nSelected == vis[i]");
    const int items = ReplaceAll(&text, "list_item[i]", "list_item[vis[i]]");
    const int pick = ReplaceAll(&text, "nSelected = lit", "nSelected = vis[lit]");
    const std::string sync =
        "if (filter_class < 0)\r\n{\r\nvis_count = list_size\r\n}\r\n";
    bool ok = sizes >= 15 && status == 2 && sel == 1 && items == 2 && pick == 1;
    ok = ok && InsertAfterLine(&text, "mech_status [max_items]",
                               "int filter_class = -1\r\n"
                               "int vis[max_items]\r\n"
                               "int vis_count = 0\r\n"
                               "int mwi\r\nint mwk\r\nint mwbest\r\nint mwlen\r\n"
                               "string mwn\r\nstring mws\r\nstring mwt\r\nstring mwc\r\n"
                               "int mwj\r\nint mwready = 0\r\nstring mwnames[128]\r\n"
                               "int count_classes = 0\r\nint mwrc\r\n"
                               "int row_class[max_items]\r\nint class_present[5]\r\nint mwdbg = 1\r\n"
                               "for (mwi = 0; mwi < max_items; mwi++)\r\n{\r\nvis[mwi] = mwi\r\n}\r\n");
    ok = ok && InsertInsideBlock(&text, "GUI_INIT",
                                 "if (filter_class < 0 && count_classes == 0)\r\n{\r\n"
                                 "for (mwi = 0; mwi < list_size; mwi++)\r\n{\r\nvis[mwi] = mwi\r\n}\r\n"
                                 "vis_count = list_size\r\n}\r\nelse\r\n{\r\n"
                                 // The chassis names without spaces, '_' and '-' (the list
                                 // writes "Black Heart" where the string is "Blackheart").
                                 "if (mwready == 0)\r\n{\r\n"
                                 "for (mwk = 0; mwk < 128; mwk++)\r\n{\r\n"
                                 "mws = localize$(4500 + mwk)\r\n" MW_SQUEEZE("mws")
                                 "mwnames[mwk] = mwt\r\n}\r\nmwready = 1\r\n}\r\n"
                                 "for (mwk = 0; mwk < 5; mwk++)\r\n{\r\nclass_present[mwk] = 0\r\n}\r\n"
                                 "vis_count = 0\r\n"
                                 "for (mwi = 0; mwi < list_size; mwi++)\r\n{\r\n"
                                 "mws = list_item[mwi]\r\n"
                                 "mwc = left$(mws, 1)\r\n"
                                 "mwrc = -1\r\n"
                                 // Battle armour ('-') and infantry ('~'): class 4.
                                 "if (equal$(mwc, \"-\"))\r\n{\r\nmwrc = 4\r\n}\r\n"
                                 "if (equal$(mwc, \"~\"))\r\n{\r\nmwrc = 4\r\n}\r\n"
                                 "if (mwrc < 0)\r\n{\r\n" MW_SQUEEZE("mws")
                                 "mwn = mwt\r\n"
                                 "mwbest = -1\r\nmwlen = 0\r\n"
                                 "for (mwk = 0; mwk < 128; mwk++)\r\n{\r\n"
                                 "if (length$(mwnames[mwk]) > mwlen)\r\n{\r\n"
                                 "if (equal$(left$(mwn, length$(mwnames[mwk])), mwnames[mwk]))\r\n{\r\n"
                                 "mwbest = mwk\r\nmwlen = length$(mwnames[mwk])\r\n}\r\n}\r\n}\r\n"
                                 "if (mwbest >= 0)\r\n{\r\n"
                                 "mwrc = @MechBay_main@mechinfo.mechWeightClassArray[mwbest]\r\n}\r\n}\r\n"
                                 "row_class[mwi] = mwrc\r\n"
                                 "if (mwrc >= 0)\r\n{\r\nclass_present[mwrc] = 1\r\n}\r\n"
                                 // Unknown rows (e.g. a renamed 'Mech) stay in every class.
                                 "if (filter_class < 0 || mwrc < 0 || mwrc == filter_class)\r\n"
                                 "{\r\nvis[vis_count] = mwi\r\nvis_count++\r\n}\r\n"
                                 "}\r\n}\r\n" MW_LIST_TRACE);
    // Keep the scroll position inside the (possibly shorter) list.
    ok = ok && InsertAfterLine(&text, "o_thumbregionbot.location = location.x+itemWidth,",
                               "if (top_of_list > vis_count - num_displayed)\r\n{\r\n"
                               "top_of_list = vis_count - num_displayed\r\n}\r\n"
                               "if (top_of_list < 0)\r\n{\r\ntop_of_list = 0\r\n}\r\n");
    for (const char* handler :
         {"LBUTTON_UPDATE", "GUI_MAILBOX", "GUI_CHAR", "GUI_EXECUTE", "GUI_DRAW"}) {
        ok = ok && InsertInsideBlock(&text, handler, sync);
    }
    // -77 from the owner: pick the first visible row, as a click would.
    ok = ok && InsertInsideBlock(&text, "GUI_MAILBOX",
                                 "if (sender == parent)\r\n{\r\n"
                                 "if (getmessage() == -77)\r\n{\r\n"
                                 "if (vis_count > 0)\r\n{\r\n"
                                 "nSelected = vis[0]\r\nmail(nselected)\r\n}\r\n}\r\n}\r\n");
    if (!ok) {
        Log("script: cm_listboxes.script layout not recognised (%d/%d/%d/%d/%d); "
            "MechLab class filter not added\n",
            sizes, status, sel, items, pick);
        return;
    }
    // Test aid ([debug] automation=1): the list reports its visible rows to the
    // hook (option 208: filter and count, 207: each row and its class), which
    // logs them for tools/Test-Mw4Regression.ps1. Left out when automation is
    // off, or when it would not fit the member's buffer.
    const size_t trace = text.find(kListTraceMarker);
    if (trace != std::string::npos &&
        (!GetConfig().debug_automation || !FitsBack(size, text))) {
        text.erase(trace, kListTrace.size());
    }
    WriteBack(data, size, text, "cm_listboxes.script", "MechLab class filter (list)");
}

void InjectClassChoice(char* data, size_t size) {
    std::string text(data, size);
    if (text.compare(0, sizeof(kMarker) + 1, std::string("//") + kMarker) == 0) {
        return;
    }
    if (text.find("o_availmechsdroplist") == std::string::npos) {
        return;
    }
    // Type list two thirds of the row, Class the last third, each with its
    // label above (the Type list was 269 + 16 wide, x 22..307).
    // Classes: 0 All, 1 Light, 2 Medium, 3 Heavy, 4 Assault, 5 Elemental
    // (battle armour and infantry, last as least likely). The droplist only
    // offers the classes present in the list (in the campaign: the classes
    // of the company's 'Mechs); mwfMap maps its rows to classes.
    const std::string create =
        "o_availmechsdroplist.itemWidth = 170\r\n"
        "o_availmechsdroplist.count_classes = 1\r\n"
        "o_availmechsdroplist.filter_class = -1\r\n"
        "initialize(o_availmechsdroplist)\r\n"
        "string mwfNames[6]\r\n"
        "mwfNames[0] = \"All\"\r\nmwfNames[1] = \"Light\"\r\nmwfNames[2] = \"Medium\"\r\n"
        "mwfNames[3] = \"Heavy\"\r\nmwfNames[4] = \"Assault\"\r\nmwfNames[5] = \"Elemental\"\r\n"
        "int mwfMap[6]\r\n"
        "int mwfN = 1\r\n"
        "int mwfK\r\n"
        "object o_mwfClass = s_droplistbox\r\n"
        "if $$m_isCampaign$$ == 0\r\n{\r\no_mwfClass.location = 216, 135, 52\r\n}\r\n"
        "if $$m_isCampaign$$ == 1\r\n{\r\no_mwfClass.location = 216, boxy + 55, 52\r\n}\r\n"
        "o_mwfClass.label = \"Class:\"\r\n"
        "o_mwfClass.offsetLabel = -1,-17\r\n"
        "o_mwfClass.list_item[0] = mwfNames[0]\r\n"
        "mwfMap[0] = 0\r\n"
        "for (mwfK = 1; mwfK < 6; mwfK++)\r\n{\r\n"
        "if (o_availmechsdroplist.class_present[mwfK - 1] == 1)\r\n{\r\n"
        "o_mwfClass.list_item[mwfN] = mwfNames[mwfK]\r\nmwfMap[mwfN] = mwfK\r\nmwfN++\r\n}\r\n}\r\n"
        "o_mwfClass.list_size = mwfN\r\n"
        "o_mwfClass.itemWidth = 75\r\n"
        "o_mwfClass.max_displayed = mwfN\r\n"
        "o_mwfClass.boxFont3d = FPATH localize$(IDS_F_ML_LABEL)\r\n"
        "o_mwfClass.itemHeight = 20\r\n"
        "o_mwfClass.labelFont3d = FPATH localize$(IDS_F_ML_LABEL)\r\n"
        "o_mwfClass.arrowHeight = 15\r\n"
        "o_mwfClass.id = 230\r\n"
        "int mwfCls = 0\r\n"
        "callback($$Shell_CallbackHandler$$, 50, 206, mwfCls)\r\n"
        "o_mwfClass.nSelected = 0\r\n"
        "for (mwfK = 0; mwfK < mwfN; mwfK++)\r\n{\r\n"
        "if (mwfMap[mwfK] == mwfCls)\r\n{\r\no_mwfClass.nSelected = mwfK\r\n}\r\n}\r\n"
        "initialize(o_mwfClass)\r\n"
        "o_availmechsdroplist.filter_class = mwfMap[o_mwfClass.nSelected] - 1\r\n"
        "initialize(o_availmechsdroplist)\r\n";
    const std::string handler =
        "if sender == o_mwfClass\r\n{\r\n"
        "callback($$Shell_CallbackHandler$$, 49, 206, mwfMap[o_mwfClass.nSelected])\r\n"
        "o_availmechsdroplist.filter_class = mwfMap[o_mwfClass.nSelected] - 1\r\n"
        "initialize(o_availmechsdroplist)\r\n"
        // A class: load its first 'Mech, as picking it in the list would.
        "if (o_mwfClass.nSelected > 0)\r\n{\r\nmail(-77, o_availmechsdroplist)\r\n}\r\n}\r\n";
    bool ok = InsertAfterLine(&text, "initialize(o_camodroplist)", create);
    const size_t camo = text.find("if sender == o_camodroplist");
    ok = ok && camo != std::string::npos;
    if (ok) {
        text.insert(camo, handler);
    }
    // The tab's GUI_DEACTIVATE / GUI_ACTIVATE hide and show its controls.
    ok = ok && InsertAfterLine(&text, "deactivate(o_availmechsdroplist)",
                               "deactivate(o_mwfClass)\r\n");
    const size_t act = text.find("\tactivate(o_availmechsdroplist)");
    ok = ok && act != std::string::npos;
    if (ok) {
        const size_t eol = text.find('\n', act);
        ok = eol != std::string::npos;
        if (ok) {
            text.insert(eol + 1, "activate(o_mwfClass)\r\n");
        }
    }
    if (!ok) {
        Log("script: chassis.script anchors not found; MechLab class filter not added\n");
        return;
    }
    WriteBack(data, size, text, "chassis.script", "MechLab class filter (droplist)");
}

bool EndsWithI(const char* name, size_t len, const char* suffix) {
    const size_t n = strlen(suffix);
    return len >= n && _strnicmp(name + len - n, suffix, n) == 0;
}

void __fastcall Hook_MemberFetch(unsigned char* entry, void* edx, void* archive) {
    g_member_fetch(entry, edx, archive);
    __try {
        if (GetConfig().debug_automation) {
            DumpScript(entry);
        }
        SharpenHudImage(entry, archive);
        const char* name = reinterpret_cast<char*>(entry + kEntryName);
        const size_t len = entry[kEntryNameLen];
        const bool video = len == sizeof(kTargetName) - 1 && _stricmp(name, kTargetName) == 0;
        const bool controls =
            len == sizeof(kControlsTarget) - 1 && _stricmp(name, kControlsTarget) == 0;
        const bool game = len == sizeof(kGameTarget) - 1 && _stricmp(name, kGameTarget) == 0;
        const bool join = len == sizeof(kJoinTarget) - 1 && _stricmp(name, kJoinTarget) == 0;
        const bool menu = GetConfig().menu_badge && len == sizeof(kMainMenuTarget) - 1 &&
                          _stricmp(name, kMainMenuTarget) == 0;
        const bool list = GetConfig().mechlab_filter && EndsWithI(name, len, kListBoxSuffix);
        const bool chassis = GetConfig().mechlab_filter && EndsWithI(name, len, kChassisSuffix);
        if (!video && !controls && !game && !join && !menu && !list && !chassis) {
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
        } else if (controls) {
            InjectJoystickNone(data, size);
        } else if (game) {
            InjectGameSwitches(data, size);
        } else if (join) {
            InjectCamoChoice(data, size);
        } else if (menu) {
            InjectMainMenuBadge(data, size);
        } else if (list) {
            InjectListFilter(data, size);
        } else {
            InjectClassChoice(data, size);
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
    // The fetch allocates a decompressed member with the game's allocator
    // (call at +0x2C: alloc(size, archive heap)) and frees the packed copy
    // (call at +0x8D); the HUD image sharpener (hudtex.cpp) uses the same two.
    if (fetch[0x2C] == 0xE8 && fetch[0x8D] == 0xE8) {
        SetGameAllocator(fetch + 0x2C + 5 + *reinterpret_cast<int*>(fetch + 0x2D),
                         fetch + 0x8D + 5 + *reinterpret_cast<int*>(fetch + 0x8E));
    } else {
        Log("script: game allocator calls not found; HUD sharpening off\n");
    }
    InstallDetour(fetch, 7, reinterpret_cast<void*>(&Hook_MemberFetch),
                  reinterpret_cast<void**>(&g_member_fetch));
    Log("script: archive member hook %s\n", g_member_fetch ? "ok" : "failed");
    return true;
}

}  // namespace mw4hook