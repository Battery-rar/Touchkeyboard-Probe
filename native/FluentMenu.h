#pragma once
#include <windows.h>
enum Command : UINT { openKeyboard = 1, hideTray, showTray, exitApp, keyboardSettings, personalization };
inline constexpr wchar_t menuWindowClass[] = L"TouchKeyboardProbe.MenuHost";
inline constexpr UINT menuClosingMessage = WM_APP + 8;

int RunMenu(POINT anchor, bool above, bool hidden, HWND owner);
