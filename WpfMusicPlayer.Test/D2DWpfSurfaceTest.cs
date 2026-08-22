// SPDX-License-Identifier: MIT

using System.Threading;
using WpfMusicPlayer.Helpers;

namespace WpfMusicPlayer.Test;

[TestClass]
public sealed class D2DWpfSurfaceTest
{
    [TestMethod]
    public void DrawText_WritesNonTransparentPixels()
    {
        RunOnSta(() =>
        {
            using var surface = new D2DWpfSurface();
            surface.EnsureSize(160, 80, 96, 96);
            Assert.IsTrue(surface.BeginDraw());
            surface.ClearTransparent();
            surface.DrawText("ABC", 28, true, 0, 16, 160, 48, 1, 1, 1, 1);
            surface.EndDraw();

            var bitmap = surface.Bitmap;
            Assert.IsNotNull(bitmap);
            var pixels = new byte[bitmap.PixelWidth * bitmap.PixelHeight * 4];
            bitmap.CopyPixels(pixels, bitmap.PixelWidth * 4, 0);
            var sawAlpha = false;
            for (var i = 3; i < pixels.Length; i += 4)
            {
                if (pixels[i] != 0)
                {
                    sawAlpha = true;
                    break;
                }
            }

            Assert.IsTrue(sawAlpha, "D2D text should write non-zero alpha into the WIC bitmap.");
        });
    }

    [TestMethod]
    public void ComputeKaraokeLineWidths_ZeroProgress_YieldsAllZeros()
    {
        RunOnSta(() =>
        {
            using var surface = new D2DWpfSurface();
            surface.EnsureSize(400, 200, 96, 96);
            var widths = surface.ComputeKaraokeLineWidths("你好世界 hello world", 24, true, 380, 0.0);
            Assert.IsNotNull(widths);
            Assert.IsTrue(widths.Length > 0);
            foreach (var w in widths)
                Assert.AreEqual(0f, w, 0.0001f);
        });
    }

    [TestMethod]
    public void ComputeKaraokeLineWidths_FullProgress_YieldsPositiveWidths()
    {
        RunOnSta(() =>
        {
            using var surface = new D2DWpfSurface();
            surface.EnsureSize(400, 200, 96, 96);
            var widths = surface.ComputeKaraokeLineWidths("你好世界 hello world", 24, true, 380, 1.0);
            Assert.IsNotNull(widths);
            Assert.IsTrue(widths.Length > 0);
            foreach (var w in widths)
                Assert.IsGreaterThan(0f, w, "fully sung lines must have a positive highlight width");
        });
    }

    [TestMethod]
    public void ComputeKaraokeLineWidths_PartialProgress_IsMonotonicAcrossLines()
    {
        RunOnSta(() =>
        {
            using var surface = new D2DWpfSurface();
            surface.EnsureSize(220, 200, 96, 96);
            // Long text forced to wrap into multiple lines at a narrow width.
            const string text = "AAAA BBBB CCCC DDDD EEEE FFFF GGGG HHHH";
            var full = surface.ComputeKaraokeLineWidths(text, 28, true, 200, 1.0);
            var half = surface.ComputeKaraokeLineWidths(text, 28, true, 200, 0.5);
            Assert.AreEqual(full.Length, half.Length, "line count must match between passes");

            var lastPositive = -1;
            for (var i = 0; i < half.Length; i++)
            {
                // No line may exceed its fully-sung width.
                Assert.IsLessThanOrEqualTo(full[i] + 0.5f, half[i],
                    $"line {i} partial width exceeds full width");
                if (half[i] > 0f)
                    lastPositive = i;
            }

            Assert.IsGreaterThanOrEqualTo(0, lastPositive, "half progress should highlight something");
            // Lines after the last positive one must be untouched (0).
            for (var i = lastPositive + 1; i < half.Length; i++)
                Assert.AreEqual(0f, half[i], 0.0001f, $"line {i} after the sung position must stay 0");
        });
    }

    [TestMethod]
    public void ComputeKaraokeLineWidths_FullySungWrappedLines_KeepFullLineWidth()
    {
        RunOnSta(() =>
        {
            using var surface = new D2DWpfSurface();
            surface.EnsureSize(220, 200, 96, 96);
            // Long text forced to wrap into multiple lines at a narrow width.
            const string text = "AAAA BBBB CCCC DDDD EEEE FFFF GGGG HHHH";
            var (_, charWidth) = surface.MeasureText("A", 28, true, 1000);
            Assert.IsGreaterThan(0f, charWidth);

            var full = surface.ComputeKaraokeLineWidths(text, 28, true, 200, 1.0);
            Assert.IsTrue(full.Length > 1, "test text must wrap into multiple lines");

            // Regression: once karaoke progress moves past a wrapped line, that line must
            // keep (approximately) its full width. Measuring the first character of the
            // NEXT line instead collapsed the highlight to roughly one character width.
            for (var i = 0; i < full.Length; i++)
            {
                Assert.IsGreaterThan(charWidth * 2, full[i],
                    $"fully sung line {i} collapsed to roughly one character width");
            }
        });
    }

    [TestMethod]
    public void DrawKaraokeText_WritesNonTransparentPixels()
    {
        RunOnSta(() =>
        {
            using var surface = new D2DWpfSurface();
            surface.EnsureSize(320, 160, 96, 96);
            var widths = surface.ComputeKaraokeLineWidths("你好世界 hello", 28, true, 300, 0.5);
            Assert.IsTrue(surface.BeginDraw());
            surface.ClearTransparent();
            surface.DrawKaraokeText(
                "你好世界 hello", 28, true, 0, 8, 300, 120, 1f,
                0.5f, 0.5f, 0.5f, 0.5f,
                1f, 1f, 1f, 1f,
                widths);
            surface.EndDraw();

            var bitmap = surface.Bitmap;
            Assert.IsNotNull(bitmap);
            var pixels = new byte[bitmap.PixelWidth * bitmap.PixelHeight * 4];
            bitmap.CopyPixels(pixels, bitmap.PixelWidth * 4, 0);
            var sawAlpha = false;
            for (var i = 3; i < pixels.Length; i += 4)
            {
                if (pixels[i] != 0)
                {
                    sawAlpha = true;
                    break;
                }
            }

            Assert.IsTrue(sawAlpha, "Karaoke text should write non-zero alpha into the WIC bitmap.");
        });
    }

    private static void RunOnSta(Action action)
    {
        Exception? failure = null;
        var thread = new Thread(() =>
        {
            try
            {
                action();
            }
            catch (Exception ex)
            {
                failure = ex;
            }
        });
        thread.SetApartmentState(ApartmentState.STA);
        thread.Start();
        thread.Join();
        if (failure is not null)
            throw failure;
    }
}
