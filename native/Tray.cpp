#include <windows.h>
#include <shellapi.h>
#include <windowsx.h>
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
constexpr UINT_PTR trayTimer = 1, helperTimer = 2;

struct Shell {
    HMODULE module = LoadLibraryExW(L"shell32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    ~Shell() { if (module) FreeLibrary(module); }
    template<class T> T Find(const char* name) const {
        return module ? reinterpret_cast<T>(GetProcAddress(module, name)) : nullptr;
    }
    bool Notify(DWORD command, NOTIFYICONDATAW& data) const {
        auto notify = Find<decltype(&Shell_NotifyIconW)>("Shell_NotifyIconW");
        return notify && notify(command, &data);
    }
};

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

void QueueWatchUpdate(HWND hint = nullptr) {
    if (auto owner = FindWindowW(windowClass, nullptr))
        PostMessageW(owner, foregroundMessage, reinterpret_cast<WPARAM>(hint), 0);
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

void CALLBACK ForegroundChanged(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD) {
    QueueWatchUpdate();
}

HANDLE StartHelper(wchar_t* args, HANDLE job, bool allowForeground = false) {
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
    if (ResumeThread(process.hThread) == DWORD(-1)) {
        TerminateProcess(process.hProcess, 0);
        CloseHandle(process.hProcess);
        process.hProcess = nullptr;
    }
    CloseHandle(process.hThread);
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
        if (!TouchInput::Register(window_)) {
            DestroyWindow(window_);
            return false;
        }
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
        UpdateXamlWatch(GetForegroundWindow());
        if (!hidden_) AddTray();
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
            AddTray();
            return 0;
        }
        switch (message) {
        case WM_INPUT:
            UpdatePosture();
            if (slate_ && focusHook_) {
                const auto time = EventTick(static_cast<DWORD>(GetMessageTime()));
                auto tap = input_.Read(reinterpret_cast<HRAWINPUT>(lp));
                if (tap.started) BeginTap(tap.device, *tap.started, time);
                if (tap.completed && tap.device == contactDevice_) {
                    FocusTrace("up valid=%d changed=%d focus=%p expected=%p", bool(tap.point),
                        FocusChangedSince(contactWindow_, contactDownTime_), FocusWindow(contactWindow_), contactFocus_);
                    contactDevice_ = nullptr;
                    if (tap.point && ValidateTap()) CompleteTap(*tap.point);
                }
            }
            break;
        case WM_INPUT_DEVICE_CHANGE:
            if (auto device = reinterpret_cast<HANDLE>(lp)) {
                input_.DeviceChanged(device);
                CancelTap();
            }
            return 0;
        case WM_DISPLAYCHANGE:
            input_ = TouchInput{};
            CancelTap();
            break;
        case WM_SETTINGCHANGE:
            UpdatePosture();
            RefreshIcon();
            break;
        case WM_THEMECHANGED:
            RefreshIcon();
            break;
        case WM_TIMER:
            if (wp == trayTimer) AddTray();
            if (wp == helperTimer) CheckHelpers();
            return 0;
        case trayMessage:
            if (LOWORD(lp) == WM_CONTEXTMENU || (!version4_ && LOWORD(lp) == WM_RBUTTONUP))
                Menu(true, version4_ ? wp : 0);
            else if (LOWORD(lp) == NIN_SELECT || LOWORD(lp) == NIN_KEYSELECT
                || (!version4_ && LOWORD(lp) == WM_LBUTTONUP))
                StartKeyboard();
            return 0;
        case recoverMessage:
            Menu();
            return 0;
        case commandMessage:
            CommandReceived(static_cast<UINT>(wp));
            return 0;
        case statusMessage: return hidden_ ? 2 : iconAdded_ ? 1 : 3;
        case validateTapMessage:
            if (wp != tapTicket_ || contactDevice_ || !ValidateTap()) return 0;
            for (auto& helper : focusHelpers_) {
                if (helper.process && helper.ticket == wp) {
                    helper.ticket = 0;
                    return 1;
                }
            }
            return 0;
        case menuClosingMessage:
            if (menuProcess_ && wp == GetProcessId(menuProcess_) && !menuClosingDeadline_)
                menuClosingDeadline_ = GetTickCount64() + 4000;
            return 0;
        case foregroundMessage: {
            const auto foreground = GetForegroundWindow();
            if (!wp || reinterpret_cast<HWND>(wp) == foreground) UpdateXamlWatch(foreground);
            return 0;
        }
        case focusSnapshotMessage:
            if (watch_.process && DWORD(wp >> 32) == watch_.generation && DWORD(wp) > watch_.revision
                && GetForegroundWindow() == watch_.window) {
                watch_.revision = DWORD(wp);
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
            TouchInput::Register(nullptr);
            if (focusHook_) UnhookWinEvent(focusHook_);
            if (foregroundHook_) UnhookWinEvent(foregroundHook_);
            RemoveTray();
            CancelTap();
            KillTimer(window_, helperTimer);
            if (job_) CloseHandle(job_);
            if (watch_.process) CloseHandle(watch_.process);
            for (auto& helper : focusHelpers_) if (helper.process) CloseHandle(helper.process);
            if (menuProcess_) CloseHandle(menuProcess_);
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
            KillTimer(window_, trayTimer);
        } else {
            SetTimer(window_, trayTimer, 2000, nullptr);
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
        KillTimer(window_, trayTimer);
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
        else AddTray();
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

    void StartKeyboard() {
        wchar_t args[] = L"TouchKeyboardProbe --keyboard-helper";
        StartFocusHelper(args);
    }

    void StopXamlWatch() {
        if (watch_.process) {
            TerminateProcess(watch_.process, 0);
            CloseHandle(watch_.process);
        }
        const auto generation = watch_.generation;
        watch_ = {};
        watch_.generation = generation;
    }

    void UpdateXamlWatch(HWND foreground) {
        if (watch_.process && watch_.window == foreground && slate_
            && WaitForSingleObject(watch_.process, 0) == WAIT_TIMEOUT) return;
        if (watch_.process) StopXamlWatch();
        if (!slate_ || !UsesXamlFocus(foreground) || !EnsureHelperTimer()) return;
        wchar_t args[144];
        if (swprintf(args, ARRAYSIZE(args), L"TouchKeyboardProbe --focus-watch %llu %llu %lu",
            static_cast<unsigned long long>(reinterpret_cast<UINT_PTR>(window_)),
            static_cast<unsigned long long>(reinterpret_cast<UINT_PTR>(foreground)), ++watch_.generation) <= 0)
            return;
        watch_.process = StartHelper(args, job_);
        watch_.window = foreground;
        watch_.started = GetTickCount64();
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
        if (UsesXamlFocus(foreground)) {
            UpdateXamlWatch(foreground);
            if (!watch_.identity || watch_.changed >= time) {
                FocusTrace("XAML touch-down has no stable snapshot");
                return;
            }
            contactIdentity_ = watch_.identity;
        }
        contactDevice_ = device;
        contactWindow_ = foreground;
        contactFocus_ = focus;
        contactDownTime_ = time;
        contactPoint_ = point;
    }

    bool ValidateTap() const {
        return contactFocus_ && !GetSystemMetrics(SM_CONVERTIBLESLATEMODE)
            && FocusWindow(contactWindow_) == contactFocus_
            && (contactIdentity_ ? watch_.window == contactWindow_ && watch_.identity == contactIdentity_
                && watch_.changed < contactDownTime_ : !FocusChangedSince(contactWindow_, contactDownTime_));
    }

    void CompleteTap(POINT point) {
        wchar_t args[224];
        const auto size = swprintf(args, ARRAYSIZE(args),
            L"TouchKeyboardProbe --tap-helper %llu %llu %llu %llu %ld %ld %ld %ld %llu",
            static_cast<unsigned long long>(reinterpret_cast<UINT_PTR>(window_)),
            static_cast<unsigned long long>(tapTicket_),
            static_cast<unsigned long long>(reinterpret_cast<UINT_PTR>(contactWindow_)),
            static_cast<unsigned long long>(reinterpret_cast<UINT_PTR>(contactFocus_)),
            contactPoint_.x, contactPoint_.y, point.x, point.y, static_cast<unsigned long long>(contactIdentity_));
        if (size > 0) {
            if (auto helper = StartFocusHelper(args)) helper->ticket = tapTicket_;
        }
    }

    void CancelTap() {
        ++tapTicket_;
        for (auto& helper : focusHelpers_) {
            if (helper.process && helper.ticket && TerminateProcess(helper.process, 0))
                helper.ticket = 0;
        }
        contactDevice_ = nullptr;
        contactFocus_ = nullptr;
        contactIdentity_ = 0;
    }

    void UpdatePosture() {
        const bool slate = !GetSystemMetrics(SM_CONVERTIBLESLATEMODE);
        if (slate == slate_) return;
        slate_ = slate;
        input_ = TouchInput{};
        CancelTap();
        UpdateXamlWatch(GetForegroundWindow());
    }

    void Menu(bool fromTray = false, WPARAM coordinates = 0) {
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
            || (!watch_.revision && now - watch_.started >= 4000))) StopXamlWatch();
        for (auto& helper : focusHelpers_) {
            if (!helper.process) continue;
            if (WaitForSingleObject(helper.process, 0) == WAIT_OBJECT_0) {
                CloseHandle(helper.process);
                helper = {};
            } else if (now - helper.started >= 4000) {
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
            if (watch_.process && !watch_.revision) return;
            for (const auto& helper : focusHelpers_) if (helper.process) return;
            KillTimer(window_, helperTimer);
            helperTimerRunning_ = false;
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
    HWND menuPreviousWindow_{};
    ULONGLONG menuChecked_{}, menuResponsive_{}, menuClosingDeadline_{};
    HWINEVENTHOOK focusHook_{};
    HWINEVENTHOOK foregroundHook_{};
    bool slate_{};
    HANDLE contactDevice_{};
    HWND contactWindow_{}, contactFocus_{};
    POINT contactPoint_{};
    UINT_PTR tapTicket_{};
    UINT_PTR contactIdentity_{};
    ULONGLONG contactDownTime_{};
    TouchInput input_;
    std::array<Helper, 8> focusHelpers_{};
    struct {
        HANDLE process{};
        HWND window{};
        DWORD generation{}, revision{};
        UINT_PTR identity{};
        ULONGLONG started{}, changed{};
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
