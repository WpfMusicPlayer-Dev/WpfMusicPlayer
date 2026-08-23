// SPDX-License-Identifier: MIT

using System.Collections.Specialized;
using System.ComponentModel;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Media.Animation;
using WpfMusicPlayer.Helpers;
using WpfMusicPlayer.ViewModels;

namespace WpfMusicPlayer.Views;

public sealed class LyricsD2DControl : Grid
{
    public static readonly DependencyProperty VerticalOffsetProperty =
        DependencyProperty.Register(
            nameof(VerticalOffset),
            typeof(double),
            typeof(LyricsD2DControl),
            new PropertyMetadata(0.0, OnVerticalOffsetChanged));

    public static readonly DependencyProperty ScrollBarStyleProperty =
        DependencyProperty.Register(
            nameof(ScrollBarStyle),
            typeof(Style),
            typeof(LyricsD2DControl),
            new PropertyMetadata(null, OnScrollBarStyleChanged));

    public static readonly DependencyProperty LineContextMenuProperty =
        DependencyProperty.Register(
            nameof(LineContextMenu),
            typeof(ContextMenu),
            typeof(LyricsD2DControl));

    private static readonly Color LyricNormal = Color.FromArgb(0x88, 0xDD, 0xDD, 0xDD);
    private static readonly Color LyricHighlight = Colors.White;
    private static readonly Color SecondaryNormal = Color.FromArgb(0x66, 0xDD, 0xDD, 0xDD);
    private static readonly Color SecondaryHighlight = Color.FromArgb(0xBB, 0xDD, 0xDD, 0xDD);
    private static readonly Color HoverFill = Color.FromArgb(0x2A, 0xFF, 0xFF, 0xFF);

    private readonly Image _image = new()
    {
        Stretch = Stretch.Fill,
        SnapsToDevicePixels = true
    };

    private readonly ScrollBar _scrollBar = new()
    {
        Orientation = Orientation.Vertical,
        HorizontalAlignment = HorizontalAlignment.Right,
        VerticalAlignment = VerticalAlignment.Stretch,
        Width = 8,
        Minimum = 0,
        ViewportSize = 1
    };

    private D2DWpfSurface? _surface;
    private LyricsViewModel? _viewModel;
    private bool _renderHookActive;
    private bool _dirty = true;
    private bool _layoutDirty = true;
    private bool _updatingScrollBar;
    private bool _pointerDown;
    private bool _isDragging;
    private Point _pointerDownPosition;
    private double _offsetAtPointerDown;
    private int _pendingCenterIndex = -1;
    private bool _autoScrollEnabled = true;
    private DateTime _lastUserScrollUtc = DateTime.MinValue;
    private int _hoverIndex = -1;
    private int _hoverPaintIndex = -1;
    private float _hoverAlpha;
    private long _hoverTick;

    private float[] _lineTops = [];
    private float[] _lineHeights = [];
    private float[] _textHeights = [];
    private float[] _translationHeights = [];
    private float[] _romanjiHeights = [];
    private float[] _displayFontSizes = [];
    private float[] _displaySecondaryFontSizes = [];
    private float[] _animFromFont = [];
    private float[] _animToFont = [];
    private float[] _animFromSecondary = [];
    private float[] _animToSecondary = [];
    private bool[] _animating = [];
    private long[] _animStartTicks = [];
    private bool[] _lastHighlighted = [];
    private LyricsLayoutEngine.LyricSizeMetrics[] _focusedMetrics = [];
    // Per wrapped-line karaoke highlight widths (DIPs) for the currently highlighted line.
    // Recomputed only when the highlighted line's Progress/text/layout changes.
    private float[] _karaokeLineWidths = [];
    private int _karaokeLineIndex = -1;
    private double _karaokeProgress = -1;
    private string? _karaokeText;
    private float _karaokeLayoutWidth = -1f;
    private double _contentHeight;
    private float _layoutWidth;
    private float _cachedLayoutWidth;
    private bool _cachedShowTranslation;
    private bool _cachedShowRomanji;
    private bool _metricsCacheDirty = true;
    private bool _surfaceFailed;

    public LyricsD2DControl()
    {
        Background = Brushes.Transparent;
        ClipToBounds = true;
        Focusable = true;
        _image.HorizontalAlignment = HorizontalAlignment.Stretch;
        _image.VerticalAlignment = VerticalAlignment.Stretch;
        RenderOptions.SetBitmapScalingMode(_image, BitmapScalingMode.NearestNeighbor);
        Children.Add(_image);
        Children.Add(_scrollBar);

        _scrollBar.ValueChanged += OnScrollBarValueChanged;
        Loaded += OnLoaded;
        Unloaded += OnUnloaded;
        SizeChanged += OnSizeChanged;
        DataContextChanged += OnDataContextChanged;
    }

    public double VerticalOffset
    {
        get => (double)GetValue(VerticalOffsetProperty);
        set => SetValue(VerticalOffsetProperty, value);
    }

    public Style? ScrollBarStyle
    {
        get => (Style?)GetValue(ScrollBarStyleProperty);
        set => SetValue(ScrollBarStyleProperty, value);
    }

    public ContextMenu? LineContextMenu
    {
        get => (ContextMenu?)GetValue(LineContextMenuProperty);
        set => SetValue(LineContextMenuProperty, value);
    }

    public void ScrollLyricToCenter(int index)
    {
        if (!_autoScrollEnabled)
        {
            if (!LyricsLayoutEngine.ShouldResumeAutoFollow(
                    true, DateTime.UtcNow - _lastUserScrollUtc))
            {
                _pendingCenterIndex = index;
                return;
            }

            _autoScrollEnabled = true;
        }

        _pendingCenterIndex = index;
        EnsureLayout();
        if (_lineHeights.Length == 0 || index < 0 || index >= _lineHeights.Length)
            return;

        var target = LyricsLayoutEngine.ComputeCenterOffset(
            _lineTops[index],
            _lineHeights[index],
            ActualHeight,
            _contentHeight);
        AnimateScrollTo(target);
        _pendingCenterIndex = -1;
    }

    protected override void OnMouseWheel(MouseWheelEventArgs e)
    {
        SuspendAutoScroll();
        BeginAnimation(VerticalOffsetProperty, null);
        VerticalOffset = LyricsLayoutEngine.ClampOffset(
            VerticalOffset - e.Delta,
            ActualHeight,
            _contentHeight);
        e.Handled = true;
        base.OnMouseWheel(e);
    }

    protected override void OnMouseMove(MouseEventArgs e)
    {
        var pos = e.GetPosition(this);
        if (_pointerDown && IsMouseCaptured)
        {
            var dy = pos.Y - _pointerDownPosition.Y;
            if (!_isDragging && Math.Abs(dy) >= 6)
            {
                _isDragging = true;
                SuspendAutoScroll();
            }

            if (_isDragging)
            {
                BeginAnimation(VerticalOffsetProperty, null);
                VerticalOffset = LyricsLayoutEngine.ClampOffset(
                    _offsetAtPointerDown - dy,
                    ActualHeight,
                    _contentHeight);
            }
        }

        if (!_isDragging)
            UpdateHoverIndex(HitTest(pos.Y));

        base.OnMouseMove(e);
    }

    protected override void OnMouseLeave(MouseEventArgs e)
    {
        if (!_isDragging)
            UpdateHoverIndex(-1);
        base.OnMouseLeave(e);
    }

    protected override void OnMouseLeftButtonDown(MouseButtonEventArgs e)
    {
        _pointerDown = true;
        _isDragging = false;
        _pointerDownPosition = e.GetPosition(this);
        _offsetAtPointerDown = VerticalOffset;
        CaptureMouse();
        Focus();
        base.OnMouseLeftButtonDown(e);
    }

    protected override void OnMouseLeftButtonUp(MouseButtonEventArgs e)
    {
        if (_pointerDown)
        {
            var dragged = _isDragging;
            _pointerDown = false;
            _isDragging = false;
            if (IsMouseCaptured)
                ReleaseMouseCapture();

            if (!dragged && _viewModel is not null)
            {
                var pos = e.GetPosition(this);
                if ((pos - _pointerDownPosition).Length < 6)
                {
                    var index = HitTest(pos.Y);
                    if (index >= 0 && index < _viewModel.Lyrics.Count)
                        _viewModel.SeekToLyric(_viewModel.Lyrics[index]);
                }
            }
        }

        base.OnMouseLeftButtonUp(e);
    }

    protected override void OnLostMouseCapture(MouseEventArgs e)
    {
        _pointerDown = false;
        _isDragging = false;
        base.OnLostMouseCapture(e);
    }

    protected override void OnMouseRightButtonUp(MouseButtonEventArgs e)
    {
        var index = HitTest(e.GetPosition(this).Y);
        if (index >= 0 && _viewModel is not null && index < _viewModel.Lyrics.Count && LineContextMenu is { } menu)
        {
            menu.DataContext = _viewModel.Lyrics[index];
            menu.PlacementTarget = this;
            menu.IsOpen = true;
            e.Handled = true;
        }

        base.OnMouseRightButtonUp(e);
    }

    protected override void OnDpiChanged(DpiScale oldDpi, DpiScale newDpi)
    {
        base.OnDpiChanged(oldDpi, newDpi);
        _metricsCacheDirty = true;
        _layoutDirty = true;
        _dirty = true;
        ResizeSurface();
    }

    private void OnLoaded(object sender, RoutedEventArgs e)
    {
        AttachViewModel(DataContext as LyricsViewModel);
        CreateSurface();
        StartRendering();
        _layoutDirty = true;
        _dirty = true;
    }

    private void OnUnloaded(object sender, RoutedEventArgs e)
    {
        StopRendering();
        DetachViewModel();
        DisposeSurface();
    }

    private void OnSizeChanged(object sender, SizeChangedEventArgs e)
    {
        _metricsCacheDirty = true;
        _layoutDirty = true;
        _dirty = true;
        ResizeSurface();
        if (_autoScrollEnabled && _pendingCenterIndex >= 0)
            ScrollLyricToCenter(_pendingCenterIndex);
        else
            VerticalOffset = LyricsLayoutEngine.ClampOffset(VerticalOffset, ActualHeight, _contentHeight);
    }

    private void OnDataContextChanged(object sender, DependencyPropertyChangedEventArgs e)
    {
        AttachViewModel(e.NewValue as LyricsViewModel);
        _layoutDirty = true;
        _dirty = true;
    }

    private void AttachViewModel(LyricsViewModel? vm)
    {
        if (ReferenceEquals(_viewModel, vm))
            return;

        DetachViewModel();
        _viewModel = vm;
        if (vm is null)
            return;

        vm.PropertyChanged += OnViewModelPropertyChanged;
        vm.Lyrics.CollectionChanged += OnLyricsCollectionChanged;
        foreach (var line in vm.Lyrics)
            line.PropertyChanged += OnLyricLinePropertyChanged;
        ResetLineState(animate: false);
    }

    private void DetachViewModel()
    {
        if (_viewModel is null)
            return;

        _viewModel.PropertyChanged -= OnViewModelPropertyChanged;
        _viewModel.Lyrics.CollectionChanged -= OnLyricsCollectionChanged;
        foreach (var line in _viewModel.Lyrics)
            line.PropertyChanged -= OnLyricLinePropertyChanged;
        _viewModel = null;
    }

    private void OnViewModelPropertyChanged(object? sender, PropertyChangedEventArgs e)
    {
        if (e.PropertyName is nameof(LyricsViewModel.IsTranslationVisible)
            or nameof(LyricsViewModel.IsRomanjiVisible))
        {
            _metricsCacheDirty = true;
            _layoutDirty = true;
            _dirty = true;
            return;
        }

        if (e.PropertyName == nameof(LyricsViewModel.CurrentLyricIndex))
            _dirty = true;
    }

    private void OnLyricsCollectionChanged(object? sender, NotifyCollectionChangedEventArgs e)
    {
        if (e.OldItems is not null)
        {
            foreach (var item in e.OldItems)
            {
                if (item is LyricLineViewModel line)
                    line.PropertyChanged -= OnLyricLinePropertyChanged;
            }
        }

        if (e.NewItems is not null)
        {
            foreach (var item in e.NewItems)
            {
                if (item is LyricLineViewModel line)
                    line.PropertyChanged += OnLyricLinePropertyChanged;
            }
        }

        if (e.Action == NotifyCollectionChangedAction.Reset && _viewModel is not null)
        {
            foreach (var line in _viewModel.Lyrics)
                line.PropertyChanged += OnLyricLinePropertyChanged;
        }

        ResetLineState(animate: false);
        _metricsCacheDirty = true;
        _layoutDirty = true;
        _dirty = true;
    }

    private void OnLyricLinePropertyChanged(object? sender, PropertyChangedEventArgs e)
    {
        if (e.PropertyName == nameof(LyricLineViewModel.Progress))
        {
            // Karaoke progress advanced; only the highlighted line repaints.
            _dirty = true;
            return;
        }

        if (e.PropertyName != nameof(LyricLineViewModel.IsHighlighted))
            return;
        if (sender is not LyricLineViewModel line || _viewModel is null)
            return;

        var index = _viewModel.Lyrics.IndexOf(line);
        if (index >= 0)
            StartFontAnimation(index, line.IsHighlighted);
        _dirty = true;
    }

    private void ResetLineState(bool animate)
    {
        var count = _viewModel?.Lyrics.Count ?? 0;
        Array.Resize(ref _lineTops, count);
        Array.Resize(ref _lineHeights, count);
        Array.Resize(ref _textHeights, count);
        Array.Resize(ref _translationHeights, count);
        Array.Resize(ref _romanjiHeights, count);
        Array.Resize(ref _displayFontSizes, count);
        Array.Resize(ref _displaySecondaryFontSizes, count);
        Array.Resize(ref _animFromFont, count);
        Array.Resize(ref _animToFont, count);
        Array.Resize(ref _animFromSecondary, count);
        Array.Resize(ref _animToSecondary, count);
        Array.Resize(ref _animating, count);
        Array.Resize(ref _animStartTicks, count);
        Array.Resize(ref _lastHighlighted, count);
        Array.Resize(ref _focusedMetrics, count);
        _metricsCacheDirty = true;

        for (var i = 0; i < count; i++)
        {
            var highlighted = _viewModel!.Lyrics[i].IsHighlighted;
            _lastHighlighted[i] = highlighted;
            _displayFontSizes[i] = highlighted
                ? LyricsLayoutEngine.HighlightFontSize
                : LyricsLayoutEngine.NormalFontSize;
            _displaySecondaryFontSizes[i] = highlighted
                ? LyricsLayoutEngine.SecondaryHighlightFontSize
                : LyricsLayoutEngine.SecondaryNormalFontSize;
            _animating[i] = false;
            if (animate)
                StartFontAnimation(i, highlighted);
        }
    }

    private void StartFontAnimation(int index, bool highlighted)
    {
        if ((uint)index >= (uint)_displayFontSizes.Length)
            return;

        _animFromFont[index] = _displayFontSizes[index];
        _animToFont[index] = highlighted
            ? LyricsLayoutEngine.HighlightFontSize
            : LyricsLayoutEngine.NormalFontSize;
        _animFromSecondary[index] = _displaySecondaryFontSizes[index];
        _animToSecondary[index] = highlighted
            ? LyricsLayoutEngine.SecondaryHighlightFontSize
            : LyricsLayoutEngine.SecondaryNormalFontSize;
        _animStartTicks[index] = DateTime.UtcNow.Ticks;
        _animating[index] = Math.Abs(_animFromFont[index] - _animToFont[index]) > 0.01f
                            || Math.Abs(_animFromSecondary[index] - _animToSecondary[index]) > 0.01f;
        _lastHighlighted[index] = highlighted;
        _layoutDirty = true;
        _dirty = true;
    }

    private bool TickAnimations()
    {
        var any = false;
        var now = DateTime.UtcNow.Ticks;
        for (var i = 0; i < _animating.Length; i++)
        {
            if (!_animating[i])
                continue;

            var elapsed = (now - _animStartTicks[i]) / (double)TimeSpan.TicksPerSecond;
            var t = (float)(elapsed / LyricsLayoutEngine.AnimationDurationSeconds);
            if (t >= 1f)
            {
                _displayFontSizes[i] = _animToFont[i];
                _displaySecondaryFontSizes[i] = _animToSecondary[i];
                _animating[i] = false;
            }
            else
            {
                _displayFontSizes[i] = LyricsLayoutEngine.AnimateToward(_animFromFont[i], _animToFont[i], t);
                _displaySecondaryFontSizes[i] = LyricsLayoutEngine.AnimateToward(
                    _animFromSecondary[i],
                    _animToSecondary[i],
                    t);
                any = true;
            }

            _layoutDirty = true;
            _dirty = true;
        }

        return any;
    }

    private void CreateSurface()
    {
        if (_surfaceFailed)
            return;

        DisposeSurface();
        try
        {
            _surface = new D2DWpfSurface();
            ResizeSurface();
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine(ex);
            _surfaceFailed = true;
            DisposeSurface();
        }
    }

    private void DisposeSurface()
    {
        if (_surface is null)
            return;

        _image.Source = null;
        _surface.Dispose();
        _surface = null;
    }

    private void ResizeSurface()
    {
        if (_surface is null || ActualWidth <= 0 || ActualHeight <= 0)
            return;

        var dpi = VisualTreeHelper.GetDpi(this);
        var width = Math.Max(1, (int)Math.Ceiling(ActualWidth * dpi.DpiScaleX));
        var height = Math.Max(1, (int)Math.Ceiling(ActualHeight * dpi.DpiScaleY));
        try
        {
            if (_surface.EnsureSize(width, height, (float)dpi.PixelsPerInchX, (float)dpi.PixelsPerInchY))
            {
                _metricsCacheDirty = true;
                _layoutDirty = true;
                _dirty = true;
            }

            if (!ReferenceEquals(_image.Source, _surface.Bitmap))
                _image.Source = _surface.Bitmap;
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine(ex);
            _surfaceFailed = true;
            DisposeSurface();
        }
    }

    private void StartRendering()
    {
        if (_renderHookActive)
            return;
        _renderHookActive = true;
        CompositionTarget.Rendering += OnCompositionRendering;
    }

    private void StopRendering()
    {
        if (!_renderHookActive)
            return;
        _renderHookActive = false;
        CompositionTarget.Rendering -= OnCompositionRendering;
    }

    private void OnCompositionRendering(object? sender, EventArgs e)
    {
        if (_surfaceFailed)
            return;

        try
        {
            var animating = TickAnimations();
            var hoverAnimating = TickHover();
            if (!_dirty && !animating && !hoverAnimating)
                return;

            EnsureLayout();
            RenderFrame();
            _dirty = animating || hoverAnimating;
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine(ex);
            _dirty = true;
        }
    }

    private void EnsureLayout()
    {
        if (!_layoutDirty && !_metricsCacheDirty)
            return;

        var vm = _viewModel;
        var count = vm?.Lyrics.Count ?? 0;
        if (count != _lineHeights.Length)
            ResetLineState(animate: false);

        var width = Math.Max(1f, (float)ActualWidth
            - LyricsLayoutEngine.ContentPaddingLeft
            - LyricsLayoutEngine.ContentPaddingRight);
        _layoutWidth = width;
        if (vm is null || _surface is null || count == 0)
        {
            _layoutDirty = false;
            _metricsCacheDirty = count == 0;
            _contentHeight = 0;
            UpdateScrollBar();
            return;
        }

        var showTranslation = vm.IsTranslationVisible;
        var showRomanji = vm.IsRomanjiVisible;
        if (Math.Abs(width - _cachedLayoutWidth) > 0.5f
            || showTranslation != _cachedShowTranslation
            || showRomanji != _cachedShowRomanji)
        {
            _metricsCacheDirty = true;
        }

        if (_metricsCacheDirty)
            RebuildMetricsCache(vm, width, showTranslation, showRomanji);

        for (var i = 0; i < count; i++)
        {
            var mainScale = LyricsLayoutEngine.FontScale(
                _displayFontSizes[i], LyricsLayoutEngine.HighlightFontSize);
            var secondaryScale = LyricsLayoutEngine.FontScale(
                _displaySecondaryFontSizes[i], LyricsLayoutEngine.SecondaryHighlightFontSize);
            LyricsLayoutEngine.ScaleFocusedMetrics(
                _focusedMetrics[i],
                mainScale,
                secondaryScale,
                out _textHeights[i],
                out _translationHeights[i],
                out _romanjiHeights[i],
                out _lineHeights[i],
                out _,
                out _,
                out _);
        }

        float y = 0;
        for (var i = 0; i < count; i++)
        {
            _lineTops[i] = y;
            y += _lineHeights[i];
        }

        _contentHeight = y;
        var clamped = LyricsLayoutEngine.ClampOffset(VerticalOffset, ActualHeight, _contentHeight);
        if (Math.Abs(clamped - VerticalOffset) > 0.5)
            VerticalOffset = clamped;
        UpdateScrollBar();
        _layoutDirty = false;
    }

    private void RebuildMetricsCache(
        LyricsViewModel vm,
        float width,
        bool showTranslation,
        bool showRomanji)
    {
        var count = vm.Lyrics.Count;
        if (_focusedMetrics.Length != count)
            Array.Resize(ref _focusedMetrics, count);

        for (var i = 0; i < count; i++)
        {
            _focusedMetrics[i] = MeasureFocusedLine(
                vm.Lyrics[i],
                showTranslation,
                showRomanji,
                width);
        }

        _cachedLayoutWidth = width;
        _cachedShowTranslation = showTranslation;
        _cachedShowRomanji = showRomanji;
        _metricsCacheDirty = false;
    }

    private LyricsLayoutEngine.LyricSizeMetrics MeasureFocusedLine(
        LyricLineViewModel line,
        bool showTranslation,
        bool showRomanji,
        float width)
    {
        if (_surface is null)
            return default;

        var text = _surface.MeasureText(
            line.Text, LyricsLayoutEngine.HighlightFontSize, true, width);
        var translation = showTranslation && line.HasTranslation
            ? _surface.MeasureText(
                line.Translation ?? string.Empty, LyricsLayoutEngine.SecondaryHighlightFontSize, false, width)
            : (0f, 0f);
        var romanji = showRomanji && line.HasRomanji
            ? _surface.MeasureText(
                line.Romanji ?? string.Empty, LyricsLayoutEngine.SecondaryHighlightFontSize, false, width)
            : (0f, 0f);
        return new LyricsLayoutEngine.LyricSizeMetrics(
            text.Height,
            translation.Item1,
            romanji.Item1,
            text.Width,
            translation.Item2,
            romanji.Item2);
    }

    private void RenderFrame()
    {
        if (_surface is null || ActualWidth <= 0 || ActualHeight <= 0)
            return;

        if (!_surface.BeginDraw())
            return;

        try
        {
            _surface.ClearTransparent();
            var vm = _viewModel;
            if (vm is null)
                return;

            var offset = (float)VerticalOffset;
            var viewportHeight = (float)ActualHeight;
            var width = _layoutWidth;
            var textX = LyricsLayoutEngine.ContentPaddingLeft;
            var count = vm.Lyrics.Count;
            if (_hoverPaintIndex >= 0 && _hoverPaintIndex < count && _hoverAlpha > 0.01f)
            {
                var hoverTop = _lineTops[_hoverPaintIndex] - offset;
                DrawHoverBackground(hoverTop, _lineHeights[_hoverPaintIndex]);
            }

            for (var i = 0; i < count; i++)
            {
                var top = _lineTops[i] - offset;
                var bottom = top + _lineHeights[i];
                if (bottom < 0 || top > viewportHeight)
                    continue;

                var line = vm.Lyrics[i];
                var highlighted = line.IsHighlighted;
                var mainScale = LyricsLayoutEngine.FontScale(
                    _displayFontSizes[i], LyricsLayoutEngine.HighlightFontSize);
                var secondaryScale = LyricsLayoutEngine.FontScale(
                    _displaySecondaryFontSizes[i], LyricsLayoutEngine.SecondaryHighlightFontSize);
                var focused = i < _focusedMetrics.Length ? _focusedMetrics[i] : default;
                var y = top + LyricsLayoutEngine.ItemPaddingY;

                DrawMainLineText(i, line, highlighted, textX, y, width, focused.TextHeight, mainScale);
                y += _textHeights[i];

                if (_translationHeights[i] > 0 && line.Translation is not null)
                {
                    y += LyricsLayoutEngine.SecondaryLineGap;
                    DrawLineText(
                        line.Translation,
                        LyricsLayoutEngine.SecondaryHighlightFontSize,
                        false,
                        textX,
                        y,
                        width,
                        focused.TranslationHeight,
                        highlighted ? SecondaryHighlight : SecondaryNormal,
                        secondaryScale);
                    y += _translationHeights[i];
                }

                if (_romanjiHeights[i] > 0 && line.Romanji is not null)
                {
                    y += LyricsLayoutEngine.SecondaryLineGap;
                    DrawLineText(
                        line.Romanji,
                        LyricsLayoutEngine.SecondaryHighlightFontSize,
                        false,
                        textX,
                        y,
                        width,
                        focused.RomanjiHeight,
                        highlighted ? SecondaryHighlight : SecondaryNormal,
                        secondaryScale);
                }
            }
        }
        finally
        {
            _surface.EndDraw();
        }
    }

    private void DrawLineText(
        string text,
        float fontSize,
        bool bold,
        float x,
        float y,
        float width,
        float height,
        Color color,
        float scale)
    {
        if (_surface is null)
            return;

        _surface.DrawText(
            text,
            fontSize,
            bold,
            x,
            y,
            width,
            height,
            color.R / 255f,
            color.G / 255f,
            color.B / 255f,
            color.A / 255f,
            scale);
    }

    /// <summary>
    /// Draws the primary lyric text. For a highlighted line that carries karaoke progress
    /// this replicates the original karaoke behaviour: the dim base text is painted first,
    /// then the sung portion of every wrapped line is over-painted in the highlight colour,
    /// clipped per wrapped line to the length reported by DirectWrite hit-testing.
    /// </summary>
    private void DrawMainLineText(
        int index,
        LyricLineViewModel line,
        bool highlighted,
        float x,
        float y,
        float width,
        float height,
        float scale)
    {
        if (_surface is null)
            return;

        if (!highlighted || !line.IsProgressEnabled)
        {
            DrawLineText(
                line.Text,
                LyricsLayoutEngine.HighlightFontSize,
                true,
                x,
                y,
                width,
                height,
                highlighted ? LyricHighlight : LyricNormal,
                scale);
            return;
        }

        var progress = Math.Clamp(line.Progress, 0.0, 1.0);
        var lineWidths = GetKaraokeLineWidths(index, line, width, progress);

        _surface.DrawKaraokeText(
            line.Text,
            LyricsLayoutEngine.HighlightFontSize,
            true,
            x,
            y,
            width,
            height,
            scale,
            LyricNormal.R / 255f,
            LyricNormal.G / 255f,
            LyricNormal.B / 255f,
            LyricNormal.A / 255f,
            LyricHighlight.R / 255f,
            LyricHighlight.G / 255f,
            LyricHighlight.B / 255f,
            LyricHighlight.A / 255f,
            lineWidths);
    }

    private float[] GetKaraokeLineWidths(int index, LyricLineViewModel line, float width, double progress)
    {
        if (_surface is null)
            return [];

        // Recompute only when the highlighted line, its text, the layout width, or the
        // progress changes. This keeps the expensive DirectWrite hit-testing off the
        // per-frame path unless karaoke progress actually advances.
        if (_karaokeLineIndex == index
            && string.Equals(_karaokeText, line.Text, StringComparison.Ordinal)
            && Math.Abs(_karaokeProgress - progress) < 0.0005
            && Math.Abs(_karaokeLayoutWidth - width) < 0.5f
            && _karaokeLineWidths.Length > 0)
        {
            return _karaokeLineWidths;
        }

        _karaokeLineWidths = _surface.ComputeKaraokeLineWidths(
            line.Text,
            LyricsLayoutEngine.HighlightFontSize,
            true,
            width,
            progress);
        _karaokeLineIndex = index;
        _karaokeText = line.Text;
        _karaokeProgress = progress;
        _karaokeLayoutWidth = width;
        return _karaokeLineWidths;
    }

    private void DrawHoverBackground(float top, float height)
    {
        if (_surface is null || height <= 0)
            return;

        var alpha = HoverFill.A / 255f * _hoverAlpha;
        _surface.FillRoundedRectangle(
            8f,
            top,
            Math.Max(0f, (float)ActualWidth - 16f),
            height,
            8f,
            HoverFill.R / 255f,
            HoverFill.G / 255f,
            HoverFill.B / 255f,
            alpha);
    }

    private void UpdateHoverIndex(int index)
    {
        if (index == _hoverIndex)
            return;
        _hoverIndex = index;
        if (index >= 0)
            _hoverPaintIndex = index;
        Cursor = index >= 0 ? Cursors.Hand : Cursors.Arrow;
        _dirty = true;
    }

    private bool TickHover()
    {
        var target = _hoverIndex >= 0 ? 1f : 0f;
        if (Math.Abs(_hoverAlpha - target) < 0.01f)
        {
            _hoverAlpha = target;
            _hoverTick = 0;
            if (target <= 0f)
                _hoverPaintIndex = _hoverIndex;
            return false;
        }

        var now = DateTime.UtcNow.Ticks;
        var dt = _hoverTick == 0 ? 1.0 / 60.0 : (now - _hoverTick) / (double)TimeSpan.TicksPerSecond;
        _hoverTick = now;
        if (dt > 0.1)
            dt = 1.0 / 60.0;
        var step = (float)(dt / LyricsLayoutEngine.HoverFadeSeconds);
        if (target > _hoverAlpha)
            _hoverAlpha = Math.Min(target, _hoverAlpha + step);
        else
            _hoverAlpha = Math.Max(target, _hoverAlpha - step);
        return true;
    }

    private void SuspendAutoScroll()
    {
        _autoScrollEnabled = false;
        _lastUserScrollUtc = DateTime.UtcNow;
    }

    private int HitTest(double viewY)
    {
        EnsureLayout();
        return LyricsLayoutEngine.HitTestLine(_lineTops, _lineHeights, viewY + VerticalOffset);
    }

    private void AnimateScrollTo(double target)
    {
        target = LyricsLayoutEngine.ClampOffset(target, ActualHeight, _contentHeight);
        BeginAnimation(VerticalOffsetProperty, null);

        var animation = new DoubleAnimation
        {
            From = VerticalOffset,
            To = target,
            Duration = new Duration(TimeSpan.FromMilliseconds(LyricsLayoutEngine.ScrollAnimationMilliseconds)),
            EasingFunction = new CubicEase { EasingMode = EasingMode.EaseInOut }
        };
        animation.Completed += (_, _) =>
        {
            BeginAnimation(VerticalOffsetProperty, null);
            VerticalOffset = target;
        };
        BeginAnimation(VerticalOffsetProperty, animation);
    }

    private void UpdateScrollBar()
    {
        var viewport = Math.Max(1d, ActualHeight);
        var scrollable = Math.Max(0d, _contentHeight - ActualHeight);
        _updatingScrollBar = true;
        _scrollBar.Maximum = scrollable;
        _scrollBar.ViewportSize = viewport;
        _scrollBar.Value = VerticalOffset;
        _scrollBar.Visibility = scrollable > 0.5 ? Visibility.Visible : Visibility.Collapsed;
        _updatingScrollBar = false;
    }

    private void OnScrollBarValueChanged(object sender, RoutedPropertyChangedEventArgs<double> e)
    {
        if (_updatingScrollBar)
            return;
        SuspendAutoScroll();
        BeginAnimation(VerticalOffsetProperty, null);
        VerticalOffset = e.NewValue;
    }

    private static void OnVerticalOffsetChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        var control = (LyricsD2DControl)d;
        var clamped = LyricsLayoutEngine.ClampOffset((double)e.NewValue, control.ActualHeight, control._contentHeight);
        if (Math.Abs(clamped - (double)e.NewValue) > 0.01)
        {
            control.VerticalOffset = clamped;
            return;
        }

        control.UpdateScrollBar();
        control._dirty = true;
    }

    private static void OnScrollBarStyleChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        ((LyricsD2DControl)d)._scrollBar.Style = e.NewValue as Style;
    }
}
