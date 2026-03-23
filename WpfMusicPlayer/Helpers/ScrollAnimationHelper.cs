using System.Windows;
using System.Windows.Controls;
using System.Windows.Media.Animation;

namespace WpfMusicPlayer.Helpers;

public static class ScrollAnimationHelper
{
    public static readonly DependencyProperty AnimatableVerticalOffsetProperty =
        DependencyProperty.RegisterAttached(
            "AnimatableVerticalOffset",
            typeof(double),
            typeof(ScrollAnimationHelper),
            new PropertyMetadata(0.0, OnAnimatableVerticalOffsetChanged));

    private static void OnAnimatableVerticalOffsetChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        if (d is ScrollViewer sv)
            sv.ScrollToVerticalOffset((double)e.NewValue);
    }

    // 使用立方体缓动滚动列表框到指定垂直偏移
    public static void AnimateScrollToVerticalOffset(ScrollViewer scrollViewer, double toOffset, TimeSpan duration)
    {
        toOffset = Math.Max(0, Math.Min(toOffset, scrollViewer.ScrollableHeight));

        scrollViewer.SetValue(AnimatableVerticalOffsetProperty, scrollViewer.VerticalOffset);

        var animation = new DoubleAnimation
        {
            From = scrollViewer.VerticalOffset,
            To = toOffset,
            Duration = new Duration(duration),
            EasingFunction = new CubicEase { EasingMode = EasingMode.EaseInOut }
        };
        animation.Completed += (_, _) =>
        {
            scrollViewer.BeginAnimation(AnimatableVerticalOffsetProperty, null);
            scrollViewer.SetValue(AnimatableVerticalOffsetProperty, toOffset);
        };

        scrollViewer.BeginAnimation(AnimatableVerticalOffsetProperty, animation);
    }
}

