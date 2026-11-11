#include "TouchInput.h"
#include <algorithm>
#include <cmath>
#include <cwchar>

namespace {
constexpr USAGE digitizer = 0x0d, touchScreen = 0x04, tipSwitch = 0x42, contactCount = 0x54;
constexpr USAGE desktop = 0x01, xAxis = 0x30, yAxis = 0x31;

std::vector<HIDP_VALUE_CAPS> AxisCaps(PHIDP_PREPARSED_DATA descriptor, USAGE axis, USHORT count) {
    std::vector<HIDP_VALUE_CAPS> caps(count);
    if (count && HidP_GetSpecificValueCaps(HidP_Input, desktop, 0, axis,
        caps.data(), &count, descriptor) == HIDP_STATUS_SUCCESS)
        caps.resize(count);
    else
        caps.clear();
    return caps;
}

bool LocateScreen(HANDLE device, RECT& bounds, DWORD& rotation) {
    RECT deviceRect{}, screen{};
    if (!GetPointerDeviceRects(device, &deviceRect, &screen) || IsRectEmpty(&screen)) {
        // A raw input handle can differ from the pointer handle. Only fall back on one screen.
        if (GetSystemMetrics(SM_CMONITORS) != 1) return false;
        screen = {0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    }
    MONITORINFOEXW monitor{};
    monitor.cbSize = sizeof(monitor);
    if (!GetMonitorInfoW(MonitorFromRect(&screen, MONITOR_DEFAULTTONULL), &monitor)) return false;
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    if (!EnumDisplaySettingsW(monitor.szDevice, ENUM_CURRENT_SETTINGS, &mode)) return false;
    bounds = screen;
    rotation = mode.dmDisplayOrientation;
    return true;
}

double Normalize(ULONG raw, const HIDP_VALUE_CAPS& axis) {
    double value = raw;
    if (axis.LogicalMin < 0) {
        const unsigned shift = 32 - axis.BitSize;
        value = static_cast<LONG>(raw << shift) >> shift;
    }
    return std::clamp((value - axis.LogicalMin) / (double(axis.LogicalMax) - axis.LogicalMin), 0.0, 1.0);
}

POINT ToScreen(RECT bounds, DWORD rotation, double x, double y) {
    switch (rotation) {
    case DMDO_90:  std::swap(x, y); x = 1 - x; break;
    case DMDO_180: x = 1 - x; y = 1 - y; break;
    case DMDO_270: std::swap(x, y); y = 1 - y; break;
    }
    return {bounds.left + LONG(std::lround(x * (bounds.right - bounds.left - 1))),
        bounds.top + LONG(std::lround(y * (bounds.bottom - bounds.top - 1)))};
}
}

Tap TapRecognizer::Observe(unsigned contacts, std::optional<POINT> point, ULONGLONG now) {
    if (!contacts) {
        Tap result{start_.has_value() || cancelled_};
        if (start_ && !cancelled_ && now - started_ <= 550) result.point = last_;
        start_.reset();
        cancelled_ = false;
        return result;
    }
    if (contacts > 1 || !point) cancelled_ = true;
    if (cancelled_) return {};
    last_ = *point;
    if (!start_) {
        start_ = point;
        started_ = now;
        return {false, {}, point};
    } else if (std::abs(last_.x - start_->x) > 24 || std::abs(last_.y - start_->y) > 24) {
        cancelled_ = true;
    }
    return {};
}

bool TouchInput::Register(HWND window) {
    const DWORD flags = window ? RIDEV_INPUTSINK | RIDEV_DEVNOTIFY : RIDEV_REMOVE;
    const RAWINPUTDEVICE device{digitizer, touchScreen, flags, window};
    return RegisterRawInputDevices(&device, 1, sizeof(device)) != FALSE;
}

void TouchInput::DeviceChanged(HANDLE device) { devices_.erase(device); }

TouchInput::Device::Device(HANDLE device) : handle(device) {
    UINT length = 0;
    if (GetRawInputDeviceInfoW(handle, RIDI_DEVICENAME, nullptr, &length) != UINT(-1)
        && length && length <= 4096) {
        std::vector<wchar_t> name(length + 1);
        if (GetRawInputDeviceInfoW(handle, RIDI_DEVICENAME, name.data(), &length) != UINT(-1)) {
            CharUpperBuffW(name.data(), length);
            if (wcsstr(name.data(), L"VIRTUAL_DIGITIZER")) return;
        }
    }
    length = 0;
    if (GetRawInputDeviceInfoW(handle, RIDI_PREPARSEDDATA, nullptr, &length) == UINT(-1) || !length) {
        retry = true;
        return;
    }
    if (length > 65536) return;
    descriptor.resize(length);
    auto data = reinterpret_cast<PHIDP_PREPARSED_DATA>(descriptor.data());
    if (const auto read = GetRawInputDeviceInfoW(handle, RIDI_PREPARSEDDATA, data, &length);
        read == UINT(-1) || read != length) {
        retry = true;
        return;
    }
    HIDP_CAPS caps{};
    if (HidP_GetCaps(data, &caps) != HIDP_STATUS_SUCCESS
        || caps.UsagePage != digitizer || caps.Usage != touchScreen) return;
    const auto xs = AxisCaps(data, xAxis, caps.NumberInputValueCaps);
    const auto ys = AxisCaps(data, yAxis, caps.NumberInputValueCaps);
    for (const auto& x : xs) for (const auto& y : ys) {
        if (x.LinkCollection == y.LinkCollection && x.ReportID == y.ReportID
            && x.BitSize && x.BitSize <= 32 && y.BitSize && y.BitSize <= 32
            && x.LogicalMax > x.LogicalMin && y.LogicalMax > y.LogicalMin)
            axes.push_back({x, y});
    }
}

Tap TouchInput::Device::Observe(PCHAR report, DWORD length) {
    if (axes.empty()) return {};
    auto data = reinterpret_cast<PHIDP_PREPARSED_DATA>(descriptor.data());
    ULONG count = 0;
    bool hasCount = HidP_GetUsageValue(HidP_Input, digitizer, 0, contactCount,
        &count, data, report, length) == HIDP_STATUS_SUCCESS;
    unsigned active = 0;
    bool recognized = false;
    std::optional<POINT> point;
    for (const auto& axis : axes) {
        if (axis.x.ReportID != static_cast<BYTE>(report[0])) continue;
        USAGE buttons[32];
        ULONG size = ARRAYSIZE(buttons);
        if (HidP_GetUsages(HidP_Input, digitizer, axis.x.LinkCollection, buttons, &size,
            data, report, length) != HIDP_STATUS_SUCCESS) continue;
        recognized = true;
        if (std::find(buttons, buttons + size, tipSwitch) == buttons + size) continue;
        if (++active > 1 || count > 1) break;
        ULONG x = 0, y = 0;
        if (HidP_GetUsageValue(HidP_Input, desktop, axis.x.LinkCollection, xAxis, &x, data, report, length)
            == HIDP_STATUS_SUCCESS
            && HidP_GetUsageValue(HidP_Input, desktop, axis.y.LinkCollection, yAxis, &y, data, report, length)
            == HIDP_STATUS_SUCCESS
            && (!IsRectEmpty(&display) || LocateScreen(handle, display, rotation)))
            point = ToScreen(display, rotation, Normalize(x, axis.x), Normalize(y, axis.y));
    }
    if (!recognized && !hasCount) return {};
    auto result = taps.Observe(count > 1 ? 2 : active, point, GetTickCount64());
    if (result.completed) SetRectEmpty(&display);
    return result;
}

void TouchInput::Read(HRAWINPUT input, void* context, void (*onTap)(void*, const Tap&)) {
    UINT size = 0;
    if (GetRawInputData(input, RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER)) != 0
        || size < offsetof(RAWINPUT, data.hid.bRawData) || size > 65536) return;
    if (size > buffer_.size()) buffer_.resize(size);
    if (GetRawInputData(input, RID_INPUT, buffer_.data(), &size, sizeof(RAWINPUTHEADER)) != size) return;
    const auto* raw = reinterpret_cast<const RAWINPUT*>(buffer_.data());
    const auto& hid = raw->data.hid;
    if (raw->header.dwType != RIM_TYPEHID || !raw->header.hDevice || !hid.dwSizeHid
        || hid.dwCount > (size - offsetof(RAWINPUT, data.hid.bRawData)) / hid.dwSizeHid) return;
    if (devices_.size() >= 32 && !devices_.contains(raw->header.hDevice)) return;
    const auto entry = devices_.try_emplace(raw->header.hDevice, raw->header.hDevice).first;
    auto& device = entry->second;
    if (device.retry) {
        devices_.erase(entry);
        return;
    }
    for (DWORD i = 0; i < hid.dwCount; ++i) {
        auto tap = device.Observe(reinterpret_cast<PCHAR>(const_cast<BYTE*>(hid.bRawData) + i * hid.dwSizeHid),
            hid.dwSizeHid);
        if (tap.started || tap.completed) {
            tap.device = raw->header.hDevice;
            onTap(context, tap);
        }
    }
}
