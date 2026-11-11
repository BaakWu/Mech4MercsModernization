#include "hook.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace mw4hook {
namespace {

HANDLE g_log_file = INVALID_HANDLE_VALUE;
CRITICAL_SECTION g_log_lock;
Config g_config;

void BuildLogPath(wchar_t* out, size_t cch) {
    DWORD len = GetModuleFileNameW(nullptr, out, static_cast<DWORD>(cch));
    if (len == 0 || len >= cch) {
        out[0] = L'\0';
        return;
    }
    wchar_t* slash = wcsrchr(out, L'\\');
    if (!slash) {
        out[0] = L'\0';
        return;
    }
    slash[1] = L'\0';
    wchar_t name[64];
    wsprintfW(name, L"mw4hook-%lu.log", GetCurrentProcessId());
    wcscat_s(out, cch, name);
}

void LoadConfig() {
    wchar_t path[MAX_PATH];
    DWORD len = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        return;
    }
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash) {
        return;
    }
    slash[1] = L'\0';
    wcscat_s(path, MAX_PATH, L"mw4hook.ini");

    g_config.window_width = static_cast<int>(GetPrivateProfileIntW(
        L"window", L"width", g_config.window_width, path));
    g_config.window_height = static_cast<int>(GetPrivateProfileIntW(
        L"window", L"height", g_config.window_height, path));
    g_config.window_enabled =
        GetPrivateProfileIntW(L"window", L"enabled", 1, path) != 0;
    g_config.hooks_enabled =
        GetPrivateProfileIntW(L"hook", L"enabled", 1, path) != 0;
    g_config.resolution_enabled =
        GetPrivateProfileIntW(L"resolution", L"enabled", 1, path) != 0;
    g_config.resolution_width = static_cast<int>(GetPrivateProfileIntW(
        L"resolution", L"width", g_config.resolution_width, path));
    g_config.resolution_height = static_cast<int>(GetPrivateProfileIntW(
        L"resolution", L"height", g_config.resolution_height, path));
    g_config.log_enabled = GetPrivateProfileIntW(L"log", L"enabled", 1, path) != 0;
}

}  // namespace

void LogInit(HINSTANCE /*self*/) {
    InitializeCriticalSection(&g_log_lock);
    LoadConfig();

    if (!g_config.log_enabled) {
        return;
    }

    wchar_t path[MAX_PATH];
    BuildLogPath(path, MAX_PATH);
    if (!path[0]) {
        return;
    }

    g_log_file = CreateFileW(path, FILE_APPEND_DATA,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_log_file == INVALID_HANDLE_VALUE) {
        return;
    }

    SYSTEMTIME now;
    GetLocalTime(&now);
    char header[128];
    int n = wsprintfA(header, "mw4hook log %04u-%02u-%02u %02u:%02u:%02u pid=%lu\r\n",
                      now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute,
                      now.wSecond, GetCurrentProcessId());
    DWORD written = 0;
    WriteFile(g_log_file, header, static_cast<DWORD>(n), &written, nullptr);

    wchar_t exe_path[MAX_PATH];
    GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    Log("exe: %ls\n", exe_path);
    Log("cmdline: %ls\n", GetCommandLineW());
}

void Log(const char* fmt, ...) {
    if (g_log_file == INVALID_HANDLE_VALUE) {
        return;
    }

    char buffer[2048];
    va_list args;
    va_start(args, fmt);
    int n = _vsnprintf_s(buffer, sizeof(buffer), _TRUNCATE, fmt, args);
    va_end(args);
    if (n <= 0) {
        return;
    }

    EnterCriticalSection(&g_log_lock);
    DWORD written = 0;
    WriteFile(g_log_file, buffer, static_cast<DWORD>(n), &written, nullptr);
    LeaveCriticalSection(&g_log_lock);
}

const Config& GetConfig() { return g_config; }

}  // namespace mw4hook
