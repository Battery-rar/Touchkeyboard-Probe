#include <array>
#include <cstring>
#define private public
#include "../native/TouchInput.h"
#undef private

namespace {
const HANDLE deviceHandle = reinterpret_cast<HANDLE>(1);
std::array<std::array<BYTE, 3>, 2> reports{};
DWORD reportCount = 0;

UINT TestRawInput(HRAWINPUT, UINT, LPVOID data, PUINT size, UINT) {
    constexpr auto offset = offsetof(RAWINPUT, data.hid.bRawData);
    *size = UINT(offset + reportCount * 3);
    if (!data) return 0;
    auto* raw = static_cast<RAWINPUT*>(data);
    raw->header.dwType = RIM_TYPEHID;
    raw->header.hDevice = deviceHandle;
    raw->data.hid.dwSizeHid = 3;
    raw->data.hid.dwCount = reportCount;
    std::memcpy(static_cast<std::byte*>(data) + offset, reports.data(), reportCount * 3);
    return *size;
}

UINT TestDeviceInfo(HANDLE, UINT, LPVOID, PUINT) { return UINT(-1); }

NTSTATUS TestUsageValue(HIDP_REPORT_TYPE, USAGE, USHORT, USAGE usage, PULONG value,
    PHIDP_PREPARSED_DATA, PCHAR report, ULONG) {
    if (usage == 0x54) *value = report[1];
    else if (usage == 0x30) *value = report[2];
    else *value = 40;
    return HIDP_STATUS_SUCCESS;
}

NTSTATUS TestUsages(HIDP_REPORT_TYPE, USAGE, USHORT, PUSAGE usage, PULONG count,
    PHIDP_PREPARSED_DATA, PCHAR report, ULONG) {
    *count = report[1];
    if (*count) *usage = 0x42;
    return HIDP_STATUS_SUCCESS;
}

BOOL TestPointerRects(HANDLE, LPRECT, LPRECT screen) { *screen = {0, 0, 101, 101}; return TRUE; }
HMONITOR TestMonitorFromRect(LPCRECT, DWORD) { return reinterpret_cast<HMONITOR>(1); }
BOOL TestMonitorInfo(HMONITOR, LPMONITORINFO) { return TRUE; }
BOOL TestDisplaySettings(LPCWSTR, DWORD, LPDEVMODEW mode) { mode->dmDisplayOrientation = DMDO_DEFAULT; return TRUE; }

struct RecordedTaps {
    std::array<LONG, 5> events{};
    unsigned count{};
};

void RecordTap(void* context, const Tap& tap) {
    auto& record = *static_cast<RecordedTaps*>(context);
    if (record.count >= record.events.size()) return;
    if (tap.device != deviceHandle || (tap.started && tap.completed)) record.events[record.count++] = 0;
    else if (tap.started) record.events[record.count++] = tap.started->x;
    else if (tap.completed && tap.point) record.events[record.count++] = -tap.point->x;
}
}

#define GetRawInputData TestRawInput
#define GetRawInputDeviceInfoW TestDeviceInfo
#define HidP_GetUsageValue TestUsageValue
#define HidP_GetUsages TestUsages
#define GetPointerDeviceRects TestPointerRects
#define MonitorFromRect TestMonitorFromRect
#define GetMonitorInfoW TestMonitorInfo
#define EnumDisplaySettingsW TestDisplaySettings
#include "../native/TouchInput.cpp"

int main() {
    TouchInput input;
    auto& device = input.devices_.try_emplace(deviceHandle, deviceHandle).first->second;
    TouchInput::Device::Axes axes{};
    axes.x.ReportID = axes.y.ReportID = 1;
    axes.x.LogicalMax = axes.y.LogicalMax = 100;
    device.axes.push_back(axes);
    RecordedTaps record;

    reportCount = 1;
    reports[0] = {1, 1, 20};
    input.Read(nullptr, &record, RecordTap);
    if (record.count != 1 || record.events[0] != 20) return 1;

    // One WM_INPUT can carry the prior touch-up and the next touch-down.
    reportCount = 2;
    reports[0] = {1, 0, 20};
    reports[1] = {1, 1, 60};
    input.Read(nullptr, &record, RecordTap);
    if (record.count != 3 || record.events[1] != -20 || record.events[2] != 60) return 2;

    reportCount = 1;
    reports[0] = {1, 0, 60};
    input.Read(nullptr, &record, RecordTap);
    return record.count == 4 && record.events[3] == -60 ? 0 : 3;
}
