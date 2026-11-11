#include "../native/Focus.cpp"

int main() {
    wchar_t name[80];
    swprintf(name, ARRAYSIZE(name), L"Local\\TouchKeyboardProbe.LockTest.%lu", GetCurrentProcessId());

    // A second handle can keep the mutex alive after its previous owner exits.
    auto existing = CreateMutexW(nullptr, FALSE, name);
    if (!existing) return 1;
    auto acquired = TryLockKeyboard(name);
    if (!acquired) return 2;

    struct Attempt { const wchar_t* name; HANDLE acquired; ULONGLONG elapsed; } attempt{name, nullptr, 0};
    auto competitor = CreateThread(nullptr, 0, [](void* context) -> DWORD {
        auto& state = *static_cast<Attempt*>(context);
        const auto start = GetTickCount64();
        state.acquired = TryLockKeyboard(state.name, 60);
        state.elapsed = GetTickCount64() - start;
        if (state.acquired) {
            ReleaseMutex(state.acquired);
            CloseHandle(state.acquired);
        }
        return 0;
    }, &attempt, 0, nullptr);
    if (!competitor || WaitForSingleObject(competitor, 3000) != WAIT_OBJECT_0) return 3;
    CloseHandle(competitor);
    if (attempt.acquired || attempt.elapsed < 30) return 4;

    ReleaseMutex(acquired);
    CloseHandle(acquired);
    auto owner = CreateThread(nullptr, 0, [](void* mutex) -> DWORD {
        return WaitForSingleObject(mutex, 0) == WAIT_OBJECT_0 ? 0 : 1;
    }, existing, 0, nullptr);
    if (!owner || WaitForSingleObject(owner, 3000) != WAIT_OBJECT_0) return 5;
    DWORD result = 0;
    if (!GetExitCodeThread(owner, &result) || result) return 6;
    CloseHandle(owner);
    acquired = TryLockKeyboard(name);
    if (!acquired) return 7;
    const bool released = ReleaseMutex(acquired);
    CloseHandle(acquired);
    CloseHandle(existing);
    return released ? 0 : 8;
}
