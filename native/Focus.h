#pragma once
#include <windows.h>

HWND FocusWindow(HWND foreground);
bool IsTextInputHost(DWORD pid);
inline constexpr UINT validateTapMessage = WM_APP + 5;
inline constexpr UINT focusSnapshotMessage = WM_APP + 6;
int CheckTap(HWND owner, UINT_PTR ticket, HWND foreground, HWND focus, POINT down, POINT up,
    UINT_PTR previousFocus);
int WatchFocus(HWND owner, HWND foreground, DWORD generation);
int OpenKeyboard();
