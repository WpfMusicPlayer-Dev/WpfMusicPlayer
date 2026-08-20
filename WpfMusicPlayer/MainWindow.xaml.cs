using System.ComponentModel;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Forms;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Media.Animation;
using System.Windows.Threading;
using WpfMusicPlayer.Helpers;
using WpfMusicPlayer.Services.Abstractions;
using WpfMusicPlayer.ViewModels;
using WpfMusicPlayer.Views;
using static WpfMusicPlayer.Models.ConfigData;
using DataFormats = System.Windows.DataFormats;
using DragEventArgs = System.Windows.DragEventArgs;
using ListBox = System.Windows.Controls.ListBox;

namespace WpfMusicPlayer
{
    // View
    // 没有业务逻辑，业务逻辑去ViewModel写
    // 谁在这里写业务逻辑我打死谁
    public partial class MainWindow : Window
    {
        private MainViewModel ViewModel => (MainViewModel)DataContext;
        private TranslateTransform PlaylistTranslate => (TranslateTransform)PlaylistContent.RenderTransform;
        private TranslateTransform SettingsTranslate => (TranslateTransform)SettingsContent.RenderTransform;
        private TranslateTransform PortraitSongInfoTranslate => (TranslateTransform)PortraitSongInfoView.RenderTransform;
        private bool _isSidebarOpen;
        private bool _isEqualizerOpen;
        private bool _backgroundInitialized;
        private DecodingDialog? _decodingDialog;
        private readonly DispatcherTimer _spectrumTimer;
        private DesktopLyricWindow? _desktopLyricWindow;
        private MiniPlayerWindow? _miniPlayerWindow;
        private bool _isHidingToTray;
        private bool _hideToTrayPending;
        private bool _windowVisualRefreshPending;
        private bool _isClosing;
        private bool _hasDesktopLyricStateBeforeMiniPlayer;
        private bool _wasDesktopLyricVisibleBeforeMiniPlayer;
        private bool _isAudioSettingsApplyPromptOpen;
        private UISettings.BackgroundMode _appliedBackgroundMode;

        public MainWindow(MainViewModel viewModel, ISmtcService smtcService)
        {
            if (DesignerProperties.GetIsInDesignMode(new DependencyObject()))
            {
                return; // 在Blend中阻止构造函数运行
            }

            InitializeComponent();
            DataContext = viewModel;
            SourceInitialized += (s, e) =>
            {
                var handle = new System.Windows.Interop.WindowInteropHelper(this).Handle;
                smtcService.Initialize(handle);
                OnSourceInitialized(s, e);
            };
            ViewModel.PropertyChanged += ViewModel_PropertyChanged;
            ViewModel.Lyrics.PropertyChanged += LyricsPropertyChanged;
            ViewModel.DesktopLyric.PropertyChanged += DesktopLyricPropertyChanged;
            ViewModel.WasapiFallbackOccurred += ShowWasapiFallbackWarning;
            ViewModel.AudioPipelineReinitializationFailed += ShowAudioPipelineError;
            Loaded += (_, _) =>
            {
                if (ViewModel.ConsumePendingWasapiFallbackWarning())
                    ShowWasapiFallbackWarning();
            };

            // 从命令行恢复时，IsDesktopLyricVisible可能已经设置
            if (ViewModel.DesktopLyric.IsDesktopLyricVisible)
            {
                _desktopLyricWindow ??= new DesktopLyricWindow(ViewModel.DesktopLyric);
                _desktopLyricWindow.Show();
            }

            _spectrumTimer = new DispatcherTimer(DispatcherPriority.Render)
            {
                Interval = TimeSpan.FromMilliseconds(16)
            };
            _spectrumTimer.Tick += (_, _) => ViewModel.PollSpectrumData();
            _spectrumTimer.Start();

            if (ViewModel.ActiveView != ActiveView.Player)
            {
                _previousView = ViewModel.ActiveView;
                LandscapeContent.Visibility = Visibility.Collapsed;
                GetViewElement(ViewModel.ActiveView).Visibility = Visibility.Visible;
            }
        }

        private void ApplyBackgroundMode(UISettings.BackgroundMode mode)
        {
            if (OsVersionHelper.IsWindows11() || !_backgroundInitialized)
            {
                ApplyBackgroundModeCore(mode);
            } 
            else
            {
                // Windows 10: need restart the whole application
                WpfMessageBoxResult selection =
                    WpfMessageBox.Show("您需要重启以应用设置更改吗？", "应用背景设置", WpfMessageBoxButton.OKCancel, WpfMessageBoxIcon.Information);
                if (selection == WpfMessageBoxResult.OK)
                {
                    RebootApplicationHelper.RebootApplication();
                }
            }
            _backgroundInitialized = true;
        }

        private void ApplyBackgroundModeCore(UISettings.BackgroundMode mode)
        {
            switch (mode)
            {
                case UISettings.BackgroundMode.Solid:
                    GaussianBlueHelper.EnableSolid(this);
                    BackgroundImageBorder.Visibility = Visibility.Collapsed;
                    // byd 这里开了Black就会把标题栏一起渲染成黑的
                    Background = Brushes.Transparent;
                    break;

                case UISettings.BackgroundMode.Acrylic:
                    BackgroundImageBorder.Visibility = Visibility.Collapsed;
                    Background = Brushes.Transparent;
                    GaussianBlueHelper.EnableAcrylic(this);
                    break;

                case UISettings.BackgroundMode.ImageBlur:
                    GaussianBlueHelper.EnableImageBlur(this);
                    BackgroundImageBorder.Visibility = Visibility.Visible;
                    Background = Brushes.Transparent;
                    break;
            }

            _appliedBackgroundMode = mode;
        }

        private void ReapplyWindowBackground()
        {
            if (!_backgroundInitialized)
                return;

            GaussianBlueHelper.EnableDarkMode(this);
            ApplyBackgroundModeCore(_appliedBackgroundMode);
        }

        private void OnSourceInitialized(object? sender, EventArgs e)
        {
            GaussianBlueHelper.EnableDarkMode(this);
            ApplyBackgroundMode(ViewModel.CurrentBackgroundMode);
        }

        private void ViewModel_PropertyChanged(object? sender, PropertyChangedEventArgs e)
        {
            if (e.PropertyName == nameof(MainViewModel.CurrentBackgroundMode))
            {
                ApplyBackgroundMode(ViewModel.CurrentBackgroundMode);
                return;
            }

            if (e.PropertyName == nameof(MainViewModel.IsAudioSettingsApplyRequired))
            {
                PromptApplyAudioSettingsChange();
                return;
            }

            if (e.PropertyName == nameof(MainViewModel.IsAudioPipelineReinitializing))
            {
                SettingsContent.IsEnabled = !ViewModel.IsAudioPipelineReinitializing;
                return;
            }

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

            if (e.PropertyName == nameof(MainViewModel.ActiveView))
            {
                var gen = ++_viewAnimGen;
                if (_viewAnimGen == int.MaxValue)
                    _viewAnimGen = 0;
                var oldView = _previousView;
                _previousView = ViewModel.ActiveView;
                AnimateViewTransition(oldView, ViewModel.ActiveView, gen);
            }
        }

        private async void PromptApplyAudioSettingsChange()
        {
            if (!ViewModel.IsAudioSettingsApplyRequired || _isAudioSettingsApplyPromptOpen)
                return;

            _isAudioSettingsApplyPromptOpen = true;
            try
            {
                var selection = WpfMessageBox.Show(
                    $"以下音频设置需要重新初始化音频管线：\n{ViewModel.PendingAudioSettingsSummary}\n\n是否立即应用？",
                    "应用音频设置",
                    WpfMessageBoxButton.OKCancel,
                    WpfMessageBoxIcon.Information);

                if (selection == WpfMessageBoxResult.OK)
                {
                    var result = await ViewModel.ReinitializeAudioPipelineAsync();
                    if (result.WasapiFallback && !_isClosing)
                    {
                        ShowWasapiFallbackWarning();
                    }
                    else if (!result.Succeeded && result.Error is not null &&
                             result.Error is not OperationCanceledException &&
                             !_isClosing)
                    {
                        ShowAudioPipelineError(result.Error);
                    }
                }
                else
                {
                    ViewModel.DiscardPendingAudioSettings();
                }
            }
            finally
            {
                _isAudioSettingsApplyPromptOpen = false;
            }
        }

        private void ShowWasapiFallbackWarning()
        {
            if (_isClosing)
                return;

            WpfMessageBox.Show(
                "WASAPI 独占模式初始化失败，已回退至 FAudio 共享模式。",
                "WASAPI 初始化失败",
                WpfMessageBoxIcon.Warning);
        }

        private void ShowAudioPipelineError(Exception exception)
        {
            if (_isClosing || exception is OperationCanceledException)
                return;

            WpfMessageBox.Show(
                $"无法重新初始化音频管线。\n{exception.Message}",
                "应用音频设置失败",
                WpfMessageBoxIcon.Error);
        }

        private void LyricsPropertyChanged(object? sender, PropertyChangedEventArgs e)
        {
            var index = ViewModel.Lyrics.CurrentLyricIndex;
            if (index < 0) return;
            if (e.PropertyName is nameof(LyricsViewModel.IsTranslationVisible)
                                 or nameof(LyricsViewModel.IsRomanjiVisible))
            {
                Dispatcher.BeginInvoke(delegate
                {
                    if (ViewModel.Lyrics.CurrentLyricIndex < 0) return;
                    ScrollLyricToCenter(_isPortrait ? PortraitLyricsView.LyricsList : LandscapeLyricsView.LyricsList, index);
                }, DispatcherPriority.Loaded);
                return;
            }

            if (e.PropertyName != nameof(LyricsViewModel.CurrentLyricIndex)) return;

            ScrollLyricToCenter(LandscapeLyricsView.LyricsList, index);
            ScrollLyricToCenter(PortraitLyricsView.LyricsList, index);
        }

        
        private void DesktopLyricPropertyChanged(object? sender, PropertyChangedEventArgs e)
        {
            if (e.PropertyName != nameof(DesktopLyricViewModel.IsDesktopLyricVisible)) return;

            if (ViewModel.DesktopLyric.IsDesktopLyricVisible)
            {
                _desktopLyricWindow ??= new DesktopLyricWindow(ViewModel.DesktopLyric);
                _desktopLyricWindow.Show();
            }
            else
            {
                if (_desktopLyricWindow is { IsLocked: true })
                    _desktopLyricWindow.Unlock();
                _desktopLyricWindow?.Hide();
            }
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

        private bool _closeConfirmed;
        private bool _shutdownInProgress;

        private async void MainWindow_Closing(object sender, CancelEventArgs e)
        {
            if (_closeConfirmed)
                return;

            e.Cancel = true;
            if (_shutdownInProgress)
                return;

            _shutdownInProgress = true;
            _isClosing = true;
            var handlersDetached = false;
            var shutdownUiApplied = false;
            try
            {
                if (ViewModel.HasUnsavedPlaylistChanges)
                {
                    var result = WpfMessageBox.Show(
                        "播放列表有未保存的更改，是否保存？",
                        "确认",
                        WpfMessageBoxButton.YesNoCancel,
                        WpfMessageBoxIcon.Question);

                    switch (result)
                    {
                        case WpfMessageBoxResult.Yes:
                            await ViewModel.Playlist.SavePlaylistAsync();
                            break;
                        case WpfMessageBoxResult.Cancel:
                            return;
                    }
                }

                _spectrumTimer.Stop();
                SettingsContent.IsEnabled = false;
                shutdownUiApplied = true;
                ViewModel.PropertyChanged -= ViewModel_PropertyChanged;
                ViewModel.Lyrics.PropertyChanged -= LyricsPropertyChanged;
                ViewModel.DesktopLyric.PropertyChanged -= DesktopLyricPropertyChanged;
                ViewModel.WasapiFallbackOccurred -= ShowWasapiFallbackWarning;
                ViewModel.AudioPipelineReinitializationFailed -= ShowAudioPipelineError;
                handlersDetached = true;

                await ViewModel.DisposeAsync();
                _desktopLyricWindow?.Close();
                _miniPlayerWindow?.Close();
                _decodingDialog?.Close();
                _decodingDialog = null;
                _closeConfirmed = true;
                e.Cancel = false;
            }
            catch (Exception exception)
            {
                WpfMessageBox.Show(
                    $"应用程序无法安全关闭。\n{exception.Message}",
                    "关闭失败",
                    WpfMessageBoxIcon.Error);
            }
            finally
            {
                if (!_closeConfirmed)
                {
                    if (handlersDetached)
                    {
                        ViewModel.PropertyChanged += ViewModel_PropertyChanged;
                        ViewModel.Lyrics.PropertyChanged += LyricsPropertyChanged;
                        ViewModel.DesktopLyric.PropertyChanged += DesktopLyricPropertyChanged;
                        ViewModel.WasapiFallbackOccurred += ShowWasapiFallbackWarning;
                        ViewModel.AudioPipelineReinitializationFailed += ShowAudioPipelineError;
                    }
                    if (shutdownUiApplied)
                    {
                        SettingsContent.IsEnabled =
                            !ViewModel.IsAudioPipelineReinitializing;
                        _spectrumTimer.Start();
                    }
                    _isClosing = false;
                    _shutdownInProgress = false;
                }
            }
        }

        private async void MainWindow_Drop(object sender, DragEventArgs e)
        {
            try
            {
                if (!e.Data.GetDataPresent(DataFormats.FileDrop)) return;
                var files = (string[]?)e.Data.GetData(DataFormats.FileDrop);
                if (!(files?.Length > 0)) return;
                // 在UI线程上缓存ViewModel引用
                var vm = ViewModel;
                try
                {
                    if (files[0].EndsWith(".wppl", StringComparison.OrdinalIgnoreCase))
                    {
                        await vm.OpenExternalPlaylist(files[0]);
                        return;
                    }

                    var validFiles = files
                        .Where(f => vm.ExtensionList.Any(ext => f.EndsWith(ext, StringComparison.OrdinalIgnoreCase)))
                        .ToArray();
                    if (validFiles.Length > 0)
                        // 巨坑：不能直接在后台线程访问DataContext！！！
                        // 直接OpenFile会导致阻塞UI线程
                        await Task.Run(() => vm.OpenFile(validFiles[0]));
                    else
                        WpfMessageBox.Show("不要什么东西都往窗口上拖啊喂！", "别拖了！", WpfMessageBoxIcon.Error);
                }
                catch (Exception ex)
                {
                    WpfMessageBox.Show($"{ex.Message}\n{files[0]}", "Error", WpfMessageBoxIcon.Error);
                }
            }
            catch (Exception)
            {
                //  ignored
            }
        }

        private bool _isPortrait;
        private bool _layoutInitialized;

        private void MainWindow_SizeChanged(object sender, SizeChangedEventArgs e)
        {
            if (WindowState == WindowState.Minimized || e.NewSize.Width <= 0 || e.NewSize.Height <= 0)
                return;

            var shouldBePortrait = e.NewSize.Height > e.NewSize.Width;

            if (!_layoutInitialized)
            {
                _layoutInitialized = true;
                _isPortrait = shouldBePortrait;
                if (ViewModel.ActiveView == ActiveView.Player)
                {
                    LandscapeContent.Visibility = shouldBePortrait ? Visibility.Collapsed : Visibility.Visible;
                    PortraitContent.Visibility = shouldBePortrait ? Visibility.Visible : Visibility.Collapsed;
                }
                PlayerToolbar.VolumePanelElement.Visibility = shouldBePortrait ? Visibility.Collapsed : Visibility.Visible;
                return;
            }

            if (shouldBePortrait == _isPortrait) return;
            _isPortrait = shouldBePortrait;

            if (ViewModel.ActiveView != ActiveView.Player)
            {
                PlayerToolbar.VolumePanelElement.Visibility = shouldBePortrait ? Visibility.Collapsed : Visibility.Visible;
                return;
            }

            var gen = ++_layoutAnimGen;
            if (_layoutAnimGen == int.MaxValue)
                _layoutAnimGen = 0;
            AnimateLayoutSwitch(shouldBePortrait, gen);
        }

        // 动画防抖（布局切换，视图切换）
        // 用代数标记每次动画
        // 旧代数的动画即使在队列中也会被忽略
        private int _layoutAnimGen;
        private int _viewAnimGen;
        private ActiveView _previousView;

        private void AnimateViewTransition(ActiveView from, ActiveView to, int gen)
        {
            if (from == to) return;
            if (to == ActiveView.Player)
                AnimateOverlayToPlayer(from, gen);
            else
                AnimateToOverlay(from, to, gen);
        }

        private FrameworkElement GetViewElement(ActiveView view) => view switch
        {
            ActiveView.Player => _isPortrait ? PortraitContent : LandscapeContent,
            ActiveView.Playlist => PlaylistContent,
            ActiveView.Settings => SettingsContent,
            _ => throw new ArgumentOutOfRangeException(nameof(view))
        };

        private TranslateTransform GetOverlayTranslate(ActiveView view) => view switch
        {
            ActiveView.Playlist => PlaylistTranslate,
            ActiveView.Settings => SettingsTranslate,
            _ => throw new ArgumentOutOfRangeException(nameof(view))
        };

        private void ClearOverlayTransformAnimations(ActiveView view)
        {
            var translate = GetOverlayTranslate(view);
            translate.BeginAnimation(TranslateTransform.XProperty, null);
            translate.BeginAnimation(TranslateTransform.YProperty, null);
            translate.X = 0;
            translate.Y = 0;
        }

        private void AnimateToOverlay(ActiveView from, ActiveView to, int gen)
        {
            ClearLayoutAnimations();

            var outgoing = GetViewElement(from);
            var incoming = GetViewElement(to);
            var incomingTranslate = GetOverlayTranslate(to);

            var duration = new Duration(TimeSpan.FromMilliseconds(350));
            var easing = new CubicEase { EasingMode = EasingMode.EaseInOut };

            incoming.Visibility = Visibility.Visible;
            incoming.Opacity = 0;
            incomingTranslate.Y = 40;

            var fadeOut = new DoubleAnimation(0, new Duration(TimeSpan.FromMilliseconds(220)))
            {
                EasingFunction = new CubicEase { EasingMode = EasingMode.EaseIn }
            };
            fadeOut.Completed += (_, _) =>
            {
                if (_viewAnimGen != gen) return;
                outgoing.Visibility = Visibility.Collapsed;
                outgoing.BeginAnimation(OpacityProperty, null);
                outgoing.Opacity = 1;
            };
            outgoing.BeginAnimation(OpacityProperty, fadeOut);

            var fadeIn = new DoubleAnimation(1, new Duration(TimeSpan.FromMilliseconds(320)))
            {
                EasingFunction = easing,
                BeginTime = TimeSpan.FromMilliseconds(80)
            };
            fadeIn.Completed += (_, _) =>
            {
                if (_viewAnimGen != gen) return;
                incoming.BeginAnimation(OpacityProperty, null);
                incoming.Opacity = 1;
                ClearOverlayTransformAnimations(to);
            };
            incoming.BeginAnimation(OpacityProperty, fadeIn);

            var slideUp = new DoubleAnimation(0, duration)
            {
                EasingFunction = easing,
                BeginTime = TimeSpan.FromMilliseconds(80)
            };
            incomingTranslate.BeginAnimation(TranslateTransform.YProperty, slideUp);
        }

        private void AnimateOverlayToPlayer(ActiveView from, int gen)
        {
            ClearLayoutAnimations();

            var incoming = _isPortrait ? (FrameworkElement)PortraitContent : LandscapeContent;
            var outgoing = GetViewElement(from);
            var outgoingTranslate = GetOverlayTranslate(from);

            var duration = new Duration(TimeSpan.FromMilliseconds(350));
            var easing = new CubicEase { EasingMode = EasingMode.EaseInOut };

            incoming.Visibility = Visibility.Visible;
            incoming.Opacity = 0;

            var inTranslate = _isPortrait ? PortraitLyricsView.LyricsTranslate : LandscapeLyricsView.LyricsTranslate;
            inTranslate.Y = 30;

            outgoingTranslate.Y = 0;
            var fadeOut = new DoubleAnimation(0, new Duration(TimeSpan.FromMilliseconds(220)))
            {
                EasingFunction = new CubicEase { EasingMode = EasingMode.EaseIn }
            };
            fadeOut.Completed += (_, _) =>
            {
                if (_viewAnimGen != gen) return;
                outgoing.Visibility = Visibility.Collapsed;
                outgoing.BeginAnimation(OpacityProperty, null);
                outgoing.Opacity = 1;
                ClearOverlayTransformAnimations(from);
            };
            outgoing.BeginAnimation(OpacityProperty, fadeOut);

            var slideDown = new DoubleAnimation(30, new Duration(TimeSpan.FromMilliseconds(250)))
            {
                EasingFunction = new CubicEase { EasingMode = EasingMode.EaseIn }
            };
            outgoingTranslate.BeginAnimation(TranslateTransform.YProperty, slideDown);

            var fadeIn = new DoubleAnimation(1, new Duration(TimeSpan.FromMilliseconds(320)))
            {
                EasingFunction = easing,
                BeginTime = TimeSpan.FromMilliseconds(80)
            };
            fadeIn.Completed += (_, _) =>
            {
                if (_viewAnimGen != gen) return;
                incoming.BeginAnimation(OpacityProperty, null);
                incoming.Opacity = 1;
                ClearTransformAnimations(inTranslate, null);

                Dispatcher.BeginInvoke(() =>
                {
                    ScrollLyricToCenter(
                        _isPortrait ? PortraitLyricsView.LyricsList : LandscapeLyricsView.LyricsList,
                        ViewModel.Lyrics.CurrentLyricIndex);
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
                    ViewModel.Lyrics.CurrentLyricIndex);
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
            SettingsContent.BeginAnimation(OpacityProperty, null);
            SettingsContent.Opacity = 1;

            ClearTransformAnimations(LandscapeAlbumTranslate, LandscapeAlbumScale);
            ClearTransformAnimations(PlayerToolbar.LandscapeSongInfoTranslate, null);
            ClearTransformAnimations(LandscapeLyricsView.LyricsTranslate, null);
            ClearTransformAnimations(PortraitAlbumTranslate, PortraitAlbumScale);
            ClearTransformAnimations(PortraitSongInfoTranslate, null);
            ClearTransformAnimations(PortraitLyricsView.LyricsTranslate, null);
            ClearOverlayTransformAnimations(ActiveView.Playlist);
            ClearOverlayTransformAnimations(ActiveView.Settings);
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

        private void HomeButton_Click(object sender, RoutedEventArgs e)
        {
            ViewModel.ActiveView = ActiveView.Player;
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
                    ViewModel.ActiveView = ViewModel.ActiveView == ActiveView.Playlist
                        ? ActiveView.Player : ActiveView.Playlist;
                    break;
                case 2: // Equalizer
                    OpenEqualizer();
                    break;
                case 3: // Settings
                    ViewModel.ActiveView = ViewModel.ActiveView == ActiveView.Settings
                        ? ActiveView.Player : ActiveView.Settings;
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
                case 0: // Reboot
                    if (WpfMessageBox.Show("您确定要重启应用吗？", "重启应用", WpfMessageBoxButton.OKCancel, WpfMessageBoxIcon.Question) == WpfMessageBoxResult.OK)
                    {
                        RebootApplicationHelper.RebootApplication();
                    }
                    break;
                case 1: // About
                    WpfMessageBox.Show(
                        $"Version: {ViewModel.Version}\nCommit: {ViewModel.CommitId}",
                        "关于 WpfMusicPlayer...",
                        WpfMessageBoxIcon.Information);
                    break;
            }
        }

        private void HideApplicationButton_Click(object sender, RoutedEventArgs e)
        {
            ShowMiniPlayer();
        }

        private void MainWindow_StateChanged(object sender, EventArgs e)
        {
            if (_isClosing)
                return;

            if (WindowState == WindowState.Minimized)
            {
                QueueHideMainWindowToTray();
                return;
            }

            QueueWindowVisualRefresh();
        }

        private void QueueHideMainWindowToTray()
        {
            if (_isHidingToTray || _hideToTrayPending)
                return;

            _hideToTrayPending = true;
            Dispatcher.BeginInvoke(() =>
            {
                _hideToTrayPending = false;

                if (_isClosing || !IsVisible || WindowState != WindowState.Minimized)
                    return;

                HideMainWindowToTray();
            }, DispatcherPriority.Background);
        }

        private void HideMainWindowToTray()
        {
            _isHidingToTray = true;
            try
            {
                ViewModel.DesktopTrayIcon.EnableTaskbarIcon();
                Hide();
                _miniPlayerWindow?.Hide();
            }
            finally
            {
                _isHidingToTray = false;
            }
        }

        private void QueueWindowVisualRefresh()
        {
            if (_windowVisualRefreshPending)
                return;

            _windowVisualRefreshPending = true;
            Dispatcher.BeginInvoke(() =>
            {
                _windowVisualRefreshPending = false;

                if (_isClosing || !IsVisible || WindowState == WindowState.Minimized)
                    return;

                ReapplyWindowBackground();
                ResetActiveViewVisibility();
                InvalidateVisual();
                ContentArea.InvalidateVisual();
                UpdateLayout();
            }, DispatcherPriority.ContextIdle);
        }

        private void ResetActiveViewVisibility()
        {
            ClearLayoutAnimations();

            if (ViewModel.ActiveView == ActiveView.Player)
            {
                LandscapeContent.Visibility = _isPortrait ? Visibility.Collapsed : Visibility.Visible;
                PortraitContent.Visibility = _isPortrait ? Visibility.Visible : Visibility.Collapsed;
                PlaylistContent.Visibility = Visibility.Collapsed;
                SettingsContent.Visibility = Visibility.Collapsed;
                return;
            }

            LandscapeContent.Visibility = Visibility.Collapsed;
            PortraitContent.Visibility = Visibility.Collapsed;
            PlaylistContent.Visibility = ViewModel.ActiveView == ActiveView.Playlist ? Visibility.Visible : Visibility.Collapsed;
            SettingsContent.Visibility = ViewModel.ActiveView == ActiveView.Settings ? Visibility.Visible : Visibility.Collapsed;
        }

        private void ShowMiniPlayer()
        {
            if (_miniPlayerWindow == null)
            {
                _miniPlayerWindow = new MiniPlayerWindow(ViewModel);
                _miniPlayerWindow.RestoreRequested += MiniPlayerWindow_RestoreRequested;
                _miniPlayerWindow.Closed += MiniPlayerWindow_Closed;
            }

            PositionMiniPlayer(_miniPlayerWindow);
            HideDesktopLyricForMiniPlayer();
            ViewModel.DesktopTrayIcon.DisableTaskbarIcon();
            _miniPlayerWindow.Show();
            _miniPlayerWindow.Activate();
            Hide();
        }

        private void MiniPlayerWindow_RestoreRequested(object? sender, EventArgs e)
        {
            var miniPlayer = _miniPlayerWindow;
            miniPlayer?.Hide();
            RestoreMainWindowFromMiniPlayer(miniPlayer);
        }

        private void MiniPlayerWindow_Closed(object? sender, EventArgs e)
        {
            var closedMiniPlayer = sender as Window ?? _miniPlayerWindow;
            if (_miniPlayerWindow != null)
            {
                _miniPlayerWindow.RestoreRequested -= MiniPlayerWindow_RestoreRequested;
                _miniPlayerWindow.Closed -= MiniPlayerWindow_Closed;
                _miniPlayerWindow = null;
            }

            if (!_isClosing && !IsVisible)
                RestoreMainWindowFromMiniPlayer(closedMiniPlayer);
        }

        private void RestoreMainWindowFromMiniPlayer(Window? miniPlayer)
        {
            ViewModel.DesktopTrayIcon.DisableTaskbarIcon();
            if (miniPlayer != null)
                PositionMainWindowFromMiniPlayer(miniPlayer);

            Show();
            WindowState = WindowState.Normal;
            Activate();
            QueueWindowVisualRefresh();
            RestoreDesktopLyricAfterMiniPlayer();
        }

        private void PositionMiniPlayer(Window miniPlayer)
        {
            var screenBounds = GetVirtualScreenBounds();
            var left = Left + Math.Max(0, (ActualWidth - miniPlayer.Width) / 2);
            var top = Top + Math.Max(0, (ActualHeight - miniPlayer.Height) / 2);

            miniPlayer.Left = ClampWindowCoordinate(left, screenBounds.Left, screenBounds.Right - miniPlayer.Width);
            miniPlayer.Top = ClampWindowCoordinate(top, screenBounds.Top, screenBounds.Bottom - miniPlayer.Height);
        }

        private void PositionMainWindowFromMiniPlayer(Window miniPlayer)
        {
            var screenBounds = GetVirtualScreenBounds();
            var mainSize = GetMainWindowPlacementSize();
            var left = miniPlayer.Left - Math.Max(0, (mainSize.Width - miniPlayer.Width) / 2);
            var top = miniPlayer.Top - Math.Max(0, (mainSize.Height - miniPlayer.Height) / 2);

            WindowState = WindowState.Normal;
            Left = ClampWindowCoordinate(left, screenBounds.Left, screenBounds.Right - mainSize.Width);
            Top = ClampWindowCoordinate(top, screenBounds.Top, screenBounds.Bottom - mainSize.Height);
        }

        private Size GetMainWindowPlacementSize()
        {
            var width = WindowState == WindowState.Normal ? ActualWidth : RestoreBounds.Width;
            var height = WindowState == WindowState.Normal ? ActualHeight : RestoreBounds.Height;

            if (width <= 0 || double.IsNaN(width))
                width = !double.IsNaN(Width) && Width > 0 ? Width : MinWidth;
            if (height <= 0 || double.IsNaN(height))
                height = !double.IsNaN(Height) && Height > 0 ? Height : MinHeight;

            return new Size(width, height);
        }

        private static Rect GetVirtualScreenBounds()
            => new(
                SystemParameters.VirtualScreenLeft,
                SystemParameters.VirtualScreenTop,
                SystemParameters.VirtualScreenWidth,
                SystemParameters.VirtualScreenHeight);

        private static double ClampWindowCoordinate(double value, double min, double max)
            => max < min ? min : Math.Clamp(value, min, max);

        private void HideDesktopLyricForMiniPlayer()
        {
            if (_hasDesktopLyricStateBeforeMiniPlayer) return;

            _wasDesktopLyricVisibleBeforeMiniPlayer = ViewModel.DesktopLyric.IsDesktopLyricVisible;
            _hasDesktopLyricStateBeforeMiniPlayer = true;

            if (_wasDesktopLyricVisibleBeforeMiniPlayer)
                ViewModel.DesktopLyric.IsDesktopLyricVisible = false;
        }

        private void RestoreDesktopLyricAfterMiniPlayer()
        {
            if (!_hasDesktopLyricStateBeforeMiniPlayer) return;

            var shouldRestore = _wasDesktopLyricVisibleBeforeMiniPlayer;
            _hasDesktopLyricStateBeforeMiniPlayer = false;
            _wasDesktopLyricVisibleBeforeMiniPlayer = false;

            if (shouldRestore)
                ViewModel.DesktopLyric.IsDesktopLyricVisible = true;
        }

    }
}
