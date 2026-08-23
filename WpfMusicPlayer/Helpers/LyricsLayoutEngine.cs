// SPDX-License-Identifier: MIT

namespace WpfMusicPlayer.Helpers;

public static class LyricsLayoutEngine
{
    public const float ItemPaddingY = 13f;
    public const float SecondaryLineGap = 2f;
    public const float ContentPaddingLeft = 16f;
    public const float ContentPaddingRight = 20f;
    public const float NormalFontSize = 18f;
    public const float HighlightFontSize = 24f;
    public const float SecondaryNormalFontSize = 15f;
    public const float SecondaryHighlightFontSize = 17f;
    public const double AnimationDurationSeconds = 0.25;
    public const double HoverFadeSeconds = 0.18;
    public const double ScrollAnimationMilliseconds = 250;
    public static readonly TimeSpan AutoFollowIdleThreshold = TimeSpan.FromSeconds(10);

    public static bool ShouldResumeAutoFollow(bool followSuspended, TimeSpan idleDuration) =>
        followSuspended && idleDuration >= AutoFollowIdleThreshold;

    public static float EaseOutCubic(float t)
    {
        t = Math.Clamp(t, 0f, 1f);
        var inv = 1f - t;
        return 1f - inv * inv * inv;
    }

    public static float Lerp(float a, float b, float t) => a + (b - a) * t;

    public static float AnimateToward(float from, float to, float linearT) =>
        Lerp(from, to, EaseOutCubic(linearT));

    public readonly struct LyricSizeMetrics
    {
        public LyricSizeMetrics(
            float textHeight,
            float translationHeight,
            float romanjiHeight,
            float textWidth = 0f,
            float translationWidth = 0f,
            float romanjiWidth = 0f)
        {
            TextHeight = Math.Max(0f, textHeight);
            TranslationHeight = Math.Max(0f, translationHeight);
            RomanjiHeight = Math.Max(0f, romanjiHeight);
            TextWidth = Math.Max(0f, textWidth);
            TranslationWidth = Math.Max(0f, translationWidth);
            RomanjiWidth = Math.Max(0f, romanjiWidth);
            LineHeight = MeasureLineHeight(
                TextHeight,
                TranslationHeight > 0f ? TranslationHeight : null,
                RomanjiHeight > 0f ? RomanjiHeight : null);
        }

        public float TextHeight { get; }
        public float TranslationHeight { get; }
        public float RomanjiHeight { get; }
        public float TextWidth { get; }
        public float TranslationWidth { get; }
        public float RomanjiWidth { get; }
        public float LineHeight { get; }
    }

    public static float MeasureLineHeight(float textHeight, float? translationHeight, float? romanjiHeight)
    {
        var height = ItemPaddingY * 2f + Math.Max(0f, textHeight);
        if (translationHeight is > 0f)
            height += SecondaryLineGap + translationHeight.Value;
        if (romanjiHeight is > 0f)
            height += SecondaryLineGap + romanjiHeight.Value;
        return height;
    }

    public static float FontScale(float currentSize, float focusedSize) =>
        focusedSize <= 0.01f ? 1f : Math.Clamp(currentSize / focusedSize, 0.01f, 1f);

    public static void ScaleFocusedMetrics(
        in LyricSizeMetrics focused,
        float mainScale,
        float secondaryScale,
        out float textHeight,
        out float translationHeight,
        out float romanjiHeight,
        out float lineHeight,
        out float textWidth,
        out float translationWidth,
        out float romanjiWidth)
    {
        mainScale = Math.Clamp(mainScale, 0.01f, 1f);
        secondaryScale = Math.Clamp(secondaryScale, 0.01f, 1f);
        textHeight = focused.TextHeight * mainScale;
        translationHeight = focused.TranslationHeight * secondaryScale;
        romanjiHeight = focused.RomanjiHeight * secondaryScale;
        textWidth = focused.TextWidth * mainScale;
        translationWidth = focused.TranslationWidth * secondaryScale;
        romanjiWidth = focused.RomanjiWidth * secondaryScale;
        lineHeight = MeasureLineHeight(
            textHeight,
            translationHeight > 0f ? translationHeight : null,
            romanjiHeight > 0f ? romanjiHeight : null);
    }

    public static double ComputeContentHeight(IReadOnlyList<float> lineHeights)
    {
        double total = 0;
        for (var i = 0; i < lineHeights.Count; i++)
            total += lineHeights[i];
        return total;
    }

    public static double ComputeCenterOffset(
        double itemTop,
        double itemHeight,
        double viewportHeight,
        double contentHeight)
    {
        var target = itemTop + itemHeight / 2d - viewportHeight / 2d;
        var max = Math.Max(0d, contentHeight - viewportHeight);
        return Math.Clamp(target, 0d, max);
    }

    public static double ClampOffset(double offset, double viewportHeight, double contentHeight)
    {
        var max = Math.Max(0d, contentHeight - viewportHeight);
        return Math.Clamp(offset, 0d, max);
    }

    public static int HitTestLine(IReadOnlyList<float> lineTops, IReadOnlyList<float> lineHeights, double y)
    {
        var count = Math.Min(lineTops.Count, lineHeights.Count);
        for (var i = 0; i < count; i++)
        {
            var top = lineTops[i];
            var bottom = top + lineHeights[i];
            if (y >= top && y < bottom)
                return i;
        }

        return -1;
    }

    public static void BuildLineTops(IReadOnlyList<float> lineHeights, IList<float> lineTops)
    {
        float y = 0;
        for (var i = 0; i < lineHeights.Count; i++)
        {
            if (i < lineTops.Count)
                lineTops[i] = y;
            else
                lineTops.Add(y);
            y += lineHeights[i];
        }

        while (lineTops.Count > lineHeights.Count)
            lineTops.RemoveAt(lineTops.Count - 1);
    }
}
