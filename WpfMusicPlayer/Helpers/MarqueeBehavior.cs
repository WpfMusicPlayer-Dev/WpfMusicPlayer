using System.Windows;
using System.Windows.Media;
using System.Windows.Media.Animation;
using System.Windows.Threading;

namespace WpfMusicPlayer.Helpers;


// 当歌词启用Progress且文本过长时，使用这个行为根据Progress值滚动文本以显示不同部分
public static class MarqueeBehavior
{


    public static readonly DependencyProperty IsActiveProperty =
        DependencyProperty.RegisterAttached(
            "IsActive", typeof(bool), typeof(MarqueeBehavior),
            new PropertyMetadata(false, OnIsActiveChanged));

    public static bool GetIsActive(DependencyObject obj) => (bool)obj.GetValue(IsActiveProperty);
    public static void SetIsActive(DependencyObject obj, bool value) => obj.SetValue(IsActiveProperty, value);


    public static readonly DependencyProperty ProgressProperty =
        DependencyProperty.RegisterAttached(
            "Progress", typeof(double), typeof(MarqueeBehavior),
            new PropertyMetadata(0.0, OnProgressChanged));

    public static double GetProgress(DependencyObject obj) => (double)obj.GetValue(ProgressProperty);
    public static void SetProgress(DependencyObject obj, double value) => obj.SetValue(ProgressProperty, value);


    public static readonly DependencyProperty IsProgressDrivenProperty =
        DependencyProperty.RegisterAttached(
            "IsProgressDriven", typeof(bool), typeof(MarqueeBehavior),
            new PropertyMetadata(false));

    public static bool GetIsProgressDriven(DependencyObject obj) => (bool)obj.GetValue(IsProgressDrivenProperty);
    public static void SetIsProgressDriven(DependencyObject obj, bool value) => obj.SetValue(IsProgressDrivenProperty, value);


    // 当启用时，监听SizeChanged事件以在元素大小变化时更新滚动位置。 还会在加载时强制更新一次以处理初始布局
    private static void OnIsActiveChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        if (d is not FrameworkElement element) return;

        if ((bool)e.NewValue)
        {
            if (!GetIsProgressDriven(element)) return;

            element.SizeChanged += OnElementSizeChanged;

            element.Dispatcher.BeginInvoke(() =>
            {
                if (GetIsActive(element) && GetIsProgressDriven(element))
                    UpdateProgressPosition(element);
            }, DispatcherPriority.Loaded);
        }
        else
        {
            element.SizeChanged -= OnElementSizeChanged;
            ResetTransform(element);
        }
    }

    private static void OnProgressChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        if (d is not FrameworkElement element) return;
        if (!GetIsActive(element) || !GetIsProgressDriven(element)) return;

        UpdateProgressPosition(element);
    }

    private static void OnElementSizeChanged(object sender, SizeChangedEventArgs e)
    {
        if (sender is not FrameworkElement element) return;
        if (!GetIsActive(element) || !GetIsProgressDriven(element)) return;
        UpdateProgressPosition(element);
    }

    // 在文本两端留下Padding，方便用户阅读开头和结尾的歌词内容。
    private const double ScrollPadding = 30.0;

    // 根据Progress值计算文本的滚动位置。 进度为0时文本完全左对齐，进度为1时文本完全右对齐。 
    private static void UpdateProgressPosition(FrameworkElement element)
    {
        if (VisualTreeHelper.GetParent(element) is not FrameworkElement parent) return;

        var containerWidth = parent.ActualWidth;
        var contentWidth = element.ActualWidth;
        var overflow = contentWidth - containerWidth;
        if (overflow <= 1)
        {
            ResetTransform(element);
            return;
        }

        var transform = EnsureTransform(element);
        var progress = GetProgress(element);

        transform.X = ScrollPadding - (overflow + ScrollPadding * 2) * progress;
    }

    // 确保元素具有一个可用的TranslateTransform进行动画。 如果已经存在一个TranslateTransform，则重置其动画以允许直接设置X属性
    private static TranslateTransform EnsureTransform(FrameworkElement element)
    {
        if (element.RenderTransform is TranslateTransform t)
        {
            t.BeginAnimation(TranslateTransform.XProperty, null);
            return t;
        }

        var transform = new TranslateTransform();
        element.RenderTransform = transform;
        return transform;
    }

    // 如果文本不再溢出或行为被禁用，重置TranslateTransform以停止滚动并将文本返回到其原始位置
    private static void ResetTransform(FrameworkElement element)
    {
        if (element.RenderTransform is TranslateTransform t)
        {
            t.BeginAnimation(TranslateTransform.XProperty, null);
            t.X = 0;
        }
    }
}
