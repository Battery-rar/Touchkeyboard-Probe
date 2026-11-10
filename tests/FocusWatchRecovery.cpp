#include <windows.h>
#include <dwmapi.h>
#include <array>
#include <cstddef>
#include <cwchar>
#include <optional>
#include <unordered_map>
#include <vector>
#include "../native/Focus.h"

namespace {
const HWND testWindow = reinterpret_cast<HWND>(1);
const HWND testFocus = reinterpret_cast<HWND>(2);
const HWND trayWindow = reinterpret_cast<HWND>(4);
constexpr DWORD watcherThread = 10;
HWND foreground = testWindow;
unsigned refreshes = 0;
unsigned restores = 0;
DWORD cloaked = 0;

HWND TestForegroundWindow() { return foreground; }
HWND TestAncestor(HWND window, UINT) { return window == testFocus ? testWindow : window; }
HWND TestWindowFromPoint(POINT) { return testWindow; }
int TestClassName(HWND window, LPWSTR name, int count) {
    const auto cls = window == trayWindow ? L"Shell_TrayWnd" : L"ApplicationFrameWindow";
    return lstrlenW(lstrcpynW(name, cls, count));
}
BOOL TestSetForegroundWindow(HWND window) {
    foreground = window;
    ++restores;
    return TRUE;
}
BOOL TestIsWindowVisible(HWND window) { return window != nullptr; }
BOOL TestIsIconic(HWND) { return FALSE; }
UINT_PTR TestSetTimer(HWND, UINT_PTR, UINT, TIMERPROC) { return 0; }
HRESULT WINAPI TestDwmAttribute(HWND, DWORD, PVOID result, DWORD) {
    *static_cast<DWORD*>(result) = cloaked;
    return S_OK;
}
FARPROC TestGetProcAddress(HMODULE module, LPCSTR name) {
    if (!lstrcmpA(name, "DwmGetWindowAttribute")) return reinterpret_cast<FARPROC>(TestDwmAttribute);
    return GetProcAddress(module, name);
}
int TestSystemMetrics(int index) { return index == SM_CONVERTIBLESLATEMODE ? 0 : GetSystemMetrics(index); }
BOOL TestPostThreadMessage(DWORD thread, UINT message, WPARAM wp, LPARAM) {
    if (thread == watcherThread && message == readFocusMessage && wp) ++refreshes;
    return TRUE;
}
}

#define GetForegroundWindow TestForegroundWindow
#define GetAncestor TestAncestor
#define WindowFromPoint TestWindowFromPoint
#define GetClassNameW TestClassName
#define GetSystemMetrics TestSystemMetrics
#define PostThreadMessageW TestPostThreadMessage
#define SetForegroundWindow TestSetForegroundWindow
#define IsWindowVisible TestIsWindowVisible
#define IsIconic TestIsIconic
#define SetTimer TestSetTimer
#define GetProcAddress TestGetProcAddress
#define private public
#include "../native/Tray.cpp"
#undef private

HWND FocusWindow(HWND window) { return window == foreground ? testFocus : nullptr; }
bool IsTextInputHost(DWORD) { return false; }
int OpenKeyboard() { return 0; }
int WatchFocus(HWND, HWND, DWORD) { return 0; }
int CheckTap(HWND, UINT_PTR, HWND, HWND, POINT, POINT, UINT_PTR) { return 0; }
int RunMenu(POINT, bool, bool, HWND) { return 0; }

int main() {
    App app;
    const auto running = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!running) return 1;
    app.slate_ = true;
    app.watch_.process = running;
    app.watch_.window = testWindow;
    app.watch_.generation = 7;
    app.watch_.revision = 1;
    app.watch_.identity = 9;
    app.watch_.changed = 1;
    app.watch_.thread = watcherThread;

    // The foreground notification can reach the tray after the same window is already back.
    app.OnMessage(nullptr, foregroundMessage, reinterpret_cast<WPARAM>(testWindow), TRUE);
    if (refreshes != 1 || app.watch_.identity != 9) return 2;
    app.OnMessage(nullptr, foregroundMessage, reinterpret_cast<WPARAM>(testWindow), 0);
    if (refreshes != 1) return 3;

    const auto device = reinterpret_cast<HANDLE>(1);
    app.BeginTap(device, {50, 50}, GetTickCount64() + 1);
    app.contactDevice_ = nullptr;
    const auto ticket = app.tapTicket_;
    app.OnMessage(nullptr, focusSnapshotMismatchMessage, ticket - 1, 0);
    if (refreshes != 1) return 4;
    foreground = reinterpret_cast<HWND>(3);
    app.OnMessage(nullptr, focusSnapshotMismatchMessage, ticket, 0);
    if (refreshes != 1) return 9;
    foreground = testWindow;
    app.OnMessage(nullptr, focusSnapshotMismatchMessage, ticket, 0);
    if (refreshes != 2 || app.watch_.identity != 9) return 5;

    // Publishing the same identity again must not invalidate an already focused tap.
    app.OnMessage(nullptr, focusSnapshotMessage, (UINT_PTR(7) << 32) | 2, 9);
    app.OnMessage(nullptr, focusSnapshotMessage, (UINT_PTR(6) << 32) | 3, 12);
    if (!app.ValidateTap() || app.watch_.identity != 9) return 10;

    // A refresh response repairs future taps, never the request carrying the old identity.
    app.OnMessage(nullptr, focusSnapshotMessage, (UINT_PTR(7) << 32) | 3, 12);
    if (app.ValidateTap() || app.watch_.identity != 12) return 6;
    app.BeginTap(device, {50, 50}, GetTickCount64() + 1);
    if (!app.ValidateTap() || app.contactIdentity_ != 12) return 7;

    // An empty snapshot can otherwise prevent all tap helpers from ever running.
    app.OnMessage(nullptr, focusSnapshotMessage, (UINT_PTR(7) << 32) | 4, 0);
    app.BeginTap(device, {50, 50}, GetTickCount64() + 1);
    if (refreshes != 3 || app.contactDevice_) return 8;
    CloseHandle(running);
    app.watch_ = {};
    app.slate_ = false;

    // Shell activation can put the taskbar in the foreground before it sends NIN_SELECT.
    app.OnMessage(nullptr, foregroundMessage, reinterpret_cast<WPARAM>(testWindow), TRUE);
    foreground = trayWindow;
    app.OnMessage(nullptr, foregroundMessage, reinterpret_cast<WPARAM>(trayWindow), TRUE);
    app.OnMessage(nullptr, trayMessage, 0, NIN_SELECT);
    if (foreground != testWindow || restores != 1) return 11;

    // Keep a foreground app in place, and never switch to a cloaked window.
    foreground = reinterpret_cast<HWND>(5);
    app.OnMessage(nullptr, trayMessage, 0, NIN_SELECT);
    if (restores != 1) return 12;
    foreground = trayWindow;
    cloaked = DWM_CLOAKED_SHELL;
    app.OnMessage(nullptr, trayMessage, 0, NIN_SELECT);
    if (foreground != trayWindow || restores != 1) return 13;
    return 0;
}
