#include "core/common.h"

#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cwchar>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace radial_menu_mod {
namespace {

constexpr std::size_t kMaxQueuedLines = 1024;

std::mutex g_log_mutex;
std::condition_variable g_log_cv;
std::deque<std::string> g_log_queue;
std::thread g_log_thread;
bool g_log_started = false;
bool g_log_stopping = false;
std::uint64_t g_dropped_lines = 0;

std::wstring ResolveLogPath()
{
    HMODULE module = nullptr;
    GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&Log),
        &module);

    wchar_t module_path[MAX_PATH] = {};
    if (!GetModuleFileNameW(module, module_path, MAX_PATH)) return L"RadialMenu.log";

    wchar_t* sep = std::wcsrchr(module_path, L'\\');
    if (!sep) return L"RadialMenu.log";

    *(sep + 1) = L'\0';
    std::wstring log_path(module_path);
    log_path += L"RadialMenu.log";
    return log_path;
}

void WriteLine(HANDLE file, const std::string& line)
{
    if (file == INVALID_HANDLE_VALUE) return;

    DWORD written = 0;
    WriteFile(file, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    WriteFile(file, "\r\n", 2, &written, nullptr);
}

void LogWorker()
{
    const std::wstring log_path = ResolveLogPath();
    HANDLE file = CreateFileW(log_path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

    std::vector<std::string> batch;
    batch.reserve(64);

    for (;;) {
        std::uint64_t dropped = 0;
        {
            std::unique_lock lock(g_log_mutex);
            g_log_cv.wait(lock, [] { return g_log_stopping || !g_log_queue.empty(); });

            if (g_log_queue.empty() && g_log_stopping) break;

            dropped = g_dropped_lines;
            g_dropped_lines = 0;
            while (!g_log_queue.empty()) {
                batch.push_back(std::move(g_log_queue.front()));
                g_log_queue.pop_front();
            }
        }

        if (dropped != 0) {
            char message[128] = {};
            std::snprintf(message, sizeof(message), "Log queue dropped %llu older messages.",
                static_cast<unsigned long long>(dropped));
            WriteLine(file, message);
        }

        for (const std::string& line : batch) WriteLine(file, line);
        batch.clear();
        if (file != INVALID_HANDLE_VALUE) FlushFileBuffers(file);
    }

    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
}

void EnsureLogThreadStarted()
{
    if (g_log_started || g_log_stopping) return;
    g_log_thread = std::thread(LogWorker);
    g_log_started = true;
}

void QueueLogLine(std::string line)
{
    std::lock_guard lock(g_log_mutex);
    if (g_log_stopping) return;

    EnsureLogThreadStarted();
    if (!g_log_started) return;

    if (g_log_queue.size() >= kMaxQueuedLines) {
        g_log_queue.pop_front();
        ++g_dropped_lines;
    }
    g_log_queue.push_back(std::move(line));
    g_log_cv.notify_one();
}

std::string FormatTimestampedLine(const char* message)
{
    SYSTEMTIME now{};
    GetLocalTime(&now);

    char line[1200] = {};
    std::snprintf(line, sizeof(line), "[%02u:%02u:%02u.%03u] %s",
        static_cast<unsigned>(now.wHour),
        static_cast<unsigned>(now.wMinute),
        static_cast<unsigned>(now.wSecond),
        static_cast<unsigned>(now.wMilliseconds),
        message);
    return line;
}

}  // namespace

void Log(const char* format, ...)
{
    char buffer[1024] = {};
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    std::string line = FormatTimestampedLine(buffer);

    OutputDebugStringA("[RadialMenu] ");
    OutputDebugStringA(line.c_str());
    OutputDebugStringA("\n");

    QueueLogLine(std::move(line));
}

void ShutdownLog()
{
    {
        std::lock_guard lock(g_log_mutex);
        if (!g_log_started || g_log_stopping) return;
        g_log_stopping = true;
    }

    g_log_cv.notify_one();
    if (g_log_thread.joinable()) g_log_thread.join();
}

}  // namespace radial_menu_mod
