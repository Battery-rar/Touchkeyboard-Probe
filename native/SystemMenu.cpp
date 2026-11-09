#include "FluentMenu.h"
#include <appmodel.h>
#undef GetCurrentTime
#include <winrt/windows.foundation.collections.h>
#include <winrt/Microsoft.UI.Content.h>
#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Hosting.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>

namespace {
HMODULE AutomationLibrary() {
    static const auto module = LoadLibraryExW(L"oleaut32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    return module;
}

template<class T> T AutomationExport(const char* name) {
    const auto module = AutomationLibrary();
    return module ? reinterpret_cast<T>(GetProcAddress(module, name)) : nullptr;
}
}

// C++/WinRT uses these imports in the menu process only.
int32_t __stdcall WINRT_IMPL_GetErrorInfo(uint32_t reserved, void** info) noexcept {
    const auto call = AutomationExport<decltype(&WINRT_IMPL_GetErrorInfo)>("GetErrorInfo");
    return call ? call(reserved, info) : E_NOTIMPL;
}
int32_t __stdcall WINRT_IMPL_SetErrorInfo(uint32_t reserved, void* info) noexcept {
    const auto call = AutomationExport<decltype(&WINRT_IMPL_SetErrorInfo)>("SetErrorInfo");
    return call ? call(reserved, info) : E_NOTIMPL;
}
void __stdcall WINRT_IMPL_SysFreeString(winrt::impl::bstr value) noexcept {
    const auto call = AutomationExport<decltype(&WINRT_IMPL_SysFreeString)>("SysFreeString");
    if (call) call(value);
}
uint32_t __stdcall WINRT_IMPL_SysStringLen(winrt::impl::bstr value) noexcept {
    const auto call = AutomationExport<decltype(&WINRT_IMPL_SysStringLen)>("SysStringLen");
    return call ? call(value) : 0;
}

namespace {
using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Controls;
using namespace Controls::Primitives;
using namespace Hosting;

constexpr UINT_PTR startupTimer = 1;
constexpr int closeRequested = 1;
struct Item { UINT id; const wchar_t* label; const wchar_t* glyph; };

void AddRuntime() {
    using Pin = HRESULT(WINAPI*)(PSID, PCWSTR, PACKAGE_VERSION, DWORD, int, PCWSTR, DWORD, PWSTR*);
    using Add = HRESULT(WINAPI*)(PCWSTR, INT32, DWORD, void**, PWSTR*);
    auto kernel = GetModuleHandleW(L"kernelbase.dll");
    auto pin = reinterpret_cast<Pin>(GetProcAddress(kernel, "TryCreatePackageDependency"));
    auto add = reinterpret_cast<Add>(GetProcAddress(kernel, "AddPackageDependency"));
    check_bool(pin && add);
    PWSTR dependency = nullptr;
    constexpr DWORD arm64Architecture = 0x10;
    constexpr int processLifetime = 0;
    // The dependency lives only as long as this menu process.
    check_hresult(pin(nullptr, L"Microsoft.WindowsAppRuntime.2_8wekyb3d8bbwe",
        {}, arm64Architecture, processLifetime, nullptr, 0, &dependency));
    void* context = nullptr;
    const auto result = add(dependency, 0, 0, &context, nullptr);
    HeapFree(GetProcessHeap(), 0, dependency);
    check_hresult(result);
}

LRESULT CALLBACK HostProcedure(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_TIMER && wp == startupTimer) PostQuitMessage(0);
    if (message == WM_CLOSE) {
        PostQuitMessage(closeRequested);
        return 0;
    }
    return DefWindowProcW(window, message, wp, lp);
}

void NotifyClosing(HWND owner) {
    if (owner) PostMessageW(owner, menuClosingMessage, GetCurrentProcessId(), 0);
}

void OpenFlyout(HWND host, HWND owner, bool above, const Item (&items)[6], bool& handled, int& selected) {
    AddRuntime();
    auto windowing = LoadLibraryW(L"Microsoft.UI.Windowing.Core.dll");
    auto interop = LoadLibraryW(L"Microsoft.Internal.FrameworkUdk.dll");
    check_bool(windowing && interop);
    using GetWindowId = HRESULT(WINAPI*)(HWND, Microsoft::UI::WindowId*);
    using Translate = BOOL(WINAPI*)(const MSG*);
    auto getWindowId = reinterpret_cast<GetWindowId>(GetProcAddress(interop, "Windowing_GetWindowIdFromWindow"));
    auto preTranslate = reinterpret_cast<Translate>(GetProcAddress(windowing, "ContentPreTranslateMessage"));
    check_bool(getWindowId && preTranslate);
    Microsoft::UI::WindowId id{};
    check_hresult(getWindowId(host, &id));

    auto queue = Microsoft::UI::Dispatching::DispatcherQueueController::CreateOnCurrentThread();
    auto manager = WindowsXamlManager::InitializeForCurrentThread();
    auto app = Application::Current();
    if (!app) app = Application{};
    DesktopWindowXamlSource source;
    source.Initialize(id);
    auto bridge = source.SiteBridge();
    bridge.MoveAndResize({0, 0, 2, 2});
    bridge.Show();

    Grid root;
    MenuFlyout flyout;
    flyout.ShouldConstrainToRootBounds(false);
    flyout.Placement(above ? FlyoutPlacementMode::TopEdgeAlignedRight : FlyoutPlacementMode::BottomEdgeAlignedRight);
    try {
        Style style({hstring(name_of<MenuFlyoutPresenter>()), Windows::UI::Xaml::Interop::TypeKind::Metadata});
        style.Setters().Append(Setter(Control::CornerRadiusProperty(), box_value(CornerRadius{8, 8, 8, 8})));
        HIGHCONTRASTW contrast{sizeof(contrast)};
        if (SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0)
            && !(contrast.dwFlags & HCF_HIGHCONTRASTON)) {
            flyout.SystemBackdrop(Media::DesktopAcrylicBackdrop{});
            style.Setters().Append(Setter(Control::BackgroundProperty(), Media::SolidColorBrush(Windows::UI::Color{})));
        }
        flyout.MenuFlyoutPresenterStyle(style);
    } catch (const hresult_error&) {}
    for (const auto& item : items) {
        if (!item.id) {
            flyout.Items().Append(MenuFlyoutSeparator{});
            continue;
        }
        MenuFlyoutItem row;
        row.Text(item.label);
        FontIcon icon;
        icon.FontFamily(Media::FontFamily(L"Segoe Fluent Icons"));
        icon.Glyph(item.glyph);
        row.Icon(icon);
        row.Click([&, id = item.id](auto&&, auto&&) { selected = static_cast<int>(id); });
        flyout.Items().Append(row);
    }
    flyout.Opened([&](auto&&, auto&&) {
        handled = true;
        KillTimer(host, startupTimer);
    });
    auto closed = flyout.Closed(auto_revoke, [](auto&&, auto&&) { PostQuitMessage(0); });
    auto loaded = root.Loaded(auto_revoke, [&](auto&&, auto&&) {
        if (handled) return;
        try { flyout.ShowAt(root); }
        catch (const hresult_error&) { PostQuitMessage(0); }
    });
    check_bool(SetTimer(host, startupTimer, 4000, nullptr) != 0);
    source.Content(root);
    ShowWindow(host, SW_SHOW);
    SetForegroundWindow(host);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (message.hwnd == host && message.message == WM_CLOSE) {
            handled = true;
            break;
        }
        if (!preTranslate(&message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    if (message.message == WM_QUIT && message.wParam == closeRequested) handled = true;
    if (handled) NotifyClosing(owner);
    KillTimer(host, startupTimer);
    loaded.revoke();
    closed.revoke();
    if (flyout.IsOpen()) flyout.Hide();
    source.Close();
    manager.Close();
    queue.ShutdownQueue();
}

int OpenStandardMenu(HWND host, POINT anchor, bool above, const Item (&items)[6]) {
    HMENU menu = CreatePopupMenu();
    if (!menu) return 0;
    for (const auto& item : items)
        AppendMenuW(menu, item.id ? MF_STRING : MF_SEPARATOR, item.id, item.label);
    SetForegroundWindow(host);
    const auto selection = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON
        | TPM_RIGHTALIGN | (above ? TPM_BOTTOMALIGN : TPM_TOPALIGN), anchor.x, anchor.y, 0, host, nullptr);
    DestroyMenu(menu);
    return static_cast<int>(selection);
}
}

int RunMenu(POINT anchor, bool above, bool hidden, HWND owner) {
    const Item items[]{
        {openKeyboard, L"打开触摸键盘", L"\uE765"},
        {keyboardSettings, L"打开键盘设置", L"\uE713"},
        {personalization, L"个性化", L"\uE771"},
        {},
        {hidden ? showTray : hideTray, hidden ? L"显示托盘图标" : L"彻底隐藏托盘图标", L"\uE7B3"},
        {exitApp, L"退出", L"\uE8BB"},
    };
    const auto instance = GetModuleHandleW(nullptr);
    WNDCLASSW klass{};
    klass.hInstance = instance;
    klass.lpfnWndProc = HostProcedure;
    klass.lpszClassName = menuWindowClass;
    if (!RegisterClassW(&klass)) return 0;
    auto host = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, menuWindowClass, L"触摸键盘菜单",
        WS_POPUP, anchor.x - 2, above ? anchor.y - 2 : anchor.y, 2, 2, nullptr, nullptr, instance, nullptr);
    if (!host) return 0;

    int result = 0;
    bool handled = false;
    bool initialized = false;
    try {
        init_apartment(apartment_type::single_threaded);
        initialized = true;
        OpenFlyout(host, owner, above, items, handled, result);
    } catch (const hresult_error&) {}
    KillTimer(host, startupTimer);
    if (!handled) result = OpenStandardMenu(host, anchor, above, items);
    NotifyClosing(owner);
    DestroyWindow(host);
    if (initialized) uninit_apartment();
    return result;
}
