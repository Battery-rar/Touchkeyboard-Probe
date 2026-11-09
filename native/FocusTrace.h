#pragma once
#ifdef FOCUS_TRACE
#include <windows.h>
#include <cstdarg>
#include <cstdio>
inline void FocusTrace(const char* format, ...) {
    wchar_t path[4096];
    const auto size = GetEnvironmentVariableW(L"TOUCHKEYBOARD_TRACE_PATH", path, ARRAYSIZE(path));
    if (!size || size >= ARRAYSIZE(path)) return;
    char line[512];
    auto used = snprintf(line, sizeof(line), "%llu %lu ", GetTickCount64(), GetCurrentProcessId());
    if (used < 0 || used >= int(sizeof(line) - 2)) return;
    va_list args;
    va_start(args, format);
    const int count = vsnprintf(line + used, sizeof(line) - used - 1, format, args);
    va_end(args);
    used += count < 0 ? 0 : (count > int(sizeof(line) - used - 2) ? int(sizeof(line) - used - 2) : count);
    line[used++] = '\n';
    // One writer at a time keeps the size check and append within the same limit.
    auto file = CreateFileW(path, FILE_APPEND_DATA | FILE_READ_ATTRIBUTES, FILE_SHARE_READ, nullptr,
        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER sizeOnDisk{};
    constexpr LONGLONG maxBytes = 1024 * 1024;
    if (GetFileSizeEx(file, &sizeOnDisk) && sizeOnDisk.QuadPart <= maxBytes - used) {
        DWORD written = 0;
        WriteFile(file, line, DWORD(used), &written, nullptr);
    }
    CloseHandle(file);
}
#else
#define FocusTrace(...) ((void)0)
#endif
