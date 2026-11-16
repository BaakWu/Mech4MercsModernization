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
    // Final text: a marker comment (padded with spaces to the original
    // length) followed by the compacted script.
    const std::string body = Compact(text);
    std::string out = std::string("//") + kMarker;
    if (out.size() + 2 + body.size() > size) {
        Log("script: OM_Video.script rewrite needs %zu bytes, have %zu; skipped\n",
            out.size() + 2 + body.size(), size);
        return;
    }
    out.append(size - out.size() - 2 - body.size(), ' ');
    out += "\r\n";
    out += body;
    memcpy(data, out.data(), size);
    Log("script: Windowed checkbox added to OM_Video.script (%zu bytes, %zu spare)\n",
        size, size - 2 - body.size() - sizeof(kMarker) - 1);
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
        if (entry[kEntryNameLen] != sizeof(kTargetName) - 1 ||
            _stricmp(reinterpret_cast<char*>(entry + kEntryName), kTargetName) != 0) {
            return;
        }
        const DWORD stored = *reinterpret_cast<DWORD*>(entry + kEntryStored);
        const DWORD size = *reinterpret_cast<DWORD*>(entry + kEntrySize);
        char* data = *reinterpret_cast<char**>(entry + kEntryData);
        // Only a decompressed member owns a private buffer that is safe to edit.
        if (!data || stored >= size || size > 1024 * 1024) {
            Log("script: OM_Video.script not in a private buffer; skipped\n");
            return;
        }
        InjectWindowedCheckbox(data, size);
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