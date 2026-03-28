using System.Windows;
using System.Windows.Media;

namespace WpfMusicPlayer.Helpers;

public class SpectrumVisualizerControl : FrameworkElement
{
    private const float SmoothRise = 20.0f;
    private const float SmoothFall = 4.5f;
    private const float PeakHoldTime = 0.2f;
    private const float PeakFallAcceleration = 2.0f;

    private int _segmentCount;
    private float[] _displayValues = [];
    private float[] _peakValues = [];
    private float[] _peakHoldTimers = [];
    private float[] _peakFallSpeeds = [];
    private bool _renderHookActive;
    private TimeSpan _lastRenderTime;

    public static readonly DependencyProperty SpectrumDataProperty =
        DependencyProperty.Register(
            nameof(SpectrumData),
            typeof(float[]),
            typeof(SpectrumVisualizerControl),
            new PropertyMetadata(null, OnSpectrumDataChanged));

    public static readonly DependencyProperty BarBrushProperty =
        DependencyProperty.Register(
            nameof(BarBrush),
            typeof(Brush),
            typeof(SpectrumVisualizerControl),
            new FrameworkPropertyMetadata(Brushes.White, FrameworkPropertyMetadataOptions.AffectsRender));

    public static readonly DependencyProperty BarSpacingProperty =
        DependencyProperty.Register(
            nameof(BarSpacing),
            typeof(double),
            typeof(SpectrumVisualizerControl),
            new FrameworkPropertyMetadata(2.0, FrameworkPropertyMetadataOptions.AffectsRender));

    public static readonly DependencyProperty BarCornerRadiusProperty =
        DependencyProperty.Register(
            nameof(BarCornerRadius),
            typeof(double),
            typeof(SpectrumVisualizerControl),
            new FrameworkPropertyMetadata(2.0, FrameworkPropertyMetadataOptions.AffectsRender));

    public static readonly DependencyProperty PeakBrushProperty =
        DependencyProperty.Register(
            nameof(PeakBrush),
            typeof(Brush),
            typeof(SpectrumVisualizerControl),
            new FrameworkPropertyMetadata(
                new SolidColorBrush(Color.FromArgb(180, 255, 255, 255)),
                FrameworkPropertyMetadataOptions.AffectsRender));

    public float[]? SpectrumData
    {
        get => (float[]?)GetValue(SpectrumDataProperty);
        set => SetValue(SpectrumDataProperty, value);
    }

    public Brush BarBrush
    {
        get => (Brush)GetValue(BarBrushProperty);
        set => SetValue(BarBrushProperty, value);
    }

    public double BarSpacing
    {
        get => (double)GetValue(BarSpacingProperty);
        set => SetValue(BarSpacingProperty, value);
    }

    public double BarCornerRadius
    {
        get => (double)GetValue(BarCornerRadiusProperty);
        set => SetValue(BarCornerRadiusProperty, value);
    }

    public Brush PeakBrush
    {
        get => (Brush)GetValue(PeakBrushProperty);
        set => SetValue(PeakBrushProperty, value);
    }

    public SpectrumVisualizerControl()
    {
        Loaded += (_, _) => StartRendering();
        Unloaded += (_, _) => StopRendering();
    }

    private static void OnSpectrumDataChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        var ctrl = (SpectrumVisualizerControl)d;
        if (e.NewValue is float[] { Length: > 0 } data && data.Length != ctrl._segmentCount)
        {
            ctrl.ResizeArrays(data.Length);
        }
    }

    private void ResizeArrays(int count)
    {
        _segmentCount = count;
        Array.Resize(ref _displayValues, count);
        Array.Resize(ref _peakValues, count);
        Array.Resize(ref _peakHoldTimers, count);
        Array.Resize(ref _peakFallSpeeds, count);
    }

    private void StartRendering()
    {
        if (_renderHookActive) return;
        _renderHookActive = true;
        _lastRenderTime = TimeSpan.Zero;
        CompositionTarget.Rendering += OnCompositionRendering;
    }

    private void StopRendering()
    {
        if (!_renderHookActive) return;
        _renderHookActive = false;
        CompositionTarget.Rendering -= OnCompositionRendering;
    }

    private void OnCompositionRendering(object? sender, EventArgs e)
    {
        if (e is not RenderingEventArgs args) return;

        // ~60fps
        if (_lastRenderTime != TimeSpan.Zero
            && args.RenderingTime - _lastRenderTime < TimeSpan.FromMilliseconds(15))
            return;

        var dt = _lastRenderTime == TimeSpan.Zero
            ? 1f / 60f
            : (float)(args.RenderingTime - _lastRenderTime).TotalSeconds;
        _lastRenderTime = args.RenderingTime;
        if (dt > 0.1f) dt = 1f / 60f;

        if (_segmentCount == 0) return;

        var target = SpectrumData;
        bool anyActive = false;

        for (int i = 0; i < _segmentCount; i++)
        {
            float targetVal = (target is { Length: > 0 } && i < target.Length)
                ? Math.Clamp(target[i], 0f, 1f) : 0f;
            float current = _displayValues[i];

            float speed = targetVal > current ? SmoothRise : SmoothFall;
            float newVal = current + (targetVal - current) * Math.Min(1f, speed * dt);
            if (newVal < 0.0005f) newVal = 0f;
            _displayValues[i] = newVal;

            // peak tracking
            if (newVal >= _peakValues[i])
            {
                _peakValues[i] = newVal;
                _peakHoldTimers[i] = PeakHoldTime;
                _peakFallSpeeds[i] = 0;
            }
            else if (_peakHoldTimers[i] > 0)
            {
                _peakHoldTimers[i] -= dt;
            }
            else
            {
                _peakFallSpeeds[i] += PeakFallAcceleration * dt;
                _peakValues[i] -= _peakFallSpeeds[i] * dt;
                if (_peakValues[i] < 0) _peakValues[i] = 0;
            }

            if (newVal > 0.001f || _peakValues[i] > 0.001f)
                anyActive = true;
        }

        if (anyActive)
            InvalidateVisual();
    }

    protected override void OnRender(DrawingContext dc)
    {
        base.OnRender(dc);

        double w = ActualWidth, h = ActualHeight;
        if (w <= 0 || h <= 0 || _segmentCount == 0) return;

        double spacing = BarSpacing;
        double barW = (w - spacing * (_segmentCount - 1)) / _segmentCount;
        if (barW < 1) barW = 1;
        double cr = BarCornerRadius;


        // main bars
        var brush = BarBrush;
        for (int i = 0; i < _segmentCount; i++)
        {
            double bh = _displayValues[i] * h;
            if (bh < 0.5) continue;
            double x = i * (barW + spacing);
            double y = h - bh;
            dc.DrawRoundedRectangle(brush, null, new Rect(x, y, barW, bh), cr, cr);
        }

        // peak indicators
        var peakBrush = PeakBrush;
        for (int i = 0; i < _segmentCount; i++)
        {
            if (_peakValues[i] < 0.01f) continue;
            double py = h - _peakValues[i] * h;
            double x = i * (barW + spacing);
            dc.DrawRoundedRectangle(peakBrush, null,
                new Rect(x, py, barW, 2), 1, 1);
        }
    }
}
