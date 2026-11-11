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

constexpr const wchar_t* kToolVersion = L"0.1.0";
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

void Print(const wchar_t* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vwprintf(fmt, args);
    va_end(args);
    fflush(stdout);
}

void PrintError(const wchar_t* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vfwprintf(stderr, fmt, args);
    va_end(args);
    fflush(stderr);
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
        {"bitdepth", "32"}};
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

int CommandInstall(const std::wstring& install, bool assumeYes) {
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

    Print(L"\nPlan:\n");
    Print(L"  - %ls display keys -> 1024x768, 32-bit\n",
          existingOptions ? L"patch" : L"create");
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
    if (!PatchOptionsIni(install, error)) {
        PrintError(L"error: %ls\n", error.c_str());
        Rollback(install, state);
        return 1;
    }
    state.optionsPatched = true;

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
    removeIfMatches(iniPath, recordIniHash);

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
    Print(L"  mw4patch.exe install   [install-dir] [--yes]\n");
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

}  // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    std::wstring command = L"install";
    std::wstring install;
    bool assumeYes = false;

    for (int i = 1; i < argc; ++i) {
        std::wstring arg = argv[i];
        if (arg == L"--yes" || arg == L"-y") {
            assumeYes = true;
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
        int result = CommandInstall(install, assumeYes);
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
