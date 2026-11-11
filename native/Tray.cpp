#include <windows.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <windowsx.h>
#include <wtsapi32.h>
#include <array>
#include <cwchar>
#include "TouchInput.h"
#include "Focus.h"
#include "FocusTrace.h"
#include "FluentMenu.h"

namespace {
constexpr wchar_t appName[] = L"触摸键盘工具";
constexpr wchar_t windowClass[] = L"TouchKeyboardProbe.Native.1";
constexpr wchar_t instanceName[] = L"Local\\TouchKeyboardSecondTap.Native";
constexpr wchar_t settingsKey[] = L"Software\\TouchKeyboardProbe";
constexpr wchar_t hiddenValue[] = L"TrayHidden";
constexpr UINT trayMessage = WM_APP + 1, recoverMessage = WM_APP + 2;
constexpr UINT commandMessage = WM_APP + 3, statusMessage = WM_APP + 4;
constexpr UINT foregroundMessage = WM_APP + 7;
constexpr UINT_PTR recoveryTimer = 1, helperTimer = 2;
constexpr ULONGLONG helperTimeout = 4000;

struct SystemLibrary {
    explicit SystemLibrary(const wchar_t* name) : module(LoadLibraryExW(name, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) {}
    ~SystemLibrary() { if (module) FreeLibrary(module); }
    SystemLibrary(const SystemLibrary&) = delete;

    template<class T> T Find(const char* name) const {
        return module ? reinterpret_cast<T>(GetProcAddress(module, name)) : nullptr;
    }

private:
    HMODULE module;
};

struct Shell : SystemLibrary {
    Shell() : SystemLibrary(L"shell32.dll") {}
    bool Notify(DWORD command, NOTIFYICONDATAW& data) const {
        auto notify = Find<decltype(&Shell_NotifyIconW)>("Shell_NotifyIconW");
        return notify && notify(command, &data);
    }
};

bool SessionNotifications(HWND window, bool enable) {
    const SystemLibrary wts(L"wtsapi32.dll");
    if (enable) {
        const auto subscribe = wts.Find<decltype(&WTSRegisterSessionNotification)>("WTSRegisterSessionNotification");
        return subscribe && subscribe(window, NOTIFY_FOR_THIS_SESSION);
    }
    const auto unsubscribe = wts.Find<decltype(&WTSUnRegisterSessionNotification)>("WTSUnRegisterSessionNotification");
    return unsubscribe && unsubscribe(window);
}

struct FocusEvent {
    HWND window{};
    LONG object{}, child{};
    ULONGLONG time{};
} focusEvent;

ULONGLONG EventTick(DWORD time) {
    const auto now = GetTickCount64();
    const auto age = static_cast<DWORD>(now - time);
    return age <= now ? now - age : now;
}

bool UsesXamlFocus(HWND window) {
    wchar_t name[64]{};
    GetClassNameW(window, name, ARRAYSIZE(name));
    return !wcscmp(name, L"ApplicationFrameWindow") || !wcscmp(name, L"Windows.UI.Core.CoreWindow")
        || !wcscmp(name, L"WinUIDesktopWin32WindowClass") || !wcscmp(name, L"XamlExplorerHostIslandWindow");
}

bool IsTraySurface(HWND window) {
    auto isTray = [](HWND candidate) {
        wchar_t name[64]{};
        GetClassNameW(candidate, name, ARRAYSIZE(name));
        return !wcscmp(name, L"Shell_TrayWnd") || !wcscmp(name, L"Shell_SecondaryTrayWnd")
            || !wcscmp(name, L"NotifyIconOverflowWindow") || !wcscmp(name, L"TopLevelWindowForOverflowXamlIsland");
    };
    const auto root = GetAncestor(window, GA_ROOT);
    const auto owner = GetAncestor(window, GA_ROOTOWNER);
    return isTray(root) || (owner != root && isTray(owner));
}

void QueueWatchUpdate(HWND hint, bool foregroundChanged = false) {
    if (auto owner = FindWindowW(windowClass, nullptr))
        PostMessageW(owner, foregroundMessage, reinterpret_cast<WPARAM>(hint), foregroundChanged);
}

void CALLBACK FocusChanged(HWINEVENTHOOK, DWORD, HWND window, LONG object, LONG child, DWORD, DWORD time) {
    const auto foreground = GetForegroundWindow();
    if (!window || GetAncestor(window, GA_ROOT) != foreground) return;
    if (window == focusEvent.window && object == focusEvent.object && child == focusEvent.child) return;
    focusEvent = {window, object, child, EventTick(time)};
    FocusTrace("event hwnd=%p object=%ld child=%ld time=%lu", window, object, child, time);
    // A hosted XAML window can report native focus without a usable foreground notification.
    if (UsesXamlFocus(foreground)) QueueWatchUpdate(foreground);
}

bool FocusChangedSince(HWND foreground, ULONGLONG time) {
    return focusEvent.window && GetAncestor(focusEvent.window, GA_ROOT) == foreground
        && focusEvent.time >= time;
}

void CALLBACK ForegroundChanged(HWINEVENTHOOK, DWORD, HWND window, LONG, LONG, DWORD, DWORD) {
    QueueWatchUpdate(window, true);
}

void StopHelper(HANDLE& process) {
    if (!process) return;
    TerminateProcess(process, 0);
    CloseHandle(process);
    process = nullptr;
}

HANDLE StartHelper(wchar_t* args, HANDLE job, bool allowForeground = false, DWORD* thread = nullptr) {
    static wchar_t executable[32768];
    const auto length = GetModuleFileNameW(nullptr, executable, ARRAYSIZE(executable));
    if (!length || length >= ARRAYSIZE(executable)) return nullptr;
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    auto attributes = static_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(HeapAlloc(GetProcessHeap(), 0, size));
    if (!attributes) return nullptr;
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &size)) {
        HeapFree(GetProcessHeap(), 0, attributes);
        return nullptr;
    }
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.lpAttributeList = attributes;
    PROCESS_INFORMATION process{};
    // Associate the job during creation so an abrupt parent exit cannot leave a suspended child.
    const bool started = UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_JOB_LIST,
        &job, sizeof(job), nullptr, nullptr)
        && CreateProcessW(executable, args, nullptr, nullptr, FALSE, EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED,
            nullptr, nullptr, &startup.StartupInfo, &process);
    DeleteProcThreadAttributeList(attributes);
    HeapFree(GetProcessHeap(), 0, attributes);
    if (!started) return nullptr;
    if (allowForeground) AllowSetForegroundWindow(process.dwProcessId);
    if (ResumeThread(process.hThread) == DWORD(-1)) StopHelper(process.hProcess);
    CloseHandle(process.hThread);
    if (thread) *thread = process.hProcess ? process.dwThreadId : 0;
    return process.hProcess;
}

void LaunchSettings(UINT selection) {
    Shell shell;
    if (auto execute = shell.Find<decltype(&ShellExecuteW)>("ShellExecuteW"))
        execute(nullptr, L"open", selection == keyboardSettings ? L"ms-settings:typing"
            : L"ms-settings:personalization-textinput", nullptr, nullptr, SW_SHOWNORMAL);
}

bool ReadHidden() {
    DWORD value = 0, length = sizeof(value);
    return RegGetValueW(HKEY_CURRENT_USER, settingsKey, hiddenValue, RRF_RT_REG_DWORD,
        nullptr, &value, &length) == ERROR_SUCCESS && value != 0;
}

bool WriteHidden(bool hidden) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, settingsKey, 0, nullptr, 0, KEY_SET_VALUE,
        nullptr, &key, nullptr) != ERROR_SUCCESS) return false;
    const DWORD value = hidden;
    const auto result = RegSetValueExW(key, hiddenValue, 0, REG_DWORD,
        reinterpret_cast<const BYTE*>(&value), sizeof(value));
    RegCloseKey(key);
    return result == ERROR_SUCCESS;
}

COLORREF TrayColor() {
    HIGHCONTRASTW contrast{sizeof(contrast)};
    if (SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0)
        && (contrast.dwFlags & HCF_HIGHCONTRASTON))
        return GetSysColor(COLOR_WINDOWTEXT);
    DWORD light = 0, length = sizeof(light);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &length);
    return light ? RGB(25, 25, 25) : RGB(245, 245, 245);
}

HICON LoadKeyboardIcon(COLORREF color) {
    const int size = GetSystemMetrics(SM_CXSMICON);
    auto instance = GetModuleHandleW(nullptr);
    const auto resource = MAKEINTRESOURCEW(GetRValue(color) > 127 ? 2 : 3);
    auto icon = static_cast<HICON>(LoadImageW(instance, resource, IMAGE_ICON, size, size, LR_DEFAULTCOLOR));
    return icon ? icon : static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(1),
        IMAGE_ICON, size, size, LR_DEFAULTCOLOR));
}
class App {
public:
    ~App() { if (window_) DestroyWindow(window_); }

    bool Start(HINSTANCE instance) {
        WNDCLASSW klass{};
        klass.hInstance = instance;
        klass.lpfnWndProc = Procedure;
        klass.lpszClassName = windowClass;
        taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");
        if (!RegisterClassW(&klass)) return false;
        window_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, windowClass, appName,
            WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance, this);
        if (!window_) return false;
        focusHook_ = SetWinEventHook(EVENT_OBJECT_FOCUS, EVENT_OBJECT_FOCUS, nullptr, FocusChanged,
            0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
        foregroundHook_ = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr,
            ForegroundChanged, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
        job_ = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!focusHook_ || !foregroundHook_ || !job_
            || !SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
            DestroyWindow(window_);
            return false;
        }
        slate_ = !GetSystemMetrics(SM_CONVERTIBLESLATEMODE);
        const auto foreground = GetForegroundWindow();
        RememberApplication(foreground);
        UpdateXamlWatch(foreground);
        RepairRegistrations();
        return true;
    }

    void CommandReceived(UINT command) {
        switch (command) {
        case openKeyboard: StartKeyboard(); break;
        case hideTray: SetHidden(true); break;
        case showTray: SetHidden(false); break;
        case exitApp: if (window_) DestroyWindow(window_); break;
        }
    }

private:
    static LRESULT CALLBACK Procedure(HWND window, UINT message, WPARAM wp, LPARAM lp) {
        if (message == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lp);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        }
        auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (app) {
            try { return app->OnMessage(window, message, wp, lp); }
            catch (...) {
                if (message == WM_INPUT) return DefWindowProcW(window, message, wp, lp);
            }
        }
        return DefWindowProcW(window, message, wp, lp);
    }

    LRESULT OnMessage(HWND window, UINT message, WPARAM wp, LPARAM lp) {
        if (taskbarCreated_ && message == taskbarCreated_) {
            iconAdded_ = false;
            RepairRegistrations();
            return 0;
        }
        switch (message) {
        case WM_INPUT:
            ReadTouch(reinterpret_cast<HRAWINPUT>(lp));
            break;
        case WM_INPUT_DEVICE_CHANGE:
            if (auto device = reinterpret_cast<HANDLE>(lp)) {
                input_.DeviceChanged(device);
                CancelTap();
            }
            return 0;
        case WM_DISPLAYCHANGE:
            ResetTouch();
            break;
        case WM_POWERBROADCAST:
            if (wp == PBT_APMSUSPEND || wp == PBT_APMRESUMEAUTOMATIC) {
                sleeping_ = wp == PBT_APMSUSPEND;
                ResetSessionInput();
                return TRUE;
            }
            break;
        case WM_WTSSESSION_CHANGE:
            if (wp == WTS_SESSION_LOCK || wp == WTS_SESSION_UNLOCK) {
                sessionLocked_ = wp == WTS_SESSION_LOCK;
                if (!sessionLocked_) sleeping_ = false;
                ResetSessionInput();
            }
            return 0;
        case WM_SETTINGCHANGE:
            UpdatePosture();
            RefreshIcon();
            break;
        case WM_THEMECHANGED:
            RefreshIcon();
            break;
        case WM_TIMER:
            if (wp == recoveryTimer) RepairRegistrations();
            if (wp == helperTimer) CheckHelpers();
            return 0;
        case trayMessage:
            if (InputPaused()) return 0;
            if (LOWORD(lp) == WM_CONTEXTMENU || (!version4_ && LOWORD(lp) == WM_RBUTTONUP))
                Menu(true, version4_ ? wp : 0);
            else if (LOWORD(lp) == NIN_SELECT || LOWORD(lp) == NIN_KEYSELECT
                || (!version4_ && LOWORD(lp) == WM_LBUTTONUP)) {
                RestoreTrayFocus();
                StartKeyboard();
            }
            return 0;
        case recoverMessage:
            Menu();
            return 0;
        case commandMessage:
            CommandReceived(static_cast<UINT>(wp));
            return 0;
        case statusMessage: return hidden_ ? 2 : iconAdded_ ? 1 : 3;
        case validateTapMessage:
            if (wp != tapTicket_ || contact_.device || !ValidateTap()) return 0;
            for (auto& helper : focusHelpers_) {
                if (helper.process && helper.ticket == wp) {
                    helper.ticket = 0;
                    return 1;
                }
            }
            return 0;
        case menuClosingMessage:
            if (menuProcess_ && wp == GetProcessId(menuProcess_) && !menuClosingDeadline_)
                menuClosingDeadline_ = GetTickCount64() + helperTimeout;
            return 0;
        case foregroundMessage: {
            const auto foreground = GetForegroundWindow();
            const auto hint = reinterpret_cast<HWND>(wp);
            if (lp) RememberApplication(hint);
            if (lp || hint == foreground) UpdateXamlWatch(foreground, lp != 0);
            return 0;
        }
        case focusSnapshotMismatchMessage:
            if (wp == tapTicket_ && !contact_.device && contact_.identity && ValidateTap())
                RefreshXamlWatch();
            return 0;
        case focusSnapshotMessage:
            if (watch_.process && DWORD(wp >> 32) == watch_.generation && DWORD(wp) > watch_.revision
                && GetForegroundWindow() == watch_.window) {
                watch_.revision = DWORD(wp);
                watch_.deadline = 0;
                const auto identity = static_cast<UINT_PTR>(lp);
                if (identity != watch_.identity) {
                    watch_.identity = identity;
                    watch_.changed = GetTickCount64();
                }
            }
            return 0;
        case WM_CLOSE:
            // The tray tool stops only through its menu or --exit; a stray WM_CLOSE must not kill it.
            return 0;
        case WM_ENDSESSION:
            if (wp) DestroyWindow(window_);
            return 0;
        case WM_DESTROY:
            KillTimer(window_, recoveryTimer);
            if (sessionRegistered_) SessionNotifications(window_, false);
            TouchInput::Register(nullptr);
            if (focusHook_) UnhookWinEvent(focusHook_);
            if (foregroundHook_) UnhookWinEvent(foregroundHook_);
            RemoveTray();
            StopWorkers();
            if (job_) CloseHandle(job_);
            window_ = nullptr;
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(window, message, wp, lp);
    }

    NOTIFYICONDATAW TrayData() const {
        NOTIFYICONDATAW data{};
        data.cbSize = sizeof(data);
        data.hWnd = window_;
        data.uID = 1;
        return data;
    }

    void AddTray() {
        if (hidden_ || iconAdded_) return;
        if (!icon_) {
            iconColor_ = TrayColor();
            icon_ = LoadKeyboardIcon(iconColor_);
        }
        auto data = TrayData();
        data.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE | NIF_SHOWTIP;
        data.uCallbackMessage = trayMessage;
        data.hIcon = icon_;
        lstrcpynW(data.szTip, L"触摸键盘", ARRAYSIZE(data.szTip));
        Shell shell;
        iconAdded_ = icon_ && shell.Notify(NIM_ADD, data);
        if (iconAdded_) {
            data.uVersion = NOTIFYICON_VERSION_4;
            version4_ = shell.Notify(NIM_SETVERSION, data);
        }
    }

    void RefreshIcon() {
        if (!iconAdded_) return;
        const auto color = TrayColor();
        if (color == iconColor_) return;
        auto icon = LoadKeyboardIcon(color);
        if (!icon) return;
        auto data = TrayData();
        data.uFlags = NIF_ICON;
        data.hIcon = icon;
        if (Shell{}.Notify(NIM_MODIFY, data)) {
            DestroyIcon(icon_);
            icon_ = icon;
            iconColor_ = color;
        } else {
            DestroyIcon(icon);
        }
    }

    void RemoveTray() {
        if (iconAdded_) {
            auto data = TrayData();
            Shell{}.Notify(NIM_DELETE, data);
            iconAdded_ = false;
        }
        if (icon_) {
            DestroyIcon(icon_);
            icon_ = nullptr;
        }
    }

    void SetHidden(bool hidden) {
        if (hidden != hidden_ && !WriteHidden(hidden)) return;
        hidden_ = hidden;
        if (hidden_) RemoveTray();
        RepairRegistrations();
    }

    bool InputPaused() const { return sleeping_ || sessionLocked_; }

    void RepairRegistrations() {
        if (InputPaused()) return;
        if (!inputRegistered_) inputRegistered_ = TouchInput::Register(window_);
        if (!sessionRegistered_) sessionRegistered_ = SessionNotifications(window_, true);
        AddTray();
        if (inputRegistered_ && sessionRegistered_ && (hidden_ || iconAdded_))
            KillTimer(window_, recoveryTimer);
        else
            SetTimer(window_, recoveryTimer, 2000, nullptr);
    }

    struct Helper {
        HANDLE process{};
        ULONGLONG started{};
        UINT_PTR ticket{};
    };

    bool EnsureHelperTimer() {
        if (!helperTimerRunning_) helperTimerRunning_ = SetTimer(window_, helperTimer, 200, nullptr) != 0;
        return helperTimerRunning_;
    }

    void StopHelperTimer() {
        KillTimer(window_, helperTimer);
        helperTimerRunning_ = false;
    }

    Helper* StartFocusHelper(wchar_t* args) {
        for (auto& helper : focusHelpers_) {
            if (helper.process && WaitForSingleObject(helper.process, 0) == WAIT_OBJECT_0) {
                CloseHandle(helper.process);
                helper = {};
            }
            if (helper.process) continue;
            if (!EnsureHelperTimer()) return nullptr;
            helper.process = StartHelper(args, job_);
            helper.started = GetTickCount64();
            return helper.process ? &helper : nullptr;
        }
        return nullptr;
    }

    void RememberApplication(HWND window) {
        if (!window || window == window_ || IsTraySurface(window)) return;
        wchar_t name[64]{};
        if (!GetClassNameW(window, name, ARRAYSIZE(name)) || !wcscmp(name, menuWindowClass)) return;
        lastApplicationWindow_ = !wcscmp(name, L"Progman") || !wcscmp(name, L"WorkerW")
            ? nullptr : window;
    }

    void RestoreTrayFocus() {
        const auto foreground = GetForegroundWindow();
        const auto target = lastApplicationWindow_;
        if (!target || !IsTraySurface(foreground) || !IsWindowVisible(target) || IsIconic(target)) return;
        const SystemLibrary dwm(L"dwmapi.dll");
        const auto attribute = dwm.Find<decltype(&DwmGetWindowAttribute)>("DwmGetWindowAttribute");
        DWORD cloaked = 0;
        const bool visible = attribute && SUCCEEDED(attribute(target, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && !cloaked;
        if (visible && GetForegroundWindow() == foreground) SetForegroundWindow(target);
    }

    void StartKeyboard() {
        if (InputPaused()) return;
        wchar_t args[] = L"TouchKeyboardProbe --keyboard-helper";
        StartFocusHelper(args);
    }

    void StopXamlWatch() {
        StopHelper(watch_.process);
        const auto generation = watch_.generation;
        watch_ = {};
        watch_.generation = generation;
    }

    void RefreshXamlWatch() {
        if (!watch_.thread || watch_.deadline || !EnsureHelperTimer()) return;
        PostThreadMessageW(watch_.thread, readFocusMessage, 1, 0);
        watch_.deadline = GetTickCount64() + helperTimeout;
    }

    void UpdateXamlWatch(HWND foreground, bool refresh = false) {
        if (InputPaused()) return;
        if (watch_.process && watch_.window == foreground && slate_
            && WaitForSingleObject(watch_.process, 0) == WAIT_TIMEOUT) {
            if (refresh) RefreshXamlWatch();
            return;
        }
        if (watch_.process) StopXamlWatch();
        if (!slate_ || !UsesXamlFocus(foreground) || !EnsureHelperTimer()) return;
        wchar_t args[144];
        if (swprintf(args, ARRAYSIZE(args), L"TouchKeyboardProbe --focus-watch %llu %llu %lu",
            static_cast<unsigned long long>(reinterpret_cast<UINT_PTR>(window_)),
            static_cast<unsigned long long>(reinterpret_cast<UINT_PTR>(foreground)), ++watch_.generation) <= 0)
            return;
        watch_.process = StartHelper(args, job_, false, &watch_.thread);
        watch_.window = foreground;
        watch_.deadline = watch_.process ? GetTickCount64() + helperTimeout : 0;
    }

    void ReadTouch(HRAWINPUT input) {
        if (InputPaused()) return;
        const auto time = EventTick(static_cast<DWORD>(GetMessageTime()));
        if (time < inputSince_) return;
        UpdatePosture();
        if (!slate_ || !focusHook_) return;
        struct InputContext { App& app; ULONGLONG time; };
        InputContext state{*this, time};
        input_.Read(input, &state, [](void* context, const Tap& tap) {
            const auto& [app, time] = *static_cast<InputContext*>(context);
            if (tap.started) app.BeginTap(tap.device, *tap.started, time);
            if (!tap.completed || tap.device != app.contact_.device) return;
            FocusTrace("up valid=%d changed=%d focus=%p expected=%p", bool(tap.point),
                FocusChangedSince(app.contact_.window, app.contact_.downTime),
                FocusWindow(app.contact_.window), app.contact_.focus);
            app.contact_.device = nullptr;
            if (tap.point && app.ValidateTap()) app.CompleteTap(*tap.point);
        });
    }

    void BeginTap(HANDLE device, POINT point, ULONGLONG time) {
        CancelTap();
        const auto foreground = GetForegroundWindow();
        const auto focus = FocusWindow(foreground);
        const auto hit = WindowFromPoint(point);
        const auto hitRoot = GetAncestor(hit, GA_ROOT);
        FocusTrace("down %ld,%ld time=%llu foreground=%p focus=%p hitRoot=%p changed=%d", point.x, point.y, time,
            foreground, focus, hitRoot, FocusChangedSince(foreground, time));
        if (watch_.process && watch_.window != foreground) StopXamlWatch();
        if (!focus) return;
        // The keyboard may disappear before touch-up, exposing the editor underneath.
        if (hit && hitRoot != foreground) {
            DWORD pid = 0;
            if (GetWindowThreadProcessId(hit, &pid) && IsTextInputHost(pid)) return;
        }
        UINT_PTR identity = 0;
        if (UsesXamlFocus(foreground)) {
            UpdateXamlWatch(foreground);
            if (!watch_.identity || watch_.changed >= time) {
                if (!watch_.identity && watch_.revision) RefreshXamlWatch();
                FocusTrace("XAML touch-down has no stable snapshot");
                return;
            }
            identity = watch_.identity;
        }
        contact_ = {device, foreground, focus, point, time, identity};
    }

    bool ValidateTap() const {
        return !InputPaused() && contact_.focus && !GetSystemMetrics(SM_CONVERTIBLESLATEMODE)
            && FocusWindow(contact_.window) == contact_.focus
            && (contact_.identity ? watch_.window == contact_.window && watch_.identity == contact_.identity
                && watch_.changed < contact_.downTime : !FocusChangedSince(contact_.window, contact_.downTime));
    }

    void CompleteTap(POINT point) {
        wchar_t args[224];
        const auto size = swprintf(args, ARRAYSIZE(args),
            L"TouchKeyboardProbe --tap-helper %llu %llu %llu %llu %ld %ld %ld %ld %llu",
            static_cast<unsigned long long>(reinterpret_cast<UINT_PTR>(window_)),
            static_cast<unsigned long long>(tapTicket_),
            static_cast<unsigned long long>(reinterpret_cast<UINT_PTR>(contact_.window)),
            static_cast<unsigned long long>(reinterpret_cast<UINT_PTR>(contact_.focus)),
            contact_.point.x, contact_.point.y, point.x, point.y, static_cast<unsigned long long>(contact_.identity));
        if (size > 0) {
            if (auto helper = StartFocusHelper(args)) helper->ticket = tapTicket_;
        }
    }

    void ClearTap() {
        ++tapTicket_;
        contact_ = {};
    }

    void CancelTap() {
        ClearTap();
        for (auto& helper : focusHelpers_) {
            if (helper.process && helper.ticket && TerminateProcess(helper.process, 0))
                helper.ticket = 0;
        }
    }

    void ResetTouch() {
        input_ = TouchInput{};
        CancelTap();
    }

    void StopWorkers() {
        ClearTap();
        StopXamlWatch();
        for (auto& helper : focusHelpers_) {
            StopHelper(helper.process);
            helper = {};
        }
        StopHelper(menuProcess_);
        StopHelperTimer();
    }

    void ResetSessionInput() {
        StopWorkers();
        input_ = TouchInput{};
        focusEvent = {};
        lastApplicationWindow_ = nullptr;
        if (InputPaused()) {
            KillTimer(window_, recoveryTimer);
            return;
        }
        inputSince_ = GetTickCount64();
        inputRegistered_ = false;
        RepairRegistrations();
        slate_ = !GetSystemMetrics(SM_CONVERTIBLESLATEMODE);
        const auto foreground = GetForegroundWindow();
        RememberApplication(foreground);
        UpdateXamlWatch(foreground);
    }

    void UpdatePosture() {
        const bool slate = !GetSystemMetrics(SM_CONVERTIBLESLATEMODE);
        if (slate == slate_) return;
        slate_ = slate;
        ResetTouch();
        UpdateXamlWatch(GetForegroundWindow());
    }

    void Menu(bool fromTray = false, WPARAM coordinates = 0) {
        if (InputPaused()) return;
        if (menuProcess_ && WaitForSingleObject(menuProcess_, 0) == WAIT_OBJECT_0) CheckHelpers();
        if (!window_ || menuProcess_) return;
        POINT anchor{};
        GetCursorPos(&anchor);
        if (coordinates)
            anchor = {GET_X_LPARAM(coordinates), GET_Y_LPARAM(coordinates)};
        RECT iconRect{};
        if (fromTray) {
            NOTIFYICONIDENTIFIER identity{sizeof(identity), window_, 1};
            Shell shell;
            auto getRect = shell.Find<decltype(&Shell_NotifyIconGetRect)>("Shell_NotifyIconGetRect");
            if (getRect && FAILED(getRect(&identity, &iconRect))) SetRectEmpty(&iconRect);
            if (!IsRectEmpty(&iconRect)) anchor = {iconRect.right - 1, iconRect.top};
        }
        MONITORINFO monitor{sizeof(monitor)};
        if (!GetMonitorInfoW(MonitorFromPoint(anchor, MONITOR_DEFAULTTONEAREST), &monitor)) return;
        const auto& work = monitor.rcWork;
        const bool above = anchor.y > work.top + (work.bottom - work.top) / 2;
        if (!above && !IsRectEmpty(&iconRect)) anchor.y = iconRect.bottom;

        wchar_t args[128];
        if (swprintf(args, ARRAYSIZE(args), L"TouchKeyboardProbe --system-menu %ld %ld %d %d %llu",
            anchor.x, anchor.y, int(above), int(hidden_),
            static_cast<unsigned long long>(reinterpret_cast<UINT_PTR>(window_))) <= 0) return;
        menuPreviousWindow_ = GetForegroundWindow();
        if (!EnsureHelperTimer()) return;
        menuProcess_ = StartHelper(args, job_, true);
        menuClosingDeadline_ = 0;
        menuResponsive_ = menuChecked_ = GetTickCount64();
    }

    void CheckHelpers() {
        const auto now = GetTickCount64();
        if (watch_.process && (WaitForSingleObject(watch_.process, 0) == WAIT_OBJECT_0
            || (watch_.deadline && now >= watch_.deadline))) {
            const auto foreground = GetForegroundWindow();
            // Retry a stalled refresh once. A watcher that cannot start waits for the next input.
            const bool restart = watch_.deadline && watch_.revision && watch_.window == foreground;
            StopXamlWatch();
            if (restart) UpdateXamlWatch(foreground);
        }
        for (auto& helper : focusHelpers_) {
            if (!helper.process) continue;
            if (WaitForSingleObject(helper.process, 0) == WAIT_OBJECT_0) {
                CloseHandle(helper.process);
                helper = {};
            } else if (now - helper.started >= helperTimeout) {
                TerminateProcess(helper.process, 0);
            }
        }
        if (menuProcess_) {
            DWORD selection = 0;
            if (WaitForSingleObject(menuProcess_, 0) == WAIT_OBJECT_0) {
                GetExitCodeProcess(menuProcess_, &selection);
                CloseHandle(menuProcess_);
                menuProcess_ = nullptr;
            } else if (menuClosingDeadline_) {
                if (now >= menuClosingDeadline_) TerminateProcess(menuProcess_, 0);
            } else if (now - menuChecked_ >= 1000) {
                menuChecked_ = now;
                const auto host = FindWindowW(menuWindowClass, nullptr);
                DWORD pid = 0;
                if (host) GetWindowThreadProcessId(host, &pid);
                if (pid == GetProcessId(menuProcess_)
                    && SendMessageTimeoutW(host, WM_NULL, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 20, nullptr))
                    menuResponsive_ = now;
                if (now - menuResponsive_ >= 8000) TerminateProcess(menuProcess_, 0);
            }
            if (selection == openKeyboard && menuPreviousWindow_) SetForegroundWindow(menuPreviousWindow_);
            CommandReceived(selection);
            if (!window_) return;
        }
        if (!menuProcess_) {
            if (watch_.deadline) return;
            for (const auto& helper : focusHelpers_) if (helper.process) return;
            StopHelperTimer();
        }
    }

    HWND window_{};
    UINT taskbarCreated_{};
    HICON icon_{};
    COLORREF iconColor_{};
    bool hidden_ = ReadHidden();
    bool iconAdded_{};
    bool version4_{};
    bool helperTimerRunning_{};
    HANDLE menuProcess_{};
    HANDLE job_{};
    HWND menuPreviousWindow_{}, lastApplicationWindow_{};
    ULONGLONG menuChecked_{}, menuResponsive_{}, menuClosingDeadline_{};
    HWINEVENTHOOK focusHook_{};
    HWINEVENTHOOK foregroundHook_{};
    bool slate_{};
    bool sleeping_{}, sessionLocked_{};
    bool inputRegistered_{}, sessionRegistered_{};
    ULONGLONG inputSince_{};
    UINT_PTR tapTicket_{};
    struct {
        HANDLE device{};
        HWND window{}, focus{};
        POINT point{};
        ULONGLONG downTime{};
        UINT_PTR identity{};
    } contact_;
    TouchInput input_;
    std::array<Helper, 8> focusHelpers_{};
    struct {
        HANDLE process{};
        HWND window{};
        DWORD generation{}, revision{}, thread{};
        UINT_PTR identity{};
        ULONGLONG deadline{}, changed{};
    } watch_;
};

int SendToRunningApp(const wchar_t* option) {
    HWND window = nullptr;
    for (unsigned i = 0; !window && i < 50; ++i) {
        window = FindWindowW(windowClass, nullptr);
        if (!window) Sleep(20);
    }
    if (!window) return 4;
    if (wcscmp(option, L"--status") == 0) {
        DWORD_PTR status = 0;
        return SendMessageTimeoutW(window, statusMessage, 0, 0, SMTO_ABORTIFHUNG, 1000, &status)
            ? static_cast<int>(status) : 4;
    }
    DWORD owner = 0;
    GetWindowThreadProcessId(window, &owner);
    if (!wcscmp(option, L"--exit")) {
        auto process = OpenProcess(SYNCHRONIZE, FALSE, owner);
        if (!process) return 4;
        const bool posted = PostMessageW(window, commandMessage, exitApp, 0) != FALSE;
        const bool exited = posted && WaitForSingleObject(process, 4000) == WAIT_OBJECT_0;
        CloseHandle(process);
        return exited ? 0 : posted ? 5 : 4;
    }
    AllowSetForegroundWindow(owner);
    if (!*option) return PostMessageW(window, recoverMessage, 0, 0) ? 0 : 4;
    const auto command = wcscmp(option, L"--show-tray") == 0 ? showTray
        : wcscmp(option, L"--hide-tray") == 0 ? hideTray : openKeyboard;
    return PostMessageW(window, commandMessage, command, 0) ? 0 : 4;
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR option, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    POINT anchor{};
    int above = 0, hidden = 0;
    unsigned long long menuOwner = 0;
    if (swscanf(option, L"--system-menu %ld %ld %d %d %llu", &anchor.x, &anchor.y, &above, &hidden, &menuOwner) >= 4) {
        const auto selection = RunMenu(anchor, above != 0, hidden != 0, reinterpret_cast<HWND>(menuOwner));
        if (selection == keyboardSettings || selection == personalization) {
            LaunchSettings(selection);
            return 0;
        }
        return selection;
    }
    if (!wcscmp(option, L"--keyboard-helper")) return OpenKeyboard();
    unsigned long long owner = 0, ticket = 0, foreground = 0, focus = 0, previousFocus = 0;
    DWORD generation = 0;
    if (swscanf(option, L"--focus-watch %llu %llu %lu", &owner, &foreground, &generation) == 3
        && owner && foreground)
        return WatchFocus(reinterpret_cast<HWND>(owner), reinterpret_cast<HWND>(foreground), generation);
    POINT up{};
    if (swscanf(option, L"--tap-helper %llu %llu %llu %llu %ld %ld %ld %ld %llu", &owner, &ticket, &foreground, &focus,
        &anchor.x, &anchor.y, &up.x, &up.y, &previousFocus) == 9 && owner && foreground && focus)
        return CheckTap(reinterpret_cast<HWND>(owner), ticket, reinterpret_cast<HWND>(foreground),
            reinterpret_cast<HWND>(focus), anchor, up, previousFocus);
    if (*option && wcscmp(option, L"--status") && wcscmp(option, L"--show-tray")
        && wcscmp(option, L"--hide-tray") && wcscmp(option, L"--open-keyboard")
        && wcscmp(option, L"--exit")) return 2;
    HANDLE mutex = CreateMutexW(nullptr, FALSE, instanceName);
    if (!mutex) return 1;
    const bool running = GetLastError() == ERROR_ALREADY_EXISTS;
    if (running || !wcscmp(option, L"--status") || !wcscmp(option, L"--exit")) {
        const int code = running ? SendToRunningApp(option) : 4;
        CloseHandle(mutex);
        return code;
    }
    if (!wcscmp(option, L"--hide-tray") && !WriteHidden(true)) {
        CloseHandle(mutex);
        return 1;
    }
    App app;
    if (!app.Start(instance)) {
        MessageBoxW(nullptr, L"无法启动触摸输入监听。", appName, MB_OK | MB_ICONERROR);
        CloseHandle(mutex);
        return 1;
    }
    if (!wcscmp(option, L"--show-tray")) app.CommandReceived(showTray);
    else if (!wcscmp(option, L"--open-keyboard")) app.CommandReceived(openKeyboard);
    MSG message{};
    BOOL received = 0;
    while ((received = GetMessageW(&message, nullptr, 0, 0)) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    app.CommandReceived(exitApp);
    CloseHandle(mutex);
    return received < 0 ? 1 : 0;
}
