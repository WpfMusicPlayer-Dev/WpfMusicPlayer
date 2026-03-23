using System.Globalization;
using System.Windows;
using System.Windows.Data;
using System.Windows.Media;

namespace WpfMusicPlayer.Helpers;

// 卡拉OK提词器（简化版）
// 现在只根据Progress值裁剪文本，不再做逐字处理
// 1. 逐字处理性能开销过大
// 2. 现在能保证歌词文本在同一行内滚动显示，不再需要处理换行等复杂情况
public sealed class KaraokeClipConverter : IMultiValueConverter
{
    public object Convert(object[] values, Type targetType, object? parameter, CultureInfo culture)
    {
        if (values.Length < 2
            || values[0] is not double progress
            || values[1] is not double actualWidth
            || progress <= 0 || actualWidth <= 0)
            return Geometry.Empty;

        var clipWidth = Math.Min(progress, 1.0) * actualWidth;
        var result = new RectangleGeometry(new Rect(0, 0, clipWidth, 1000));
        result.Freeze();
        return result;
    }

    public object[] ConvertBack(object value, Type[] targetTypes, object? parameter, CultureInfo culture)
        => throw new NotSupportedException();
}

