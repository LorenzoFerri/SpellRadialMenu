#pragma once

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cwchar>

namespace radial_spell_menu {

inline void LogToFileAdjacentToAddress(const void* address, const wchar_t* filename, const char* format, ...);

inline void Log(const char* format, ...)
{
    char buffer[1024] = {};

    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    OutputDebugStringA("[RadialSpellMenu] ");
    OutputDebugStringA(buffer);
    OutputDebugStringA("\n");

    LogToFileAdjacentToAddress(reinterpret_cast<const void*>(&Log), L"RadialSpellMenu.log", "%s\n", buffer);
}

inline void LogToFileAdjacentToAddress(const void* address, const wchar_t* filename, const char* format, ...)
{
    wchar_t log_path[MAX_PATH] = {};
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            static_cast<LPCWSTR>(address),
            &module)) {
        return;
    }

    wchar_t module_path[MAX_PATH] = {};
    if (GetModuleFileNameW(module, module_path, static_cast<DWORD>(sizeof(module_path) / sizeof(module_path[0]))) == 0) {
        return;
    }

    wchar_t* last_separator = wcsrchr(module_path, L'\\');
    if (last_separator == nullptr) {
        return;
    }

    *(last_separator + 1) = L'\0';

    if (swprintf(log_path, sizeof(log_path) / sizeof(log_path[0]), L"%ls%ls", module_path, filename) < 0) {
        return;
    }

    char buffer[2048] = {};
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    HANDLE file = CreateFileW(
        log_path,
        FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }

    const DWORD length = static_cast<DWORD>(strnlen(buffer, sizeof(buffer)));
    DWORD bytes_written = 0;
    WriteFile(file, buffer, length, &bytes_written, nullptr);
    WriteFile(file, "\r\n", 2, &bytes_written, nullptr);
    CloseHandle(file);
}

inline bool BuildAdjacentPathFromAddress(const void* address, const wchar_t* filename, wchar_t* output, std::size_t output_count)
{
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            static_cast<LPCWSTR>(address),
            &module)) {
        return false;
    }

    wchar_t module_path[MAX_PATH] = {};
    if (GetModuleFileNameW(module, module_path, static_cast<DWORD>(sizeof(module_path) / sizeof(module_path[0]))) == 0) {
        return false;
    }

    wchar_t* last_separator = wcsrchr(module_path, L'\\');
    if (last_separator == nullptr) {
        return false;
    }

    *(last_separator + 1) = L'\0';
    return swprintf(output, output_count, L"%ls%ls", module_path, filename) >= 0;
}

template <typename T>
inline void SafeRelease(T*& pointer)
{
    if (pointer != nullptr) {
        pointer->Release();
        pointer = nullptr;
    }
}

}  // namespace radial_spell_menu
