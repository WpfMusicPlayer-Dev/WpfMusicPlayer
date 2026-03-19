using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Media.Animation;
using MusicPlayerLibrary;
using WpfMusicPlayer.Helpers;
using WpfMusicPlayer.Services;
using WpfMusicPlayer.ViewModels;

namespace WpfMusicPlayer
{
    /// <summary>
    /// Interaction logic for MainWindow.xaml
    /// </summary>
    public partial class MainWindow : Window
    {
        private MainViewModel ViewModel => (MainViewModel)DataContext;
        private bool _isSidebarOpen;

        public MainWindow()
        {
            InitializeComponent();
            DataContext = new MainViewModel(new FileDialogService());
            AtlTraceRedirectManager.Init();
            SourceInitialized += OnSourceInitialized;
        }

        private void OnSourceInitialized(object? sender, EventArgs e)
        {
            GaussianBlueHelper.EnableBlur(this);
        }

        private void MainWindow_Closed(object sender, EventArgs e)
        {
            ViewModel.OnWindowClosed();
            ViewModel.Dispose();
        }

        private void MainWindow_Drop(object sender, DragEventArgs e)
        {
            if (!e.Data.GetDataPresent(DataFormats.FileDrop)) return;
            var files = (string[]?)e.Data.GetData(DataFormats.FileDrop);
            if (files?.Length > 0)
            {
                ViewModel.OpenFile(files[0]);
            }
        }

        private void ProgressSlider_PreviewMouseDown(object sender, MouseButtonEventArgs e)
        {
            ViewModel.IsDraggingSlider = true;
        }

        private void ProgressSlider_PreviewMouseUp(object sender, MouseButtonEventArgs e)
        {
            ViewModel.SeekToCurrentPosition();
        }

        private bool _isPortrait;
        private bool _layoutInitialized;

        private void MainWindow_SizeChanged(object sender, SizeChangedEventArgs e)
        {
            var shouldBePortrait = e.NewSize.Height > e.NewSize.Width;

            if (!_layoutInitialized)
            {
                _layoutInitialized = true;
                _isPortrait = shouldBePortrait;
                LandscapeContent.Visibility = shouldBePortrait ? Visibility.Collapsed : Visibility.Visible;
                PortraitContent.Visibility  = shouldBePortrait ? Visibility.Visible   : Visibility.Collapsed;
                OpenButton.Visibility  = shouldBePortrait ? Visibility.Collapsed : Visibility.Visible;
                VolumePanel.Visibility = shouldBePortrait ? Visibility.Collapsed : Visibility.Visible;
                return;
            }

            if (shouldBePortrait == _isPortrait) return;
            _isPortrait = shouldBePortrait;

            AnimateLayoutSwitch(shouldBePortrait);
        }

        private void AnimateLayoutSwitch(bool toPortrait)
        {
            OpenButton.Visibility  = toPortrait ? Visibility.Collapsed : Visibility.Visible;
            VolumePanel.Visibility = toPortrait ? Visibility.Collapsed : Visibility.Visible;

            var incoming = toPortrait ? PortraitContent : LandscapeContent;
            var outgoing = toPortrait ? LandscapeContent : PortraitContent;

            var outAlbum = toPortrait ? (FrameworkElement)LandscapeAlbumCover : PortraitAlbumCover;
            var inAlbum  = toPortrait ? (FrameworkElement)PortraitAlbumCover  : LandscapeAlbumCover;
            var outSong  = toPortrait ? (FrameworkElement)LandscapeSongInfo   : PortraitSongInfo;
            var inSong   = toPortrait ? (FrameworkElement)PortraitSongInfo    : LandscapeSongInfo;

            var inAlbumTranslate  = toPortrait ? PortraitAlbumTranslate      : LandscapeAlbumTranslate;
            var inAlbumScale      = toPortrait ? PortraitAlbumScale          : LandscapeAlbumScale;
            var inSongTranslate   = toPortrait ? PortraitSongInfoTranslate   : LandscapeSongInfoTranslate;
            var inLyricsTranslate = toPortrait ? PortraitLyricsTranslate     : LandscapeLyricsTranslate;

            ClearLayoutAnimations();

            var outAlbumPos = outAlbum.TranslatePoint(new Point(0, 0), ContentArea);
            var outSongPos  = outSong.TranslatePoint(new Point(0, 0), ContentArea);
            var outAlbumW = outAlbum.ActualWidth;

            incoming.Visibility = Visibility.Visible;
            incoming.Opacity = 0;
            incoming.UpdateLayout();

            var inAlbumPos = inAlbum.TranslatePoint(new Point(0, 0), ContentArea);
            var inSongPos  = inSong.TranslatePoint(new Point(0, 0), ContentArea);
            var inAlbumW = inAlbum.ActualWidth;

            var albumDx = outAlbumPos.X - inAlbumPos.X;
            var albumDy = outAlbumPos.Y - inAlbumPos.Y;
            var songDx  = outSongPos.X  - inSongPos.X;
            var songDy  = outSongPos.Y  - inSongPos.Y;
            var albumScaleRatio = inAlbumW > 0 ? outAlbumW / inAlbumW : 1;

            double lyricsDx = toPortrait ? 120 : -120;

            inAlbumTranslate.X = albumDx;
            inAlbumTranslate.Y = albumDy;
            inAlbumScale.ScaleX = albumScaleRatio;
            inAlbumScale.ScaleY = albumScaleRatio;
            inSongTranslate.X = songDx;
            inSongTranslate.Y = songDy;
            inLyricsTranslate.X = lyricsDx;

            var duration = new Duration(TimeSpan.FromMilliseconds(420));
            var easing = new CubicEase { EasingMode = EasingMode.EaseInOut };

            AnimateTranslate(inAlbumTranslate, 0, 0, duration, easing);
            AnimateScale(inAlbumScale, 1, 1, duration, easing);
            AnimateTranslate(inSongTranslate, 0, 0, duration, easing);
            AnimateTranslate(inLyricsTranslate, 0, 0, duration, easing);

            var fadeIn = new DoubleAnimation(1, new Duration(TimeSpan.FromMilliseconds(320)))
            {
                EasingFunction = easing,
                BeginTime = TimeSpan.FromMilliseconds(60)
            };
            fadeIn.Completed += (_, _) =>
            {
                incoming.BeginAnimation(OpacityProperty, null);
                incoming.Opacity = 1;
                ClearTransformAnimations(inAlbumTranslate, inAlbumScale);
                ClearTransformAnimations(inSongTranslate, null);
                ClearTransformAnimations(inLyricsTranslate, null);
            };
            incoming.BeginAnimation(OpacityProperty, fadeIn);

            var fadeOut = new DoubleAnimation(0, new Duration(TimeSpan.FromMilliseconds(250)))
            {
                EasingFunction = new CubicEase { EasingMode = EasingMode.EaseIn }
            };
            fadeOut.Completed += (_, _) =>
            {
                outgoing.Visibility = Visibility.Collapsed;
                outgoing.BeginAnimation(OpacityProperty, null);
                outgoing.Opacity = 1;
            };
            outgoing.BeginAnimation(OpacityProperty, fadeOut);
        }

        private void ClearLayoutAnimations()
        {
            LandscapeContent.BeginAnimation(OpacityProperty, null);
            LandscapeContent.Opacity = 1;
            PortraitContent.BeginAnimation(OpacityProperty, null);
            PortraitContent.Opacity = 1;

            ClearTransformAnimations(LandscapeAlbumTranslate, LandscapeAlbumScale);
            ClearTransformAnimations(LandscapeSongInfoTranslate, null);
            ClearTransformAnimations(LandscapeLyricsTranslate, null);
            ClearTransformAnimations(PortraitAlbumTranslate, PortraitAlbumScale);
            ClearTransformAnimations(PortraitSongInfoTranslate, null);
            ClearTransformAnimations(PortraitLyricsTranslate, null);
        }

        private static void ClearTransformAnimations(TranslateTransform t, ScaleTransform? s)
        {
            t.BeginAnimation(TranslateTransform.XProperty, null);
            t.BeginAnimation(TranslateTransform.YProperty, null);
            t.X = 0;
            t.Y = 0;
            if (s == null) return;
            s.BeginAnimation(ScaleTransform.ScaleXProperty, null);
            s.BeginAnimation(ScaleTransform.ScaleYProperty, null);
            s.ScaleX = 1;
            s.ScaleY = 1;
        }

        private static void AnimateTranslate(TranslateTransform t, double toX, double toY,
            Duration duration, IEasingFunction easing)
        {
            t.BeginAnimation(TranslateTransform.XProperty,
                new DoubleAnimation(toX, duration) { EasingFunction = easing });
            t.BeginAnimation(TranslateTransform.YProperty,
                new DoubleAnimation(toY, duration) { EasingFunction = easing });
        }

        private static void AnimateScale(ScaleTransform s, double toX, double toY,
            Duration duration, IEasingFunction easing)
        {
            s.BeginAnimation(ScaleTransform.ScaleXProperty,
                new DoubleAnimation(toX, duration) { EasingFunction = easing });
            s.BeginAnimation(ScaleTransform.ScaleYProperty,
                new DoubleAnimation(toY, duration) { EasingFunction = easing });
        }

        private void MenuButton_Click(object sender, RoutedEventArgs e)
        {
            if (_isSidebarOpen)
                CloseSidebar();
            else
                OpenSidebar();
        }

        private void SidebarBackdrop_MouseLeftButtonDown(object sender, MouseButtonEventArgs e)
        {
            CloseSidebar();
        }

        private void OpenSidebar()
        {
            _isSidebarOpen = true;
            SidebarOverlay.Visibility = Visibility.Visible;

            var duration = new Duration(TimeSpan.FromMilliseconds(250));
            var easing = new CubicEase { EasingMode = EasingMode.EaseOut };

            // Slide panel in
            var slideIn = new DoubleAnimation(0, duration) { EasingFunction = easing };
            SidebarTranslate.BeginAnimation(TranslateTransform.XProperty, slideIn);

            // Fade backdrop in
            var fadeIn = new DoubleAnimation(1, duration) { EasingFunction = easing };
            SidebarBackdrop.BeginAnimation(OpacityProperty, fadeIn);
        }

        private void CloseSidebar()
        {
            _isSidebarOpen = false;

            var duration = new Duration(TimeSpan.FromMilliseconds(200));
            var easing = new CubicEase { EasingMode = EasingMode.EaseIn };

            // Slide panel out
            var slideOut = new DoubleAnimation(-280, duration) { EasingFunction = easing };
            SidebarTranslate.BeginAnimation(TranslateTransform.XProperty, slideOut);

            // Fade backdrop out, then collapse the overlay
            var fadeOut = new DoubleAnimation(0, duration) { EasingFunction = easing };
            fadeOut.Completed += (_, _) =>
            {
                if (!_isSidebarOpen)
                    SidebarOverlay.Visibility = Visibility.Collapsed;
            };
            SidebarBackdrop.BeginAnimation(OpacityProperty, fadeOut);
        }

        private void SidebarMenu_SelectionChanged(object sender, SelectionChangedEventArgs e)
        {
            if (sender is ListBox listBox && listBox.SelectedIndex >= 0)
            {
                var index = listBox.SelectedIndex;
                listBox.SelectedIndex = -1;   // reset selection
                SidebarMenuBottomList.SelectedIndex = -1;
                CloseSidebar();

                switch (index)
                {
                    case 0: // Open File
                        ViewModel.OpenCommand.Execute(null);
                        break;
                    case 1: // Playlist
                        // TODO: implement playlist view
                        break;
                    case 2: // Settings
                        // TODO: implement settings view
                        break;
                }
            }
        }

        private void SidebarMenuBottom_SelectionChanged(object sender, SelectionChangedEventArgs e)
        {
            if (sender is not ListBox listBox || listBox.SelectedIndex < 0) return;
            var index = listBox.SelectedIndex;
            listBox.SelectedIndex = -1;
            SidebarMenuList.SelectedIndex = -1;
            CloseSidebar();

            switch (index)
            {
                case 0: // About
                    WpfMessageBox.Show(
                        "今日は魔法にかかったメイド\nささやかな晴れ舞台",
                        "关于 WpfMusicPlayer...",
                        WpfMessageBoxIcon.Information);
                    break;
            }
        }
    }
}