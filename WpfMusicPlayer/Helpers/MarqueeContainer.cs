using System.Windows;
using System.Windows.Controls;

namespace WpfMusicPlayer.Helpers;

// 容器，允许子元素在水平上测量和排列时不受父元素宽度的限制
public class MarqueeContainer : Decorator
{
    public static readonly DependencyProperty IsUnboundedProperty =
        DependencyProperty.Register(
            nameof(IsUnbounded), typeof(bool), typeof(MarqueeContainer),
            new FrameworkPropertyMetadata(false,
                FrameworkPropertyMetadataOptions.AffectsMeasure |
                FrameworkPropertyMetadataOptions.AffectsArrange));

    // 当设置为true时，子元素在水平上测量和排列时不受父元素宽度的限制
    public bool IsUnbounded
    {
        get => (bool)GetValue(IsUnboundedProperty);
        set => SetValue(IsUnboundedProperty, value);
    }

    // 通过提供无限宽度的测量约束，允许子元素测量其所需的完整宽度，但是在返回测量结果时，限制宽度不超过父元素的宽度，以避免布局剪裁
    protected override Size MeasureOverride(Size constraint)
    {
        if (Child is null) return default;

        var measureConstraint = IsUnbounded
            ? new Size(double.PositiveInfinity, constraint.Height)
            : constraint;

        Child.Measure(measureConstraint);

        // 限制子元素的宽度不超过父元素的宽度，以避免布局剪裁
        return new Size(
            Math.Min(Child.DesiredSize.Width, constraint.Width),
            Child.DesiredSize.Height);
    }

    protected override Size ArrangeOverride(Size arrangeSize)
    {
        if (Child is null) return arrangeSize;

        // 当IsUnbounded为true时，允许子元素在水平上占用比父元素更宽的空间
        // 方便实现歌词滚动
        var childWidth = IsUnbounded
            ? Math.Max(Child.DesiredSize.Width, arrangeSize.Width)
            : arrangeSize.Width;

        Child.Arrange(new Rect(0, 0, childWidth, arrangeSize.Height));
        return arrangeSize;
    }
}
