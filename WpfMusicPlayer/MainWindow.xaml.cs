using System.ComponentModel;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Media.Animation;
using System.Windows.Threading;
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
        private bool _isEqualizerOpen;

        public MainWindow()
        {
            InitializeComponent();
            var smtcService = new SmtcService();
            DataContext = new MainViewModel(new FileDialogService(), smtcService);
            AtlTraceRedirectManager.Init();
            SourceInitialized += (s, e) =>
            {
                var handle = new System.Windows.Interop.WindowInteropHelper(this).Handle;
                smtcService.Initialize(handle);
                OnSourceInitialized(s, e);
            };
            ViewModel.PropertyChanged += ViewModel_PropertyChanged;
        }

        private void OnSourceInitialized(object? sender, EventArgs e)
        {
            GaussianBlueHelper.EnableBlur(this);
        }

        private void ViewModel_PropertyChanged(object? sender, PropertyChangedEventArgs e)
        {
            var index = ViewModel.CurrentLyricIndex;
            if (index < 0) return;
            if (e.PropertyName == nameof(MainViewModel.IsTranslationVisible))
            {
                Dispatcher.BeginInvoke(delegate
                {
                    if (ViewModel.CurrentLyricIndex < 0) return;
                    ScrollLyricToCenter(_isPortrait ? PortraitLyricsList : LandscapeLyricsList, index);
                }, DispatcherPriority.Loaded);
                return;
            }
            
            if (e.PropertyName != nameof(MainViewModel.CurrentLyricIndex)) return;

            ScrollLyricToCenter(LandscapeLyricsList, index);
            ScrollLyricToCenter(PortraitLyricsList, index);
        }

        private static void ScrollLyricToCenter(ListBox listBox, int index)
        {
            if (index < 0 || index >= listBox.Items.Count) return;

            listBox.SelectedIndex = index;
            listBox.UpdateLayout();

            var container = (FrameworkElement?)listBox.ItemContainerGenerator.ContainerFromIndex(index);
            if (container == null) return;

            var scrollViewer = FindParentScrollViewer(listBox);
            if (scrollViewer == null) return;

            var transform = container.TransformToAncestor(scrollViewer);
            var itemPosition = transform.Transform(new Point(0, 0));

            var itemCenter = itemPosition.Y + container.ActualHeight / 2;
            var viewportCenter = scrollViewer.ViewportHeight / 2;
            var targetOffset = scrollViewer.VerticalOffset + itemCenter - viewportCenter;

            ScrollAnimationHelper.AnimateScrollToVerticalOffset(
                scrollViewer, targetOffset, TimeSpan.FromMilliseconds(250));
        }

        private static ScrollViewer? FindParentScrollViewer(DependencyObject child)
        {
            var parent = VisualTreeHelper.GetParent(child);
            while (parent != null)
            {
                if (parent is ScrollViewer sv) return sv;
                parent = VisualTreeHelper.GetParent(parent);
            }
            return null;
        }

        private void MainWindow_Closed(object sender, EventArgs e)
        {
            ViewModel.OnWindowClosed();
            ViewModel.Dispose();
        }

        private void LyricsList_PreviewMouseLeftButtonUp(object sender, MouseButtonEventArgs e)
        {
            if (sender is not ListBox) return;

            var source = e.OriginalSource as DependencyObject;
            while (source is not null and not ListBoxItem)
                source = VisualTreeHelper.GetParent(source);

            if (source is ListBoxItem { DataContext: LyricLineViewModel lyric })
                ViewModel.SeekToLyric(lyric);
        }

        private void LyricsList_PreviewMouseWheel(object sender, MouseWheelEventArgs e)
        {
            if (sender is not ListBox listBox) return;

            // 转发到外层
            var parentScrollViewer = FindParentScrollViewer(listBox);
            if (parentScrollViewer == null) return;

            parentScrollViewer.ScrollToVerticalOffset(parentScrollViewer.VerticalOffset - e.Delta);
            e.Handled = true;
        }

        private void MainWindow_Drop(object sender, DragEventArgs e)
        {
            if (!e.Data.GetDataPresent(DataFormats.FileDrop)) return;
            var files = (string[]?)e.Data.GetData(DataFormats.FileDrop);
            if (files?.Length > 0)
            {
                try
                {
                    ViewModel.OpenFile(files[0]);
                }
                catch (Exception ex)
                {
                    WpfMessageBox.Show($"{ex.Message}\n{files[0]}", "Error", WpfMessageBoxIcon.Error);
                }
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
                VolumePanel.Visibility = shouldBePortrait ? Visibility.Collapsed : Visibility.Visible;
                return;
            }

            if (shouldBePortrait == _isPortrait) return;
            _isPortrait = shouldBePortrait;

            if (_isAnimationPlaying) return;
            _isAnimationPlaying = true;
            AnimateLayoutSwitch(shouldBePortrait);
        }

        private bool _isAnimationPlaying; 
        private async void AnimateLayoutSwitch(bool toPortrait)
        {
            VolumePanel.Visibility = toPortrait ? Visibility.Collapsed : Visibility.Visible;

            var incoming = toPortrait ? PortraitContent : LandscapeContent;
            var outgoing = toPortrait ? LandscapeContent : PortraitContent;

            var outAlbum = toPortrait ? (FrameworkElement)LandscapeAlbumCover : PortraitAlbumCover;
            var inAlbum  = toPortrait ? (FrameworkElement)PortraitAlbumCover  : LandscapeAlbumCover;

            var inAlbumTranslate  = toPortrait ? PortraitAlbumTranslate      : LandscapeAlbumTranslate;
            var inAlbumScale      = toPortrait ? PortraitAlbumScale          : LandscapeAlbumScale;
            var inSongTranslate   = toPortrait ? PortraitSongInfoTranslate   : LandscapeSongInfoTranslate;
            var inLyricsTranslate = toPortrait ? PortraitLyricsTranslate     : LandscapeLyricsTranslate;

            ClearLayoutAnimations();

            var outAlbumPos = outAlbum.TranslatePoint(new Point(0, 0), ContentArea);
            var outAlbumW = outAlbum.ActualWidth;

            incoming.Visibility = Visibility.Visible;
            incoming.Opacity = 0;
            incoming.UpdateLayout();

            var inAlbumPos = inAlbum.TranslatePoint(new Point(0, 0), ContentArea);
            var inAlbumW = inAlbum.ActualWidth;

            var albumDx = outAlbumPos.X - inAlbumPos.X;
            var albumDy = outAlbumPos.Y - inAlbumPos.Y;
            var albumScaleRatio = inAlbumW > 0 ? outAlbumW / inAlbumW : 1;

            double lyricsDx = toPortrait ? 120 : -120;

            inAlbumTranslate.X = albumDx;
            inAlbumTranslate.Y = albumDy;
            inAlbumScale.ScaleX = albumScaleRatio;
            inAlbumScale.ScaleY = albumScaleRatio;
            inLyricsTranslate.X = lyricsDx;
            
            // 修改：LandscapeSongInfo移动至窗口底部，改为平滑淡出+淡入，而不是在页面上运动
            if (toPortrait)
            {
                LandscapeSongInfo.Visibility = Visibility.Collapsed;
                PortraitSongInfoTranslate.X = 0;
                PortraitSongInfoTranslate.Y = 15;
            }
            else
            {
                LandscapeSongInfo.Opacity = 0;
                LandscapeSongInfo.Visibility = Visibility.Visible;
                LandscapeSongInfoTranslate.X = -20;
                LandscapeSongInfoTranslate.Y = 0;
            }

            var duration = new Duration(TimeSpan.FromMilliseconds(420));
            var easing = new CubicEase { EasingMode = EasingMode.EaseInOut };

            AnimateTranslate(inAlbumTranslate, 0, 0, duration, easing);
            AnimateScale(inAlbumScale, 1, 1, duration, easing);
            AnimateTranslate(inSongTranslate, 0, 0, duration, easing);
            AnimateTranslate(inLyricsTranslate, 0, 0, duration, easing);

            // Fade LandscapeSongInfo separately since it's in the bottom bar, not
            // part of the incoming/outgoing content areas.
            if (!toPortrait)
            {
                var songFadeIn = new DoubleAnimation(1, new Duration(TimeSpan.FromMilliseconds(300)))
                {
                    EasingFunction = easing,
                    BeginTime = TimeSpan.FromMilliseconds(80)
                };
                songFadeIn.Completed += (_, _) =>
                {
                    LandscapeSongInfo.BeginAnimation(OpacityProperty, null);
                    LandscapeSongInfo.Opacity = 1;
                };
                LandscapeSongInfo.BeginAnimation(OpacityProperty, songFadeIn);
            }

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
            
            await Task.Delay(300);

            Dispatcher.BeginInvoke(() =>
            {
                ScrollLyricToCenter(
                    _isPortrait ? PortraitLyricsList : LandscapeLyricsList,
                    ViewModel.CurrentLyricIndex);
            }, DispatcherPriority.Loaded);
        }

        private void ClearLayoutAnimations()
        {
            LandscapeContent.BeginAnimation(OpacityProperty, null);
            LandscapeContent.Opacity = 1;
            PortraitContent.BeginAnimation(OpacityProperty, null);
            PortraitContent.Opacity = 1;
            LandscapeSongInfo.BeginAnimation(OpacityProperty, null);
            LandscapeSongInfo.Opacity = 1;

            ClearTransformAnimations(LandscapeAlbumTranslate, LandscapeAlbumScale);
            ClearTransformAnimations(LandscapeSongInfoTranslate, null);
            ClearTransformAnimations(LandscapeLyricsTranslate, null);
            ClearTransformAnimations(PortraitAlbumTranslate, PortraitAlbumScale);
            ClearTransformAnimations(PortraitSongInfoTranslate, null);
            ClearTransformAnimations(PortraitLyricsTranslate, null);
            _isAnimationPlaying = false;
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
            if (sender is not ListBox { SelectedIndex: >= 0 } listBox) return;
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
                case 2: // Equalizer
                    OpenEqualizer();
                    break;
                case 3: // Settings
                    // TODO: implement settings view
                    break;
            }
        }

        private void EqualizerBackdrop_MouseLeftButtonDown(object sender, MouseButtonEventArgs e)
        {
            CloseEqualizer();
        }

        private void OpenEqualizer()
        {
            _isEqualizerOpen = true;
            EqualizerOverlay.Visibility = Visibility.Visible;

            var duration = new Duration(TimeSpan.FromMilliseconds(250));
            var easing = new CubicEase { EasingMode = EasingMode.EaseOut };

            var slideIn = new DoubleAnimation(0, duration) { EasingFunction = easing };
            EqualizerTranslate.BeginAnimation(TranslateTransform.XProperty, slideIn);

            var fadeIn = new DoubleAnimation(1, duration) { EasingFunction = easing };
            EqualizerBackdrop.BeginAnimation(OpacityProperty, fadeIn);
        }

        private void CloseEqualizer()
        {
            _isEqualizerOpen = false;

            var duration = new Duration(TimeSpan.FromMilliseconds(200));
            var easing = new CubicEase { EasingMode = EasingMode.EaseIn };

            var slideOut = new DoubleAnimation(320, duration) { EasingFunction = easing };
            EqualizerTranslate.BeginAnimation(TranslateTransform.XProperty, slideOut);

            var fadeOut = new DoubleAnimation(0, duration) { EasingFunction = easing };
            fadeOut.Completed += (_, _) =>
            {
                if (!_isEqualizerOpen)
                    EqualizerOverlay.Visibility = Visibility.Collapsed;
            };
            EqualizerBackdrop.BeginAnimation(OpacityProperty, fadeOut);
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