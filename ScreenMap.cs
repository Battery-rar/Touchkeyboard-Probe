using System.Drawing;
using System.Runtime.InteropServices;
using System.Windows.Forms;

internal readonly record struct ScreenMap(Rectangle Bounds, int Rotation)
{
    internal static ScreenMap? ForDevice(nint device)
    {
        Rectangle bounds;
        Screen screen;
        if (GetPointerDeviceRects(device, out _, out var display) && display.Right > display.Left && display.Bottom > display.Top)
        {
            bounds = Rectangle.FromLTRB(display.Left, display.Top, display.Right, display.Bottom);
            screen = Screen.FromRectangle(bounds);
        }
        else
        {
            // The raw input handle may differ from the pointer device handle.
            var screens = Screen.AllScreens;
            if (screens.Length != 1)
                return null;
            screen = screens[0];
            bounds = screen.Bounds;
        }

        var mode = new DisplayMode { Size = (ushort)Marshal.SizeOf<DisplayMode>() };
        return EnumDisplaySettingsW(screen.DeviceName, -1, ref mode)
            ? new ScreenMap(bounds, mode.Orientation)
            : null;
    }

    internal Native.Point? ToScreen(double x, double y)
    {
        if (!double.IsFinite(x) || !double.IsFinite(y))
            return null;

        (x, y) = Rotation switch
        {
            1 => (1 - y, x),
            2 => (1 - x, 1 - y),
            3 => (y, 1 - x),
            _ => (x, y),
        };
        return new Native.Point(
            Bounds.Left + (int)Math.Round(x * (Bounds.Width - 1)),
            Bounds.Top + (int)Math.Round(y * (Bounds.Height - 1)));
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct Rect
    {
        internal int Left, Top, Right, Bottom;
    }

    // DEVMODEW fields used by EnumDisplaySettingsW.
    [StructLayout(LayoutKind.Explicit, Size = 220)]
    private struct DisplayMode
    {
        [FieldOffset(68)] internal ushort Size;
        [FieldOffset(84)] internal int Orientation;
    }

    [DllImport("user32.dll")]
    private static extern bool GetPointerDeviceRects(nint device, out Rect deviceRect, out Rect displayRect);

    [DllImport("user32.dll", CharSet = CharSet.Unicode, ExactSpelling = true)]
    private static extern bool EnumDisplaySettingsW(string device, int mode, ref DisplayMode settings);
}
