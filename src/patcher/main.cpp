// mw4patch.exe - MechWarrior 4: Mercenaries 3.1c modern patch (development).
//
// A single self-contained executable that:
//   1. verifies the supported 3.1c build against the core manifest,
//   2. backs up options.ini and any conflicting hook files,
//   3. patches the display configuration,
//   4. extracts the embedded dinput.dll hook, ddraw.dll frontend scaler, and
//      mw4hook.ini,
//   5. creates a windowed shortcut,
//   6. writes an install record, and supports uninstall/rollback.
//
// No game files are redistributed. The embedded payload is our own hook.
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include <wincrypt.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cwchar>
#include <cwctype>
#include <map>
#include <string>
#include <vector>

#include "resource.h"

namespace {

constexpr const wchar_t* kToolVersion = L"1.3.0";
constexpr const char* kExpectedExeSha256 =
    "72bbb1ba3e5e5e7dc7607ca403f3d9aca301d0b16814a6b577bda7ec5c922b41";
constexpr const wchar_t* kGameExe = L"MW4Mercs.exe";
constexpr const wchar_t* kRecordName = L"mw4patch-install.ini";
constexpr const wchar_t* kOptionsName = L"options.ini";
constexpr const wchar_t* kOptionsBackup = L"options.ini.mw4patch-backup";
constexpr const wchar_t* kHookName = L"dinput.dll";
constexpr const wchar_t* kHookBackup = L"dinput.dll.mw4patch-backup";
constexpr const wchar_t* kDdrawName = L"ddraw.dll";
constexpr const wchar_t* kDdrawBackup = L"ddraw.dll.mw4patch-backup";
constexpr const wchar_t* kIniName = L"mw4hook.ini";
constexpr const wchar_t* kIniBackup = L"mw4hook.ini.mw4patch-backup";
constexpr const wchar_t* kShortcutName =
    L"MechWarrior 4 Mercenaries (Windowed).lnk";
constexpr const wchar_t* kExistingShortcutName =
    L"Shortcut to MW4Mercs.exe.lnk";
constexpr const wchar_t* kExistingShortcutBackup =
    L"Shortcut to MW4Mercs.exe.lnk.mw4patch-backup";

// ---------------------------------------------------------------------------
// Console and string helpers
// ---------------------------------------------------------------------------

// The GUI installer build (MW4PATCH_GUI) sends all output to its log box
// through this sink instead of the console.
void (*g_output_sink)(const wchar_t* text) = nullptr;

void PrintTo(FILE* stream, const wchar_t* fmt, va_list args) {
    if (g_output_sink) {
        wchar_t buffer[2048];
        _vsnwprintf_s(buffer, _TRUNCATE, fmt, args);
        g_output_sink(buffer);
        return;
    }
    vfwprintf(stream, fmt, args);
    fflush(stream);
}

void Print(const wchar_t* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    PrintTo(stdout, fmt, args);
    va_end(args);
}

void PrintError(const wchar_t* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    PrintTo(stderr, fmt, args);
    va_end(args);
}

std::wstring Join(const std::wstring& dir, const wchar_t* name) {
    std::wstring result = dir;
    if (!result.empty() && result.back() != L'\\' && result.back() != L'/') {
        result += L'\\';
    }
    result += name;
    return result;
}

std::wstring Join(const std::wstring& dir, const std::wstring& name) {
    return Join(dir, name.c_str());
}

bool FileExists(const std::wstring& path) {
    DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool DirectoryExists(const std::wstring& path) {
    DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

std::string TrimAscii(const std::string& input) {
    size_t begin = 0;
    while (begin < input.size() &&
           (input[begin] == ' ' || input[begin] == '\t' ||
            input[begin] == '\r' || input[begin] == '\n')) {
        ++begin;
    }
    size_t end = input.size();
    while (end > begin &&
           (input[end - 1] == ' ' || input[end - 1] == '\t' ||
            input[end - 1] == '\r' || input[end - 1] == '\n')) {
        --end;
    }
    return input.substr(begin, end - begin);
}

std::string ToLowerAscii(const std::string& input) {
    std::string result = input;
    for (char& c : result) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return result;
}

std::wstring Utf8ToWide(const std::string& input) {
    if (input.empty()) {
        return std::wstring();
    }
    int size = MultiByteToWideChar(CP_UTF8, 0, input.c_str(),
                                   static_cast<int>(input.size()), nullptr, 0);
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, input.c_str(),
                        static_cast<int>(input.size()), &result[0], size);
    return result;
}

std::string WideToUtf8(const std::wstring& input) {
    if (input.empty()) {
        return std::string();
    }
    int size = WideCharToMultiByte(CP_UTF8, 0, input.c_str(),
                                   static_cast<int>(input.size()), nullptr, 0,
                                   nullptr, nullptr);
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, input.c_str(),
                        static_cast<int>(input.size()), &result[0], size,
                        nullptr, nullptr);
    return result;
}

std::string HexEncode(const BYTE* data, DWORD size) {
    static const char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(size * 2);
    for (DWORD i = 0; i < size; ++i) {
        result.push_back(digits[data[i] >> 4]);
        result.push_back(digits[data[i] & 0x0F]);
    }
    return result;
}

std::wstring IsoUtcNow() {
    SYSTEMTIME time;
    GetSystemTime(&time);
    wchar_t buffer[64];
    swprintf_s(buffer, L"%04u-%02u-%02uT%02u:%02u:%02uZ", time.wYear,
               time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond);
    return buffer;
}

// ---------------------------------------------------------------------------
// File helpers
// ---------------------------------------------------------------------------

bool ReadFileAll(const std::wstring& path, std::vector<BYTE>& out) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart > 0x7FFFFFFF) {
        CloseHandle(file);
        return false;
    }
    out.resize(static_cast<size_t>(size.QuadPart));
    size_t offset = 0;
    while (offset < out.size()) {
        DWORD read = 0;
        DWORD chunk = static_cast<DWORD>(
            std::min<size_t>(out.size() - offset, 1 << 20));
        if (!ReadFile(file, out.data() + offset, chunk, &read, nullptr) ||
            read == 0) {
            CloseHandle(file);
            return false;
        }
        offset += read;
    }
    CloseHandle(file);
    return true;
}

bool WriteFileAll(const std::wstring& path, const void* data, size_t size) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    const BYTE* bytes = static_cast<const BYTE*>(data);
    size_t offset = 0;
    while (offset < size) {
        DWORD written = 0;
        DWORD chunk =
            static_cast<DWORD>(std::min<size_t>(size - offset, 1 << 20));
        if (!WriteFile(file, bytes + offset, chunk, &written, nullptr) ||
            written == 0) {
            CloseHandle(file);
            return false;
        }
        offset += written;
    }
    FlushFileBuffers(file);
    CloseHandle(file);
    return true;
}

bool CopyFileTo(const std::wstring& source, const std::wstring& target) {
    return CopyFileW(source.c_str(), target.c_str(), FALSE) != FALSE;
}

bool LoadResourceBytes(int id, std::vector<BYTE>& out) {
    HRSRC resource = FindResourceW(nullptr, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!resource) {
        return false;
    }
    DWORD size = SizeofResource(nullptr, resource);
    HGLOBAL handle = LoadResource(nullptr, resource);
    if (!handle) {
        return false;
    }
    const void* data = LockResource(handle);
    if (!data || size == 0) {
        return false;
    }
    out.assign(static_cast<const BYTE*>(data),
               static_cast<const BYTE*>(data) + size);
    return true;
}

// ---------------------------------------------------------------------------
// SHA-256
// ---------------------------------------------------------------------------

bool Sha256Bytes(const void* data, size_t size, std::string& hex) {
    HCRYPTPROV provider = 0;
    HCRYPTHASH hash = 0;
    bool ok = false;
    if (CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_AES,
                             CRYPT_VERIFYCONTEXT)) {
        if (CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash)) {
            if (CryptHashData(hash, static_cast<const BYTE*>(data),
                              static_cast<DWORD>(size), 0)) {
                BYTE digest[32];
                DWORD length = sizeof(digest);
                if (CryptGetHashParam(hash, HP_HASHVAL, digest, &length, 0)) {
                    hex = HexEncode(digest, length);
                    ok = true;
                }
            }
            CryptDestroyHash(hash);
        }
        CryptReleaseContext(provider, 0);
    }
    return ok;
}

bool Sha256File(const std::wstring& path, std::string& hex) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    HCRYPTPROV provider = 0;
    HCRYPTHASH hash = 0;
    bool ok = false;
    if (CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_AES,
                             CRYPT_VERIFYCONTEXT)) {
        if (CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash)) {
            std::vector<BYTE> buffer(1 << 20);
            ok = true;
            for (;;) {
                DWORD read = 0;
                if (!ReadFile(file, buffer.data(),
                              static_cast<DWORD>(buffer.size()), &read,
                              nullptr)) {
                    if (GetLastError() == ERROR_HANDLE_EOF) {
                        break;
                    }
                    ok = false;
                    break;
                }
                if (read == 0) {
                    break;
                }
                if (!CryptHashData(hash, buffer.data(), read, 0)) {
                    ok = false;
                    break;
                }
            }
            if (ok) {
                BYTE digest[32];
                DWORD length = sizeof(digest);
                if (CryptGetHashParam(hash, HP_HASHVAL, digest, &length, 0)) {
                    hex = HexEncode(digest, length);
                } else {
                    ok = false;
                }
            }
            CryptDestroyHash(hash);
        }
        CryptReleaseContext(provider, 0);
    }
    CloseHandle(file);
    return ok;
}

// ---------------------------------------------------------------------------
// Manifest verification
// ---------------------------------------------------------------------------

struct VerifyResult {
    int verified = 0;
    int missing = 0;
    int mismatch = 0;
    bool Ok() const { return missing == 0 && mismatch == 0; }
};

VerifyResult VerifyCoreFiles(const std::wstring& install, bool verbose) {
    VerifyResult result;
    std::vector<BYTE> manifest;
    if (!LoadResourceBytes(IDR_CORE_MANIFEST, manifest)) {
        PrintError(L"error: embedded core manifest is missing\n");
        result.mismatch = 1;
        return result;
    }

    std::string text(manifest.begin(), manifest.end());
    size_t line_start = 0;
    int entries = 0;
    while (line_start <= text.size()) {
        size_t line_end = text.find('\n', line_start);
        if (line_end == std::string::npos) {
            line_end = text.size();
        }
        std::string line = TrimAscii(text.substr(line_start, line_end - line_start));
        line_start = line_end + 1;
        if (line.empty() || line[0] == '#') {
            continue;
        }
        size_t space = line.find(' ');
        if (space == std::string::npos) {
            continue;
        }
        std::string expected = ToLowerAscii(line.substr(0, space));
        std::string relative = line.substr(space + 1);
        while (!relative.empty() &&
               (relative[0] == ' ' || relative[0] == '*')) {
            relative.erase(relative.begin());
        }
        std::replace(relative.begin(), relative.end(), '/', '\\');
        ++entries;

        std::wstring path = Join(install, Utf8ToWide(relative));
        if (!FileExists(path)) {
            ++result.missing;
            Print(L"  missing : %ls\n", path.c_str());
            continue;
        }
        std::string actual;
        if (!Sha256File(path, actual)) {
            ++result.mismatch;
            Print(L"  hash err: %ls\n", path.c_str());
            continue;
        }
        if (actual != expected) {
            ++result.mismatch;
            Print(L"  mismatch: %ls\n", path.c_str());
            Print(L"            expected %hs\n", expected.c_str());
            Print(L"            actual   %hs\n", actual.c_str());
        } else {
            ++result.verified;
            if (verbose) {
                Print(L"  ok      : %ls\n", relative.c_str());
            }
        }
    }

    Print(L"Core manifest: %d verified, %d missing, %d mismatch (%d entries)\n",
          result.verified, result.missing, result.mismatch, entries);
    return result;
}

// ---------------------------------------------------------------------------
// Key/value install record
// ---------------------------------------------------------------------------

bool ReadKeyValueFile(const std::wstring& path,
                      std::map<std::string, std::string>& values) {
    std::vector<BYTE> data;
    if (!ReadFileAll(path, data)) {
        return false;
    }
    std::string text(data.begin(), data.end());
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) {
            end = text.size();
        }
        std::string line = TrimAscii(text.substr(start, end - start));
        start = end + 1;
        if (line.empty() || line[0] == '#' || line[0] == '[') {
            continue;
        }
        size_t equals = line.find('=');
        if (equals == std::string::npos) {
            continue;
        }
        values[TrimAscii(line.substr(0, equals))] =
            TrimAscii(line.substr(equals + 1));
    }
    return true;
}

bool WriteKeyValueFile(const std::wstring& path,
                       const std::map<std::string, std::string>& values) {
    std::string text;
    for (const auto& pair : values) {
        text += pair.first;
        text += '=';
        text += pair.second;
        text += "\r\n";
    }
    return WriteFileAll(path, text.data(), text.size());
}

// ---------------------------------------------------------------------------
// options.ini display patch
// ---------------------------------------------------------------------------

bool IsSectionLine(const std::string& line, std::string& name) {
    std::string trimmed = TrimAscii(line);
    if (trimmed.size() < 2 || trimmed.front() != '[' ||
        trimmed.back() != ']') {
        return false;
    }
    name = ToLowerAscii(TrimAscii(trimmed.substr(1, trimmed.size() - 2)));
    return true;
}

void ReplaceAll(std::string& text, const std::string& from,
                const std::string& to) {
    size_t position = 0;
    while ((position = text.find(from, position)) != std::string::npos) {
        text.replace(position, from.size(), to);
        position += to.size();
    }
}

bool PatchOptionsIni(const std::wstring& install, std::wstring& error) {
    const std::wstring path = Join(install, kOptionsName);
    std::string text;
    if (FileExists(path)) {
        std::vector<BYTE> data;
        if (!ReadFileAll(path, data)) {
            error = L"could not read options.ini";
            return false;
        }
        text.assign(data.begin(), data.end());
    } else {
        std::vector<BYTE> templateData;
        if (!LoadResourceBytes(IDR_OPTIONS_TEMPLATE, templateData)) {
            error = L"embedded options.ini template is missing";
            return false;
        }
        text.assign(templateData.begin(), templateData.end());
        ReplaceAll(text, "{{VIDEODRIVERINDEX}}", "0");
        ReplaceAll(text, "{{WIDTH}}", "1024");
        ReplaceAll(text, "{{HEIGHT}}", "768");
        ReplaceAll(text, "{{BITDEPTH}}", "32");
        const std::wstring temp = path + L".mw4patch-new";
        if (!WriteFileAll(temp, text.data(), text.size()) ||
            !MoveFileExW(temp.c_str(), path.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            DeleteFileW(temp.c_str());
            error = L"could not create options.ini";
            return false;
        }
        return true;
    }

    std::vector<std::string> lines;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) {
            end = text.size();
        }
        std::string line = text.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(line);
        if (end == text.size()) {
            break;
        }
        start = end + 1;
    }

    const std::vector<std::pair<std::string, std::string>> wanted = {
        {"videodriverindex", "0"},
        {"screenwidth", "1024"},
        {"screenheight", "768"},
        {"bitdepth", "32"},
        // Options > Video > Windowed (read by the hook; default on).
        {"windowed", "true"}};
    std::vector<bool> done(wanted.size(), false);

    int sectionStart = -1;
    int sectionEnd = -1;
    for (size_t i = 0; i < lines.size(); ++i) {
        std::string name;
        if (IsSectionLine(lines[i], name) && name == "graphics options") {
            sectionStart = static_cast<int>(i);
            sectionEnd = static_cast<int>(lines.size());
            for (size_t j = i + 1; j < lines.size(); ++j) {
                if (IsSectionLine(lines[j], name)) {
                    sectionEnd = static_cast<int>(j);
                    break;
                }
            }
            break;
        }
    }

    if (sectionStart < 0) {
        if (!lines.empty() && !lines.back().empty()) {
            lines.push_back(std::string());
        }
        lines.push_back("[graphics options]");
        for (const auto& pair : wanted) {
            lines.push_back(pair.first + "=" + pair.second);
        }
    } else {
        for (int i = sectionStart + 1; i < sectionEnd; ++i) {
            std::string line = TrimAscii(lines[i]);
            size_t equals = line.find('=');
            if (equals == std::string::npos) {
                continue;
            }
            std::string key = ToLowerAscii(TrimAscii(line.substr(0, equals)));
            for (size_t w = 0; w < wanted.size(); ++w) {
                if (key == wanted[w].first) {
                    lines[i] = wanted[w].first + "=" + wanted[w].second;
                    done[w] = true;
                    break;
                }
            }
        }
        std::vector<std::string> missing;
        for (size_t w = 0; w < wanted.size(); ++w) {
            if (!done[w]) {
                missing.push_back(wanted[w].first + "=" + wanted[w].second);
            }
        }
        if (!missing.empty()) {
            lines.insert(lines.begin() + sectionEnd, missing.begin(),
                         missing.end());
        }
    }

    std::string output;
    for (const auto& line : lines) {
        output += line;
        output += "\r\n";
    }
    const std::wstring temp = path + L".mw4patch-new";
    if (!WriteFileAll(temp, output.data(), output.size()) ||
        !MoveFileExW(temp.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temp.c_str());
        error = L"could not write options.ini";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Shortcut
// ---------------------------------------------------------------------------

bool CreateWindowedShortcut(const std::wstring& install,
                            const wchar_t* link_name, std::wstring& error) {
    const std::wstring exe = Join(install, kGameExe);
    const std::wstring linkPath = Join(install, link_name);

    IShellLinkW* link = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr,
                                  CLSCTX_INPROC_SERVER, IID_IShellLinkW,
                                  reinterpret_cast<void**>(&link));
    if (FAILED(hr) || !link) {
        error = L"could not create the shell link";
        return false;
    }

    link->SetPath(exe.c_str());
    link->SetArguments(L"-window");
    link->SetWorkingDirectory(install.c_str());
    link->SetIconLocation(exe.c_str(), 0);
    link->SetDescription(L"MechWarrior 4: Mercenaries (windowed)");

    IPersistFile* file = nullptr;
    hr = link->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&file));
    if (SUCCEEDED(hr) && file) {
        hr = file->Save(linkPath.c_str(), TRUE);
        file->Release();
    } else {
        hr = E_FAIL;
    }
    link->Release();

    if (FAILED(hr)) {
        error = L"could not save the windowed shortcut";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Process check
// ---------------------------------------------------------------------------

bool IsGameRunning() {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return false;
    }
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    bool running = false;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, kGameExe) == 0) {
                running = true;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return running;
}

// ---------------------------------------------------------------------------
// Install / uninstall
// ---------------------------------------------------------------------------

struct RollbackState {
    bool optionsPatched = false;
    bool dinputWritten = false;
    bool ddrawWritten = false;
    bool iniWritten = false;
    bool shortcutWritten = false;
    bool existingShortcutWritten = false;
    bool recordWritten = false;
    bool optionsBackup = false;
    bool dinputBackup = false;
    bool ddrawBackup = false;
    bool iniBackup = false;
    bool existingShortcutBackup = false;
};

void Rollback(const std::wstring& install, const RollbackState& state) {
    Print(L"Rolling back...\n");
    if (state.recordWritten) {
        DeleteFileW(Join(install, kRecordName).c_str());
    }
    if (state.shortcutWritten) {
        DeleteFileW(Join(install, kShortcutName).c_str());
    }
    if (state.existingShortcutWritten) {
        const std::wstring target = Join(install, kExistingShortcutName);
        const std::wstring backup = Join(install, kExistingShortcutBackup);
        if (state.existingShortcutBackup && FileExists(backup)) {
            CopyFileTo(backup, target);
        } else {
            DeleteFileW(target.c_str());
        }
    }
    if (state.dinputWritten) {
        const std::wstring target = Join(install, kHookName);
        const std::wstring backup = Join(install, kHookBackup);
        if (state.dinputBackup && FileExists(backup)) {
            CopyFileTo(backup, target);
        } else {
            DeleteFileW(target.c_str());
        }
    }
    if (state.ddrawWritten) {
        const std::wstring target = Join(install, kDdrawName);
        const std::wstring backup = Join(install, kDdrawBackup);
        if (state.ddrawBackup && FileExists(backup)) {
            CopyFileTo(backup, target);
        } else {
            DeleteFileW(target.c_str());
        }
    }
    if (state.iniWritten) {
        const std::wstring target = Join(install, kIniName);
        const std::wstring backup = Join(install, kIniBackup);
        if (state.iniBackup && FileExists(backup)) {
            CopyFileTo(backup, target);
        } else {
            DeleteFileW(target.c_str());
        }
    }
    if (state.optionsPatched) {
        const std::wstring target = Join(install, kOptionsName);
        const std::wstring backup = Join(install, kOptionsBackup);
        if (state.optionsBackup && FileExists(backup)) {
            CopyFileTo(backup, target);
        }
    }
    Print(L"Rollback complete. Backups were not deleted.\n");
}

bool PromptYesNo(const wchar_t* question) {
    Print(L"%ls [y/N] ", question);
    wchar_t buffer[32] = {};
    if (!fgetws(buffer, 32, stdin)) {
        return false;
    }
    return buffer[0] == L'y' || buffer[0] == L'Y';
}

// The [controls] mw5 value in an installed mw4hook.ini: 1, 0, or -1 when
// there is no such file or key.
int InstalledMw5(const std::wstring& install) {
    const std::wstring path = Join(install, kIniName);
    if (!FileExists(path)) {
        return -1;
    }
    return static_cast<int>(GetPrivateProfileIntW(L"controls", L"mw5", -1, path.c_str()));
}

// Sets "key=value" inside "[section]" of an ini held as text (CRLF lines).
// Adds the key, or the section, when missing.
void SetIniValue(std::string& text, const std::string& section, const std::string& key,
                 const std::string& value) {
    const std::string header = "[" + section + "]";
    size_t at = text.find(header);
    if (at == std::string::npos) {
        if (!text.empty() && text.back() != '\n') {
            text += "\r\n";
        }
        text += "\r\n" + header + "\r\n" + key + "=" + value + "\r\n";
        return;
    }
    size_t line = text.find('\n', at);
    line = line == std::string::npos ? text.size() : line + 1;
    size_t end = text.find("\n[", line);
    end = end == std::string::npos ? text.size() : end + 1;
    for (size_t pos = line; pos < end;) {
        size_t eol = text.find('\n', pos);
        eol = eol == std::string::npos ? text.size() : eol;
        if (text.compare(pos, key.size() + 1, key + "=") == 0) {
            size_t value_end = eol;
            if (value_end > pos && text[value_end - 1] == '\r') {
                --value_end;
            }
            text.replace(pos + key.size() + 1, value_end - pos - key.size() - 1, value);
            return;
        }
        pos = eol + 1;
    }
    text.insert(end, key + "=" + value + "\r\n");
}

// mw5: 1 / 0 = MW5-style controls on / off; -1 = keep the installed value
// (off on a first install). The Throttle Decay choice made in the game
// (Options > Game, saved in mw4hook.ini) is kept across re-installs.
int CommandInstall(const std::wstring& install, bool assumeYes, int mw5 = -1) {
    Print(L"mw4patch %ls - install\n", kToolVersion);
    Print(L"Install: %ls\n\n", install.c_str());

    if (!DirectoryExists(install)) {
        PrintError(L"error: install directory not found\n");
        return 1;
    }
    const std::wstring exe = Join(install, kGameExe);
    if (!FileExists(exe)) {
        PrintError(L"error: MW4Mercs.exe not found in the install directory\n");
        return 1;
    }
    if (IsGameRunning()) {
        PrintError(L"error: MW4Mercs.exe is running. Close the game first.\n");
        return 3;
    }

    std::string exeHash;
    if (!Sha256File(exe, exeHash)) {
        PrintError(L"error: could not hash MW4Mercs.exe\n");
        return 1;
    }
    Print(L"MW4Mercs.exe SHA-256: %hs\n", exeHash.c_str());
    if (exeHash != kExpectedExeSha256) {
        PrintError(L"error: unsupported MW4Mercs.exe build\n");
        PrintError(L"expected: %hs\n", kExpectedExeSha256);
        PrintError(L"actual:   %hs\n", exeHash.c_str());
        return 2;
    }

    Print(L"\nVerifying core files:\n");
    VerifyResult verify = VerifyCoreFiles(install, false);
    if (!verify.Ok()) {
        PrintError(L"\nerror: this install does not match the supported 3.1c "
                   L"baseline. Nothing was changed.\n");
        return 2;
    }

    std::vector<BYTE> dinput;
    std::vector<BYTE> ddraw;
    std::vector<BYTE> ini;
    std::vector<BYTE> optionsTemplate;
    if (!LoadResourceBytes(IDR_HOOK_DLL, dinput) ||
        !LoadResourceBytes(IDR_DDRAW_DLL, ddraw) ||
        !LoadResourceBytes(IDR_HOOK_INI, ini)) {
        PrintError(L"error: embedded hook payload is missing\n");
        return 1;
    }
    {
        const int installed_mw5 = InstalledMw5(install);
        const bool use_mw5 = mw5 >= 0 ? mw5 != 0 : installed_mw5 == 1;
        const std::wstring existing = Join(install, kIniName);
        const int decay = FileExists(existing)
                              ? static_cast<int>(GetPrivateProfileIntW(L"controls", L"decay", 0,
                                                                       existing.c_str()))
                              : 0;
        std::string text(ini.begin(), ini.end());
        SetIniValue(text, "controls", "mw5", use_mw5 ? "1" : "0");
        SetIniValue(text, "controls", "decay", decay ? "1" : "0");
        // The mouse aim point is part of the MW5-style controls choice.
        SetIniValue(text, "aim", "track_pointer", use_mw5 ? "1" : "0");
        // Player tuning kept from an existing install.
        if (FileExists(existing)) {
            for (const wchar_t* key : {L"sensitivity", L"marker", L"gyro"}) {
                wchar_t value[64] = {};
                GetPrivateProfileStringW(L"aim", key, L"", value, 64, existing.c_str());
                if (value[0]) {
                    char narrow_key[32] = {};
                    char narrow_value[64] = {};
                    WideCharToMultiByte(CP_ACP, 0, key, -1, narrow_key, 32, nullptr, nullptr);
                    WideCharToMultiByte(CP_ACP, 0, value, -1, narrow_value, 64, nullptr, nullptr);
                    SetIniValue(text, "aim", narrow_key, narrow_value);
                }
            }
        }
        ini.assign(text.begin(), text.end());
        Print(L"MechWarrior 5-style controls: %ls\n", use_mw5 ? L"on" : L"off");
    }
    std::string dinputHash;
    std::string ddrawHash;
    std::string iniHash;
    if (!Sha256Bytes(dinput.data(), dinput.size(), dinputHash) ||
        !Sha256Bytes(ddraw.data(), ddraw.size(), ddrawHash) ||
        !Sha256Bytes(ini.data(), ini.size(), iniHash)) {
        PrintError(L"error: could not hash the embedded payload\n");
        return 1;
    }

    const std::wstring optionsPath = Join(install, kOptionsName);
    const std::wstring dinputPath = Join(install, kHookName);
    const std::wstring ddrawPath = Join(install, kDdrawName);
    const std::wstring iniPath = Join(install, kIniName);
    const std::wstring optionsBackup = Join(install, kOptionsBackup);
    const std::wstring dinputBackup = Join(install, kHookBackup);
    const std::wstring ddrawBackup = Join(install, kDdrawBackup);
    const std::wstring iniBackup = Join(install, kIniBackup);
    const std::wstring recordPath = Join(install, kRecordName);

    bool existingDinput = FileExists(dinputPath);
    bool existingDdraw = FileExists(ddrawPath);
    bool existingIni = FileExists(iniPath);
    bool existingOptions = FileExists(optionsPath);
    bool backupOptions = existingOptions && !FileExists(optionsBackup);
    bool backupDinput = existingDinput && !FileExists(dinputBackup);
    bool backupDdraw = existingDdraw && !FileExists(ddrawBackup);
    bool backupIni = existingIni && !FileExists(iniBackup);

    if (existingDinput && backupDinput) {
        std::string existingHash;
        if (Sha256File(dinputPath, existingHash) && existingHash == dinputHash) {
            backupDinput = false;  // re-install over our own hook
        }
    }
    if (existingDdraw && backupDdraw) {
        std::string existingHash;
        if (Sha256File(ddrawPath, existingHash) && existingHash == ddrawHash) {
            backupDdraw = false;
        }
    }
    if (existingIni && backupIni) {
        std::string existingHash;
        if (Sha256File(iniPath, existingHash) && existingHash == iniHash) {
            backupIni = false;
        }
    }
    // Updating our own install: the hook files there are ours (an older
    // version, or mw4hook.ini changed by the game), not files to restore.
    if (FileExists(recordPath)) {
        backupDinput = false;
        backupDdraw = false;
        backupIni = false;
    }

    // An update keeps the player's display settings (resolution, Windowed);
    // only a first install sets the known-good 1024x768 / 32-bit keys.
    const bool updating = FileExists(recordPath);
    Print(L"\nPlan:\n");
    if (updating) {
        Print(L"  - keep the display settings in options.ini (update)\n");
    } else {
        Print(L"  - %ls display keys -> 1024x768, 32-bit\n",
              existingOptions ? L"patch" : L"create");
    }
    Print(L"  - install %ls (%hs)\n", kHookName, dinputHash.c_str());
    Print(L"  - install %ls (%hs)\n", kDdrawName, ddrawHash.c_str());
    Print(L"  - install %ls\n", kIniName);
    Print(L"  - create %ls\n", kShortcutName);
    if (backupOptions || backupDinput || backupDdraw || backupIni) {
        Print(L"  - backups:");
        if (backupOptions) {
            Print(L" %ls", kOptionsBackup);
        }
        if (backupDinput) {
            Print(L" %ls", kHookBackup);
        }
        if (backupDdraw) {
            Print(L" %ls", kDdrawBackup);
        }
        if (backupIni) {
            Print(L" %ls", kIniBackup);
        }
        Print(L"\n");
    }
    Print(L"\n");

    if (!assumeYes && !PromptYesNo(L"Apply the patch?")) {
        Print(L"Aborted.\n");
        return 3;
    }

    RollbackState state;
    state.optionsBackup = backupOptions;
    state.dinputBackup = backupDinput;
    state.ddrawBackup = backupDdraw;
    state.iniBackup = backupIni;

    if (backupOptions && !CopyFileTo(optionsPath, optionsBackup)) {
        PrintError(L"error: could not back up options.ini\n");
        return 1;
    }
    if (backupDinput && !CopyFileTo(dinputPath, dinputBackup)) {
        PrintError(L"error: could not back up dinput.dll\n");
        return 1;
    }
    if (backupDdraw && !CopyFileTo(ddrawPath, ddrawBackup)) {
        PrintError(L"error: could not back up ddraw.dll\n");
        return 1;
    }
    if (backupIni && !CopyFileTo(iniPath, iniBackup)) {
        PrintError(L"error: could not back up mw4hook.ini\n");
        return 1;
    }

    std::wstring error;
    if (!updating) {
        if (!PatchOptionsIni(install, error)) {
            PrintError(L"error: %ls\n", error.c_str());
            Rollback(install, state);
            return 1;
        }
        state.optionsPatched = true;
    }

    const std::wstring dinputTemp = dinputPath + L".mw4patch-new";
    if (!WriteFileAll(dinputTemp, dinput.data(), dinput.size()) ||
        !MoveFileExW(dinputTemp.c_str(), dinputPath.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(dinputTemp.c_str());
        PrintError(L"error: could not install dinput.dll\n");
        Rollback(install, state);
        return 1;
    }
    state.dinputWritten = true;

    const std::wstring ddrawTemp = ddrawPath + L".mw4patch-new";
    if (!WriteFileAll(ddrawTemp, ddraw.data(), ddraw.size()) ||
        !MoveFileExW(ddrawTemp.c_str(), ddrawPath.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(ddrawTemp.c_str());
        PrintError(L"error: could not install ddraw.dll\n");
        Rollback(install, state);
        return 1;
    }
    state.ddrawWritten = true;

    const std::wstring iniTemp = iniPath + L".mw4patch-new";
    if (!WriteFileAll(iniTemp, ini.data(), ini.size()) ||
        !MoveFileExW(iniTemp.c_str(), iniPath.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(iniTemp.c_str());
        PrintError(L"error: could not install mw4hook.ini\n");
        Rollback(install, state);
        return 1;
    }
    state.iniWritten = true;

    if (!CreateWindowedShortcut(install, kShortcutName, error)) {
        PrintError(L"warning: %ls\n", error.c_str());
    } else {
        state.shortcutWritten = true;
    }

    // Make the player's existing desktop-style shortcut use the windowed
    // launch too. Back it up first so uninstall can restore it.
    const std::wstring existingShortcut = Join(install, kExistingShortcutName);
    const std::wstring existingShortcutBackup =
        Join(install, kExistingShortcutBackup);
    bool updateExistingShortcut = FileExists(existingShortcut);
    if (updateExistingShortcut) {
        if (!FileExists(existingShortcutBackup)) {
            if (!CopyFileTo(existingShortcut, existingShortcutBackup)) {
                PrintError(L"warning: could not back up the existing shortcut; "
                           L"leaving it unchanged\n");
                updateExistingShortcut = false;
            } else {
                state.existingShortcutBackup = true;
            }
        } else {
            state.existingShortcutBackup = true;
        }
    }
    if (updateExistingShortcut) {
        if (CreateWindowedShortcut(install, kExistingShortcutName, error)) {
            state.existingShortcutWritten = true;
        } else {
            PrintError(L"warning: could not update the existing shortcut: %ls\n",
                       error.c_str());
        }
    }

    std::map<std::string, std::string> record;
    record["tool_version"] = WideToUtf8(kToolVersion);
    record["installed_utc"] = WideToUtf8(IsoUtcNow());
    record["install_path"] = WideToUtf8(install);
    record["target_sha256"] = kExpectedExeSha256;
    record["file.dinput.dll"] = dinputHash;
    record["file.ddraw.dll"] = ddrawHash;
    record["file.mw4hook.ini"] = iniHash;
    if (state.shortcutWritten) {
        record["file.shortcut"] = WideToUtf8(kShortcutName);
    }
    if (state.existingShortcutWritten) {
        record["file.existing_shortcut"] = WideToUtf8(kExistingShortcutName);
    }
    if (backupOptions) {
        record["backup.options.ini"] = WideToUtf8(kOptionsBackup);
    }
    if (backupDinput) {
        record["backup.dinput.dll"] = WideToUtf8(kHookBackup);
    }
    if (backupDdraw) {
        record["backup.ddraw.dll"] = WideToUtf8(kDdrawBackup);
    }
    if (backupIni) {
        record["backup.mw4hook.ini"] = WideToUtf8(kIniBackup);
    }
    if (state.existingShortcutBackup) {
        record["backup.existing_shortcut"] =
            WideToUtf8(kExistingShortcutBackup);
    }
    if (!WriteKeyValueFile(recordPath, record)) {
        PrintError(L"error: could not write the install record\n");
        Rollback(install, state);
        return 1;
    }
    state.recordWritten = true;

    Print(L"\nInstalled:\n");
    Print(L"  dinput.dll   %hs\n", dinputHash.c_str());
    Print(L"  ddraw.dll    %hs\n", ddrawHash.c_str());
    Print(L"  mw4hook.ini %hs\n", iniHash.c_str());
    Print(L"  record      %ls\n", recordPath.c_str());
    Print(L"\nLaunch with the new shortcut, or:\n  MW4Mercs.exe -window\n");
    return 0;
}

int CommandUninstall(const std::wstring& install, bool assumeYes) {
    Print(L"mw4patch %ls - uninstall\n", kToolVersion);
    Print(L"Install: %ls\n\n", install.c_str());

    if (!DirectoryExists(install)) {
        PrintError(L"error: install directory not found\n");
        return 1;
    }
    if (IsGameRunning()) {
        PrintError(L"error: MW4Mercs.exe is running. Close the game first.\n");
        return 3;
    }

    const std::wstring recordPath = Join(install, kRecordName);
    const std::wstring dinputPath = Join(install, kHookName);
    const std::wstring ddrawPath = Join(install, kDdrawName);
    const std::wstring iniPath = Join(install, kIniName);
    const std::wstring optionsPath = Join(install, kOptionsName);
    const std::wstring dinputBackup = Join(install, kHookBackup);
    const std::wstring ddrawBackup = Join(install, kDdrawBackup);
    const std::wstring iniBackup = Join(install, kIniBackup);
    const std::wstring optionsBackup = Join(install, kOptionsBackup);
    const std::wstring shortcutPath = Join(install, kShortcutName);

    std::map<std::string, std::string> record;
    bool haveRecord = ReadKeyValueFile(recordPath, record);
    if (!haveRecord) {
        Print(L"No install record found; removing known hook files only.\n");
    }

    std::string embeddedDinputHash;
    std::string embeddedDdrawHash;
    std::string embeddedIniHash;
    std::vector<BYTE> dinput;
    std::vector<BYTE> ddraw;
    std::vector<BYTE> ini;
    if (LoadResourceBytes(IDR_HOOK_DLL, dinput)) {
        Sha256Bytes(dinput.data(), dinput.size(), embeddedDinputHash);
    }
    if (LoadResourceBytes(IDR_DDRAW_DLL, ddraw)) {
        Sha256Bytes(ddraw.data(), ddraw.size(), embeddedDdrawHash);
    }
    if (LoadResourceBytes(IDR_HOOK_INI, ini)) {
        Sha256Bytes(ini.data(), ini.size(), embeddedIniHash);
    }

    auto removeIfMatches = [&](const std::wstring& path,
                               const std::string& expected) -> bool {
        if (!FileExists(path)) {
            return true;
        }
        std::string actual;
        if (!Sha256File(path, actual)) {
            PrintError(L"warning: could not hash %ls; leaving it\n", path.c_str());
            return false;
        }
        if (!expected.empty() && actual != expected) {
            PrintError(L"warning: %ls does not match the installed hook; "
                       L"leaving it\n",
                       path.c_str());
            return false;
        }
        return DeleteFileW(path.c_str()) != FALSE;
    };

    std::string recordDinputHash;
    auto it = record.find("file.dinput.dll");
    if (it != record.end()) {
        recordDinputHash = it->second;
    }
    if (recordDinputHash.empty()) {
        recordDinputHash = embeddedDinputHash;
    }
    std::string recordDdrawHash;
    it = record.find("file.ddraw.dll");
    if (it != record.end()) {
        recordDdrawHash = it->second;
    }
    if (recordDdrawHash.empty()) {
        recordDdrawHash = embeddedDdrawHash;
    }
    std::string recordIniHash;
    it = record.find("file.mw4hook.ini");
    if (it != record.end()) {
        recordIniHash = it->second;
    }
    if (recordIniHash.empty()) {
        recordIniHash = embeddedIniHash;
    }

    Print(L"Plan:\n");
    Print(L"  - remove %ls (if it matches our hook)\n", kHookName);
    Print(L"  - remove %ls (if it matches our hook)\n", kDdrawName);
    Print(L"  - remove %ls\n", kIniName);
    Print(L"  - remove %ls\n", kShortcutName);
    Print(L"  - restore backups\n\n");
    if (!assumeYes && !PromptYesNo(L"Remove the patch?")) {
        Print(L"Aborted.\n");
        return 3;
    }

    removeIfMatches(dinputPath, recordDinputHash);
    removeIfMatches(ddrawPath, recordDdrawHash);
    // mw4hook.ini is the patch's own settings file; the game writes to it
    // (Throttle Decay), so its hash changes. Always remove it (a
    // previous one, if any, is restored from its backup below).
    (void)recordIniHash;
    if (FileExists(iniPath)) {
        DeleteFileW(iniPath.c_str());
    }
    // The hook's per-pilot records (new pilots' pending setup, camouflage).
    DeleteFileW(Join(install, L"mw4hook-pilots.ini").c_str());
    // Hook log files.
    WIN32_FIND_DATAW found{};
    HANDLE find = FindFirstFileW(Join(install, L"mw4hook-*.log").c_str(), &found);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            DeleteFileW(Join(install, found.cFileName).c_str());
        } while (FindNextFileW(find, &found));
        FindClose(find);
    }

    if (FileExists(shortcutPath)) {
        DeleteFileW(shortcutPath.c_str());
    }

    if (FileExists(optionsBackup)) {
        if (CopyFileTo(optionsBackup, optionsPath)) {
            DeleteFileW(optionsBackup.c_str());
            Print(L"Restored options.ini\n");
        } else {
            PrintError(L"warning: could not restore options.ini\n");
        }
    }
    if (FileExists(dinputBackup)) {
        if (!FileExists(dinputPath) || removeIfMatches(dinputPath, recordDinputHash)) {
            if (MoveFileExW(dinputBackup.c_str(), dinputPath.c_str(),
                            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                Print(L"Restored previous dinput.dll\n");
            }
        } else {
            PrintError(L"warning: left %ls in place; backup kept as %ls\n",
                       dinputPath.c_str(), dinputBackup.c_str());
        }
    }
    if (FileExists(ddrawBackup)) {
        if (!FileExists(ddrawPath) || removeIfMatches(ddrawPath, recordDdrawHash)) {
            if (MoveFileExW(ddrawBackup.c_str(), ddrawPath.c_str(),
                            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                Print(L"Restored previous ddraw.dll\n");
            }
        } else {
            PrintError(L"warning: left %ls in place; backup kept as %ls\n",
                       ddrawPath.c_str(), ddrawBackup.c_str());
        }
    }
    if (FileExists(iniBackup)) {
        if (MoveFileExW(iniBackup.c_str(), iniPath.c_str(),
                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            Print(L"Restored previous mw4hook.ini\n");
        }
    }

    const std::wstring existingShortcutBackup =
        Join(install, kExistingShortcutBackup);
    if (FileExists(existingShortcutBackup)) {
        if (CopyFileTo(existingShortcutBackup,
                       Join(install, kExistingShortcutName))) {
            DeleteFileW(existingShortcutBackup.c_str());
            Print(L"Restored previous shortcut\n");
        }
    }

    DeleteFileW(recordPath.c_str());
    Print(L"\nUninstall complete.\n");
    return 0;
}

int CommandVerify(const std::wstring& install) {
    Print(L"mw4patch %ls - verify\n", kToolVersion);
    Print(L"Install: %ls\n\n", install.c_str());

    const std::wstring exe = Join(install, kGameExe);
    if (!FileExists(exe)) {
        PrintError(L"error: MW4Mercs.exe not found\n");
        return 1;
    }
    std::string exeHash;
    if (!Sha256File(exe, exeHash)) {
        PrintError(L"error: could not hash MW4Mercs.exe\n");
        return 1;
    }
    Print(L"MW4Mercs.exe SHA-256: %hs\n", exeHash.c_str());
    bool exeOk = exeHash == kExpectedExeSha256;
    Print(L"Supported build: %ls\n\n", exeOk ? L"yes" : L"NO");

    VerifyResult verify = VerifyCoreFiles(install, false);

    const std::wstring dinputPath = Join(install, kHookName);
    const std::wstring ddrawPath = Join(install, kDdrawName);
    const std::wstring recordPath = Join(install, kRecordName);
    std::string embeddedHash;
    std::string embeddedDdrawHash;
    std::vector<BYTE> dinput;
    std::vector<BYTE> ddraw;
    if (LoadResourceBytes(IDR_HOOK_DLL, dinput)) {
        Sha256Bytes(dinput.data(), dinput.size(), embeddedHash);
    }
    if (LoadResourceBytes(IDR_DDRAW_DLL, ddraw)) {
        Sha256Bytes(ddraw.data(), ddraw.size(), embeddedDdrawHash);
    }
    if (FileExists(dinputPath)) {
        std::string actual;
        if (Sha256File(dinputPath, actual)) {
            Print(L"dinput.dll hook: %ls\n",
                  actual == embeddedHash ? L"installed (matches)"
                                         : L"present (unknown hash)");
        }
    } else {
        Print(L"dinput.dll hook: not installed\n");
    }
    if (FileExists(ddrawPath)) {
        std::string actual;
        if (Sha256File(ddrawPath, actual)) {
            Print(L"ddraw.dll frontend scaler: %ls\n",
                  actual == embeddedDdrawHash ? L"installed (matches)"
                                              : L"present (unknown hash)");
        }
    } else {
        Print(L"ddraw.dll frontend scaler: not installed\n");
    }
    Print(L"Install record: %ls\n",
          FileExists(recordPath) ? L"present" : L"absent");

    return (exeOk && verify.Ok()) ? 0 : 2;
}

int CommandInfo(const std::wstring& install) {
    Print(L"mw4patch %ls - info\n", kToolVersion);
    Print(L"Install: %ls\n\n", install.c_str());

    const std::wstring optionsPath = Join(install, kOptionsName);
    if (FileExists(optionsPath)) {
        std::vector<BYTE> data;
        if (ReadFileAll(optionsPath, data)) {
            std::string text(data.begin(), data.end());
            Print(L"options.ini display keys:\n");
            size_t start = 0;
            while (start < text.size()) {
                size_t end = text.find('\n', start);
                if (end == std::string::npos) {
                    end = text.size();
                }
                std::string line = TrimAscii(text.substr(start, end - start));
                std::string lower = ToLowerAscii(line);
                if (lower.rfind("screenwidth=", 0) == 0 ||
                    lower.rfind("screenheight=", 0) == 0 ||
                    lower.rfind("bitdepth=", 0) == 0 ||
                    lower.rfind("windowed=", 0) == 0 ||
                    lower.rfind("videodriverindex=", 0) == 0) {
                    Print(L"  %hs\n", line.c_str());
                }
                start = end + 1;
            }
        }
    } else {
        Print(L"options.ini: absent\n");
    }

    const std::wstring recordPath = Join(install, kRecordName);
    std::map<std::string, std::string> record;
    if (ReadKeyValueFile(recordPath, record)) {
        Print(L"\nInstall record:\n");
        for (const auto& pair : record) {
            Print(L"  %hs=%hs\n", pair.first.c_str(), pair.second.c_str());
        }
    } else {
        Print(L"\nInstall record: absent\n");
    }
    return 0;
}

void PrintUsage() {
    Print(L"mw4patch %ls\n", kToolVersion);
    Print(L"Usage:\n");
    Print(L"  mw4patch.exe install   [install-dir] [--yes] [--mw5 | --no-mw5]\n");
    Print(L"  mw4patch.exe uninstall [install-dir] [--yes]\n");
    Print(L"  mw4patch.exe verify    [install-dir]\n");
    Print(L"  mw4patch.exe info      [install-dir]\n");
    Print(L"\nIf install-dir is omitted, the current directory is used.\n");
}

std::wstring DefaultInstall() {
    wchar_t buffer[MAX_PATH] = {};
    DWORD length = GetCurrentDirectoryW(MAX_PATH, buffer);
    if (length == 0 || length >= MAX_PATH) {
        return std::wstring();
    }
    return buffer;
}

std::wstring NormalizePath(const std::wstring& path) {
    wchar_t buffer[MAX_PATH];
    DWORD length = GetFullPathNameW(path.c_str(), MAX_PATH, buffer, nullptr);
    if (length == 0 || length >= MAX_PATH) {
        return path;
    }
    std::wstring result = buffer;
    while (result.size() > 3 && (result.back() == L'\\' || result.back() == L'/')) {
        result.pop_back();
    }
    return result;
}

// ---------------------------------------------------------------------------
// Double-click mode. Explorer starts the patcher with no arguments in a
// console of its own that closes as soon as the process exits, so: find the
// game folder automatically (or ask for it), offer install or uninstall, and
// wait for Enter before closing.
// ---------------------------------------------------------------------------

bool OwnsConsole() {
    DWORD processes[4] = {};
    return GetConsoleProcessList(processes, 4) <= 1;
}

void WaitForEnter() {
    Print(L"\nPress Enter to close...");
    wchar_t buffer[16] = {};
    fgetws(buffer, 16, stdin);
}

bool IsGameDir(const std::wstring& dir) {
    return !dir.empty() && FileExists(Join(dir, L"MW4Mercs.exe"));
}

std::wstring ExeDir() {
    wchar_t buffer[MAX_PATH] = {};
    DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return std::wstring();
    }
    std::wstring path = buffer;
    size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

std::wstring FindGameDir() {
    const std::wstring exe = ExeDir();
    const std::wstring folder = L"Mechwarrior Mercenaries - Mektek Mekpak";
    std::vector<std::wstring> candidates = {exe, DefaultInstall()};
    // Repository layout: dist\mw4patch.exe next to MekTek\<game folder>.
    if (!exe.empty()) {
        candidates.push_back(Join(Join(Join(exe, L".."), L"MekTek"), folder));
    }
    wchar_t programs[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"ProgramFiles(x86)", programs, MAX_PATH)) {
        candidates.push_back(Join(Join(programs, L"MekTek.net"), folder));
    }
    if (GetEnvironmentVariableW(L"ProgramFiles", programs, MAX_PATH)) {
        candidates.push_back(Join(Join(programs, L"MekTek.net"), folder));
    }
    for (const std::wstring& candidate : candidates) {
        if (IsGameDir(candidate)) {
            return NormalizePath(candidate);
        }
    }
    return std::wstring();
}

std::wstring AskGameDir() {
    for (;;) {
        Print(L"Could not find MW4Mercs.exe automatically.\n");
        Print(L"Drag the game folder (or MW4Mercs.exe) into this window and press Enter,\n");
        Print(L"or press Enter on an empty line to quit:\n> ");
        wchar_t buffer[MAX_PATH * 2] = {};
        if (!fgetws(buffer, MAX_PATH * 2, stdin)) {
            return std::wstring();
        }
        std::wstring path = buffer;
        while (!path.empty() && (path.back() == L'\n' || path.back() == L'\r' ||
                                 path.back() == L' ' || path.back() == L'"')) {
            path.pop_back();
        }
        while (!path.empty() && (path.front() == L' ' || path.front() == L'"')) {
            path.erase(0, 1);
        }
        if (path.empty()) {
            return std::wstring();
        }
        const std::wstring exe_name = L"mw4mercs.exe";
        if (path.size() >= exe_name.size() &&
            _wcsicmp(path.c_str() + path.size() - exe_name.size(), exe_name.c_str()) == 0) {
            size_t slash = path.find_last_of(L"\\/");
            path = slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
        }
        if (IsGameDir(path)) {
            return NormalizePath(path);
        }
        Print(L"\nMW4Mercs.exe is not in: %ls\n\n", path.c_str());
    }
}

int RunInteractive() {
    Print(L"mw4patch %ls - MechWarrior 4: Mercenaries 3.1c compatibility patch\n\n",
          kToolVersion);
    std::wstring install = FindGameDir();
    if (install.empty()) {
        install = AskGameDir();
        if (install.empty()) {
            Print(L"Nothing done.\n");
            return 3;
        }
    }
    if (FileExists(Join(install, kRecordName))) {
        Print(L"The patch is already installed in:\n  %ls\n\n", install.c_str());
        return CommandUninstall(install, false);
    }
    return CommandInstall(install, false);
}

}  // namespace

#ifdef MW4PATCH_GUI
// ---------------------------------------------------------------------------
// GUI installer (MW4Mercs-WindowedPatch-Setup.exe). Same install/uninstall
// code as the command line build; this is only the window around it.
// ---------------------------------------------------------------------------
#include <commctrl.h>
#include <shellapi.h>
#include <shobjidl.h>

#include <thread>

namespace gui {

enum : int {
    kIdFolder = 100,
    kIdBrowse,
    kIdStatus,
    kIdWindowed,
    kIdDecay,
    kIdMw5,
    kIdLog,
    kIdInstall,
    kIdUninstall,
    kIdClose,
};
constexpr UINT kMsgLog = WM_APP + 1;   // lParam: heap string (free())
constexpr UINT kMsgDone = WM_APP + 2;  // wParam: result, lParam: 1 install, 2 uninstall
constexpr UINT_PTR kCheckTimer = 1;

HWND g_wnd = nullptr;
HWND g_folder = nullptr;
HWND g_status = nullptr;
HWND g_mw5 = nullptr;
HWND g_log = nullptr;
HWND g_install = nullptr;
HWND g_uninstall = nullptr;
HWND g_browse = nullptr;
HFONT g_font = nullptr;
HFONT g_title_font = nullptr;
HFONT g_mono = nullptr;
int g_dpi = 96;
bool g_busy = false;
bool g_supported = false;
bool g_installed = false;

int S(int v) { return MulDiv(v, g_dpi, 96); }

void Sink(const wchar_t* text) {
    std::wstring t;
    for (const wchar_t* p = text; *p; ++p) {
        if (*p == L'\n') {
            t += L"\r\n";
        } else {
            t += *p;
        }
    }
    PostMessageW(g_wnd, kMsgLog, 0, reinterpret_cast<LPARAM>(_wcsdup(t.c_str())));
}

std::wstring FolderText() {
    wchar_t buffer[MAX_PATH * 2] = {};
    GetWindowTextW(g_folder, buffer, MAX_PATH * 2);
    std::wstring path = buffer;
    while (!path.empty() && (path.back() == L' ' || path.back() == L'"')) {
        path.pop_back();
    }
    while (!path.empty() && (path.front() == L' ' || path.front() == L'"')) {
        path.erase(0, 1);
    }
    const std::wstring exe_name = L"mw4mercs.exe";
    if (path.size() >= exe_name.size() &&
        _wcsicmp(path.c_str() + path.size() - exe_name.size(), exe_name.c_str()) == 0) {
        size_t slash = path.find_last_of(L"\\/");
        path = slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
    }
    return path.empty() ? path : NormalizePath(path);
}

void UpdateState() {
    const std::wstring dir = FolderText();
    g_supported = false;
    g_installed = false;
    const wchar_t* status = L"";
    if (dir.empty()) {
        status = L"Choose the folder that contains MW4Mercs.exe.";
    } else if (!IsGameDir(dir)) {
        status = L"MW4Mercs.exe was not found in this folder.";
    } else {
        std::string hash;
        if (Sha256File(Join(dir, kGameExe), hash) && hash == kExpectedExeSha256) {
            g_supported = true;
            g_installed = FileExists(Join(dir, kRecordName));
            status = g_installed ? L"\x2714  MechWarrior 4: Mercenaries 3.1c - the patch is installed."
                                 : L"\x2714  MechWarrior 4: Mercenaries 3.1c found.";
        } else {
            status = L"This MW4Mercs.exe is not the supported 3.1c version (51.03.01.0030).";
        }
    }
    SetWindowTextW(g_status, status);
    if (g_installed) {
        SendMessageW(g_mw5, BM_SETCHECK, InstalledMw5(dir) == 1 ? BST_CHECKED : BST_UNCHECKED, 0);
    }
    SetWindowTextW(g_install, g_installed ? L"Update" : L"Install");
    EnableWindow(g_install, g_supported && !g_busy);
    EnableWindow(g_uninstall, g_installed && !g_busy);
    EnableWindow(g_browse, !g_busy);
    EnableWindow(g_folder, !g_busy);
    EnableWindow(g_mw5, !g_busy);
}

void Browse() {
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dialog)))) {
        return;
    }
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dialog->SetTitle(L"Select the MechWarrior 4: Mercenaries folder (contains MW4Mercs.exe)");
    if (SUCCEEDED(dialog->Show(g_wnd))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                SetWindowTextW(g_folder, path);
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dialog->Release();
    UpdateState();
}

// Folders under Program Files usually need administrator rights.
bool CanWrite(const std::wstring& dir) {
    const std::wstring probe = Join(dir, L"mw4patch-write-test.tmp");
    HANDLE file = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    CloseHandle(file);
    return true;
}

bool RestartAsAdmin(const std::wstring& dir) {
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring params = L"--dir \"" + dir + L"\"";
    if (SendMessageW(g_mw5, BM_GETCHECK, 0, 0) == BST_CHECKED) {
        params += L" --mw5";
    }
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.lpVerb = L"runas";
    info.lpFile = exe;
    info.lpParameters = params.c_str();
    info.nShow = SW_SHOWNORMAL;
    return ShellExecuteExW(&info) != FALSE;
}

void Run(int action) {
    const std::wstring dir = FolderText();
    if (!CanWrite(dir)) {
        if (MessageBoxW(g_wnd,
                        L"Windows does not allow changes in this folder without administrator "
                        L"rights.\n\nRestart the installer as administrator?",
                        L"Administrator rights needed", MB_YESNO | MB_ICONQUESTION) == IDYES &&
            RestartAsAdmin(dir)) {
            DestroyWindow(g_wnd);
        }
        return;
    }
    if (action == 2 &&
        MessageBoxW(g_wnd,
                    L"Remove the patch? The game's own files were never changed; your previous "
                    L"options.ini and shortcut are restored.",
                    L"Uninstall", MB_YESNO | MB_ICONQUESTION) != IDYES) {
        return;
    }
    const bool mw5 = SendMessageW(g_mw5, BM_GETCHECK, 0, 0) == BST_CHECKED;
    g_busy = true;
    UpdateState();
    SetWindowTextW(g_log, L"");
    std::thread([dir, mw5, action]() {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        const int result = action == 1 ? CommandInstall(dir, true, mw5 ? 1 : 0)
                                       : CommandUninstall(dir, true);
        CoUninitialize();
        PostMessageW(g_wnd, kMsgDone, static_cast<WPARAM>(result), action);
    }).detach();
}

void Finished(int result, int action) {
    g_busy = false;
    UpdateState();
    if (action == 2) {
        MessageBoxW(g_wnd,
                    result == 0 ? L"The patch was removed."
                                : L"Uninstall did not complete. See the log for details.",
                    L"Uninstall", MB_OK | (result == 0 ? MB_ICONINFORMATION : MB_ICONWARNING));
        return;
    }
    if (result != 0) {
        const wchar_t* why = result == 3 ? L"Close MechWarrior 4 and try again."
                           : result == 2 ? L"This is not an unmodified MechWarrior 4: "
                                           L"Mercenaries 3.1c install. Nothing was changed."
                                         : L"See the log for details. Nothing was left half "
                                           L"installed.";
        MessageBoxW(g_wnd, why, L"Installation failed", MB_OK | MB_ICONWARNING);
        return;
    }
    std::wstring text =
        L"The patch is installed. Start the game as usual.\n\n"
        L"\x2022 Windowed / borderless: Options > Video > Windowed (Alt+Enter switches)\n"
        L"\x2022 Throttle Decay: Options > Game (off until you turn it on)";
    if (SendMessageW(g_mw5, BM_GETCHECK, 0, 0) == BST_CHECKED) {
        text += L"\n\x2022 MechWarrior 5-style controls: new pilots get them automatically; "
                L"for an existing pilot press DEFAULT in Options > Controls, then OK";
    }
    MessageBoxW(g_wnd, text.c_str(), L"Installed", MB_OK | MB_ICONINFORMATION);
}

HWND Child(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h,
           int id, HFONT font = nullptr) {
    HWND hwnd = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w),
                                S(h), g_wnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                GetModuleHandleW(nullptr), nullptr);
    SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(font ? font : g_font), TRUE);
    return hwnd;
}

void CreateControls(const std::wstring& initial_dir, bool initial_mw5) {
    std::wstring title = std::wstring(L"MechWarrior 4: Mercenaries \x2014 Windowed Patch ") +
                         kToolVersion;
    Child(L"STATIC", title.c_str(), 0, 16, 14, 580, 26, 0, g_title_font);
    Child(L"STATIC",
          L"For MechWarrior 4: Mercenaries 3.1c (MekTek). No game file is changed; uninstall "
          L"puts everything back.",
          0, 16, 44, 580, 20, 0);

    Child(L"STATIC", L"Game folder (contains MW4Mercs.exe):", 0, 16, 76, 400, 18, 0);
    g_folder = Child(L"EDIT", initial_dir.c_str(), WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, 16,
                     96, 480, 24, kIdFolder);
    g_browse = Child(L"BUTTON", L"Browse...", WS_TABSTOP | BS_PUSHBUTTON, 504, 95, 92, 26,
                     kIdBrowse);
    g_status = Child(L"STATIC", L"", 0, 16, 126, 580, 20, kIdStatus);

    Child(L"BUTTON", L"Components", BS_GROUPBOX, 16, 154, 580, 212, 0);
    HWND windowed = Child(L"BUTTON", L"Windowed mode and display fixes (required)",
                          BS_AUTOCHECKBOX, 30, 176, 550, 20, kIdWindowed);
    SendMessageW(windowed, BM_SETCHECK, BST_CHECKED, 0);
    EnableWindow(windowed, FALSE);
    Child(L"STATIC",
          L"Windowed checkbox in Options > Video (framed window or borderless fullscreen, "
          L"Alt+Enter switches); frame rate, audio, terrain and second-monitor fixes.",
          0, 50, 197, 530, 32, 0);
    HWND decay = Child(L"BUTTON", L"Throttle Decay setting (included)", BS_AUTOCHECKBOX, 30,
                       236, 550, 20, kIdDecay);
    SendMessageW(decay, BM_SETCHECK, BST_CHECKED, 0);
    EnableWindow(decay, FALSE);
    Child(L"STATIC",
          L"Adds a Throttle Decay On/Off switch to Options > Game. Off until you turn it on.", 0,
          50, 257, 530, 18, 0);
    g_mw5 = Child(L"BUTTON", L"MechWarrior 5-style controls (optional)",
                  WS_TABSTOP | BS_AUTOCHECKBOX, 30, 284, 550, 20, kIdMw5);
    SendMessageW(g_mw5, BM_SETCHECK, initial_mw5 ? BST_CHECKED : BST_UNCHECKED, 0);
    Child(L"STATIC",
          L"MW5-style key layout (X full stop, P power, numpad throttle presets), faster W/S "
          L"throttle, and mouse aiming with an aim point the torso turns to. Leave unticked "
          L"to keep MW4's controls.",
          0, 50, 305, 530, 48, 0);

    g_log = Child(L"EDIT", L"",
                  WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, 16, 376,
                  580, 112, kIdLog, g_mono);
    g_install = Child(L"BUTTON", L"Install", WS_TABSTOP | BS_DEFPUSHBUTTON, 288, 500, 96, 30,
                      kIdInstall);
    g_uninstall =
        Child(L"BUTTON", L"Uninstall", WS_TABSTOP | BS_PUSHBUTTON, 394, 500, 96, 30, kIdUninstall);
    Child(L"BUTTON", L"Close", WS_TABSTOP | BS_PUSHBUTTON, 500, 500, 96, 30, kIdClose);
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
        case WM_COMMAND:
            switch (LOWORD(wparam)) {
                case kIdBrowse:
                    Browse();
                    return 0;
                case kIdInstall:
                    Run(1);
                    return 0;
                case kIdUninstall:
                    Run(2);
                    return 0;
                case kIdClose:
                    if (!g_busy) {
                        DestroyWindow(hwnd);
                    }
                    return 0;
                case kIdFolder:
                    if (HIWORD(wparam) == EN_CHANGE) {
                        SetTimer(hwnd, kCheckTimer, 400, nullptr);
                    }
                    return 0;
            }
            break;
        case WM_TIMER:
            if (wparam == kCheckTimer) {
                KillTimer(hwnd, kCheckTimer);
                UpdateState();
            }
            return 0;
        case kMsgLog: {
            wchar_t* text = reinterpret_cast<wchar_t*>(lparam);
            const int length = GetWindowTextLengthW(g_log);
            SendMessageW(g_log, EM_SETSEL, length, length);
            SendMessageW(g_log, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(text));
            free(text);
            return 0;
        }
        case kMsgDone:
            Finished(static_cast<int>(wparam), static_cast<int>(lparam));
            return 0;
        case WM_CTLCOLORSTATIC:
            if (reinterpret_cast<HWND>(lparam) != g_log) {
                SetBkMode(reinterpret_cast<HDC>(wparam), TRANSPARENT);
                return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
            }
            break;
        case WM_CLOSE:
            if (!g_busy) {
                DestroyWindow(hwnd);
            }
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

}  // namespace gui

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    SetProcessDPIAware();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);

    // --dir "<folder>" and --mw5 are passed when restarting as administrator.
    std::wstring dir;
    bool mw5 = false;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; argv && i < argc; ++i) {
        if (_wcsicmp(argv[i], L"--dir") == 0 && i + 1 < argc) {
            dir = argv[++i];
        } else if (_wcsicmp(argv[i], L"--mw5") == 0) {
            mw5 = true;
        }
    }
    if (argv) {
        LocalFree(argv);
    }
    if (dir.empty()) {
        dir = FindGameDir();
        if (!dir.empty()) {
            mw5 = InstalledMw5(dir) == 1;
        }
    }

    HDC screen = GetDC(nullptr);
    gui::g_dpi = GetDeviceCaps(screen, LOGPIXELSY);
    ReleaseDC(nullptr, screen);
    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof(metrics);
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0);
    gui::g_font = CreateFontIndirectW(&metrics.lfMessageFont);
    LOGFONTW title = metrics.lfMessageFont;
    title.lfHeight = -MulDiv(14, gui::g_dpi, 72);
    title.lfWeight = FW_SEMIBOLD;
    gui::g_title_font = CreateFontIndirectW(&title);
    LOGFONTW mono = metrics.lfMessageFont;
    wcscpy_s(mono.lfFaceName, L"Consolas");
    gui::g_mono = CreateFontIndirectW(&mono);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = gui::WindowProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.lpszClassName = L"MW4WindowedPatchSetup";
    RegisterClassExW(&wc);

    RECT rect = {0, 0, gui::S(612), gui::S(546)};
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRect(&rect, style, FALSE);
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    gui::g_wnd = CreateWindowExW(0, wc.lpszClassName, L"MechWarrior 4: Mercenaries Windowed Patch",
                                 style, (GetSystemMetrics(SM_CXSCREEN) - width) / 2,
                                 (GetSystemMetrics(SM_CYSCREEN) - height) / 2, width, height,
                                 nullptr, nullptr, instance, nullptr);
    g_output_sink = gui::Sink;
    gui::CreateControls(dir, mw5);
    gui::UpdateState();
    ShowWindow(gui::g_wnd, show);
    UpdateWindow(gui::g_wnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(gui::g_wnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    CoUninitialize();
    return 0;
}
#else

int wmain(int argc, wchar_t** argv) {
    if (argc == 1) {
        SetConsoleOutputCP(CP_UTF8);
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        const bool owns_console = OwnsConsole();
        int result = RunInteractive();
        CoUninitialize();
        if (owns_console) {
            WaitForEnter();
        }
        return result;
    }

    SetConsoleOutputCP(CP_UTF8);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    std::wstring command = L"install";
    std::wstring install;
    bool assumeYes = false;
    int mw5 = -1;

    for (int i = 1; i < argc; ++i) {
        std::wstring arg = argv[i];
        if (arg == L"--yes" || arg == L"-y") {
            assumeYes = true;
        } else if (arg == L"--mw5") {
            mw5 = 1;
        } else if (arg == L"--no-mw5") {
            mw5 = 0;
        } else if (arg == L"--help" || arg == L"-h" || arg == L"/?") {
            PrintUsage();
            CoUninitialize();
            return 0;
        } else if (arg == L"install" || arg == L"uninstall" ||
                   arg == L"verify" || arg == L"info") {
            command = arg;
        } else if (!arg.empty() && arg[0] == L'-') {
            PrintError(L"error: unknown option: %ls\n", arg.c_str());
            PrintUsage();
            CoUninitialize();
            return 1;
        } else {
            install = arg;
        }
    }

    if (install.empty()) {
        install = DefaultInstall();
    }
    install = NormalizePath(install);

    if (command == L"install") {
        int result = CommandInstall(install, assumeYes, mw5);
        CoUninitialize();
        return result;
    }
    if (command == L"uninstall") {
        int result = CommandUninstall(install, assumeYes);
        CoUninitialize();
        return result;
    }
    if (command == L"verify") {
        int result = CommandVerify(install);
        CoUninitialize();
        return result;
    }
    if (command == L"info") {
        int result = CommandInfo(install);
        CoUninitialize();
        return result;
    }

    PrintUsage();
    CoUninitialize();
    return 1;
}
#endif  // MW4PATCH_GUI
