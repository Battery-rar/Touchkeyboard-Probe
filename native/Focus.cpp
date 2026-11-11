#include "Focus.h"
#include "FocusTrace.h"
#include <UIAutomationClient.h>
#include <oleacc.h>
#include <shobjidl.h>
#include <atomic>
#include <cwchar>
#include <memory>

bool IsTextInputHost(DWORD pid) {
    const auto process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return false;
    wchar_t path[MAX_PATH];
    DWORD size = ARRAYSIZE(path);
    const bool found = QueryFullProcessImageNameW(process, 0, path, &size);
    CloseHandle(process);
    const auto name = found ? std::wcsrchr(path, L'\\') : nullptr;
    return name && CompareStringOrdinal(name + 1, -1, L"TextInputHost.exe", -1, TRUE) == CSTR_EQUAL;
}

namespace {
struct Release {
    void operator()(IUnknown* object) const { if (object) object->Release(); }
};
template<class T> using Com = std::unique_ptr<T, Release>;

const CLSID automationClass = __uuidof(CUIAutomation8);
constexpr CLSID keyboardClass{0x4ce576fa, 0x83dc, 0x4f88, {0x95, 0x1c, 0x9d, 0x07, 0x82, 0xb4, 0xe3, 0x76}};
constexpr CLSID paneClass{0xd5120aa3, 0x46ba, 0x44c5, {0x82, 0x2d, 0xca, 0x80, 0x92, 0xc1, 0xfc, 0x72}};
constexpr IID tipId{0x37c994e7, 0x432b, 0x4834, {0xa2, 0xf7, 0xdc, 0xe1, 0xf1, 0x3b, 0x83, 0x4b}};

struct ITipInvocation : IUnknown { virtual HRESULT STDMETHODCALLTYPE Toggle(HWND window) = 0; };

class ComRuntime {
public:
    ComRuntime() {
        auto initialize = Proc<decltype(&CoInitializeEx)>(ole_, "CoInitializeEx");
        if (initialize && uninitialize_ && create && clear)
            initialized_ = SUCCEEDED(initialize(nullptr, COINIT_MULTITHREADED));
    }
    ~ComRuntime() {
        if (initialized_) uninitialize_();
        if (automation_) FreeLibrary(automation_);
        if (ole_) FreeLibrary(ole_);
    }
    ComRuntime(const ComRuntime&) = delete;
    bool Ready() const { return initialized_; }

    template<class T> Com<T> Create(REFCLSID clsid, REFIID iid, DWORD context) const {
        T* object = nullptr;
        create(clsid, nullptr, context, iid, reinterpret_cast<void**>(&object));
        return Com<T>(object);
    }

private:
    template<class T> static T Proc(HMODULE module, const char* name) {
        return module ? reinterpret_cast<T>(GetProcAddress(module, name)) : nullptr;
    }
    HMODULE ole_ = LoadLibraryExW(L"ole32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    HMODULE automation_ = LoadLibraryExW(L"oleaut32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    decltype(&CoUninitialize) uninitialize_ = Proc<decltype(&CoUninitialize)>(ole_, "CoUninitialize");
    decltype(&CoCreateInstance) create = Proc<decltype(&CoCreateInstance)>(ole_, "CoCreateInstance");
    bool initialized_{};

public:
    decltype(&VariantClear) clear = Proc<decltype(&VariantClear)>(automation_, "VariantClear");
};

Com<IUIAutomation> CreateAutomation(const ComRuntime& runtime) {
    auto ui = runtime.Create<IUIAutomation>(automationClass, __uuidof(IUIAutomation), CLSCTX_INPROC_SERVER);
    IUIAutomation2* raw = nullptr;
    if (ui && SUCCEEDED(ui->QueryInterface(__uuidof(IUIAutomation2), reinterpret_cast<void**>(&raw)))) {
        Com<IUIAutomation2> settings(raw);
        settings->put_ConnectionTimeout(750);
        settings->put_TransactionTimeout(750);
    }
    return ui;
}

HANDLE TryLockKeyboard(const wchar_t* name, DWORD timeout = 0) {
    auto lock = CreateMutexW(nullptr, FALSE, name);
    if (!lock) return nullptr;
    const auto state = WaitForSingleObject(lock, timeout);
    if (state == WAIT_OBJECT_0 || state == WAIT_ABANDONED) return lock;
    CloseHandle(lock);
    return nullptr;
}

template<class CanOpen> void OpenSystemKeyboard(const ComRuntime& runtime, CanOpen canOpen, DWORD lockTimeout = 0) {
    auto lock = TryLockKeyboard(L"Local\\TouchKeyboardProbe.Keyboard", lockTimeout);
    if (!lock) {
        FocusTrace("keyboard lock unavailable");
        return;
    }
    auto pane = runtime.Create<IFrameworkInputPane>(paneClass, __uuidof(IFrameworkInputPane), CLSCTX_INPROC_SERVER);
    RECT bounds{};
    const auto located = pane ? pane->Location(&bounds) : E_FAIL;
    FocusTrace("pane location hr=%lx bounds=%ld,%ld,%ld,%ld", located, bounds.left, bounds.top, bounds.right, bounds.bottom);
    if (SUCCEEDED(located) && IsRectEmpty(&bounds)) {
        auto keyboard = runtime.Create<ITipInvocation>(keyboardClass, tipId, CLSCTX_ALL);
        FocusTrace("keyboard object=%d", bool(keyboard));
        if (keyboard && canOpen()) {
            const auto toggled = keyboard->Toggle(GetDesktopWindow());
            FocusTrace("toggle hr=%lx", toggled);
            // A successful Toggle can return before Location reports the keyboard as visible.
            if (SUCCEEDED(toggled)) {
                const auto deadline = GetTickCount64() + 1500;
                do {
                    Sleep(50);
                    if (SUCCEEDED(pane->Location(&bounds)) && !IsRectEmpty(&bounds)) break;
                } while (GetTickCount64() < deadline);
            }
        }
    }
    ReleaseMutex(lock);
    CloseHandle(lock);
}

bool Same(IUIAutomation* ui, IUIAutomationElement* a, IUIAutomationElement* b) {
    BOOL match = FALSE;
    return a && b && SUCCEEDED(ui->CompareElements(a, b, &match)) && match;
}

UINT_PTR RuntimeIdentity(IUIAutomationElement* element, HMODULE automation) {
    if (!element || !automation) return 0;
    const auto access = reinterpret_cast<decltype(&SafeArrayAccessData)>(
        GetProcAddress(automation, "SafeArrayAccessData"));
    const auto unaccess = reinterpret_cast<decltype(&SafeArrayUnaccessData)>(
        GetProcAddress(automation, "SafeArrayUnaccessData"));
    const auto destroy = reinterpret_cast<decltype(&SafeArrayDestroy)>(
        GetProcAddress(automation, "SafeArrayDestroy"));
    if (!access || !unaccess || !destroy) return 0;
    SAFEARRAY* id = nullptr;
    const auto result = element->GetRuntimeId(&id);
    if (!id) return 0;
    UINT_PTR value = 0;
    LONG* entries = nullptr;
    if (SUCCEEDED(result) && id->cDims == 1 && id->cbElements == sizeof(LONG)
        && id->rgsabound[0].cElements > 0 && id->rgsabound[0].cElements <= 64
        && SUCCEEDED(access(id, reinterpret_cast<void**>(&entries)))) {
        value = 14695981039346656037ull;
        for (ULONG i = 0; i < id->rgsabound[0].cElements; ++i)
            value = (value ^ static_cast<DWORD>(entries[i])) * 1099511628211ull;
        unaccess(id);
    }
    destroy(id);
    return value;
}

class FocusObserver final : public IUIAutomationFocusChangedEventHandler {
public:
    FocusObserver(HWND owner, HWND foreground, DWORD generation)
        : owner_(owner), foreground_(foreground), generation_(generation) {
        GetWindowThreadProcessId(foreground_, &foregroundPid_);
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override {
        if (!result) return E_POINTER;
        *result = nullptr;
        if (iid != __uuidof(IUnknown) && iid != __uuidof(IUIAutomationFocusChangedEventHandler))
            return E_NOINTERFACE;
        *result = static_cast<IUIAutomationFocusChangedEventHandler*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const auto refs = --refs_;
        if (!refs) delete this;
        return refs;
    }
    HRESULT STDMETHODCALLTYPE HandleFocusChangedEvent(IUIAutomationElement*) override {
        PostThreadMessageW(thread_, readFocusMessage, 0, 0);
        return S_OK;
    }
    void ReadFocus(IUIAutomation* ui, IUIAutomationCacheRequest* cache, bool republish = false) {
        if (GetForegroundWindow() != foreground_) return;
        IUIAutomationElement* focused = nullptr;
        ui->GetFocusedElementBuildCache(cache, &focused);
        Com<IUIAutomationElement> element(focused);
        if (GetForegroundWindow() != foreground_) return;
        int source = 0;
        if (element) element->get_CachedProcessId(&source);
        // The keyboard can report UIA focus without taking the app's Win32 foreground.
        const bool inputHost = source > 0 && DWORD(source) != foregroundPid_ && IsTextInputHost(DWORD(source));
        if (inputHost && !republish) return;
        const auto identity = inputHost ? identity_ : RuntimeIdentity(element.get(), automation_);
        if (!republish && revision_ && identity == identity_) return;
        const auto revision = revision_ + 1;
        if (PostMessageW(owner_, focusSnapshotMessage, (UINT_PTR(generation_) << 32) | revision, LPARAM(identity))) {
            revision_ = revision;
            identity_ = identity;
            FocusTrace("XAML snapshot generation=%lu revision=%lu identity=%llu pid=%d",
                generation_, revision, identity, source);
        }
    }

private:
    ~FocusObserver() { if (automation_) FreeLibrary(automation_); }
    HWND owner_, foreground_;
    DWORD generation_;
    const DWORD thread_ = GetCurrentThreadId();
    DWORD foregroundPid_{};
    HMODULE automation_ = LoadLibraryExW(L"oleaut32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    std::atomic<ULONG> refs_{1};
    DWORD revision_{};
    UINT_PTR identity_{};
};

void CALLBACK NativeFocusChanged(HWINEVENTHOOK, DWORD, HWND window, LONG, LONG, DWORD, DWORD) {
    if (window && GetAncestor(window, GA_ROOT) == GetForegroundWindow())
        PostThreadMessageW(GetCurrentThreadId(), readFocusMessage, 0, 0);
}

template<class T> Com<T> Pattern(IUIAutomationElement* field, PATTERNID id) {
    T* pattern = nullptr;
    field->GetCurrentPatternAs(id, __uuidof(T), reinterpret_cast<void**>(&pattern));
    return Com<T>(pattern);
}

Com<IUIAutomationCacheRequest> FieldCache(IUIAutomation* ui) {
    IUIAutomationCacheRequest* raw = nullptr;
    if (FAILED(ui->CreateCacheRequest(&raw))) return {};
    Com<IUIAutomationCacheRequest> cache(raw);
    for (const auto property : {UIA_ControlTypePropertyId, UIA_IsEnabledPropertyId,
        UIA_BoundingRectanglePropertyId, UIA_ProcessIdPropertyId, UIA_ValueIsReadOnlyPropertyId,
        UIA_HasKeyboardFocusPropertyId, UIA_IsKeyboardFocusablePropertyId, UIA_LegacyIAccessibleRolePropertyId,
        UIA_LegacyIAccessibleStatePropertyId})
        if (FAILED(cache->AddProperty(property))) return {};
    return cache;
}

bool IsEditable(IUIAutomationElement* field, const ComRuntime& runtime) {
    BOOL enabled = FALSE;
    CONTROLTYPEID type = 0;
    if (FAILED(field->get_CachedIsEnabled(&enabled)) || !enabled
        || FAILED(field->get_CachedControlType(&type))) return false;
    VARIANT property{};
    const bool standardText = type == UIA_EditControlTypeId || type == UIA_DocumentControlTypeId;
    if (standardText) {
        const auto hr = field->GetCachedPropertyValueEx(UIA_ValueIsReadOnlyPropertyId, TRUE, &property);
        const bool supported = SUCCEEDED(hr) && property.vt == VT_BOOL;
        const bool writable = supported && property.boolVal == VARIANT_FALSE;
        runtime.clear(&property);
        if (supported) return writable;
    }
    if (SUCCEEDED(field->GetCachedPropertyValueEx(UIA_LegacyIAccessibleRolePropertyId, TRUE, &property))
        && property.vt == VT_I4 && property.lVal == ROLE_SYSTEM_TEXT) {
        runtime.clear(&property);
        const auto hr = field->GetCachedPropertyValueEx(UIA_LegacyIAccessibleStatePropertyId, TRUE, &property);
        const bool supported = SUCCEEDED(hr) && property.vt == VT_I4;
        const bool writable = supported && (property.lVal & STATE_SYSTEM_FOCUSABLE)
            && !(property.lVal & (STATE_SYSTEM_READONLY | STATE_SYSTEM_UNAVAILABLE));
        runtime.clear(&property);
        if (supported) return writable;
    }
    runtime.clear(&property);
    if (!standardText) {
        BOOL focusable = FALSE;
        if (FAILED(field->get_CachedIsKeyboardFocusable(&focusable)) || !focusable) return false;
    }
    auto text = Pattern<IUIAutomationTextPattern>(field, UIA_TextPatternId);
    if (!text) return false;

    IUIAutomationTextRangeArray* selection = nullptr;
    text->GetSelection(&selection);
    Com<IUIAutomationTextRangeArray> ranges(selection);
    IUIAutomationTextRange* range = nullptr;
    if (ranges) ranges->GetElement(0, &range);
    if (!range) text->get_DocumentRange(&range);
    Com<IUIAutomationTextRange> candidate(range);
    if (!candidate) return false;
    VARIANT value{};
    HRESULT result = candidate->GetAttributeValue(UIA_IsReadOnlyAttributeId, &value);
    bool writable = SUCCEEDED(result) && value.vt == VT_BOOL && value.boolVal == VARIANT_FALSE;
    runtime.clear(&value);
    return writable;
}

Com<IUIAutomationElement> Parent(IUIAutomationTreeWalker* walker, IUIAutomationElement* node,
    IUIAutomationCacheRequest* cache) {
    IUIAutomationElement* parent = nullptr;
    walker->GetParentElementBuildCache(node, cache, &parent);
    return Com<IUIAutomationElement>(parent);
}

Com<IUIAutomationElement> EditableAncestor(IUIAutomationTreeWalker* walker, Com<IUIAutomationElement> node,
    IUIAutomationCacheRequest* cache, const ComRuntime& runtime) {
    for (int depth = 0; node && depth < 16; ++depth) {
        if (IsEditable(node.get(), runtime)) return node;
        node = Parent(walker, node.get(), cache);
    }
    return {};
}

bool FieldContainsTap(IUIAutomationElement* field, POINT down, POINT up) {
    RECT bounds{};
    const auto hr = field->get_CachedBoundingRectangle(&bounds);
    FocusTrace("uia bounds hr=%lx point=%ld,%ld bounds=%ld,%ld,%ld,%ld",
        hr, up.x, up.y, bounds.left, bounds.top, bounds.right, bounds.bottom);
    return SUCCEEDED(hr) && PtInRect(&bounds, down) && PtInRect(&bounds, up);
}

bool TapHitsFocusedField(IUIAutomation* ui, IUIAutomationTreeWalker* walker, HWND foreground,
    POINT down, POINT up, const ComRuntime& runtime, UINT_PTR previousFocus, bool& snapshotMismatch) {
    auto cache = FieldCache(ui);
    if (!cache) return false;
    IUIAutomationElement* focused = nullptr;
    ui->GetFocusedElementBuildCache(cache.get(), &focused);
    Com<IUIAutomationElement> focus(focused);
    if (previousFocus) {
        const auto current = RuntimeIdentity(focus.get(), GetModuleHandleW(L"oleaut32.dll"));
        if (current != previousFocus) {
            snapshotMismatch = true;
            FocusTrace("XAML focus differs from touch-down snapshot expected=%llu current=%llu", previousFocus, current);
            return false;
        }
    }
    auto field = EditableAncestor(walker, std::move(focus), cache.get(), runtime);
    IUIAutomationElement* hitElement = nullptr;
    if (!field) {
        if (GetAncestor(WindowFromPoint(up), GA_ROOT) != foreground) return false;
        ui->ElementFromPointBuildCache(up, cache.get(), &hitElement);
        auto hit = EditableAncestor(walker, Com<IUIAutomationElement>(hitElement), cache.get(), runtime);
        BOOL hasFocus = FALSE;
        if (!hit || FAILED(hit->get_CachedHasKeyboardFocus(&hasFocus)) || !hasFocus) {
            FocusTrace("uia focus lookup and focused hit unavailable");
            return false;
        }
        return FieldContainsTap(hit.get(), down, up);
    }
    if (!FieldContainsTap(field.get(), down, up)) return false;

    ui->ElementFromPointBuildCache(up, cache.get(), &hitElement);
    Com<IUIAutomationElement> hit(hitElement);
    if (!hit) return false;
    CONTROLTYPEID hitType = 0;
    int hitProcess = 0, fieldProcess = -1;
    hit->get_CachedControlType(&hitType);
    hit->get_CachedProcessId(&hitProcess);
    field->get_CachedProcessId(&fieldProcess);
    FocusTrace("uia hit type=%d pid=%d field pid=%d", hitType, hitProcess, fieldProcess);
    // Task View can hit-test the desktop card while its inline name editor has focus.
    if (hitType == UIA_ListItemControlTypeId && hitProcess == fieldProcess
        && GetAncestor(WindowFromPoint(up), GA_ROOT) == foreground
        && Same(ui, hit.get(), Parent(walker, field.get(), cache.get()).get())) return true;
    for (int depth = 0; hit && depth < 10; ++depth) {
        if (Same(ui, hit.get(), field.get())) return true;
        if (IsEditable(hit.get(), runtime)) return false;
        hit = Parent(walker, hit.get(), cache.get());
    }
    // Some browser providers return a host instead of their focused edit on hit testing.
    return (hitType == UIA_PaneControlTypeId || hitType == UIA_WindowControlTypeId
        || hitType == UIA_ToolBarControlTypeId || hitType == UIA_GroupControlTypeId || hitType == UIA_CustomControlTypeId)
        && hitProcess == fieldProcess && GetAncestor(WindowFromPoint(up), GA_ROOT) == foreground;
}
}

HWND FocusWindow(HWND foreground) {
    if (!foreground || foreground != GetForegroundWindow()) return nullptr;
    GUITHREADINFO info{sizeof(info)};
    return GetGUIThreadInfo(0, &info) && foreground == GetForegroundWindow() ? info.hwndFocus : nullptr;
}

int CheckTap(HWND owner, UINT_PTR ticket, HWND foreground, HWND focus, POINT down, POINT up,
    UINT_PTR previousFocus) {
    FocusTrace("check tap initial focus=%p expected=%p", FocusWindow(foreground), focus);
    if (FocusWindow(foreground) != focus || GetSystemMetrics(SM_CONVERTIBLESLATEMODE)) return 0;
    ComRuntime runtime;
    FocusTrace("tap COM ready=%d", runtime.Ready());
    if (!runtime.Ready()) return 0;
    auto ui = CreateAutomation(runtime);
    FocusTrace("tap UIA ready=%d", bool(ui));
    if (!ui) return 0;
    IUIAutomationTreeWalker* rawWalker = nullptr;
    ui->get_RawViewWalker(&rawWalker);
    Com<IUIAutomationTreeWalker> walker(rawWalker);
    if (!walker) return 0;
    bool snapshotMismatch = false;
    const bool hit = TapHitsFocusedField(ui.get(), walker.get(), foreground, down, up, runtime, previousFocus, snapshotMismatch);
    if (snapshotMismatch) PostMessageW(owner, focusSnapshotMismatchMessage, ticket, 0);
    FocusTrace("tap field=%d focus=%p", hit, FocusWindow(foreground));
    if (!hit || FocusWindow(foreground) != focus) return 0;
    // Brief contention must not discard the current tap; its ticket is rechecked after acquiring the lock.
    OpenSystemKeyboard(runtime, [&] {
        // Leave the request cancelable while the system prepares the keyboard.
        DWORD_PTR valid = 0;
        if (!SendMessageTimeoutW(owner, validateTapMessage, ticket, 0,
            SMTO_BLOCK | SMTO_ABORTIFHUNG, 100, &valid)) {
            FocusTrace("validate ticket=%llu error=%lu", ticket, GetLastError());
            return false;
        }
        FocusTrace("validate ticket=%llu allowed=%llu", ticket, valid);
        return valid && FocusWindow(foreground) == focus && !GetSystemMetrics(SM_CONVERTIBLESLATEMODE);
    }, 250);
    return 0;
}

int WatchFocus(HWND owner, HWND foreground, DWORD generation) {
    if (GetForegroundWindow() != foreground) return 0;
    ComRuntime runtime;
    if (!runtime.Ready()) return 1;
    auto ui = CreateAutomation(runtime);
    if (!ui || GetForegroundWindow() != foreground) return 1;
    IUIAutomationCacheRequest* request = nullptr;
    if (FAILED(ui->CreateCacheRequest(&request))) return 1;
    Com<IUIAutomationCacheRequest> cache(request);
    if (FAILED(cache->AddProperty(UIA_ProcessIdPropertyId))) return 1;
    DWORD pid = 0;
    GetWindowThreadProcessId(owner, &pid);
    auto parent = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!parent) return 1;
    MSG message{};
    PeekMessageW(&message, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
    Com<FocusObserver> observer(new FocusObserver(owner, foreground, generation));
    if (SUCCEEDED(ui->AddFocusChangedEventHandler(nullptr, observer.get()))) {
        const auto hook = SetWinEventHook(EVENT_OBJECT_FOCUS, EVENT_OBJECT_FOCUS, nullptr,
            NativeFocusChanged, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
        observer->ReadFocus(ui.get(), cache.get());
        while (MsgWaitForMultipleObjectsEx(1, &parent, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE) == WAIT_OBJECT_0 + 1) {
            bool refresh = false, republish = false;
            for (int i = 0; i < 64 && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE); ++i) {
                if (!message.hwnd && message.message == readFocusMessage) {
                    refresh = true;
                    republish |= message.wParam != 0;
                } else DispatchMessageW(&message);
            }
            if (refresh) observer->ReadFocus(ui.get(), cache.get(), republish);
        }
        if (hook) UnhookWinEvent(hook);
        ui->RemoveFocusChangedEventHandler(observer.get());
    }
    CloseHandle(parent);
    return 0;
}

int OpenKeyboard() {
    ComRuntime runtime;
    if (runtime.Ready()) OpenSystemKeyboard(runtime, [] { return true; });
    return 0;
}
