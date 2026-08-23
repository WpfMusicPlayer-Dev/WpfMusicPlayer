// SPDX-License-Identifier: MIT

using System.Globalization;
using System.Windows;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using Silk.NET.Core.Native;
using Silk.NET.Direct2D;
using Silk.NET.DXGI;
using Silk.NET.Maths;
using D2DApi = Silk.NET.Direct2D.D2D;
// The Direct2D namespace ships minimal DWrite stubs; the full IDWriteTextLayout vtable
// (GetMetrics/GetLineMetrics/HitTestTextPosition/CreateTextLayout) lives in Silk.NET.DirectWrite.
using DWriteFactory = Silk.NET.DirectWrite.IDWriteFactory;
using DWriteTextLayout = Silk.NET.DirectWrite.IDWriteTextLayout;
using DWriteTextMetrics = Silk.NET.DirectWrite.TextMetrics;
using DWriteLineMetrics = Silk.NET.DirectWrite.LineMetrics;
using DWriteHitTestMetrics = Silk.NET.DirectWrite.HitTestMetrics;

namespace WpfMusicPlayer.Helpers;

internal sealed unsafe class D2DWpfSurface : IDisposable
{
    private const string FontFamilyName = "Segoe UI";

    private readonly D2DApi _d2d = D2DApi.GetApi();

    private ID2D1Factory* _d2dFactory;
    private ID2D1RenderTarget* _renderTarget;
    private void* _dwriteFactory;
    private void* _wicFactory;
    private void* _wicBitmap;
    private ID2D1SolidColorBrush* _brush;
    private readonly Dictionary<(int SizeKey, bool Bold), nint> _textFormats = [];

    // Reusable DirectWrite text-layout used both for measurement and for the
    // karaoke per-line hit-testing. Kept as a raw COM pointer so we can call
    // the full IDWriteTextLayout vtable (Silk.NET exposes it).
    private DWriteTextLayout* _measureLayout;
    private string? _measureLayoutText;
    private IDWriteTextFormat* _measureLayoutFormat;
    private float _measureLayoutWidth = -1f;

    private int _pixelWidth;
    private int _pixelHeight;
    private float _dpiX = 96f;
    private float _dpiY = 96f;
    private bool _drawing;
    private bool _disposed;

    public WriteableBitmap? Bitmap { get; private set; }

    public bool IsAvailable => _renderTarget != null && Bitmap != null;

    public bool EnsureSize(int pixelWidth, int pixelHeight, float dpiX, float dpiY)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);

        pixelWidth = Math.Max(1, pixelWidth);
        pixelHeight = Math.Max(1, pixelHeight);
        if (pixelWidth == _pixelWidth
            && pixelHeight == _pixelHeight
            && Math.Abs(dpiX - _dpiX) < 0.01f
            && Math.Abs(dpiY - _dpiY) < 0.01f
            && _renderTarget != null
            && Bitmap != null)
        {
            return false;
        }

        _pixelWidth = pixelWidth;
        _pixelHeight = pixelHeight;
        _dpiX = dpiX;
        _dpiY = dpiY;
        RecreateTarget();
        return true;
    }

    public bool BeginDraw()
    {
        if (_renderTarget == null)
            return false;

        _renderTarget->BeginDraw();
        _drawing = true;
        return true;
    }

    public void EndDraw()
    {
        if (!_drawing)
            return;

        _drawing = false;
        var hr = _renderTarget->EndDraw(null, null);
        if (hr == unchecked((int)0x8899000C))
        {
            RecreateTarget();
            return;
        }

        SilkMarshal.ThrowHResult(hr);
        CopyToBitmap();
    }

    public (float Height, float Width) MeasureText(string text, float fontSize, bool bold, float maxWidth)
    {
        var layout = EnsureMeasureLayout(text, fontSize, bold, maxWidth);
        if (layout == null)
            return (Math.Max(1f, fontSize), 0f);

        DWriteTextMetrics metrics;
        SilkMarshal.ThrowHResult(layout->GetMetrics(&metrics));
        return (
            Math.Max(metrics.Height, fontSize),
            Math.Max(metrics.WidthIncludingTrailingWhitespace, 0f));
    }

    public float MeasureTextHeight(string text, float fontSize, bool bold, float maxWidth) =>
        MeasureText(text, fontSize, bold, maxWidth).Height;

    /// <summary>
    /// Computes, for the given progress (0..1) and word-wrapped layout, the per-wrapped-line
    /// highlight widths. Each entry corresponds to one wrapped line and is the horizontal
    /// length (in DIPs, from the line's left edge) that should be coloured as "sung".
    /// This mirrors the original karaoke behaviour: charProgress = progress * text.Length,
    /// fullChars fully lit plus a sub-character partial fill, mapped onto each wrapped line
    /// via IDWriteTextLayout.HitTestTextPosition.
    /// </summary>
    public float[] ComputeKaraokeLineWidths(
        string text,
        float fontSize,
        bool bold,
        float maxWidth,
        double progress)
    {
        var layout = EnsureMeasureLayout(text, fontSize, bold, maxWidth);
        if (layout == null)
            return [];

        var length = (uint)(text?.Length ?? 0);
        if (length == 0)
            return [];

        progress = Math.Clamp(progress, 0.0, 1.0);

        // Determine how many wrapped lines the layout actually produced.
        DWriteTextMetrics textMetrics;
        SilkMarshal.ThrowHResult(layout->GetMetrics(&textMetrics));
        var lineCount = (int)textMetrics.LineCount;
        if (lineCount <= 0)
            return [];

        var lineMetrics = new DWriteLineMetrics[lineCount];
        uint actualLineCount;
        fixed (DWriteLineMetrics* lineMetricsPtr = lineMetrics)
        {
            SilkMarshal.ThrowHResult(layout->GetLineMetrics(lineMetricsPtr, (uint)lineCount, &actualLineCount));
        }

        lineCount = (int)Math.Min(actualLineCount, (uint)lineCount);
        var widths = new float[lineCount];

        if (progress <= 0.0)
            return widths; // all zeros

        // Original karaoke math: progress * charCount => full chars + fractional char.
        var charProgress = progress * length;
        var fullChars = (uint)Math.Min((int)charProgress, (int)length);
        var subProgress = Math.Clamp(charProgress - fullChars, 0.0, 1.0);

        var textOffset = 0u;
        for (var line = 0; line < lineCount; line++)
        {
            var lineLen = lineMetrics[line].Length;
            var lineEnd = textOffset + lineLen; // first text position AFTER this line

            if (fullChars >= lineEnd)
            {
                // Whole line is fully sung. Measure the trailing edge of THIS line's own
                // last character. Querying the line-end position itself would resolve to
                // the first character of the NEXT wrapped line (x near the left edge),
                // collapsing a completed line's highlight to roughly one character width.
                widths[line] = lineLen > 0
                    ? GetTrailingEdgeX(layout, lineEnd - 1)
                    : 0f;
            }
            else if (fullChars <= textOffset)
            {
                // Line not started yet, unless the partial character sits exactly on
                // the boundary (first char of this line is partially sung).
                if (fullChars == textOffset && subProgress > 0.001 && lineLen > 0)
                {
                    var startX = GetLeadingEdgeX(layout, fullChars);
                    var nextX = GetCharEndX(layout, fullChars, lineEnd);
                    widths[line] = startX + (nextX - startX) * (float)subProgress;
                }
                else
                {
                    widths[line] = 0f;
                }
            }
            else
            {
                // Partially sung line: full chars up to fullChars, plus the fractional
                // character at fullChars (if any remains on this line).
                var edgeX = GetLeadingEdgeX(layout, fullChars);
                if (subProgress > 0.001 && fullChars < lineEnd)
                {
                    var nextX = GetCharEndX(layout, fullChars, lineEnd);
                    edgeX += (nextX - edgeX) * (float)subProgress;
                }

                widths[line] = edgeX;
            }

            textOffset = lineEnd;
        }

        return widths;
    }

    /// <summary>
    /// Draws the main lyric text with the karaoke highlight applied. The base text is drawn
    /// in <paramref name="baseR/G/B/A"/>; the sung portion (per wrapped line widths computed
    /// by <see cref="ComputeKaraokeLineWidths"/>) is redrawn on top in
    /// <paramref name="highlightR/G/B/A"/> using an axis-aligned clip per line.
    /// </summary>
    public void DrawKaraokeText(
        string text,
        float fontSize,
        bool bold,
        float x,
        float y,
        float maxWidth,
        float maxHeight,
        float scale,
        float baseR, float baseG, float baseB, float baseA,
        float highlightR, float highlightG, float highlightB, float highlightA,
        float[] lineWidths)
    {
        if (_renderTarget == null || lineWidths.Length == 0)
            return;

        var layout = EnsureMeasureLayout(text, fontSize, bold, maxWidth);
        if (layout == null)
            return;

        var baseBrush = GetBrush(baseR, baseG, baseB, baseA);
        if (baseBrush == null)
            return;

        scale = Math.Clamp(scale, 0.01f, 1f);
        var layoutHeight = scale < 0.999f ? maxHeight / scale : maxHeight;

        if (scale < 0.999f)
        {
            var transform = new Matrix3X2<float>(
                scale, 0,
                0, scale,
                x * (1f - scale),
                y * (1f - scale));
            _renderTarget->SetTransform(in transform);
        }

        try
        {
            var origin = new Vector2D<float>(x, y);

            // Base (dim) text first. The Direct2D draw entry point takes the stub
            // IDWriteTextLayout; both wrap the same underlying COM pointer.
            _renderTarget->DrawTextLayout(
                origin,
                (IDWriteTextLayout*)layout,
                (ID2D1Brush*)baseBrush,
                DrawTextOptions.None);

            // Highlighted (sung) overlay clipped per wrapped line.
            var highlightBrush = GetBrush(highlightR, highlightG, highlightB, highlightA);
            if (highlightBrush == null)
                return;

            // Recompute the wrapped line tops so each clip rectangle aligns with its line.
            DWriteTextMetrics textMetrics;
            SilkMarshal.ThrowHResult(layout->GetMetrics(&textMetrics));
            var lineCount = (int)Math.Min(textMetrics.LineCount, (uint)lineWidths.Length);
            if (lineCount <= 0)
                return;

            var lineMetrics = new DWriteLineMetrics[textMetrics.LineCount];
            uint actual;
            fixed (DWriteLineMetrics* lm = lineMetrics)
            {
                SilkMarshal.ThrowHResult(layout->GetLineMetrics(lm, (uint)lineMetrics.Length, &actual));
            }

            var lineTop = 0f;
            for (var line = 0; line < lineCount; line++)
            {
                var w = lineWidths[line];
                var lineHeight = lineMetrics[line].Height;
                if (w > 0.01f)
                {
                    var clip = new Box2D<float>(
                        new Vector2D<float>(x, y + lineTop),
                        new Vector2D<float>(x + w, y + lineTop + lineHeight));
                    _renderTarget->PushAxisAlignedClip(in clip, AntialiasMode.Aliased);
                    _renderTarget->DrawTextLayout(
                        origin,
                        (IDWriteTextLayout*)layout,
                        (ID2D1Brush*)highlightBrush,
                        DrawTextOptions.None);
                    _renderTarget->PopAxisAlignedClip();
                }

                lineTop += lineMetrics[line].Height;
            }
        }
        finally
        {
            if (scale < 0.999f)
            {
                var identity = Matrix3X2<float>.Identity;
                _renderTarget->SetTransform(in identity);
            }
        }
    }

    public void DrawText(
        string text,
        float fontSize,
        bool bold,
        float x,
        float y,
        float width,
        float height,
        float r,
        float g,
        float b,
        float a,
        float scale = 1f)
    {
        if (_renderTarget == null)
            return;

        var brush = GetBrush(r, g, b, a);
        if (brush == null)
            return;

        text ??= string.Empty;
        var format = GetTextFormat(fontSize, bold);
        if (format == null)
            return;

        scale = Math.Clamp(scale, 0.01f, 1f);
        var layoutHeight = scale < 0.999f ? height / scale : height;
        var rect = new Box2D<float>(
            new Vector2D<float>(x, y),
            new Vector2D<float>(x + Math.Max(1f, width), y + Math.Max(1f, layoutHeight)));

        if (scale < 0.999f)
        {
            var transform = new Matrix3X2<float>(
                scale, 0,
                0, scale,
                x * (1f - scale),
                y * (1f - scale));
            _renderTarget->SetTransform(in transform);
        }

        try
        {
            fixed (char* textPtr = text)
            {
                _renderTarget->DrawTextA(
                    textPtr,
                    (uint)text.Length,
                    format,
                    in rect,
                    (ID2D1Brush*)brush,
                    DrawTextOptions.None,
                    DwriteMeasuringMode.Natural);
            }
        }
        finally
        {
            if (scale < 0.999f)
            {
                var identity = Matrix3X2<float>.Identity;
                _renderTarget->SetTransform(in identity);
            }
        }
    }

    public void FillRoundedRectangle(float x, float y, float width, float height, float radius, float r, float g, float b, float a)
    {
        if (_renderTarget == null)
            return;

        var brush = GetBrush(r, g, b, a);
        if (brush == null)
            return;

        var rounded = new RoundedRect
        {
            Rect = new Box2D<float>(
                new Vector2D<float>(x, y),
                new Vector2D<float>(x + Math.Max(0f, width), y + Math.Max(0f, height))),
            RadiusX = radius,
            RadiusY = radius
        };
        _renderTarget->FillRoundedRectangle(in rounded, (ID2D1Brush*)brush);
    }

    public void ClearTransparent()
    {
        if (_renderTarget == null)
            return;
        var clear = new D3Dcolorvalue { R = 0, G = 0, B = 0, A = 0 };
        _renderTarget->Clear(&clear);
    }

    public void Dispose()
    {
        if (_disposed)
            return;
        _disposed = true;
        ReleaseTarget();
        ReleaseDevices();
        GC.SuppressFinalize(this);
    }

    private ID2D1SolidColorBrush* GetBrush(float r, float g, float b, float a)
    {
        if (_renderTarget == null)
            return null;

        var color = new D3Dcolorvalue { R = r, G = g, B = b, A = a };
        if (_brush == null)
        {
            ID2D1SolidColorBrush* brush;
            SilkMarshal.ThrowHResult(_renderTarget->CreateSolidColorBrush(&color, null, &brush));
            _brush = brush;
        }
        else
        {
            _brush->SetColor(&color);
        }

        return _brush;
    }

    private IDWriteTextFormat* GetTextFormat(float fontSize, bool bold)
    {
        EnsureWriteFactory();
        var key = ((int)Math.Round(fontSize * 10f), bold);
        if (_textFormats.TryGetValue(key, out var cached))
            return (IDWriteTextFormat*)cached;

        var locale = CultureInfo.CurrentUICulture.Name;
        if (string.IsNullOrEmpty(locale))
            locale = "en-US";

        var format = DirectWriteNative.CreateTextFormat(_dwriteFactory, FontFamilyName, Math.Max(1f, fontSize), bold, locale);
        _textFormats[key] = (nint)format;
        return format;
    }

    private void EnsureWriteFactory()
    {
        if (_dwriteFactory == null)
            _dwriteFactory = DirectWriteNative.CreateFactory();
    }

    /// <summary>
    /// Returns a cached DirectWrite text layout for the given text/format/max-width, recreating
    /// it when the inputs change. The layout is reused by both measurement and the karaoke
    /// hit-testing so wrapping decisions are always consistent.
    /// </summary>
    private DWriteTextLayout* EnsureMeasureLayout(string text, float fontSize, bool bold, float maxWidth)
    {
        EnsureWriteFactory();
        if (_dwriteFactory == null)
            return null;

        var format = GetTextFormat(fontSize, bold);
        if (format == null)
            return null;

        text ??= string.Empty;
        maxWidth = Math.Max(1f, maxWidth);

        if (_measureLayout != null
            && string.Equals(_measureLayoutText, text, StringComparison.Ordinal)
            && _measureLayoutFormat == format
            && Math.Abs(_measureLayoutWidth - maxWidth) < 0.01f)
        {
            return _measureLayout;
        }

        DirectWriteNative.SafeRelease(ref _measureLayout);

        var factory = (DWriteFactory*)_dwriteFactory;
        DWriteTextLayout* layout;
        fixed (char* textPtr = text)
        {
            var hr = factory->CreateTextLayout(
                textPtr,
                (uint)text.Length,
                (Silk.NET.DirectWrite.IDWriteTextFormat*)format,
                maxWidth,
                100000f, // large enough; we only wrap horizontally
                &layout);
            if (hr < 0)
            {
                System.Diagnostics.Debug.WriteLine($"CreateTextLayout failed: 0x{hr:X8}");
                _measureLayout = null;
                _measureLayoutText = null;
                _measureLayoutFormat = null;
                _measureLayoutWidth = -1f;
                return null;
            }
        }

        _measureLayout = layout;
        _measureLayoutText = text;
        _measureLayoutFormat = format;
        _measureLayoutWidth = maxWidth;
        return layout;
    }

    /// <summary>X coordinate (DIP) of the leading edge of the given text position.</summary>
    private static float GetLeadingEdgeX(DWriteTextLayout* layout, uint textPosition)
    {
        float px, py;
        DWriteHitTestMetrics metrics;
        SilkMarshal.ThrowHResult(
            layout->HitTestTextPosition(textPosition, false, &px, &py, &metrics));
        return px;
    }

    /// <summary>X coordinate (DIP) of the trailing edge of the given text position.</summary>
    private static float GetTrailingEdgeX(DWriteTextLayout* layout, uint textPosition)
    {
        float px, py;
        DWriteHitTestMetrics metrics;
        SilkMarshal.ThrowHResult(
            layout->HitTestTextPosition(textPosition, true, &px, &py, &metrics));
        return px;
    }

    /// <summary>
    /// X coordinate (DIP) of the end of the character at <paramref name="charPos"/>, staying
    /// on the same wrapped line: the leading edge of the next character when it still lies
    /// within <paramref name="lineEnd"/>, otherwise the trailing edge of the character
    /// itself. Never follows the next position onto the next wrapped line, whose leading
    /// edge sits at the left edge of the layout (x ≈ 0) and would collapse the width.
    /// </summary>
    private static float GetCharEndX(DWriteTextLayout* layout, uint charPos, uint lineEnd) =>
        charPos + 1 < lineEnd
            ? GetLeadingEdgeX(layout, charPos + 1)
            : GetTrailingEdgeX(layout, charPos);

    private void RecreateTarget()
    {
        ReleaseTarget();
        EnsureDevices();

        _wicBitmap = WicNative.CreateBitmap(_wicFactory, (uint)_pixelWidth, (uint)_pixelHeight);

        var pixelFormat = new Silk.NET.Direct2D.PixelFormat(Format.FormatB8G8R8A8Unorm, Silk.NET.Direct2D.AlphaMode.Premultiplied);
        var props = new RenderTargetProperties
        {
            Type = RenderTargetType.Default,
            PixelFormat = pixelFormat,
            DpiX = _dpiX,
            DpiY = _dpiY,
            Usage = RenderTargetUsage.None,
            MinLevel = 0
        };

        ID2D1RenderTarget* renderTarget;
        SilkMarshal.ThrowHResult(
            _d2dFactory->CreateWicBitmapRenderTarget((IWICBitmap*)_wicBitmap, &props, &renderTarget));
        _renderTarget = renderTarget;
        _renderTarget->SetTextAntialiasMode(TextAntialiasMode.Grayscale);
        _renderTarget->SetAntialiasMode(AntialiasMode.PerPrimitive);

        Bitmap = new WriteableBitmap(
            _pixelWidth,
            _pixelHeight,
            _dpiX,
            _dpiY,
            PixelFormats.Pbgra32,
            null);
    }

    private void CopyToBitmap()
    {
        if (Bitmap is null || _wicBitmap == null)
            return;

        Bitmap.Lock();
        try
        {
            var stride = (uint)(_pixelWidth * 4);
            var bufferSize = stride * (uint)_pixelHeight;
            WicNative.CopyPixels(_wicBitmap, stride, bufferSize, (byte*)Bitmap.BackBuffer);
            Bitmap.AddDirtyRect(new Int32Rect(0, 0, _pixelWidth, _pixelHeight));
        }
        finally
        {
            Bitmap.Unlock();
        }
    }

    private void EnsureDevices()
    {
        if (_d2dFactory != null)
            return;

        var factoryGuid = ID2D1Factory.Guid;
        var options = new FactoryOptions { DebugLevel = DebugLevel.None };
        ID2D1Factory* d2dFactory;
        SilkMarshal.ThrowHResult(
            _d2d.D2D1CreateFactory(FactoryType.SingleThreaded, &factoryGuid, in options, (void**)&d2dFactory));
        _d2dFactory = d2dFactory;

        _wicFactory = WicNative.CreateFactory();
        EnsureWriteFactory();
        GetTextFormat(LyricsLayoutEngine.NormalFontSize, false);
        GetTextFormat(LyricsLayoutEngine.HighlightFontSize, true);
        GetTextFormat(LyricsLayoutEngine.SecondaryNormalFontSize, false);
        GetTextFormat(LyricsLayoutEngine.SecondaryHighlightFontSize, false);
    }

    private void ReleaseTarget()
    {
        _drawing = false;

        if (_brush != null)
        {
            DirectWriteNative.Release(_brush);
            _brush = null;
        }

        if (_renderTarget != null)
        {
            DirectWriteNative.Release(_renderTarget);
            _renderTarget = null;
        }

        if (_wicBitmap != null)
        {
            DirectWriteNative.Release(_wicBitmap);
            _wicBitmap = null;
        }
    }

    private void ReleaseDevices()
    {
        DirectWriteNative.SafeRelease(ref _measureLayout);
        _measureLayoutText = null;
        _measureLayoutFormat = null;
        _measureLayoutWidth = -1f;

        foreach (var format in _textFormats.Values)
            DirectWriteNative.Release((void*)format);
        _textFormats.Clear();

        DirectWriteNative.SafeRelease(ref _dwriteFactory);
        DirectWriteNative.SafeRelease(ref _wicFactory);

        if (_d2dFactory != null)
        {
            DirectWriteNative.Release(_d2dFactory);
            _d2dFactory = null;
        }

        Bitmap = null;
    }
}
