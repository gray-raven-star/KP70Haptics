using System.Globalization;
using System.Windows;
using System.Windows.Input;
using System.Windows.Media;
using Brush = System.Windows.Media.Brush;
using Color = System.Windows.Media.Color;
using KeyEventArgs = System.Windows.Input.KeyEventArgs;
using MouseEventArgs = System.Windows.Input.MouseEventArgs;
using Pen = System.Windows.Media.Pen;
using Point = System.Windows.Point;

namespace KP70Studio;

/// <summary>Editable frequency-weight curve. Points can be dragged, added
/// (double-click) or removed (right-click); the engine still receives the
/// seven fixed band values, resampled from the point list.</summary>
public sealed class FrequencyCurve : FrameworkElement
{
    internal const int BandCount = 7;
    static readonly string[] Labels = ["40 Hz", "100", "250", "630", "1.6 k", "4 k", "10 k"];
    static Brush Background => Palette.Brush("Inset");
    static Brush Label => Palette.Brush("Muted");
    static Brush Accent => Palette.Brush("Accent");
    static Brush Inactive => Palette.Brush("Muted");
    static Pen GridLine => new(Palette.Brush("Border"), 1);

    readonly List<Point> points = new();   // x,y in 0..1 (y = 1 is top)
    int drag = -1;
    bool active;

    internal event Action<int[]>? Edited;

    public FrequencyCurve()
    {
        Focusable = true;
        Cursor = System.Windows.Input.Cursors.Cross;
    }

    internal double[][] GetPoints() => points.Select(point => new[] { point.X, point.Y }).ToArray();

    internal void SetValues(int[] curve, bool enabled, double[][]? savedPoints = null)
    {
        points.Clear();
        if (enabled && savedPoints is { Length: >= 3 and <= 24 } &&
            savedPoints.All(point => point is { Length: 2 } && point.All(value => value >= 0 && value <= 1)) &&
            savedPoints.Zip(savedPoints.Skip(1)).All(pair => pair.Second[0] - pair.First[0] >= 0.009999))
        {
            points.AddRange(savedPoints.Select(point => new Point(point[0], point[1])));
        }
        else
        {
            for (int i = 0; i < BandCount; i++)
                points.Add(new Point((double)i / (BandCount - 1), Math.Clamp(curve[i], 0, 100) / 100.0));
        }
        active = enabled;
        InvalidateVisual();
    }

    int[] Resample()
    {
        var result = new int[BandCount];
        for (int band = 0; band < BandCount; band++)
        {
            double x = (double)band / (BandCount - 1);
            result[band] = (int)Math.Round(ValueAt(x) * 100);
        }
        return result;
    }

    double ValueAt(double x)
    {
        if (points.Count == 0) return 0;
        if (x <= points[0].X) return points[0].Y;
        for (int i = 1; i < points.Count; i++)
        {
            if (x > points[i].X) continue;
            var a = points[i - 1];
            var b = points[i];
            double span = Math.Max(1e-6, b.X - a.X);
            return a.Y + (b.Y - a.Y) * (x - a.X) / span;
        }
        return points[^1].Y;
    }

    Rect Plot => new(39, 14, Math.Max(1, ActualWidth - 64), Math.Max(1, ActualHeight - 43));

    Point ToScreen(Rect plot, Point p) =>
        new(plot.Left + plot.Width * p.X, plot.Bottom - plot.Height * p.Y);

    Point ToUnit(Rect plot, Point p) => new(
        Math.Clamp((p.X - plot.Left) / plot.Width, 0, 1),
        Math.Clamp((plot.Bottom - p.Y) / plot.Height, 0, 1));

    protected override void OnRender(DrawingContext drawing)
    {
        var plot = Plot;
        drawing.DrawRoundedRectangle(Background, null, new Rect(RenderSize), 9, 9);
        for (int i = 0; i <= 2; i++)
        {
            double y = plot.Top + plot.Height * i / 2;
            drawing.DrawLine(GridLine, new Point(plot.Left, y), new Point(plot.Right, y));
            Text(drawing, (100 - i * 50).ToString(CultureInfo.InvariantCulture), 5, y - 7, false);
        }
        var ink = active ? Accent : Inactive;
        var line = new Pen(ink, 2);
        for (int i = 0; i < BandCount; i++)
        {
            double x = plot.Left + plot.Width * i / (BandCount - 1);
            drawing.DrawLine(GridLine, new Point(x, plot.Top), new Point(x, plot.Bottom));
            Text(drawing, Labels[i], x, plot.Bottom + 7, true);
        }
        for (int i = 1; i < points.Count; i++)
            drawing.DrawLine(line, ToScreen(plot, points[i - 1]), ToScreen(plot, points[i]));
        foreach (var p in points)
        {
            var screen = ToScreen(plot, p);
            drawing.DrawEllipse(Background, line, screen, 4.5, 4.5);
        }
    }

    void Text(DrawingContext drawing, string text, double x, double y, bool centred)
    {
        var formatted = new FormattedText(text, CultureInfo.CurrentUICulture, System.Windows.FlowDirection.LeftToRight,
            new Typeface("Segoe UI"), 11, Label, VisualTreeHelper.GetDpi(this).PixelsPerDip);
        drawing.DrawText(formatted, new Point(centred ? x - formatted.Width / 2 : x, y));
    }

    void Changed()
    {
        active = true;
        InvalidateVisual();
        Edited?.Invoke(Resample());
    }

    int HitTest(Point unit)
    {
        for (int i = 0; i < points.Count; i++)
        {
            var d = points[i] - unit;
            if (d.Length < 0.035) return i;
        }
        return -1;
    }

    protected override void OnMouseLeftButtonDown(MouseButtonEventArgs e)
    {
        Focus();
        var plot = Plot;
        var unit = ToUnit(plot, e.GetPosition(this));
        if (e.ClickCount == 2)
        {
            if (points.Count < 24 && points.All(point => Math.Abs(point.X - unit.X) >= .01))
            {
                points.Add(unit);
                points.Sort((a, b) => a.X.CompareTo(b.X));
                drag = points.FindIndex(p => Math.Abs(p.X - unit.X) < 1e-6 && Math.Abs(p.Y - unit.Y) < 1e-6);
                Changed();
            }
            e.Handled = true;
            return;
        }
        drag = HitTest(unit);
        if (drag < 0) return;
        CaptureMouse();
        e.Handled = true;
    }

    protected override void OnMouseMove(MouseEventArgs e)
    {
        if (drag < 0 || !IsMouseCaptured) return;
        var plot = Plot;
        var unit = ToUnit(plot, e.GetPosition(this));
        double minX = drag > 0 ? points[drag - 1].X + 0.01 : 0;
        double maxX = drag < points.Count - 1 ? points[drag + 1].X - 0.01 : 1;
        points[drag] = new Point(minX <= maxX ? Math.Clamp(unit.X, minX, maxX) : points[drag].X, unit.Y);
        Changed();
    }

    protected override void OnMouseLeftButtonUp(MouseButtonEventArgs e)
    {
        if (IsMouseCaptured) ReleaseMouseCapture();
        drag = -1;
        e.Handled = true;
    }

    protected override void OnMouseRightButtonDown(MouseButtonEventArgs e)
    {
        var unit = ToUnit(Plot, e.GetPosition(this));
        int hit = HitTest(unit);
        if (hit > 0 && hit < points.Count - 1 && points.Count > 3)
        {
            points.RemoveAt(hit);
            Changed();
        }
        e.Handled = true;
    }
}
