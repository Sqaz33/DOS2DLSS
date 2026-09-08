#include "dos2dlss/shared_state.hpp"

#include <bcrypt.h>
#include <windows.h>
#include <winver.h>

#include <array>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "version.lib")

namespace
{
constexpr wchar_t kExpectedVersion[] = L"3.6.117.3735";
constexpr wchar_t kExpectedSha256[] = L"1D14A07CB7F22EBA64559789F826C9FC3A6C54C420FE6F1C8532316B5E33A33D";

HMODULE g_module = nullptr;
dos2dlss::SharedMapping g_mapping;
wchar_t g_log_path[MAX_PATH] = {};

void log_line(const wchar_t *format, ...)
{
    FILE *file = nullptr;
    if (_wfopen_s(&file, g_log_path, L"a, ccs=UTF-8") != 0 || file == nullptr)
        return;

    SYSTEMTIME time = {};
    GetLocalTime(&time);
    fwprintf(file, L"[%02u:%02u:%02u.%03u] ", time.wHour, time.wMinute, time.wSecond, time.wMilliseconds);

    va_list args;
    va_start(args, format);
    vfwprintf(file, format, args);
    va_end(args);
    fputws(L"\n", file);
    fclose(file);
}

std::wstring file_version(const wchar_t *path)
{
    DWORD ignored = 0;
    const DWORD bytes = GetFileVersionInfoSizeW(path, &ignored);
    if (bytes == 0)
        return L"unknown";

    std::vector<std::byte> data(bytes);
    if (!GetFileVersionInfoW(path, 0, bytes, data.data()))
        return L"unknown";

    VS_FIXEDFILEINFO *info = nullptr;
    UINT info_size = 0;
    if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<void **>(&info), &info_size) ||
        info == nullptr || info_size < sizeof(VS_FIXEDFILEINFO))
        return L"unknown";

    wchar_t text[32] = {};
    swprintf_s(text, L"%u.%u.%u.%u",
               HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS),
               HIWORD(info->dwFileVersionLS), LOWORD(info->dwFileVersionLS));
    return text;
}

bool sha256_file(const wchar_t *path, std::wstring &hex)
{
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    HANDLE file = INVALID_HANDLE_VALUE;
    std::vector<unsigned char> object;
    std::array<unsigned char, 32> digest = {};
    DWORD object_size = 0;
    DWORD copied = 0;
    bool ok = false;

    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        goto done;

    if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                          reinterpret_cast<PUCHAR>(&object_size), sizeof(object_size), &copied, 0) < 0)
        goto done;
    object.resize(object_size);
    if (BCryptCreateHash(algorithm, &hash, object.data(), object_size, nullptr, 0, 0) < 0)
        goto done;

    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        goto done;

    {
        std::array<unsigned char, 1024 * 1024> buffer = {};
        DWORD read = 0;
        while (ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) && read != 0)
            if (BCryptHashData(hash, buffer.data(), read, 0) < 0)
                goto done;
    }

    if (BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0)
        goto done;

    {
        wchar_t text[65] = {};
        for (std::size_t i = 0; i < digest.size(); ++i)
            swprintf_s(text + i * 2, 3, L"%02X", digest[i]);
        hex.assign(text);
    }
    ok = true;

done:
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    if (hash != nullptr) BCryptDestroyHash(hash);
    if (algorithm != nullptr) BCryptCloseAlgorithmProvider(algorithm, 0);
    return ok;
}

DWORD WINAPI initialize(LPVOID)
{
    wchar_t module_path[MAX_PATH] = {};
    GetModuleFileNameW(g_module, module_path, MAX_PATH);
    wcscpy_s(g_log_path, module_path);
    if (wchar_t *slash = wcsrchr(g_log_path, L'\\'))
        wcscpy_s(slash + 1, MAX_PATH - static_cast<size_t>(slash + 1 - g_log_path), L"DOS2DLSSNative.log");

    FILE *clear = nullptr;
    if (_wfopen_s(&clear, g_log_path, L"w, ccs=UTF-8") == 0 && clear != nullptr)
        fclose(clear);
    log_line(L"DOS2DLSSNative 0.1.0 starting");

    if (!g_mapping.open_or_create())
    {
        log_line(L"Shared state mapping failed: Windows error %lu", GetLastError());
        return 0;
    }

    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const std::wstring version = file_version(exe);
    std::wstring sha;
    const bool hash_ok = sha256_file(exe, sha);
    const bool exact = version == kExpectedVersion && hash_ok && _wcsicmp(sha.c_str(), kExpectedSha256) == 0;

    auto *state = g_mapping.get();
    wcscpy_s(state->game_version, version.c_str());
    wcscpy_s(state->game_sha256, hash_ok ? sha.c_str() : L"unavailable");
    InterlockedExchange(&state->exact_game_build, exact ? 1 : 0);
    InterlockedExchange(&state->native_ready, 1);
    wcscpy_s(state->native_status,
             exact ? L"Game build verified; render hooks are not enabled in probe 0.1.0."
                   : L"Unknown game build; all future render hooks will remain disabled.");

    log_line(L"Host: %ls", exe);
    log_line(L"Version: %ls", version.c_str());
    log_line(L"SHA-256: %ls", hash_ok ? sha.c_str() : L"unavailable");
    log_line(L"Build verdict: %ls", exact ? L"exact supported build" : L"unsupported build");
    log_line(L"Probe initialized; this build does not patch game code.");
    return 0;
}
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_module = module;
        DisableThreadLibraryCalls(module);
        if (HANDLE thread = CreateThread(nullptr, 0, initialize, nullptr, 0, nullptr))
            CloseHandle(thread);
    }
    else if (reason == DLL_PROCESS_DETACH && reserved == nullptr)
    {
        if (auto *state = g_mapping.get())
            InterlockedExchange(&state->native_ready, 0);
        g_mapping.close();
    }
    return TRUE;
}
