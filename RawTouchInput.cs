using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Threading.Channels;
using System.Windows.Interop;

internal sealed unsafe class RawTouchInput : IDisposable
{
    private const int InputMessage = 0x00FF;
    private const int DeviceChangeMessage = 0x00FE;
    private const uint ReadInput = 0x10000003;
    private const uint PreparsedData = 0x20000005;
    private const uint DeviceName = 0x20000007;
    private const ushort Digitizer = 0x0D;
    private const ushort TouchScreen = 0x04;
    private const ushort GenericDesktop = 0x01;
    private const ushort X = 0x30;
    private const ushort Y = 0x31;
    private const ushort TipSwitch = 0x42;
    private const ushort ContactCount = 0x54;
    private const int HidSuccess = 0x00110000;

    private readonly HwndSource source;
    private readonly ChannelWriter<Native.Point?> events;
    private readonly Dictionary<nint, ScreenTouchDevice?> devices = new();
    private byte[] inputBuffer = new byte[1024];
    internal int Messages { get; private set; }
    internal int Taps { get; private set; }

    internal static (int Devices, int Mapped) CheckDevices()
    {
        var supported = Native.ListRawDevices()
            .Where(device => device.Type == 2 && ScreenTouchDevice.Open(device.Handle) is not null)
            .ToArray();
        return (supported.Length, supported.Count(device => ScreenMap.ForDevice(device.Handle) is not null));
    }

    internal RawTouchInput(ChannelWriter<Native.Point?> events)
    {
        this.events = events;
        source = new(new HwndSourceParameters("Touch keyboard raw input")
        {
            ParentWindow = -3, // HWND_MESSAGE
        });
        source.AddHook(Observe);
        if (!Register(flags: 0x100 | 0x2000, source.Handle)) // RIDEV_INPUTSINK | RIDEV_DEVNOTIFY
        {
            var error = new Win32Exception();
            source.Dispose();
            throw error;
        }
    }

    private nint Observe(nint window, int message, nint wParam, nint lParam, ref bool handled)
    {
        if (message == DeviceChangeMessage)
        {
            devices.Remove(lParam);
            events.TryWrite(null);
        }
        if (message == InputMessage)
        {
            Messages++;
            ReadReports(lParam);
        }
        return 0;
    }

    private void ReadReports(nint input)
    {
        uint size = 0;
        uint headerSize = (uint)sizeof(Header);
        if (GetRawInputData(input, ReadInput, null, ref size, headerSize) != 0
            || size < headerSize + 8 || size > 65536)
            return;
        if (size > inputBuffer.Length)
            inputBuffer = new byte[size];

        fixed (byte* bytes = inputBuffer)
        {
            if (GetRawInputData(input, ReadInput, bytes, ref size, headerSize) != size)
                return;
            var header = *(Header*)bytes;
            var hid = bytes + headerSize;
            uint reportSize = *(uint*)hid;
            uint reportCount = *(uint*)(hid + 4);
            if (header.Type != 2 || header.Device == 0 || reportSize == 0
                || reportCount > (size - headerSize - 8) / reportSize)
                return;

            if (!devices.TryGetValue(header.Device, out var device))
            {
                device = ScreenTouchDevice.Open(header.Device);
                devices.Add(header.Device, device);
            }
            if (device is null)
                return;

            byte* report = hid + 8;
            for (int i = 0; i < reportCount; i++, report += reportSize)
            {
                var (completed, tap) = device.Observe(report, reportSize);
                if (!completed)
                    continue;
                if (tap.HasValue)
                    Taps++;
                events.TryWrite(tap);
            }
        }
    }

    public void Dispose()
    {
        Register(flags: 1, window: 0); // RIDEV_REMOVE
        source.Dispose();
    }

    private static bool Register(uint flags, nint window)
    {
        var device = new Registration { Page = Digitizer, Usage = TouchScreen, Flags = flags, Window = window };
        return RegisterRawInputDevices(in device, 1, (uint)sizeof(Registration));
    }

    private sealed class ScreenTouchDevice(nint handle, byte[] descriptor, Axes[] axes)
    {
        private readonly TapRecognizer taps = new();
        private ScreenMap? map;

        internal static ScreenTouchDevice? Open(nint handle)
        {
            if (IsVirtualDigitizer(handle))
                return null;
            uint length = 0;
            if (GetRawInputDeviceInfoW(handle, PreparsedData, null, ref length) == uint.MaxValue
                || length == 0 || length > 65536)
                return null;
            var data = new byte[length];
            fixed (byte* descriptor = data)
            {
                if (GetRawInputDeviceInfoW(handle, PreparsedData, descriptor, ref length) != length
                    || HidP_GetCaps(descriptor, out var caps) != HidSuccess
                    || caps.Page != Digitizer || caps.Usage != TouchScreen)
                    return null;
                var xs = ReadAxisCaps(X, caps.InputValueCount, descriptor);
                var ys = ReadAxisCaps(Y, caps.InputValueCount, descriptor);
                var axes = (from x in xs
                            from y in ys
                            where x.Collection == y.Collection && x.ReportId == y.ReportId
                            select new Axes(x, y)).ToArray();
                return axes.Length == 0 ? null : new ScreenTouchDevice(handle, data, axes);
            }
        }

        private static bool IsVirtualDigitizer(nint handle)
        {
            uint length = 0;
            if (GetRawInputDeviceInfoW(handle, DeviceName, null, ref length) == uint.MaxValue
                || length == 0 || length > 4096)
                return false;
            var name = new char[length];
            fixed (char* chars = name)
            {
                return GetRawInputDeviceInfoW(handle, DeviceName, (byte*)chars, ref length) != uint.MaxValue
                    && name.AsSpan().Contains("VIRTUAL_DIGITIZER", StringComparison.OrdinalIgnoreCase);
            }
        }

        internal (bool Completed, Native.Point? Tap) Observe(byte* report, uint length)
        {
            fixed (byte* data = descriptor)
            {
                bool hasCount = HidP_GetUsageValue(0, Digitizer, 0, ContactCount, out uint count, data, report, length) == HidSuccess;
                int active = 0;
                bool recognized = false;
                Native.Point? point = null;
                ushort* buttons = stackalloc ushort[32];
                foreach (var axis in axes)
                {
                    if (axis.X.ReportId != report[0])
                        continue;
                    uint buttonCount = 32;
                    if (HidP_GetUsages(0, Digitizer, axis.X.Collection, buttons, ref buttonCount, data, report, length) != HidSuccess)
                        continue;
                    recognized = true;
                    if (!new ReadOnlySpan<ushort>(buttons, (int)buttonCount).Contains(TipSwitch))
                        continue;
                    if (++active != 1 || count > 1)
                        break;

                    if (HidP_GetUsageValue(0, GenericDesktop, axis.X.Collection, X, out uint x, data, report, length) == HidSuccess
                        && HidP_GetUsageValue(0, GenericDesktop, axis.Y.Collection, Y, out uint y, data, report, length) == HidSuccess)
                    {
                        map ??= ScreenMap.ForDevice(handle);
                        point = map?.ToScreen(axis.X.Normalize(x), axis.Y.Normalize(y));
                    }
                }
                if (!recognized && !hasCount)
                    return default;

                // Hybrid reports carry the total count in their first packet only.
                var result = taps.Observe(count > 1 ? 2 : active, point, Environment.TickCount64);
                if (result.Completed)
                    map = null;
                return result;
            }
        }

        private static ValueCaps[] ReadAxisCaps(ushort usage, ushort capacity, byte* data)
        {
            if (capacity == 0)
                return [];
            var values = new ValueCaps[capacity];
            return HidP_GetSpecificValueCaps(0, GenericDesktop, 0, usage, values, ref capacity, data) == HidSuccess
                ? values[..capacity] : [];
        }
    }

    private readonly record struct Axes(ValueCaps X, ValueCaps Y);

    [StructLayout(LayoutKind.Sequential)]
    private struct Registration
    {
        internal ushort Page, Usage;
        internal uint Flags;
        internal nint Window;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct Header
    {
        internal uint Type, Size;
        internal nint Device, WParam;
    }

    // HIDP_CAPS and HIDP_VALUE_CAPS have the same layout on ARM64 and x64.
    [StructLayout(LayoutKind.Explicit, Size = 64)]
    private struct Caps
    {
        [FieldOffset(0)] internal ushort Usage;
        [FieldOffset(2)] internal ushort Page;
        [FieldOffset(48)] internal ushort InputValueCount;
    }

    [StructLayout(LayoutKind.Explicit, Size = 72)]
    private struct ValueCaps
    {
        [FieldOffset(2)] internal byte ReportId;
        [FieldOffset(6)] internal ushort Collection;
        [FieldOffset(18)] internal ushort Bits;
        [FieldOffset(40)] internal int Minimum;
        [FieldOffset(44)] internal int Maximum;

        internal double Normalize(uint raw)
        {
            if (Maximum <= Minimum || Bits is 0 or > 32)
                return double.NaN;
            double value = Minimum < 0 ? (int)(raw << (32 - Bits)) >> (32 - Bits) : raw;
            return Math.Clamp((value - Minimum) / ((double)Maximum - Minimum), 0, 1);
        }
    }

    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool RegisterRawInputDevices(in Registration devices, uint count, uint size);

    [DllImport("user32.dll")]
    private static extern uint GetRawInputData(nint input, uint command, byte* buffer, ref uint size, uint headerSize);

    [DllImport("user32.dll")]
    private static extern uint GetRawInputDeviceInfoW(nint device, uint command, byte* buffer, ref uint size);

    [DllImport("hid.dll")]
    private static extern int HidP_GetCaps(byte* data, out Caps caps);

    [DllImport("hid.dll")]
    private static extern int HidP_GetSpecificValueCaps(int reportType, ushort page, ushort collection,
        ushort usage, [Out] ValueCaps[] caps, ref ushort count, byte* data);

    [DllImport("hid.dll")]
    private static extern int HidP_GetUsageValue(int reportType, ushort page, ushort collection,
        ushort usage, out uint value, byte* data, byte* report, uint length);

    [DllImport("hid.dll")]
    private static extern int HidP_GetUsages(int reportType, ushort page, ushort collection,
        ushort* usages, ref uint count, byte* data, byte* report, uint length);
}
