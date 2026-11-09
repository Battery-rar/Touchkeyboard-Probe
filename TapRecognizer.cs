internal sealed class TapRecognizer
{
    private const int MaxTravel = 24;
    private const long MaxDuration = 550;
    private Native.Point? start;
    private Native.Point last;
    private long startedAt;
    private bool cancelled;

    internal (bool Completed, Native.Point? Tap) Observe(int contacts, Native.Point? point, long now)
    {
        if (contacts == 0)
        {
            bool completed = start.HasValue || cancelled;
            Native.Point? tap = start.HasValue && !cancelled && now - startedAt <= MaxDuration ? last : null;
            start = null;
            cancelled = false;
            return (completed, tap);
        }

        if (contacts > 1 || !point.HasValue)
            cancelled = true;
        if (cancelled)
            return default;

        last = point!.Value;
        if (start is not { } first)
        {
            start = last;
            startedAt = now;
        }
        else if (Math.Abs((long)last.X - first.X) > MaxTravel
            || Math.Abs((long)last.Y - first.Y) > MaxTravel)
        {
            cancelled = true;
        }
        return default;
    }
}
