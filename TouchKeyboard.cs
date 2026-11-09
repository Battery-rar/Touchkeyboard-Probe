using System.Runtime.InteropServices;

internal static class TouchKeyboard
{
    private static readonly Guid InvocationClass = new("4CE576FA-83DC-4F88-951C-9D0782B4E376");
    private static readonly Guid InputPaneClass = new("D5120AA3-46BA-44C5-822D-CA8092C1FC72");

    internal static bool Show()
    {
        if (IsVisible())
            return false;
        var instance = Create<ITipInvocation>(InvocationClass);
        try
        {
            instance.Toggle(Native.GetDesktopWindow());
            return true;
        }
        finally
        {
            Marshal.ReleaseComObject(instance);
        }
    }

    internal static void CheckActivation() => Marshal.ReleaseComObject(Create<ITipInvocation>(InvocationClass));

    internal static bool IsVisible()
    {
        var pane = Create<IFrameworkInputPane>(InputPaneClass);
        try
        {
            pane.Location(out var bounds);
            return bounds.Right > bounds.Left && bounds.Bottom > bounds.Top;
        }
        finally
        {
            Marshal.ReleaseComObject(pane);
        }
    }

    private static T Create<T>(Guid clsid) =>
        (T)Activator.CreateInstance(Type.GetTypeFromCLSID(clsid, throwOnError: true)!)!;

    [ComImport, Guid("37C994E7-432B-4834-A2F7-DCE1F13B834B"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface ITipInvocation
    {
        void Toggle(nint window);
    }

    [ComImport, Guid("5752238B-24F0-495A-82F1-2FD593056796"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface IFrameworkInputPane
    {
        void Advise(nint window, nint handler, out uint cookie);
        void AdviseWithHWND(nint window, nint handler, out uint cookie);
        void Unadvise(uint cookie);
        void Location(out Rect bounds);
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct Rect
    {
        internal int Left;
        internal int Top;
        internal int Right;
        internal int Bottom;
    }
}
