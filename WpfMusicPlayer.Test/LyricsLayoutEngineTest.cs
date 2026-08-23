// SPDX-License-Identifier: MIT

using WpfMusicPlayer.Helpers;

namespace WpfMusicPlayer.Test;

[TestClass]
public sealed class LyricsLayoutEngineTest
{
    [TestMethod]
    public void EaseOutCubic_StartsAndEndsAtExpectedValues()
    {
        Assert.AreEqual(0f, LyricsLayoutEngine.EaseOutCubic(0f), 0.0001f);
        Assert.AreEqual(1f, LyricsLayoutEngine.EaseOutCubic(1f), 0.0001f);
        Assert.IsGreaterThan(0.5f, LyricsLayoutEngine.EaseOutCubic(0.5f));
    }

    [TestMethod]
    public void AnimateToward_InterpolatesFontSizeWithEaseOut()
    {
        var mid = LyricsLayoutEngine.AnimateToward(
            LyricsLayoutEngine.NormalFontSize,
            LyricsLayoutEngine.HighlightFontSize,
            0.5f);
        Assert.IsGreaterThan(21f, mid);
        Assert.IsLessThan(24f, mid);
    }

    [TestMethod]
    public void MeasureLineHeight_IncludesPaddingAndSecondaryLines()
    {
        var textOnly = LyricsLayoutEngine.MeasureLineHeight(20f, null, null);
        Assert.AreEqual(46f, textOnly, 0.001f);

        var withTranslation = LyricsLayoutEngine.MeasureLineHeight(20f, 14f, null);
        Assert.AreEqual(62f, withTranslation, 0.001f);

        var withBoth = LyricsLayoutEngine.MeasureLineHeight(20f, 14f, 12f);
        Assert.AreEqual(76f, withBoth, 0.001f);
    }

    [TestMethod]
    public void ComputeCenterOffset_CentersItemInsideViewport()
    {
        var offset = LyricsLayoutEngine.ComputeCenterOffset(itemTop: 400, itemHeight: 40, viewportHeight: 200, contentHeight: 1000);
        Assert.AreEqual(320d, offset, 0.001);
    }

    [TestMethod]
    public void ComputeCenterOffset_ClampsWhenContentIsShorterThanViewport()
    {
        var offset = LyricsLayoutEngine.ComputeCenterOffset(itemTop: 10, itemHeight: 20, viewportHeight: 400, contentHeight: 80);
        Assert.AreEqual(0d, offset, 0.001);
    }

    [TestMethod]
    public void HitTestLine_ReturnsMatchingIndex()
    {
        float[] tops = [0f, 40f, 90f];
        float[] heights = [40f, 50f, 30f];

        Assert.AreEqual(0, LyricsLayoutEngine.HitTestLine(tops, heights, 0));
        Assert.AreEqual(1, LyricsLayoutEngine.HitTestLine(tops, heights, 40));
        Assert.AreEqual(1, LyricsLayoutEngine.HitTestLine(tops, heights, 89.9));
        Assert.AreEqual(2, LyricsLayoutEngine.HitTestLine(tops, heights, 90));
        Assert.AreEqual(-1, LyricsLayoutEngine.HitTestLine(tops, heights, 130));
    }

    [TestMethod]
    public void BuildLineTops_AccumulatesHeights()
    {
        float[] heights = [10f, 25f, 15f];
        var tops = new List<float>();
        LyricsLayoutEngine.BuildLineTops(heights, tops);

        Assert.HasCount(3, tops);
        Assert.AreEqual(0f, tops[0], 0.001f);
        Assert.AreEqual(10f, tops[1], 0.001f);
        Assert.AreEqual(35f, tops[2], 0.001f);
        Assert.AreEqual(50d, LyricsLayoutEngine.ComputeContentHeight(heights), 0.001);
    }

    [TestMethod]
    public void LyricSizeMetrics_ComputesLineHeightFromParts()
    {
        var metrics = new LyricsLayoutEngine.LyricSizeMetrics(20f, 14f, 12f);
        Assert.AreEqual(76f, metrics.LineHeight, 0.001f);
    }

    [TestMethod]
    public void ScaleFocusedMetrics_DerivesShrunkWidthFromFocusedLayout()
    {
        var focused = new LyricsLayoutEngine.LyricSizeMetrics(
            textHeight: 40f,
            translationHeight: 20f,
            romanjiHeight: 20f,
            textWidth: 200f,
            translationWidth: 160f,
            romanjiWidth: 140f);

        var mainScale = LyricsLayoutEngine.FontScale(18f, 24f);
        var secondaryScale = LyricsLayoutEngine.FontScale(15f, 17f);
        Assert.AreEqual(0.75f, mainScale, 0.0001f);

        LyricsLayoutEngine.ScaleFocusedMetrics(
            focused,
            mainScale,
            secondaryScale,
            out var textHeight,
            out var translationHeight,
            out var romanjiHeight,
            out var lineHeight,
            out var textWidth,
            out var translationWidth,
            out var romanjiWidth);

        Assert.AreEqual(30f, textHeight, 0.001f);
        Assert.AreEqual(150f, textWidth, 0.001f);
        Assert.AreEqual(20f * secondaryScale, translationHeight, 0.001f);
        Assert.AreEqual(160f * secondaryScale, translationWidth, 0.001f);
        Assert.AreEqual(20f * secondaryScale, romanjiHeight, 0.001f);
        Assert.AreEqual(140f * secondaryScale, romanjiWidth, 0.001f);
        Assert.AreEqual(
            LyricsLayoutEngine.MeasureLineHeight(textHeight, translationHeight, romanjiHeight),
            lineHeight,
            0.001f);
    }

    [TestMethod]
    public void ShouldResumeAutoFollow_WaitsForIdleThenLyricChange()
    {
        Assert.IsFalse(LyricsLayoutEngine.ShouldResumeAutoFollow(followSuspended: false, TimeSpan.FromSeconds(20)));
        Assert.IsFalse(LyricsLayoutEngine.ShouldResumeAutoFollow(followSuspended: true, TimeSpan.FromSeconds(9)));
        Assert.IsTrue(LyricsLayoutEngine.ShouldResumeAutoFollow(followSuspended: true, TimeSpan.FromSeconds(10)));
    }

    [TestMethod]
    public void ClampOffset_RespectsScrollableRange()
    {
        Assert.AreEqual(0d, LyricsLayoutEngine.ClampOffset(-20, 200, 100), 0.001);
        Assert.AreEqual(50d, LyricsLayoutEngine.ClampOffset(80, 200, 250), 0.001);
        Assert.AreEqual(30d, LyricsLayoutEngine.ClampOffset(30, 200, 400), 0.001);
    }
}
