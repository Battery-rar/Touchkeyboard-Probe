#pragma once
#include <windows.h>
#include <hidusage.h>
#include <hidpi.h>
#include <cstddef>
#include <optional>
#include <vector>
#include <unordered_map>

struct Tap {
    bool completed{};
    std::optional<POINT> point;
    std::optional<POINT> started;
    HANDLE device{};
};

class TapRecognizer {
public:
    Tap Observe(unsigned contacts, std::optional<POINT> point, ULONGLONG now);

private:
    std::optional<POINT> start_;
    POINT last_{};
    ULONGLONG started_{};
    bool cancelled_{};
};

class TouchInput {
public:
    static bool Register(HWND window);
    void DeviceChanged(HANDLE device);
    void Read(HRAWINPUT input, void* context, void (*onTap)(void*, const Tap&));

private:
    struct Device {
        Device(HANDLE handle);
        Tap Observe(PCHAR report, DWORD length);

        HANDLE handle;
        std::vector<std::byte> descriptor;
        struct Axes { HIDP_VALUE_CAPS x, y; };
        std::vector<Axes> axes;
        TapRecognizer taps;
        RECT display{};
        DWORD rotation{};
    };

    std::unordered_map<HANDLE, Device> devices_;
    std::vector<std::byte> buffer_;
};
