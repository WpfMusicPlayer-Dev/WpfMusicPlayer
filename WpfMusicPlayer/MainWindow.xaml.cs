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
using WpfMusicPlayer.Views;

namespace WpfMusicPlayer
{
    // View
    // 没有业务逻辑，业务逻辑去ViewModel写
    // 谁在这里写业务逻辑我打死谁
    public partial class MainWindow : Window
    {
        private MainViewModel ViewModel => (MainViewModel)DataContext;
        private TranslateTransform PlaylistTranslate => (TranslateTransform)PlaylistContent.RenderTransform;
        private TranslateTransform PortraitSongInfoTranslate => (TranslateTransform)PortraitSongInfoView.RenderTransform;
        private bool _isSidebarOpen;
        private bool _isEqualizerOpen;
        private DecodingDialog? _decodingDialog;

        public MainWindow()
        {
            if (DesignerProperties.GetIsInDesignMode(new DependencyObject()))
            {
                return; // 在Blend中阻止构造函数运行
            }

            InitializeComponent();
            var smtcService = new SmtcService();
            DataContext = new MainViewModel(ConfigProvider.Render, new FileDialogService(), smtcService, new SongDatabaseService());
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
            if (e.PropertyName == nameof(MainViewModel.IsDecoding))
            {
                if (ViewModel.IsDecoding)
                {
                    _decodingDialog = new DecodingDialog { Owner = this };
                    _decodingDialog.Show();
                }
                else
                {
                    _decodingDialog?.Close();
                    _decodingDialog = null;
                }
                return;
            }

            if (e.PropertyName == nameof(MainViewModel.IsPlaylistVisible))
            {
                var gen = ++_playlistAnimGen;
                if (_playlistAnimGen == int.MaxValue)
                    _playlistAnimGen = 0; // 防止溢出，但是有哪个神人会拖动播放器几千年吗
                if (ViewModel.IsPlaylistVisible)
                    AnimateToPlaylist(gen);
                else
                    AnimateToPlayer(gen);
                return;
            }

            var index = ViewModel.CurrentLyricIndex;
            if (index < 0) return;
            if (e.PropertyName is nameof(MainViewModel.IsTranslationVisible)
                                 or nameof(MainViewModel.IsRomanjiVisible))
            {
                Dispatcher.BeginInvoke(delegate
                {
                    if (ViewModel.CurrentLyricIndex < 0) return;
                    ScrollLyricToCenter(_isPortrait ? PortraitLyricsView.LyricsList : LandscapeLyricsView.LyricsList, index);
                }, DispatcherPriority.Loaded);
                return;
            }

            if (e.PropertyName != nameof(MainViewModel.CurrentLyricIndex)) return;

            ScrollLyricToCenter(LandscapeLyricsView.LyricsList, index);
            ScrollLyricToCenter(PortraitLyricsView.LyricsList, index);
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
                PortraitContent.Visibility = shouldBePortrait ? Visibility.Visible : Visibility.Collapsed;
                PlayerToolbar.VolumePanelElement.Visibility = shouldBePortrait ? Visibility.Collapsed : Visibility.Visible;
                return;
            }

            if (shouldBePortrait == _isPortrait) return;
            _isPortrait = shouldBePortrait;

            if (ViewModel.IsPlaylistVisible)
            {
                PlayerToolbar.VolumePanelElement.Visibility = shouldBePortrait ? Visibility.Collapsed : Visibility.Visible;
                return;
            }

            var gen = ++_layoutAnimGen;
            if (_layoutAnimGen == int.MaxValue)
                _layoutAnimGen = 0;
            AnimateLayoutSwitch(shouldBePortrait, gen);
        }

        // 动画防抖（布局切换，播放列表）
        // 用代数标记每次动画
        // 旧代数的动画即使在队列中也会被忽略
        private int _layoutAnimGen;
        private int _playlistAnimGen;

        private void AnimateToPlaylist(int gen)
        {
            ClearLayoutAnimations();

            var outgoing = _isPortrait ? (FrameworkElement)PortraitContent : LandscapeContent;

            var duration = new Duration(TimeSpan.FromMilliseconds(350));
            var easing = new CubicEase { EasingMode = EasingMode.EaseInOut };

            // 准备playlist：从底部偏移，透明
            PlaylistContent.Visibility = Visibility.Visible;
            PlaylistContent.Opacity = 0;
            PlaylistTranslate.Y = 40;

            // 淡出当前播放界面
            var fadeOut = new DoubleAnimation(0, new Duration(TimeSpan.FromMilliseconds(220)))
            {
                EasingFunction = new CubicEase { EasingMode = EasingMode.EaseIn }
            };
            fadeOut.Completed += (_, _) =>
            {
                if (_playlistAnimGen != gen) return;
                outgoing.Visibility = Visibility.Collapsed;
                outgoing.BeginAnimation(OpacityProperty, null);
                outgoing.Opacity = 1;
            };
            outgoing.BeginAnimation(OpacityProperty, fadeOut);

            // Playlist淡入，上移
            var fadeIn = new DoubleAnimation(1, new Duration(TimeSpan.FromMilliseconds(320)))
            {
                EasingFunction = easing,
                BeginTime = TimeSpan.FromMilliseconds(80)
            };
            fadeIn.Completed += (_, _) =>
            {
                if (_playlistAnimGen != gen) return;
                PlaylistContent.BeginAnimation(OpacityProperty, null);
                PlaylistContent.Opacity = 1;
                ClearPlaylistTransformAnimations();
            };
            PlaylistContent.BeginAnimation(OpacityProperty, fadeIn);

            var slideUp = new DoubleAnimation(0, duration)
            {
                EasingFunction = easing,
                BeginTime = TimeSpan.FromMilliseconds(80)
            };
            PlaylistTranslate.BeginAnimation(TranslateTransform.YProperty, slideUp);
        }

        private void AnimateToPlayer(int gen)
        {
            ClearLayoutAnimations();

            var incoming = _isPortrait ? (FrameworkElement)PortraitContent : LandscapeContent;

            var duration = new Duration(TimeSpan.FromMilliseconds(350));
            var easing = new CubicEase { EasingMode = EasingMode.EaseInOut };

            // 准备播放界面：透明，微向下偏移
            incoming.Visibility = Visibility.Visible;
            incoming.Opacity = 0;

            var inTranslate = _isPortrait ? PortraitLyricsView.LyricsTranslate : LandscapeLyricsView.LyricsTranslate;
            inTranslate.Y = 30;

            // 淡出playlist，下沉
            PlaylistTranslate.Y = 0;
            var fadeOut = new DoubleAnimation(0, new Duration(TimeSpan.FromMilliseconds(220)))
            {
                EasingFunction = new CubicEase { EasingMode = EasingMode.EaseIn }
            };
            fadeOut.Completed += (_, _) =>
            {
                if (_playlistAnimGen != gen) return;
                PlaylistContent.Visibility = Visibility.Collapsed;
                PlaylistContent.BeginAnimation(OpacityProperty, null);
                PlaylistContent.Opacity = 1;
                ClearPlaylistTransformAnimations();
            };
            PlaylistContent.BeginAnimation(OpacityProperty, fadeOut);

            var slideDown = new DoubleAnimation(30, new Duration(TimeSpan.FromMilliseconds(250)))
            {
                EasingFunction = new CubicEase { EasingMode = EasingMode.EaseIn }
            };
            PlaylistTranslate.BeginAnimation(TranslateTransform.YProperty, slideDown);

            // 播放界面淡入 内容上移
            var fadeIn = new DoubleAnimation(1, new Duration(TimeSpan.FromMilliseconds(320)))
            {
                EasingFunction = easing,
                BeginTime = TimeSpan.FromMilliseconds(80)
            };
            fadeIn.Completed += (_, _) =>
            {
                if (_playlistAnimGen != gen) return;
                incoming.BeginAnimation(OpacityProperty, null);
                incoming.Opacity = 1;
                ClearTransformAnimations(inTranslate, null);

                Dispatcher.BeginInvoke(() =>
                {
                    ScrollLyricToCenter(
                        _isPortrait ? PortraitLyricsView.LyricsList : LandscapeLyricsView.LyricsList,
                        ViewModel.CurrentLyricIndex);
                }, DispatcherPriority.Loaded);
            };
            incoming.BeginAnimation(OpacityProperty, fadeIn);

            var contentSlide = new DoubleAnimation(0, duration)
            {
                EasingFunction = easing,
                BeginTime = TimeSpan.FromMilliseconds(80)
            };
            inTranslate.BeginAnimation(TranslateTransform.YProperty, contentSlide);
        }

        private void ClearPlaylistTransformAnimations()
        {
            PlaylistTranslate.BeginAnimation(TranslateTransform.XProperty, null);
            PlaylistTranslate.BeginAnimation(TranslateTransform.YProperty, null);
            PlaylistTranslate.X = 0;
            PlaylistTranslate.Y = 0;
        }

        private async void AnimateLayoutSwitch(bool toPortrait, int gen)
        {
            PlayerToolbar.VolumePanelElement.Visibility = toPortrait ? Visibility.Collapsed : Visibility.Visible;

            var incoming = toPortrait ? PortraitContent : LandscapeContent;
            var outgoing = toPortrait ? LandscapeContent : PortraitContent;

            var outAlbum = toPortrait ? (FrameworkElement)LandscapeAlbumCover : PortraitAlbumCover;
            var inAlbum = toPortrait ? (FrameworkElement)PortraitAlbumCover : LandscapeAlbumCover;

            var inAlbumTranslate = toPortrait ? PortraitAlbumTranslate : LandscapeAlbumTranslate;
            var inAlbumScale = toPortrait ? PortraitAlbumScale : LandscapeAlbumScale;
            var inSongTranslate = toPortrait ? PortraitSongInfoTranslate : PlayerToolbar.LandscapeSongInfoTranslate;
            var inLyricsTranslate = toPortrait ? PortraitLyricsView.LyricsTranslate : LandscapeLyricsView.LyricsTranslate;

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
                PlayerToolbar.LandscapeSongInfo.Visibility = Visibility.Collapsed;
                PortraitSongInfoTranslate.X = 0;
                PortraitSongInfoTranslate.Y = 15;
            }
            else
            {
                PlayerToolbar.LandscapeSongInfo.Opacity = 0;
                PlayerToolbar.LandscapeSongInfo.Visibility = Visibility.Visible;
                PlayerToolbar.LandscapeSongInfoTranslate.X = -20;
                PlayerToolbar.LandscapeSongInfoTranslate.Y = 0;
            }

            var duration = new Duration(TimeSpan.FromMilliseconds(420));
            var easing = new CubicEase { EasingMode = EasingMode.EaseInOut };

            AnimateTranslate(inAlbumTranslate, 0, 0, duration, easing);
            AnimateScale(inAlbumScale, 1, 1, duration, easing);
            AnimateTranslate(inSongTranslate, 0, 0, duration, easing);
            AnimateTranslate(inLyricsTranslate, 0, 0, duration, easing);


            // 歌曲信息淡入淡出动画
            if (!toPortrait)
            {
                var songFadeIn = new DoubleAnimation(1, new Duration(TimeSpan.FromMilliseconds(300)))
                {
                    EasingFunction = easing,
                    BeginTime = TimeSpan.FromMilliseconds(80)
                };
                songFadeIn.Completed += (_, _) =>
                {
                    if (_layoutAnimGen != gen) return;
                    PlayerToolbar.LandscapeSongInfo.BeginAnimation(OpacityProperty, null);
                    PlayerToolbar.LandscapeSongInfo.Opacity = 1;
                };
                PlayerToolbar.LandscapeSongInfo.BeginAnimation(OpacityProperty, songFadeIn);
            }

            var fadeIn = new DoubleAnimation(1, new Duration(TimeSpan.FromMilliseconds(320)))
            {
                EasingFunction = easing,
                BeginTime = TimeSpan.FromMilliseconds(60)
            };
            fadeIn.Completed += (_, _) =>
            {
                if (_layoutAnimGen != gen) return;
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
                if (_layoutAnimGen != gen) return;
                outgoing.Visibility = Visibility.Collapsed;
                outgoing.BeginAnimation(OpacityProperty, null);
                outgoing.Opacity = 1;
            };
            outgoing.BeginAnimation(OpacityProperty, fadeOut);

            await Task.Delay(300);

            if (_layoutAnimGen != gen) return;
            Dispatcher.BeginInvoke(() =>
            {
                ScrollLyricToCenter(
                    _isPortrait ? PortraitLyricsView.LyricsList : LandscapeLyricsView.LyricsList,
                    ViewModel.CurrentLyricIndex);
            }, DispatcherPriority.Loaded);
        }

        private void ClearLayoutAnimations()
        {
            LandscapeContent.BeginAnimation(OpacityProperty, null);
            LandscapeContent.Opacity = 1;
            PortraitContent.BeginAnimation(OpacityProperty, null);
            PortraitContent.Opacity = 1;
            PlayerToolbar.LandscapeSongInfo.BeginAnimation(OpacityProperty, null);
            PlayerToolbar.LandscapeSongInfo.Opacity = 1;
            PlaylistContent.BeginAnimation(OpacityProperty, null);
            PlaylistContent.Opacity = 1;

            ClearTransformAnimations(LandscapeAlbumTranslate, LandscapeAlbumScale);
            ClearTransformAnimations(PlayerToolbar.LandscapeSongInfoTranslate, null);
            ClearTransformAnimations(LandscapeLyricsView.LyricsTranslate, null);
            ClearTransformAnimations(PortraitAlbumTranslate, PortraitAlbumScale);
            ClearTransformAnimations(PortraitSongInfoTranslate, null);
            ClearTransformAnimations(PortraitLyricsView.LyricsTranslate, null);
            ClearPlaylistTransformAnimations();
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

            // 面板进入缓动
            var slideIn = new DoubleAnimation(0, duration) { EasingFunction = easing };
            SidebarTranslate.BeginAnimation(TranslateTransform.XProperty, slideIn);

            // 背景淡入缓动
            var fadeIn = new DoubleAnimation(1, duration) { EasingFunction = easing };
            SidebarBackdrop.BeginAnimation(OpacityProperty, fadeIn);
        }

        private void CloseSidebar()
        {
            _isSidebarOpen = false;

            var duration = new Duration(TimeSpan.FromMilliseconds(200));
            var easing = new CubicEase { EasingMode = EasingMode.EaseIn };

            // 面板退出缓动
            var slideOut = new DoubleAnimation(-280, duration) { EasingFunction = easing };
            SidebarTranslate.BeginAnimation(TranslateTransform.XProperty, slideOut);

            // 背景淡出缓动
            // 在淡出动画完成后将背景隐藏
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
                    ViewModel.IsPlaylistVisible = !ViewModel.IsPlaylistVisible;
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
                        "今日は魔法にかかったメイド\nささやかな晴れ舞台", // 大爱MIMI！
                        "关于 WpfMusicPlayer...",
                        WpfMessageBoxIcon.Information);
                    break;
            }
        }
    }
}