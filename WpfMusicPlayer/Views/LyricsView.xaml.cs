// SPDX-License-Identifier: MIT

using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;

namespace WpfMusicPlayer.Views;

public partial class LyricsView : UserControl
{
    public static readonly DependencyProperty ButtonOrientationProperty =
        DependencyProperty.Register(nameof(ButtonOrientation), typeof(Orientation), typeof(LyricsView),
            new PropertyMetadata(Orientation.Horizontal));

    public Orientation ButtonOrientation
    {
        get => (Orientation)GetValue(ButtonOrientationProperty);
        set => SetValue(ButtonOrientationProperty, value);
    }

    public TranslateTransform LyricsTranslate => LyricsTranslateTransform;

    public LyricsView()
    {
        InitializeComponent();
    }

    public void ScrollLyricToCenter(int index) => LyricsCanvas.ScrollLyricToCenter(index);
}
