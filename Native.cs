using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;

internal static class Native
{
    internal const int MaxTouches = 95;
    internal const int ConvertibleSlateMode = 0x2003;
    private const uint DeviceListError = uint.MaxValue;
    private const uint Keyboard = 1;
    private static readonly uint DeviceSize = (uint)Marshal.SizeOf<RawInputDevice>();

    internal static int CountKeyboards() => ListRawDevices().Count(device => device.Type == Keyboard);

    internal static RawInputDevice[] ListRawDevices()
    {
        uint count = 0;
        if (GetRawInputDeviceList(null, ref count, DeviceSize) == DeviceListError)
            throw new Win32Exception();
        if (count == 0)
            return [];

        var devices = new RawInputDevice[count];
        uint listed = GetRawInputDeviceList(devices, ref count, DeviceSize);
        if (listed == DeviceListError)
            throw new Win32Exception();

        return listed == devices.Length ? devices : devices[..(int)listed];
    }

    internal static string WindowClass(nint window)
    {
        var name = new StringBuilder(256);
        GetClassName(window, name, name.Capacity);
        return name.ToString();
    }

    [StructLayout(LayoutKind.Sequential)]
    internal readonly record struct Point(int X, int Y);

    [StructLayout(LayoutKind.Sequential)]
    internal readonly struct RawInputDevice
    {
        internal readonly nint Handle;
        internal readonly uint Type;
    }

    [DllImport("user32.dll")]
    internal static extern int GetSystemMetrics(int index);

    [DllImport("user32.dll")]
    internal static extern bool SetProcessDpiAwarenessContext(nint context);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern uint GetRawInputDeviceList([Out] RawInputDevice[]? devices, ref uint count, uint itemSize);

    [DllImport("user32.dll")]
    internal static extern nint GetDesktopWindow();

    [DllImport("user32.dll")]
    internal static extern nint GetForegroundWindow();

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern int GetClassName(nint window, StringBuilder name, int capacity);

    [DllImport("user32.dll")]
    internal static extern nint WindowFromPoint(Point point);

    [DllImport("user32.dll")]
    internal static extern nint GetAncestor(nint window, uint flags);
}
