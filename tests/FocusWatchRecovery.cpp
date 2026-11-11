#include <windows.h>
#include <dwmapi.h>
#include <array>
#include <cstddef>
#include <cwchar>
#include <optional>
#include <unordered_map>
#include <vector>
#include <wtsapi32.h>
#include "../native/Focus.h"
#include "../native/TouchInput.h"

namespace {
const HWND testWindow = reinterpret_cast<HWND>(1);
const HWND testFocus = reinterpret_cast<HWND>(2);
const HWND trayWindow = reinterpret_cast<HWND>(4);
constexpr DWORD watcherThread = 10;
HWND foreground = testWindow;
unsigned refreshes = 0;
unsigned restores = 0;
DWORD cloaked = 0;
ULONGLONG ticks = GetTickCount64();
DWORD messageTime = static_cast<DWORD>(ticks);
unsigned terminated = 0, rawRegistrations = 0, rawReads = 0;
bool canRegisterInput = true;

struct TestTouchInput {
    static bool Register(HWND window) {
        if (window) ++rawRegistrations;
        return canRegisterInput;
    }
    void DeviceChanged(HANDLE) {}
    void Read(HRAWINPUT, void*, void (*)(void*, const Tap&)) { ++rawReads; }
};

BOOL TestCreateProcess(LPCWSTR, LPWSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES, BOOL,
    DWORD, LPVOID, LPCWSTR, LPSTARTUPINFOW, LPPROCESS_INFORMATION) { return FALSE; }
BOOL TestTerminateProcess(HANDLE, UINT) { ++terminated; return TRUE; }
BOOL WINAPI TestSessionRegistration(HWND, DWORD) { return TRUE; }
ULONGLONG TestGetTickCount64() { return ticks; }
LONG TestGetMessageTime() { return static_cast<LONG>(messageTime); }

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
UINT_PTR TestSetTimer(HWND, UINT_PTR id, UINT, TIMERPROC) { return id; }
HRESULT WINAPI TestDwmAttribute(HWND, DWORD, PVOID result, DWORD) {
    *static_cast<DWORD*>(result) = cloaked;
    return S_OK;
}
FARPROC TestGetProcAddress(HMODULE module, LPCSTR name) {
    if (!lstrcmpA(name, "DwmGetWindowAttribute")) return reinterpret_cast<FARPROC>(TestDwmAttribute);
    if (!lstrcmpA(name, "WTSRegisterSessionNotification")) return reinterpret_cast<FARPROC>(TestSessionRegistration);
    return GetProcAddress(module, name);
}
int TestSystemMetrics(int index) { return index == SM_CONVERTIBLESLATEMODE ? 0 : GetSystemMetrics(index); }
BOOL TestPostThreadMessage(DWORD thread, UINT message, WPARAM wp, LPARAM) {
    if (thread == watcherThread && message == readFocusMessage && wp) ++refreshes;
    return TRUE;
}
}

#define CreateProcessW TestCreateProcess
#define TerminateProcess TestTerminateProcess
#define GetTickCount64 TestGetTickCount64
#define GetMessageTime TestGetMessageTime
#define TouchInput TestTouchInput
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

    app.OnMessage(nullptr, focusSnapshotMessage, (UINT_PTR(7) << 32) | 2, 9);

    const auto device = reinterpret_cast<HANDLE>(1);
    app.BeginTap(device, {50, 50}, GetTickCount64() + 1);
    app.contact_.device = nullptr;
    const auto ticket = app.tapTicket_;
    app.OnMessage(nullptr, focusSnapshotMismatchMessage, ticket - 1, 0);
    if (refreshes != 1) return 4;
    foreground = reinterpret_cast<HWND>(3);
    app.OnMessage(nullptr, focusSnapshotMismatchMessage, ticket, 0);
    if (refreshes != 1) return 9;
    foreground = testWindow;
    app.OnMessage(nullptr, focusSnapshotMismatchMessage, ticket, 0);
    if (refreshes != 2 || app.watch_.identity != 9) return 5;
    app.OnMessage(nullptr, focusSnapshotMismatchMessage, ticket, 0);
    if (refreshes != 2) return 14;

    // Publishing the same identity again must not invalidate an already focused tap.
    app.OnMessage(nullptr, focusSnapshotMessage, (UINT_PTR(7) << 32) | 3, 9);
    app.OnMessage(nullptr, focusSnapshotMessage, (UINT_PTR(6) << 32) | 3, 12);
    if (!app.ValidateTap() || app.watch_.identity != 9) return 10;

    // A refresh response repairs future taps, never the request carrying the old identity.
    app.OnMessage(nullptr, focusSnapshotMessage, (UINT_PTR(7) << 32) | 4, 12);
    if (app.ValidateTap() || app.watch_.identity != 12) return 6;
    app.BeginTap(device, {50, 50}, GetTickCount64() + 1);
    if (!app.ValidateTap() || app.contact_.identity != 12) return 7;

    // An empty snapshot can otherwise prevent all tap helpers from ever running.
    app.OnMessage(nullptr, focusSnapshotMessage, (UINT_PTR(7) << 32) | 5, 0);
    app.BeginTap(device, {50, 50}, GetTickCount64() + 1);
    if (refreshes != 3 || app.contact_.device) return 8;
    // The watcher used to stop being monitored after its first snapshot.
    ticks += 5000;
    app.CheckHelpers();
    if (app.watch_.process || terminated != 1 || app.watch_.generation == 7) return 15;
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

    // Locking must cancel even a worker already allowed to open the keyboard.
    foreground = testWindow;
    app.hidden_ = true;
    app.window_ = testWindow;
    app.focusHook_ = reinterpret_cast<HWINEVENTHOOK>(1);
    app.slate_ = true;
    app.contact_.device = device;
    app.contact_.focus = testFocus;
    app.focusHelpers_[0].process = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!app.focusHelpers_[0].process) return 16;
    app.OnMessage(testWindow, WM_WTSSESSION_CHANGE, WTS_SESSION_LOCK, 0);
    if (app.contact_.device || app.contact_.focus || app.focusHelpers_[0].process || terminated != 2) return 17;

    app.OnMessage(testWindow, WM_POWERBROADCAST, PBT_APMSUSPEND, 0);
    app.OnMessage(testWindow, WM_POWERBROADCAST, PBT_APMRESUMEAUTOMATIC, 0);
    app.OnMessage(testWindow, WM_INPUT, 0, 0);
    if (rawRegistrations || rawReads) return 18;

    // An unlock rebuilds input registrations and retries a transient failure.
    canRegisterInput = false;
    app.OnMessage(testWindow, WM_WTSSESSION_CHANGE, WTS_SESSION_UNLOCK, 0);
    if (rawRegistrations != 1) return 19;
    canRegisterInput = true;
    app.OnMessage(testWindow, WM_TIMER, recoveryTimer, 0);
    if (rawRegistrations != 2) return 20;
    app.OnMessage(testWindow, WM_TIMER, recoveryTimer, 0);
    if (rawRegistrations != 2) return 21;

    messageTime = static_cast<DWORD>(ticks - 500);
    app.OnMessage(testWindow, WM_INPUT, 0, 0);
    if (rawReads) return 22;
    messageTime = static_cast<DWORD>(ticks);
    app.OnMessage(testWindow, WM_INPUT, 0, 0);
    if (rawReads != 1) return 23;
    app.OnMessage(testWindow, WM_POWERBROADCAST, PBT_APMSUSPEND, 0);
    app.OnMessage(testWindow, WM_INPUT, 0, 0);
    if (rawReads != 1) return 24;
    ticks += 5000;
    app.OnMessage(testWindow, WM_POWERBROADCAST, PBT_APMRESUMEAUTOMATIC, 0);
    messageTime = static_cast<DWORD>(ticks);
    app.OnMessage(testWindow, WM_INPUT, 0, 0);
    if (rawRegistrations != 3 || rawReads != 2) return 25;

    // The unlock notification also proves the desktop is available after sleep.
    app.OnMessage(testWindow, WM_POWERBROADCAST, PBT_APMSUSPEND, 0);
    app.OnMessage(testWindow, WM_WTSSESSION_CHANGE, WTS_SESSION_UNLOCK, 0);
    app.OnMessage(testWindow, WM_INPUT, 0, 0);
    if (rawRegistrations != 4 || rawReads != 3) return 26;

    app.window_ = nullptr;
    return 0;
}
