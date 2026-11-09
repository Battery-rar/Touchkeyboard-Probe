using System.ComponentModel;
using System.IO;
using System.Runtime.InteropServices;
using System.Threading.Channels;
using System.Windows.Automation;
using System.Windows.Threading;

internal static class Program
{
    private static long focusVersion;

    [STAThread]
    private static int Main(string[] args)
    {
        Native.SetProcessDpiAwarenessContext(-4); // Per-monitor DPI aware v2.
        try
        {
            if (args is ["--trace"] or ["--trace-live"])
            {
                bool showKeyboard = args[0] == "--trace-live";
                bool isFirstTraceInstance = true;
                using var traceInstance = showKeyboard
                    ? new Mutex(initiallyOwned: true, @"Local\TouchKeyboardSecondTap", out isFirstTraceInstance)
                    : null;
                using var output = new StreamWriter(new FileStream(
                    Path.Combine(AppContext.BaseDirectory, "touch-trace.log"), FileMode.Create,
                    FileAccess.Write, FileShare.ReadWrite)) { AutoFlush = true };
                var originalOut = Console.Out;
                var originalError = Console.Error;
                Console.SetOut(output);
                Console.SetError(output);
                try
                {
                    if (!isFirstTraceInstance)
                    {
                        Console.WriteLine("Another instance is already running.");
                        return 1;
                    }
                    return WatchTaps(useSlateMode: true, trace: true, showKeyboard: showKeyboard);
                }
                catch (Exception e)
                {
                    Console.Error.WriteLine(e);
                    return 1;
                }
                finally
                {
                    Console.SetOut(originalOut);
                    Console.SetError(originalError);
                }
            }

            if (args is ["--check"] or ["--probe"])
                return Probe(show: args[0] == "--probe");
            if (args.Length != 0 && args is not ["--raw-keyboards"])
            {
                Console.Error.WriteLine("Usage: TouchKeyboardProbe [--check | --probe | --trace | --trace-live | --raw-keyboards]");
                return 2;
            }

            using var instance = new Mutex(initiallyOwned: true, @"Local\TouchKeyboardSecondTap", out bool isFirst);
            if (!isFirst)
            {
                System.Windows.MessageBox.Show("工具已在运行。更新时，请先从托盘退出旧版。", "触摸键盘工具");
                return 0;
            }

            return WatchTaps(useSlateMode: args.Length == 0);
        }
        catch (Exception e)
        {
            Console.Error.WriteLine($"{e.Message} (0x{e.HResult:X8})");
            if (args.Length == 0 || args is ["--raw-keyboards"])
                System.Windows.MessageBox.Show(e.Message, "触摸键盘工具");
            return 1;
        }
    }

    private static int Probe(bool show)
    {
        int keyboardCount = Native.CountKeyboards();
        Console.WriteLine($"Touch contacts supported: {Native.GetSystemMetrics(Native.MaxTouches)}");
        Console.WriteLine($"Raw Input keyboards: {keyboardCount}");
        Console.WriteLine($"Convertible slate mode: {Native.GetSystemMetrics(Native.ConvertibleSlateMode) == 0}");
        var (devices, mapped) = RawTouchInput.CheckDevices();
        Console.WriteLine($"Readable touch descriptors: {devices}; mapped to screen: {mapped}");
        TouchKeyboard.CheckActivation();
        Console.WriteLine("ITipInvocation activated.");
        Console.WriteLine($"Input pane visible: {TouchKeyboard.IsVisible()}");
        if (!show)
            return 0;

        Console.WriteLine("Switch to an editable input field within 3 seconds.");
        Thread.Sleep(3000);
        var foreground = Native.GetForegroundWindow();
        var focused = AutomationElement.FocusedElement;
        if (!IsEditable(focused))
        {
            Console.Error.WriteLine("No editable field had focus when the timer elapsed.");
            return 1;
        }

        Console.WriteLine(TouchKeyboard.Show() ? "Toggle returned successfully." : "Input pane was already visible.");
        Thread.Sleep(1000);
        bool visible = TouchKeyboard.IsVisible();
        nint currentWindow = Native.GetForegroundWindow();
        var currentFocus = AutomationElement.FocusedElement;
        bool sameWindow = foreground == currentWindow;
        bool sameField = currentFocus is not null && Automation.Compare(focused, currentFocus);
        Console.WriteLine($"Input pane visible: {visible}; same window: {sameWindow}; same field: {sameField}");
        if (!sameWindow)
            Console.WriteLine($"Foreground window class: {Native.WindowClass(foreground)} -> {Native.WindowClass(currentWindow)}");
        return visible && sameWindow && sameField ? 0 : 1;
    }

    private static int WatchTaps(bool useSlateMode, bool trace = false, bool showKeyboard = true)
    {
        if (Native.GetSystemMetrics(Native.MaxTouches) == 0)
            throw new InvalidOperationException("Windows 在当前会话中没有检测到触摸屏。");

        var events = Channel.CreateBounded<Native.Point?>(new BoundedChannelOptions(16)
        {
            SingleReader = true,
            SingleWriter = true,
            FullMode = BoundedChannelFullMode.DropWrite,
        });
        var dispatcher = Dispatcher.CurrentDispatcher;
        using var menu = new System.Windows.Forms.ContextMenuStrip();
        menu.Items.Add("退出", null, (_, _) => dispatcher.BeginInvokeShutdown(DispatcherPriority.Send));
        using var tray = new System.Windows.Forms.NotifyIcon
        {
            Icon = System.Drawing.SystemIcons.Application,
            Text = trace ? "触摸键盘 · 诊断中" : "触摸键盘 · 再点一次打开",
            ContextMenuStrip = menu,
        };

        int status = 0;
        var worker = new Thread(() =>
        {
            status = WatchFields(events.Reader, useSlateMode, trace, showKeyboard);
            dispatcher.BeginInvokeShutdown(DispatcherPriority.Send);
        })
        {
            IsBackground = true,
            Name = "Touch keyboard and UI Automation",
        };
        using var rawInput = new RawTouchInput(events.Writer);
        using var traceTimer = new System.Windows.Forms.Timer { Interval = 1500 };
        if (trace)
        {
            long started = Environment.TickCount64;
            Console.WriteLine($"Architecture: {RuntimeInformation.ProcessArchitecture}; raw touch registered");
            traceTimer.Tick += (_, _) =>
            {
                Console.WriteLine($"Slate: {IsKeyboardUnavailable(useSlateMode)}; " +
                    $"raw touch messages: {rawInput.Messages}; recognized taps: {rawInput.Taps}");
                if (Environment.TickCount64 - started >= 300000)
                {
                    Console.WriteLine("Diagnostic capture ended after five minutes.");
                    dispatcher.BeginInvokeShutdown(DispatcherPriority.Send);
                }
            };
            traceTimer.Start();
        }
        worker.Start();
        tray.Visible = true;
        Console.CancelKeyPress += (_, e) =>
        {
            e.Cancel = true;
            dispatcher.BeginInvokeShutdown(DispatcherPriority.Send);
        };
        Console.WriteLine(useSlateMode ? "Keyboard detection: convertible slate posture." : "Keyboard detection: Raw Input.");
        Console.WriteLine("Listening for touch taps. Use the tray menu or Ctrl+C to stop.");
        Dispatcher.Run();
        events.Writer.TryComplete();
        return status;
    }

    private static int WatchFields(ChannelReader<Native.Point?> events, bool useSlateMode, bool trace, bool showKeyboard)
    {
        AutomationFocusChangedEventHandler focusChanged = (_, _) => Interlocked.Increment(ref focusVersion);
        AutomationElement? previous = null;
        nint previousWindow = 0;
        long previousFocusVersion = -1;
        long lastShow = -1000;

        try
        {
            Automation.AddAutomationFocusChangedEventHandler(focusChanged);
        }
        catch (Exception e)
        {
            Console.Error.WriteLine($"Could not subscribe to focus changes: {e.Message}");
            return 1;
        }

        try
        {
            while (events.WaitToReadAsync().AsTask().GetAwaiter().GetResult())
            {
                while (events.TryRead(out var point))
                {
                    try
                    {
                        // Let the foreground app finish applying the tap before reading its focus.
                        if (point.HasValue)
                            Thread.Sleep(100);
                        bool enabled = point.HasValue && IsKeyboardUnavailable(useSlateMode);
                        if (point.HasValue && !enabled)
                            Console.WriteLine("Touch tap ignored: keyboard detected.");
                        var target = enabled && point.HasValue
                            ? FindFocusedField(point.Value, trace)
                            : null;
                        if (target is null)
                        {
                            if (trace && enabled)
                                Console.WriteLine("Tap received; no matching focused editable control.");
                            previous = null;
                            continue;
                        }

                        var (field, window, version) = target.Value;
                        if (previous is not null && window == previousWindow && version == previousFocusVersion
                            && Automation.Compare(previous, field))
                        {
                            previous = null;
                            if (Environment.TickCount64 - lastShow >= 750 && window == Native.GetForegroundWindow()
                                && version == Interlocked.Read(ref focusVersion)
                                && IsKeyboardUnavailable(useSlateMode))
                            {
                                lastShow = Environment.TickCount64;
                                if (!showKeyboard)
                                    Console.WriteLine("Second tap matched; would request touch keyboard.");
                                else if (TouchKeyboard.Show())
                                {
                                    Console.WriteLine("Requested touch keyboard.");
                                    if (trace)
                                    {
                                        Thread.Sleep(600);
                                        Console.WriteLine($"Keyboard visible after request: {TouchKeyboard.IsVisible()}");
                                    }
                                }
                                else if (trace)
                                    Console.WriteLine("Second tap matched; keyboard was already visible.");
                            }
                        }
                        else
                        {
                            previous = field;
                            previousWindow = window;
                            previousFocusVersion = version;
                            Console.WriteLine("Touch tap focused an editable field.");
                        }
                    }
                    catch (Exception e) when (e is COMException or ElementNotAvailableException
                        or UnauthorizedAccessException or Win32Exception)
                    {
                        previous = null;
                        Console.Error.WriteLine($"{e.Message} (0x{e.HResult:X8})");
                    }
                }
            }
            return 0;
        }
        catch (Exception e)
        {
            Console.Error.WriteLine($"Input monitoring stopped: {e.Message}");
            return 1;
        }
        finally
        {
            Automation.RemoveAutomationFocusChangedEventHandler(focusChanged);
        }
    }

    private static bool IsKeyboardUnavailable(bool useSlateMode) => useSlateMode
        ? Native.GetSystemMetrics(Native.ConvertibleSlateMode) == 0
        : Native.CountKeyboards() == 0;

    private static (AutomationElement Field, nint Window, long Version)? FindFocusedField(Native.Point point, bool trace)
    {
        long version = Interlocked.Read(ref focusVersion);
        nint window = Native.GetForegroundWindow();
        if (window == 0)
            return null;

        var focus = AutomationElement.FocusedElement;
        var field = EditableAncestor(focus);
        var location = new System.Windows.Point(point.X, point.Y);
        if (field is null || !field.Current.BoundingRectangle.Contains(location))
        {
            if (trace)
                Console.WriteLine(field is null ? "Focused element has no writable text control."
                    : "Tap is outside the focused control's bounds.");
            return null;
        }

        var hit = AutomationElement.FromPoint(location);
        var hitField = EditableAncestor(hit);
        if (hitField is not null)
        {
            if (!Automation.Compare(hitField, field))
                return null;
        }
        else if (hit is null || !IsAncestorOf(hit, field))
        {
            // Some native editors expose the focus through UI Automation but report only their host on hit-test.
            if (Native.GetAncestor(Native.WindowFromPoint(point), 2) != window
                || hit?.Current.ProcessId != field.Current.ProcessId)
                return null;
        }

        return version == Interlocked.Read(ref focusVersion) && window == Native.GetForegroundWindow()
            ? (field, window, version) : null;
    }

    private static AutomationElement? EditableAncestor(AutomationElement? element)
    {
        for (var node = element; node is not null; node = TreeWalker.RawViewWalker.GetParent(node))
        {
            if (IsEditable(node, requireFocus: false))
                return node;
        }
        return null;
    }

    private static bool IsAncestorOf(AutomationElement ancestor, AutomationElement element)
    {
        for (var node = TreeWalker.RawViewWalker.GetParent(element); node is not null;
             node = TreeWalker.RawViewWalker.GetParent(node))
        {
            if (Automation.Compare(ancestor, node))
                return true;
        }
        return false;
    }

    private static bool IsEditable(AutomationElement? element, bool requireFocus = true)
    {
        if (element is null || !element.Current.IsEnabled || (requireFocus && !element.Current.HasKeyboardFocus))
            return false;
        var control = element.Current.ControlType;
        if (control != ControlType.Edit && control != ControlType.Document)
            return false;
        if (element.TryGetCurrentPattern(ValuePattern.Pattern, out var value))
            return !((ValuePattern)value).Current.IsReadOnly;
        if (!element.TryGetCurrentPattern(TextPattern.Pattern, out var text))
            return false;
        var pattern = (TextPattern)text;
        var selected = pattern.SupportedTextSelection == SupportedTextSelection.None
            ? null : pattern.GetSelection().FirstOrDefault();
        object? readOnly = (selected ?? pattern.DocumentRange).GetAttributeValue(TextPattern.IsReadOnlyAttribute);
        return Equals(readOnly, false);
    }
}
