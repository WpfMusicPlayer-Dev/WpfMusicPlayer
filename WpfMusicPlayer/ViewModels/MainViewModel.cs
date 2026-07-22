// SPDX-License-Identifier: MIT

using System.ComponentModel;
using System.Windows;
using MusicPlayerLibrary;
using System.IO;
using System.Reflection;
using System.Runtime; 
using System.Security.Cryptography;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using SkiaSharp;
using WpfMusicPlayer.Helpers;
using WpfMusicPlayer.Models;
using WpfMusicPlayer.Services.Abstractions;
using static WpfMusicPlayer.Models.ConfigData;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using Microsoft.Extensions.Logging;

namespace WpfMusicPlayer.ViewModels;

public enum ActiveView { Player, Playlist, Settings }

public enum PlayMode { Sequential, ListLoop, SingleLoop, Shuffle }

public partial class MainViewModel : ObservableObject, IDisposable
{
    // TODO: refractor god object, MainViewModel
    private readonly IConfigProvider _configProvider;
    private readonly IFileDialogService _fileDialogService;
    private readonly ISmtcService _smtcService;
    private readonly ISongDatabaseService _songDatabase;
    private readonly ICommandLineParser _commandLineParser;
    private readonly Func<INcmAlbumArtDownloader> _ncmAlbumArtDownloaderFactory;
    private readonly SynchronizationContext _syncContext;
    private readonly Random _shuffleRandom = new();
    private readonly Stack<string> _shuffleHistory = new();
    private readonly SemaphoreSlim _openFileLock = new(1, 1);
    private readonly object _playerStateLock = new();
    private MusicPlayerManaged _musicPlayer;
    private PlayerLoadContext? _activeLoadContext;
    private string? _currentFilePath;
    private string? _currentMd5;
    private int _sampleRate;
    private readonly AudioSettings.ChannelType _channelMode;
    private readonly AudioSettings.BitDepthType _bitDepth;
    private readonly AudioSettings.BackendType _audioBackend;
    private readonly string _outputDeviceId;
    private readonly int _resolvedDeviceSampleRate;
    private bool _enableAutoPlay;
    private float? _pendingSeekTime;
    private readonly GCLatencyMode _previousLatencyMode;
    private bool _isRestoredFromCommandLine;
    private bool _disableAutoAdvance;
    private bool _playCountIncrementedForCurrentSong;
    private bool _isDisposed;
    private int _albumArtVersion;
    private readonly ILogger<MainViewModel> _logger;
    private const int AlbumArtLogicalSize = 500;
    private const int BlurredAlbumArtPixelSize = 320;
    private const float BlurredAlbumArtSigma = 24f;
    private const int SpectrumSegmentCount = 32;
    private const string NoFileOpenedTitle = "No file opened";
    private const string UnknownTitle = "Unknown Title";
    private const string UnknownArtist = "Unknown Artist";
    private const string PauseString = "\uE769";
    private const string PlayString = "\uF5B0";
    private const string LosslessAudioTitle = "无损音频";
    private const string HiResLosslessAudioTitle = "高解析度无损";
    private bool _spectrumBufferHasData;

    public MainViewModel(
        IConfigProvider configProvider,
        IFileDialogService fileDialogService,
        ISmtcService smtcService,
        ISongDatabaseService songDatabase,
        ICommandLineParser commandLineParser,
        Func<INcmAlbumArtDownloader> ncmAlbumArtDownloaderFactory,
        PlaylistViewModel playlist,
        LyricsViewModel lyrics,
        DesktopLyricViewModel desktopLyric,
        DesktopTrayIconViewModel desktopTrayIcon,
        IAudioOutputFormatProvider audioOutputFormatProvider,
        ILogger<MainViewModel> logger)
    {
        _configProvider = configProvider;
        _fileDialogService = fileDialogService;
        _smtcService = smtcService;
        _songDatabase = songDatabase;
        _commandLineParser = commandLineParser;
        _ncmAlbumArtDownloaderFactory = ncmAlbumArtDownloaderFactory;
        _logger = logger;
        _previousLatencyMode = GCSettings.LatencyMode;
        _syncContext = SynchronizationContext.Current!;
        Equalizer = new EqualizerViewModel(ApplyEqualizerBand);
        Settings = new SettingsViewModel(configProvider, audioOutputFormatProvider);
        Settings.SettingChanged += OnSettingChanged;
        Playlist = playlist;
        Playlist.IsMusicPlayingQuery = () => _musicPlayer!.IsPlaying();
        Playlist.PlaySongRequested += OnPlaylistSongRequested;
        Playlist.ResetPlaylistRequested += OnPlaylistResetRequested;
        Playlist.RemovePlaylistItemRequested += OnRemovePlaylistItemRequested;
        Lyrics = lyrics;
        Lyrics.SeekRequested += OnLyricsSeekRequested;
        Lyrics.UpdateCurrentLyricRequested += OnLyricsUpdateDatabaseRequested;
        Lyrics.UpdateCurrentLyricOffsetRequested += OnLyricsUpdateOffsetMsRequired;
        DesktopLyric = desktopLyric;
        DesktopLyric.PropertyChanged += OnDesktopLyricPropertyChanged;
        DesktopTrayIcon = desktopTrayIcon;
        CurrentBackgroundMode = configProvider.GetConfig().UI.Background;
        ref var startupConfig = ref configProvider.GetConfig();
        _sampleRate = startupConfig.Audio.SampleRate;
        _channelMode = startupConfig.Audio.Channel;
        _bitDepth = startupConfig.Audio.BitDepth;
        _audioBackend = startupConfig.Audio.Backend;
        _outputDeviceId = startupConfig.Audio.OutputDeviceId ?? string.Empty;
        PendingSampleRate = _sampleRate;
        PendingChannelMode = _channelMode;
        PendingBitDepth = _bitDepth;
        PendingAudioBackend = _audioBackend;
        PendingOutputDeviceId = _outputDeviceId;
        _musicPlayer = CreateMusicPlayer();
        _resolvedDeviceSampleRate = _musicPlayer.GetDeviceOutputSampleRate();
        UpdateEqualizerSampleRate(_sampleRate);
        
        var info = Assembly
            .GetExecutingAssembly()
            .GetCustomAttributes<AssemblyInformationalVersionAttribute>()
            .First();

        var full = info.InformationalVersion;
        var parts = full.Split('+');

        Version = parts[0];
        CommitId = parts.Length > 1 ? parts[1][..7] : "N/A";

        SubscribeSmtcEvents();
        RestoreSettingsFromCommandLine();
        _logger.LogInformation(
            "MainViewModel initialized, backend: {AudioBackend}, output device: {OutputDeviceId}, sample rate: {SampleRate}, channel mode: {ChannelMode}, bit depth: {BitDepth}",
            _audioBackend, _outputDeviceId, _sampleRate, _channelMode, _bitDepth);
    }

    private MusicPlayerManaged CreateMusicPlayer() =>
        new(
            _sampleRate,
            (int)_channelMode,
            (int)_bitDepth,
            (int)_audioBackend,
            _outputDeviceId);

    internal static int ResolveEqualizerSampleRate(
        AudioSettings.BackendType backend,
        int configuredSampleRate,
        int resolvedDeviceSampleRate)
    {
#if WINDOWS
        if (backend == AudioSettings.BackendType.WasapiExclusive &&
            resolvedDeviceSampleRate > 0)
        {
            return resolvedDeviceSampleRate;
        }
#endif
        return configuredSampleRate;
    }

    private void UpdateEqualizerSampleRate(int configuredSampleRate) =>
        Equalizer.SetSampleRate(ResolveEqualizerSampleRate(
            _audioBackend,
            configuredSampleRate,
            _resolvedDeviceSampleRate));

    private static string FormatAudioBackend(AudioSettings.BackendType backend) =>
        backend switch
        {
            AudioSettings.BackendType.FAudio => "FAudio（共享模式）",
#if WINDOWS
            AudioSettings.BackendType.WasapiExclusive => "WASAPI（独占模式）",
#endif
            _ => backend.ToString()
        };

    private void OnDesktopLyricPropertyChanged(object? sender, PropertyChangedEventArgs e)
    {
        switch (e.PropertyName)
        {
            case nameof(DesktopLyric.IsDesktopLyricVisible):
                Settings.SelectedDesktopLyricEnabled = DesktopLyric.IsDesktopLyricVisible;
                break;
            case nameof(DesktopLyricViewModel.FontSize):
                Settings.SelectedDesktopLyricFontSize = DesktopLyric.FontSize;
                break;
            case nameof(DesktopLyricViewModel.AuxFontSize):
                Settings.SelectedDesktopLyricAuxFontSize = DesktopLyric.AuxFontSize;
                break;
            case nameof(DesktopLyricViewModel.IsAuxInfoCustomizable):
                Settings.SelectedDesktopLyricIsAuxInfoCustomizable = DesktopLyric.IsAuxInfoCustomizable;
                break;
        }
    }

    private void OnRemovePlaylistItemRequested(string filePath)
    {
        if (filePath == _currentFilePath)
        {
            OnPlaylistResetRequested();
        }
    }

    private void OnPlaylistSongRequested(string filePath, bool autoPlay)
    {
        _enableAutoPlay = autoPlay;
        _playCountIncrementedForCurrentSong = false;
        QueueOpenFile(filePath);
    }

    private async void OnLyricsSeekRequested(float targetTimeSec)
    {
        if (!_musicPlayer.IsInitialized()) return;

        var isPlaying = _musicPlayer.IsPlaying();
        _logger.LogInformation("OnLyricsSeekRequested: targetTimeSec={TargetTime}s", targetTimeSec);

        IsDraggingSlider = true;

        await Task.Run(() =>
        {
            _musicPlayer.SeekToPosition(targetTimeSec, true);
        });

        if (isPlaying)
        {
            _musicPlayer.Start();
        }

        await Task.Delay(200);
        IsDraggingSlider = false;
        if (!isPlaying)
        {
            ProgressValue = targetTimeSec;
            CurrentTime = FormatTime(targetTimeSec);
        }
        Lyrics.UpdateLyricProgress(targetTimeSec);
    }

    private void OnLyricsUpdateDatabaseRequested(string lyricContent, int offsetMs)
    {
        if (string.IsNullOrEmpty(_currentMd5)) return;

        var current = _songDatabase.FindByMd5(_currentMd5) ?? new SongRecord
        {
            Md5 = _currentMd5,
            Title = SongTitle,
            Artist = ArtistName
        };
        current.CustomLyric = lyricContent;
        current.CustomLyricOffsetMs = offsetMs;
        _logger.LogInformation("OnLyricsUpdateDatabaseRequired: offsetMs={offsetMs}", offsetMs);
        _songDatabase.Upsert(current);
    }

    private void OnLyricsUpdateOffsetMsRequired(int offsetMs)
    {
        var current = _songDatabase.FindByMd5(_currentMd5 ?? string.Empty);
        current?.CustomLyricOffsetMs = offsetMs;
        _logger.LogInformation("OnLyricsUpdateOffsetMsRequired: offsetMs={OffsetMs}", offsetMs);
        if (current is not null)
            _songDatabase.Upsert(current);
    }

    private async void OnPlaylistResetRequested()
    {
        ResetPlayerToUnloadedState();

        if (Playlist.PlaylistItems.Count <= 0) return;
        _enableAutoPlay = false;
        await OpenFileAsync(Playlist.PlaylistItems[0].FilePath);
    }

    private void RestoreSettingsFromCommandLine()
    {
        _logger.LogInformation("Restoring settings from command line");
        Volume = _configProvider.GetConfig().Audio.Volume;
        DesktopLyric.FontSize = _configProvider.GetConfig().DesktopLyric.DesktopLyricFontSize;
        DesktopLyric.AuxFontSize = _configProvider.GetConfig().DesktopLyric.DesktopLyricAuxFontSize;
        DesktopLyric.IsAuxInfoCustomizable = _configProvider.GetConfig().DesktopLyric.IsDesktopLyricAuxCustomizable;
        DesktopLyric.IsDesktopLyricVisible = _configProvider.GetConfig().DesktopLyric.IsDesktopLyricEnabled;

        if (string.IsNullOrEmpty(_commandLineParser.FilePath))
        {
            _logger.LogInformation("No file path from command line, using startup view: {View}", _commandLineParser.StartupView);
            ActiveView = _commandLineParser.StartupView;
            return;
        }

        Lyrics.IsTranslationVisible = _commandLineParser.TranslationToggled;
        Lyrics.IsRomanjiVisible = _commandLineParser.RomanjiToggled;

        if (_commandLineParser.AppliedEqualizerSettings.Length > 0)
        {
            for (var i = 0; i < _commandLineParser.AppliedEqualizerSettings.Length && i < Equalizer.Bands.Count; i++)
            {
                if (!Equalizer.IsBandEnabled(i)) continue;
                Equalizer.Bands[i].Value = _commandLineParser.AppliedEqualizerSettings[i];
            }
        }

        _enableAutoPlay = _commandLineParser.AutoStart;
        if (_commandLineParser.MusicCurrentTime > 0)
            _pendingSeekTime = _commandLineParser.MusicCurrentTime;

        ActiveView = _commandLineParser.StartupView;
        if (ActiveView != ActiveView.Player)
        {
            _isRestoredFromCommandLine = true;
        }

        DesktopLyric.IsDesktopLyricVisible = _commandLineParser.IsDesktopLyricToggled;
        CurrentPlayMode = _commandLineParser.CurrentPlayMode;

        _logger.LogInformation("Restoring file from command line: {FilePath}, autoPlay: {AutoPlay}, seekTime: {SeekTime}",
            _commandLineParser.FilePath, _enableAutoPlay, _pendingSeekTime);
        QueueOpenFile(_commandLineParser.FilePath);
    }

    public EqualizerViewModel Equalizer { get; }

    private void ApplyEqualizerBand(int index, int value)
    {
        if (!Equalizer.IsBandEnabled(index)) return;
        _musicPlayer.SetEqualizerBand(index, value);
        IsBitPerfect = _musicPlayer.IsBitPerfect();
    }

    [ObservableProperty]
    public partial string SongTitle { get; private set; } = NoFileOpenedTitle;

    [ObservableProperty]
    public partial string ArtistName { get; private set; } = UnknownArtist;

    [ObservableProperty]
    public partial BitmapImage? AlbumCoverImage { get; private set; }

    [ObservableProperty]
    public partial BitmapSource? BlurredAlbumCoverImage { get; private set; }

    [ObservableProperty]
    public partial string CurrentTime { get; private set; } = "0:00";

    [ObservableProperty]
    public partial string TotalTime { get; private set; } = "0:00";

    [ObservableProperty]
    public partial double ProgressValue { get; set; }

    [ObservableProperty]
    public partial double ProgressMaximum { get; private set; } = 100;

    public double Volume
    {
        get;
        set
        {
            if (SetProperty(ref field, value))
            {
                _musicPlayer.SetMasterVolume((float)value);
                Settings.SelectedVolume = Volume;
            }
        }
    } = 0.5;

    [ObservableProperty]
    public partial string PlayPauseContent { get; private set; } = PlayString;

    public bool IsDraggingSlider { get; set; }

    public bool IsFileDialogOpen { get; set; }

    public bool IsDecoding
    {
        get;
        private set
        {
            if (SetProperty(ref field, value))
            {
                CommandManager.InvalidateRequerySuggested();
            }
        }
    }

    [ObservableProperty]
    public partial float[] SpectrumData { get; private set; } = new float[SpectrumSegmentCount];

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(AudioQualityTitle))]
    public partial bool IsLoselessAudio { get; private set; }

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(AudioQualityTitle))]
    public partial bool IsHiResAudio { get; private set; }

    [ObservableProperty]
    public partial bool IsBitPerfect { get; private set; }

    [ObservableProperty]
    public partial string AudioSourceFormat { get; private set; } = string.Empty;

    [ObservableProperty] 
    public partial string DeviceOutputFormat { get; private set; } = string.Empty;

    public string AudioQualityTitle => IsLoselessAudio && IsHiResAudio
        ? HiResLosslessAudioTitle
        : LosslessAudioTitle;

    public string BitPerfectTitle => "Bit-Perfect 输出";

    public PlaylistViewModel Playlist { get; }

    public LyricsViewModel Lyrics { get; }

    [ObservableProperty]
    public partial ActiveView ActiveView { get; set; }

    public string Version { get; }

    public string CommitId { get; }

    public SettingsViewModel Settings { get; }

    public DesktopLyricViewModel DesktopLyric { get; }
    public DesktopTrayIconViewModel DesktopTrayIcon { get; }

    [ObservableProperty]
    public partial UISettings.BackgroundMode CurrentBackgroundMode { get; private set; }

    [ObservableProperty]
    public partial int PendingSampleRate { get; private set; }

    [ObservableProperty]
    public partial AudioSettings.ChannelType PendingChannelMode { get; private set; }

    [ObservableProperty]
    public partial AudioSettings.BitDepthType PendingBitDepth { get; private set; }

    [ObservableProperty]
    public partial AudioSettings.BackendType PendingAudioBackend { get; private set; }

    [ObservableProperty]
    public partial string PendingOutputDeviceId { get; private set; } = string.Empty;

    public bool IsSampleRateRestartRequired => PendingSampleRate != _sampleRate;

    public bool IsChannelModeRestartRequired => PendingChannelMode != _channelMode;

    public bool IsBitDepthRestartRequired => PendingBitDepth != _bitDepth;

    public bool IsAudioBackendRestartRequired => PendingAudioBackend != _audioBackend;

    public bool IsOutputDeviceRestartRequired => !string.Equals(
        PendingOutputDeviceId,
        _outputDeviceId,
        StringComparison.OrdinalIgnoreCase);

    public bool IsAudioSettingsRestartRequired =>
        IsAudioBackendRestartRequired ||
        IsOutputDeviceRestartRequired ||
        IsSampleRateRestartRequired ||
        IsChannelModeRestartRequired ||
        IsBitDepthRestartRequired;

    public string PendingAudioSettingsSummary
    {
        get
        {
            List<string> changes = [];
            if (IsAudioBackendRestartRequired)
                changes.Add($"音频后端：{FormatAudioBackend(PendingAudioBackend)}");
            if (IsOutputDeviceRestartRequired)
                changes.Add($"输出设备：{Settings.GetOutputDeviceDisplayName(PendingOutputDeviceId)}");
            if (IsSampleRateRestartRequired)
                changes.Add($"采样率：{MusicPlayerManaged.FormatAudioSampleRate(PendingSampleRate)}");
            if (IsChannelModeRestartRequired)
                changes.Add($"声道：{MusicPlayerManaged.FormatAudioChannelType((int)PendingChannelMode)}");
            if (IsBitDepthRestartRequired)
                changes.Add($"位宽：{MusicPlayerManaged.FormatAudioBitDepth((int)PendingBitDepth)}");
            return string.Join("\n", changes.Select(change => $"• {change}"));
        }
    }

    public PlayMode CurrentPlayMode
    {
        get;
        private set
        {
            if (!SetProperty(ref field, value)) return;
            OnPropertyChanged(nameof(PlayModeContent));
            OnPropertyChanged(nameof(PlayModeTooltip));
        }
    }

    public string PlayModeContent => CurrentPlayMode switch
    {
        PlayMode.Sequential => "\uE8FD",
        PlayMode.ListLoop => "\uE8EE",
        PlayMode.SingleLoop => "\uE8ED",
        PlayMode.Shuffle => "\uE8B1",
        _ => "\uF5E7"
    };

    public string PlayModeTooltip => CurrentPlayMode switch
    {
        PlayMode.Sequential => "顺序播放",
        PlayMode.ListLoop => "列表循环",
        PlayMode.SingleLoop => "单曲循环",
        PlayMode.Shuffle => "随机播放",
        _ => "顺序播放"
    };

    public IReadOnlyList<string> ExtensionList =>
        _fileDialogService.MusicFileExtensions;

    // for RebootApplication to build command line args
    public bool IsMusicPlaying => _musicPlayer.IsPlaying();

    public void OpenFile(string filePath) =>
        OpenFileAsync(filePath).GetAwaiter().GetResult();

    public async Task OpenFileAsync(string filePath)
    {
        // 避免对_musicPlayer的重复析构
        _logger.LogInformation("Attempting to acquire OpenFile lock");
        await _openFileLock.WaitAsync().ConfigureAwait(false);
        var player = _musicPlayer;
        int loadVersion;
        lock (_playerStateLock)
        {
            loadVersion = Interlocked.Increment(ref _albumArtVersion);
            _activeLoadContext = null;
            _disableAutoAdvance = true;
        }

        try
        {
            lock (_playerStateLock)
            {
                if (loadVersion != Volatile.Read(ref _albumArtVersion))
                {
                    return;
                }

                player.CloseFile();
            }

            _syncContext.Post(_ =>
            {
                if (loadVersion == Volatile.Read(ref _albumArtVersion))
                    IsBitPerfect = false;
            }, null);

            _logger.LogInformation("OpenFile lock acquired, filePath: {FilePath}", filePath);
            if (ActiveView != ActiveView.Player
                && ActiveView != ActiveView.Playlist
                && !_isRestoredFromCommandLine)
            {
                _isRestoredFromCommandLine = false;
                _syncContext.Post(_ => ActiveView = ActiveView.Player, null);
            }

            var isNcm = Path.GetExtension(filePath).Equals(".ncm", StringComparison.OrdinalIgnoreCase);
            _syncContext.Post(_ =>
            {
                if (loadVersion == Volatile.Read(ref _albumArtVersion))
                    IsDecoding = isNcm;
            }, null);
            if (isNcm)
                _logger.LogInformation("NCM file detected, starting decode: {FilePath}", filePath);

            var md5 = await ComputeFileMd5Async(filePath).ConfigureAwait(false);
            _logger.LogInformation("File MD5 computed: {Md5}", md5);

            var cached = _songDatabase.FindByMd5(md5);
            var skipAlbumArtLoading = cached?.AlbumArt is { Length: > 0 };
            if (skipAlbumArtLoading)
            {
                _logger.LogInformation("Cached album art hit for {Md5}; native album art loading will be skipped", md5);
            }

            var loadContext = new PlayerLoadContext(loadVersion, filePath, md5, cached?.AlbumArt);
            lock (_playerStateLock)
            {
                if (loadVersion != Volatile.Read(ref _albumArtVersion))
                {
                    return;
                }

                ApplyEqualizerSettingsToPlayer(player);
                SubscribePlayerEvents(player, loadContext);
                _activeLoadContext = loadContext;
                _disableAutoAdvance = false;
                player.OpenFile(filePath, skipAlbumArtLoading);
            }

            if (!_enableAutoPlay)
            {
                _syncContext.Post(_ => PlayPauseContent = PlayString, null);
            }
        }
        catch
        {
            ResetPlayerToUnloadedState(loadVersion);
            throw;
        }
        finally
        {
            _openFileLock.Release();
        }
    }

    private void QueueOpenFile(string filePath)
    {
        _ = Task.Run(async () =>
        {
            try
            {
                await OpenFileAsync(filePath).ConfigureAwait(false);
            }
            catch (Exception ex)
            {
                _logger.LogError(ex, "Failed to open file {FilePath}", filePath);
                _syncContext.Post(_ =>
                {
                    WpfMessageBox.Show($"{ex.Message}\n{filePath}", "Error", WpfMessageBoxIcon.Error);
                }, null);
            }
        });
    }

    public async void SeekToCurrentPosition(float pct,float precision)
    {
        if (!_musicPlayer.IsInitialized()) return;
        var timeInSec = _musicPlayer.GetMusicTimeLength();
        if(pct > 1)
            pct = 1;
        if(pct < 0)
            pct = 0;
        //var targetTime = timeInSec * (float)ProgressValue / (float)ProgressMaximum;
        var targetTime = timeInSec * pct;
        var curTime = _musicPlayer.GetCurrentMusicPosition();
        var difTime = Math.Abs(targetTime - curTime);
        var targetdiff = Math.Max(timeInSec * precision, 0.5);//least 500ms
        if(difTime > targetdiff) {
            var isPlaying = _musicPlayer.IsPlaying();
            _logger.LogInformation("SeekToCurrentPosition: targetTime={TargetTime}s, isPlaying={IsPlaying}", targetTime, isPlaying);

            IsDraggingSlider = true;

            await Task.Run(() =>
            {
                _musicPlayer.SeekToPosition(targetTime, true);
            });

            if(isPlaying) {
                _musicPlayer.Start();
            }

            await Task.Delay(200);
            IsDraggingSlider = false;
            if(!isPlaying) {
                ProgressValue = targetTime;
                CurrentTime = FormatTime(targetTime);
            }
            Lyrics.UpdateLyricProgress(targetTime);
        }
    }

    public void OnWindowClosed()
    {
        _logger.LogInformation("Window closed, stopping music player");
        if (_musicPlayer.IsInitialized())
        {
            _musicPlayer.Stop();
        }
    }

    public bool HasUnsavedPlaylistChanges => Playlist.HasUnsavedPlaylistChanges;

    private void OnSettingChanged(object? sender, SettingChangedEventArgs e)
    {
        if (e.SettingName == nameof(SettingsViewModel.SelectedBackground))
        {
            CurrentBackgroundMode = _configProvider.GetConfig().UI.Background;
            return;
        }

        if (e.SettingName == SettingsViewModel.AudioOutputSettingsChangeName)
        {
            ref var config = ref _configProvider.GetConfig();
            PendingAudioBackend = config.Audio.Backend;
            PendingOutputDeviceId = config.Audio.OutputDeviceId ?? string.Empty;
            PendingSampleRate = config.Audio.SampleRate;
            PendingChannelMode = config.Audio.Channel;
            PendingBitDepth = config.Audio.BitDepth;
            UpdateEqualizerSampleRate(PendingSampleRate);
            NotifyAudioSettingsRestartStateChanged();
            return;
        }

        if (e.SettingName == nameof(SettingsViewModel.SelectedSampleRate))
        {
            PendingSampleRate = _configProvider.GetConfig().Audio.SampleRate;
            UpdateEqualizerSampleRate(PendingSampleRate);
            NotifyAudioSettingsRestartStateChanged();
            return;
        }

        if (e.SettingName == nameof(SettingsViewModel.SelectedChannel))
        {
            PendingChannelMode = _configProvider.GetConfig().Audio.Channel;
            NotifyAudioSettingsRestartStateChanged();
            return;
        }

        if (e.SettingName == nameof(SettingsViewModel.SelectedBitDepth))
        {
            PendingBitDepth = _configProvider.GetConfig().Audio.BitDepth;
            NotifyAudioSettingsRestartStateChanged();
            return;
        }

        if (e.SettingName == nameof(SettingsViewModel.SelectedOutputDeviceId))
        {
            PendingOutputDeviceId =
                _configProvider.GetConfig().Audio.OutputDeviceId ?? string.Empty;
            NotifyAudioSettingsRestartStateChanged();
        }
    }

    private void NotifyAudioSettingsRestartStateChanged()
    {
        OnPropertyChanged(nameof(IsAudioBackendRestartRequired));
        OnPropertyChanged(nameof(IsOutputDeviceRestartRequired));
        OnPropertyChanged(nameof(IsSampleRateRestartRequired));
        OnPropertyChanged(nameof(IsChannelModeRestartRequired));
        OnPropertyChanged(nameof(IsBitDepthRestartRequired));
        OnPropertyChanged(nameof(IsAudioSettingsRestartRequired));
        OnPropertyChanged(nameof(PendingAudioSettingsSummary));
    }

    public void PollSpectrumData()
    {
        Span<float> spectrum = SpectrumData;
        if (!_musicPlayer.IsInitialized() || !_musicPlayer.IsPlaying())
        {
            if (_spectrumBufferHasData)
            {
                spectrum.Clear();
                _spectrumBufferHasData = false;
            }
            return;
        }

        int copied = _musicPlayer.CopyAudioFFTData(SpectrumData);
        if (copied <= 0)
        {
            if (_spectrumBufferHasData)
            {
                spectrum.Clear();
                _spectrumBufferHasData = false;
            }
            return;
        }

        if (copied < spectrum.Length)
            spectrum[copied..].Clear();
        _spectrumBufferHasData = true;
    }

    public void Dispose()
    {
        if (!_isDisposed)
        {
            _logger.LogInformation("MainViewModel disposing, releasing resources");
            Settings.SettingChanged -= OnSettingChanged;
            Playlist.PlaySongRequested -= OnPlaylistSongRequested;
            Playlist.ResetPlaylistRequested -= OnPlaylistResetRequested;
            Lyrics.SeekRequested -= OnLyricsSeekRequested;
            Lyrics.UpdateCurrentLyricRequested -= OnLyricsUpdateDatabaseRequested;
            Lyrics.UpdateCurrentLyricOffsetRequested -= OnLyricsUpdateOffsetMsRequired;
            GCSettings.LatencyMode = _previousLatencyMode;
            _musicPlayer.Dispose();
            Interlocked.Increment(ref _albumArtVersion);
            _logger.LogInformation("MusicPlayer disposed");
            _songDatabase.Dispose();
            _logger.LogInformation("SongDatabase disposed");
            GC.SuppressFinalize(this);
        }

        _isDisposed = true;
    }

    public Task SetSampleRate(int sampleRate)
    {
        if (sampleRate < 8000 // Telephone quality
            || sampleRate > 192000) // Above high-resolution audio
        {
            throw new ArgumentOutOfRangeException(nameof(sampleRate), "Sample rate must be between 8000 and 192000 Hz.");
        }
        _logger.LogInformation(
            "Sample rate change queued for restart: {OldRate} -> {NewRate}",
            _sampleRate,
            sampleRate);
        PendingSampleRate = sampleRate;
        UpdateEqualizerSampleRate(sampleRate);
        NotifyAudioSettingsRestartStateChanged();
        return Task.CompletedTask;
    }

    private void SubscribePlayerEvents(MusicPlayerManaged player, PlayerLoadContext loadContext)
    {
        player.OnPlayerTimeChange = time => OnTimeChanged(player, loadContext, time);
        player.OnPlayerFileInit = () => OnFileInit(player, loadContext);
        player.OnPlayerAlbumArtInit = encodedImage =>
            ApplyIncomingAlbumArt(encodedImage, player, loadContext);
        player.OnPlayerNcmRequireAlbumArtDownload = url =>
            QueueNcmAlbumArtDownload(url, player, loadContext);
        player.OnPlayerStart = () => OnStart(player, loadContext);
        player.OnPlayerPause = () => OnPause(player, loadContext);
        player.OnPlayerStop = () => OnStop(player, loadContext);
        player.OnPlayerError = exception => OnPlayerError(player, loadContext, exception);
    }

    private void SubscribeSmtcEvents()
    {
        _smtcService.PlayRequested += () => _syncContext.Post(_ =>
        {
            if (!_musicPlayer.IsPlaying()) PlayPause().ConfigureAwait(false);
        }, null);
        _smtcService.PauseRequested += () => _syncContext.Post(_ =>
        {
            if (_musicPlayer.IsPlaying()) PlayPause().ConfigureAwait(false);
        }, null);
        _smtcService.NextRequested += () => _syncContext.Post(_ => NextSongCommand.Execute(null), null);
        _smtcService.PreviousRequested += () => _syncContext.Post(_ => PrevSongCommand.Execute(null), null);
    }

    private void OnFileInit(MusicPlayerManaged player, PlayerLoadContext loadContext)
    {
        _syncContext.Post(_ =>
        {
            if (!IsCurrentLoad(player, loadContext))
                return;

            try
            {
                _logger.LogInformation("OnFileInit: file initialized, loading metadata");
                var length = player.GetMusicTimeLength();
                var cached = _songDatabase.FindByMd5(loadContext.Md5);
                var embeddedTitle = player.GetSongTitle();
                var songTitle = ResolveSongTitle(cached?.Title, embeddedTitle, loadContext.FilePath);
                var artistName = ResolveArtistName(cached?.Artist, player.GetSongArtist());
                var albumCover = TryLoadAlbumCover(loadContext.AlbumArtPng);
                var isLoselessAudio = player.IsLoselessAudio();
                var isHiResAudio = player.IsHiResAudio();
                var isBitPerfect = player.IsBitPerfect();
                var audioSourceFormat = player.GetAudioSourceFormat() ?? string.Empty;
                var deviceOutputFormat = player.GetDeviceOutputFormat() ?? string.Empty;

                if (cached is null)
                {
                    _songDatabase.Upsert(new SongRecord
                    {
                        Md5 = loadContext.Md5,
                        Title = songTitle,
                        Artist = artistName,
                        AlbumArt = loadContext.AlbumArtPng
                    });
                }

                // 新歌曲的所有可见状态都在同一个 Dispatcher 周期内提交，避免切歌时闪烁半成品。
                _currentFilePath = loadContext.FilePath;
                _currentMd5 = loadContext.Md5;
                loadContext.IsCommitted = true;
                IsDecoding = false;
                SongTitle = songTitle;
                ArtistName = artistName;
                AlbumCoverImage = albumCover;
                BlurredAlbumCoverImage = null;
                AudioSourceFormat = audioSourceFormat;
                DeviceOutputFormat = deviceOutputFormat;
                IsHiResAudio = isHiResAudio;
                IsLoselessAudio = isLoselessAudio;
                IsBitPerfect = isBitPerfect;
                ProgressValue = 0;
                CurrentTime = "0:00";
                ProgressMaximum = length;
                TotalTime = FormatTime(length);
                QueueBlurredAlbumArtRender(loadContext.AlbumArtPng, loadContext.Version);

                AddToPlaylist();
                Playlist.UpdateItemMetadata(loadContext.FilePath, songTitle, artistName, albumCover);

                // 坑：UpdateMetadata会清除缩略图；内嵌封面通常早于 FileInit 到达，NCM 下载封面也可能早于此处完成。
                _smtcService.UpdateTextMetadata(songTitle, artistName);
                if (loadContext.AlbumArtPng is { Length: > 0 } smtcAlbumArt)
                {
                    using var albumArtStream = new MemoryStream(smtcAlbumArt);
                    _smtcService.UpdateMetadata(songTitle, artistName, albumArtStream);
                }
                DesktopTrayIcon.TaskBarToolTipText = $"{songTitle} - {artistName}";

                player.SetMasterVolume((float)Volume);

                // query custom lyric from database
                var customLyric = cached?.CustomLyric ?? string.Empty;
                var customLyricSource = SuppliedLyricSource.DatabaseCache;
                if (String.IsNullOrEmpty(customLyric))
                {
                    customLyric = player.GetID3Lyric();
                    customLyricSource = SuppliedLyricSource.Embedded;
                }

                var offsetMs = cached?.CustomLyricOffsetMs ?? 0;
                _logger.LogInformation("OnFileInit: custom lyric offset: {OffsetMs}ms", offsetMs);
                float curSeekTime = _pendingSeekTime ?? 0.0f;
                if (_pendingSeekTime is not null)
                {
                    _logger.LogInformation("OnFileInit: applying pending seek to {SeekTime}s", curSeekTime);
                    _pendingSeekTime = null;
                    player.SeekToPosition(curSeekTime, true);
                    ProgressValue = curSeekTime;
                    CurrentTime = FormatTime(curSeekTime);
                }

                // let lyrics load in background
                Lyrics.LoadLyricsAsync(
                    loadContext.FilePath,
                    customLyric,
                    songTitle,
                    length,
                    offsetMs,
                    customLyricSource)
                    .ContinueWith(_ =>
                    {
                        Lyrics.UpdateLyricProgress(curSeekTime);
                    }, TaskScheduler.FromCurrentSynchronizationContext())
                    .ConfigureAwait(false);
                if (_enableAutoPlay)
                    player.Start();
            }
            catch (Exception ex)
            {
                OnPlayerError(player, loadContext, ex);
            }
        }, null);
    }

    private void QueueNcmAlbumArtDownload(
        string? url,
        MusicPlayerManaged player,
        PlayerLoadContext loadContext)
    {
        if (!IsCurrentLoad(player, loadContext))
            return;

        _ = DownloadNcmAlbumArtAsync(url, player, loadContext);
    }

    private async Task DownloadNcmAlbumArtAsync(
        string? url,
        MusicPlayerManaged player,
        PlayerLoadContext loadContext)
    {
        try
        {
            var downloader = _ncmAlbumArtDownloaderFactory();
            var encodedImage = await downloader.DownloadAsync(url ?? string.Empty).ConfigureAwait(false);
            if (!IsCurrentLoad(player, loadContext))
                return;

            ApplyIncomingAlbumArt(encodedImage, player, loadContext);
        }
        catch (Exception ex)
        {
            if (IsCurrentLoad(player, loadContext))
                _logger.LogWarning(ex, "Failed to download NCM album art from {Url}", url);
        }
    }

    private void ApplyIncomingAlbumArt(
        byte[]? encodedImage,
        MusicPlayerManaged player,
        PlayerLoadContext loadContext)
    {
        _syncContext.Post(_ =>
        {
            if (!IsCurrentLoad(player, loadContext))
                return;

            try
            {
                var cached = _songDatabase.FindByMd5(loadContext.Md5);
                byte[]? albumArtPng = null;

                if (cached?.AlbumArt is { Length: > 0 })
                {
                    albumArtPng = cached.AlbumArt;
                    _logger.LogInformation("Cached MD5 hit, discarding incoming album art stream");
                }
                else
                {
                    _logger.LogInformation("MD5 miss, decoding album art stream with SkiaSharp");
                    albumArtPng = encodedImage is { Length: > 0 }
                        ? DecodeAlbumArtToPng(encodedImage)
                        : null;

                    if (cached is not null)
                    {
                        cached.AlbumArt = albumArtPng;
                        _songDatabase.Upsert(cached);
                    }
                }

                loadContext.AlbumArtPng = albumArtPng;
                if (!loadContext.IsCommitted)
                    return;

                AlbumCoverImage = TryLoadAlbumCover(albumArtPng);
                BlurredAlbumCoverImage = null;
                QueueBlurredAlbumArtRender(albumArtPng, loadContext.Version);
                Playlist.UpdateItemMetadata(loadContext.FilePath, null, null, AlbumCoverImage);

                Stream? stream = albumArtPng is { Length: > 0 }
                    ? new MemoryStream(albumArtPng)
                    : null;
                _smtcService.UpdateMetadata(SongTitle, ArtistName, stream);
                stream?.Dispose();
            }
            catch (Exception ex)
            {
                _logger.LogError(ex, "Failed to decode album art stream");
            }
        }, null);
    }

    private void ApplyEqualizerSettingsToPlayer(MusicPlayerManaged player)
    {
        for (var i = 0; i < Equalizer.Bands.Count; i++)
        {
            if (!Equalizer.IsBandEnabled(i)) continue;
            player.SetEqualizerBand(i, Equalizer.Bands[i].Value);
        }
    }

    private void OnTimeChanged(MusicPlayerManaged player, PlayerLoadContext loadContext, double time)
    {
        _syncContext.Post(_ =>
        {
            if (!IsCurrentLoad(player, loadContext)) return;
            if (IsDraggingSlider) return;

            ProgressValue = time;
            CurrentTime = FormatTime(time);
            Lyrics.UpdateLyricProgress(time);
        }, null);
    }

    private void IncrementPlayCount()
    {
        if (_currentMd5 is not null)
        {
            _songDatabase.IncrementPlayCount(_currentMd5);
        }
        _syncContext.Post(_ =>
        {
            Playlist.IncrementItemPlayedCount(_currentFilePath);
        }, null);
    }

    private void OnStart(MusicPlayerManaged player, PlayerLoadContext loadContext)
    {
        if (!IsCurrentLoad(player, loadContext)) return;
		var isBitPerfect = player.IsBitPerfect();
        _logger.LogInformation("OnStart: playback started, switching to SustainedLowLatency GC mode");
        GCSettings.LatencyMode = GCLatencyMode.SustainedLowLatency;
        if (!_playCountIncrementedForCurrentSong)
        {
            _playCountIncrementedForCurrentSong = true;
            IncrementPlayCount();
        }
        _syncContext.Post(_ =>
        {
            if (!IsCurrentLoad(player, loadContext)) return;
			IsBitPerfect = isBitPerfect;
            PlayPauseContent = PauseString;
            _smtcService.UpdatePlaybackStatus(PlaybackState.Playing);
            _enableAutoPlay = false;
        }, null);
    }

    private void OnPause(MusicPlayerManaged player, PlayerLoadContext loadContext)
    {
        if (!IsCurrentLoad(player, loadContext)) return;
        _logger.LogInformation("OnPause: playback paused, restoring GC mode");
        GCSettings.LatencyMode = _previousLatencyMode;
        _syncContext.Post(_ =>
        {
            if (!IsCurrentLoad(player, loadContext)) return;
            PlayPauseContent = PlayString;
            _smtcService.UpdatePlaybackStatus(PlaybackState.Paused);
            Lyrics.UpdateLyricProgress((float)ProgressValue);
        }, null);
    }

    private void OnStop(MusicPlayerManaged player, PlayerLoadContext loadContext)
    {
        if (!IsCurrentLoad(player, loadContext)) return;
        _logger.LogInformation("OnStop: playback stopped, restoring GC mode");
        GCSettings.LatencyMode = _previousLatencyMode;
        _syncContext.Post(_ =>
        {
            if (!IsCurrentLoad(player, loadContext)) return;
            PlayPauseContent = PlayString;
            ProgressValue = 0;
            CurrentTime = "0:00";
            _smtcService.UpdatePlaybackStatus(PlaybackState.Stopped);

            Lyrics.OnPlaybackStopped();
            if (!_disableAutoAdvance && !_isDisposed)
                AutoAdvance();
            _disableAutoAdvance = false;
            // 自然停止，重新播放时视为增加一次播放次数
            _playCountIncrementedForCurrentSong = false;
        }, null);
    }

    private void OnPlayerError(
        MusicPlayerManaged player,
        PlayerLoadContext loadContext,
        Exception? exception)
    {
        if (!IsCurrentLoad(player, loadContext)) return;
        exception ??= new InvalidOperationException("The audio worker failed without an error message.");
        _logger.LogError(exception, "Native audio worker failed");
        _disableAutoAdvance = true;
        GCSettings.LatencyMode = _previousLatencyMode;

        if (!ResetPlayerToUnloadedState(loadContext.Version))
            return;

        _syncContext.Post(_ =>
        {
            WpfMessageBox.Show(
                $"{exception.Message}\n{loadContext.FilePath}",
                "Error",
                WpfMessageBoxIcon.Error);
        }, null);
    }

    [RelayCommand]
    private void ShowAudioQualityInfo()
    {
        if (!IsLoselessAudio)
            return;

        WpfMessageBox.Show(
            AudioSourceFormat,
            AudioQualityTitle,
            WpfMessageBoxIcon.Information);
    }

    [RelayCommand]
    private void ShowDeviceQualityInfo()
    {
        if (!IsBitPerfect)
            return;
        
        WpfMessageBox.Show(
            $"当前的音频未经过任何后处理。\n设备输出格式: {DeviceOutputFormat}",
            BitPerfectTitle,
            WpfMessageBoxIcon.Information);
    }

    [RelayCommand]
    private void PlayModeToggle()
    {
        CurrentPlayMode = CurrentPlayMode switch
        {
            PlayMode.Sequential => PlayMode.ListLoop,
            PlayMode.ListLoop => PlayMode.SingleLoop,
            PlayMode.SingleLoop => PlayMode.Shuffle,
            _ => PlayMode.Sequential
        };
        _logger.LogInformation("Play mode changed to {PlayMode}", CurrentPlayMode);
        _shuffleHistory.Clear();
    }

    [RelayCommand]
    private void PrevSong()
    {
        var items = Playlist.PlaylistItems;
        if (items.Count == 0)
        {
            _logger.LogInformation("No songs available in playlist, stopping playback");
            StopPlayback();
            return;
        }

        var currentIndex = Playlist.GetIndexByPath(_currentFilePath);
        if (CurrentPlayMode == PlayMode.Shuffle)
        {
            if (_shuffleHistory.Count > 0)
            {
                _logger.LogInformation("Popping previous shuffled track from stack");
                var prevPath = _shuffleHistory.Pop();
                PlaySongByPath(prevPath);
            }
            else
            {
                int nextIndex;
                do
                {
                    nextIndex = _shuffleRandom.Next(items.Count);
                } while (nextIndex == currentIndex && items.Count > 1);
                PlaySongByPath(items[nextIndex].FilePath);
            }
            return;
        }

        if (currentIndex < 0)
        {
            _logger.LogInformation("Invalid current index, stopping playback");
            StopPlayback();
            return;
        }

        if (CurrentPlayMode == PlayMode.SingleLoop)
        {
            _logger.LogInformation("Single Loop triggered, restarting current song from beginning");
            _ = RestartCurrentSongFromBeginningAsync();
            return;
        }

        var prevIndex = currentIndex - 1;
        if (prevIndex < 0)
        {
            if (CurrentPlayMode == PlayMode.ListLoop)
            {
                _logger.LogInformation("List loop triggered, playing song in the end of playlist");
                prevIndex = items.Count - 1;
            }
            else
            {
                StopPlayback();
                return;
            }
        }

        PlaySongByPath(items[prevIndex].FilePath);
    }

    [RelayCommand]
    private void NextSong()
    {
        var items = Playlist.PlaylistItems;
        if (items.Count == 0)
        {
            _logger.LogInformation("No songs available in playlist, stopping playback");
            StopPlayback();
            return;
        }

        if (CurrentPlayMode == PlayMode.Shuffle)
        {
            if (_currentFilePath != null)
            {
                _logger.LogInformation("Push current index to shuffle stack");
                _shuffleHistory.Push(_currentFilePath);
            }

            if (items.Count == 1)
            {
                _logger.LogInformation("Shuffle enabled and only 1 file in playlist, repeating");
                PlaySongByPath(items[0].FilePath);
                return;
            }

            int nextIndex;
            var currentIndex = Playlist.GetIndexByPath(_currentFilePath);
            do
            {
                _logger.LogInformation("Attempting to generate new playlist index");
                nextIndex = _shuffleRandom.Next(items.Count);
            } while (nextIndex == currentIndex && items.Count > 1);

            PlaySongByPath(items[nextIndex].FilePath);
            return;
        }

        var curIndex = Playlist.GetIndexByPath(_currentFilePath);
        if (curIndex < 0)
        {
            StopPlayback();
            return;
        }

        if (CurrentPlayMode == PlayMode.SingleLoop)
        {
            _logger.LogInformation("Single Loop triggered, restarting current song from beginning");
            _ = RestartCurrentSongFromBeginningAsync();
            return;
        }

        var nextIdx = curIndex + 1;
        if (nextIdx >= items.Count)
        {
            if (CurrentPlayMode == PlayMode.ListLoop)
            {
                _logger.LogInformation("List loop triggered, playing song in the start of playlist");
                nextIdx = 0;
            }
            else
            {
                // 顺序播放在下一曲不可用时停止
                _logger.LogInformation("No songs available in playlist, stop!");
                StopPlayback();
                return;
            }
        }

        PlaySongByPath(items[nextIdx].FilePath);
    }

    private void AutoAdvance()
    {
        if (Playlist.PlaylistItems.Count == 0) return;
        _logger.LogInformation("AutoAdvance triggered, current play mode: {PlayMode}", CurrentPlayMode);

        switch (CurrentPlayMode)
        {
            case PlayMode.SingleLoop:
                _ = RestartCurrentSongFromBeginningAsync();
                break;
            case PlayMode.Sequential:
            case PlayMode.ListLoop:
            case PlayMode.Shuffle:
                NextSong();
                break;
            default:
                throw new ArgumentOutOfRangeException();
        }
    }

    private async Task RestartCurrentSongFromBeginningAsync()
    {
        var filePath = _currentFilePath;
        if (filePath is null)
        {
            _logger.LogInformation("Single Loop triggered without current file path, ignoring");
            return;
        }

        var player = _musicPlayer;
        if (!player.IsInitialized())
        {
            _logger.LogInformation("Single Loop fallback: player is not initialized, reopening current song");
            PlaySongByPath(filePath);
            return;
        }

        _logger.LogInformation("Single Loop triggered, seeking current song to beginning");
        _playCountIncrementedForCurrentSong = false;
        IsDraggingSlider = true;

        try
        {
            await Task.Run(() => player.SeekToPosition(0, true));
            if (_isDisposed || !ReferenceEquals(player, _musicPlayer) || filePath != _currentFilePath)
                return;

            ProgressValue = 0;
            CurrentTime = "0:00";
            Lyrics.UpdateLyricProgress(0);
            player.Start();
        }
        catch (Exception ex)
        {
            _logger.LogError(ex, "Failed to restart current song from beginning");
            var loadContext = _activeLoadContext;
            if (loadContext is not null)
                OnPlayerError(player, loadContext, ex);
        }
        finally
        {
            IsDraggingSlider = false;
        }
    }

    private void PlaySongByPath(string filePath)
    {
        _logger.LogInformation("PlaySongByPath: {FilePath}", filePath);
        _enableAutoPlay = true;
        QueueOpenFile(filePath);
    }

    // 手动触发停止播放，不进行AutoAdvance
    private void StopPlayback()
    {
        _logger.LogInformation("StopPlayback: manual stop requested, disabling auto-advance");
        _disableAutoAdvance = true;
        if (_musicPlayer.IsInitialized() && _musicPlayer.IsPlaying())
        {
            _musicPlayer.Stop();
        }
        else
        {
            _disableAutoAdvance = false;
        }
    }

    [RelayCommand]
    private async Task PlayPause()
    {
        if (!_musicPlayer.IsInitialized())
        {
            _logger.LogInformation("PlayPause: player not initialized, ignoring");
            _syncContext.Post(_ => PlayPauseContent = PlayString, null);
            return;
        }

        if (_musicPlayer.IsPlaying())
        {
            _logger.LogInformation("PlayPause: pausing playback");
            _musicPlayer.Pause();
        }
        else
        {
            _logger.LogInformation("PlayPause: resuming playback");
            IsDraggingSlider = true;
            _musicPlayer.Start();
            await Task.Delay(200);
            IsDraggingSlider = false;
        }
    }

    [RelayCommand]
    private async Task OpenAsync()
    {
        if (IsFileDialogOpen) { return; }
        IsFileDialogOpen = true;
        _logger.LogInformation("OpenAsync: opening file dialog");
        var path = await _fileDialogService.PickMusicFileAsync();

        IsFileDialogOpen = false;
        if (path == null)
        {
            _logger.LogInformation("OpenAsync: user cancelled file dialog");
            return;
        }

        _logger.LogInformation("OpenAsync: user selected file {FilePath}", path);
        try
        {
            await OpenFileAsync(path);
        }
        catch (Exception ex)
        {
            _logger.LogError(ex, "OpenAsync: failed to open file {FilePath}", path);
            WpfMessageBox.Show($"{ex.Message}\n{path}", "Error",
                WpfMessageBoxIcon.Error);
        }
    }

    [RelayCommand]
    public async Task SavePlaylistAsync()
    {
        await Playlist.SavePlaylistAsync();
    }

    [RelayCommand]
    private void TogglePlaylist()
    {
        ActiveView = ActiveView == ActiveView.Playlist ? ActiveView.Player : ActiveView.Playlist;
    }

    [RelayCommand]
    private void ToggleDesktopLyric()
    {
        DesktopLyric.IsDesktopLyricVisible = !DesktopLyric.IsDesktopLyricVisible;
    }

    private void AddToPlaylist()
    {
        if (_currentFilePath == null
            || _currentMd5 == null) return;
        Playlist.AddSong(_currentFilePath, _currentMd5, SongTitle, ArtistName, AlbumCoverImage);
        Playlist.SetPlayingItem(_currentFilePath);
    }

    private bool IsCurrentLoad(MusicPlayerManaged player, PlayerLoadContext loadContext) =>
        !_isDisposed &&
        ReferenceEquals(player, _musicPlayer) &&
        ReferenceEquals(loadContext, _activeLoadContext) &&
        loadContext.Version == Volatile.Read(ref _albumArtVersion);

    private bool ResetPlayerToUnloadedState(int? expectedLoadVersion = null)
    {
        int resetVersion;
        lock (_playerStateLock)
        {
            if (expectedLoadVersion is not null &&
                expectedLoadVersion.Value != Volatile.Read(ref _albumArtVersion))
            {
                return false;
            }

            resetVersion = Interlocked.Increment(ref _albumArtVersion);
            _disableAutoAdvance = true;
            _activeLoadContext = null;
            _currentFilePath = null;
            _currentMd5 = null;
            _enableAutoPlay = false;
            _pendingSeekTime = null;
            _playCountIncrementedForCurrentSong = false;
            try
            {
                _musicPlayer.CloseFile();
            }
            catch (Exception ex)
            {
                _logger.LogError(ex, "Failed to close the current audio file while resetting player state");
            }
        }

        GCSettings.LatencyMode = _previousLatencyMode;
        _syncContext.Post(_ => ResetDisplayedPlayerState(resetVersion), null);
        return true;
    }

    private void ResetDisplayedPlayerState(int resetVersion)
    {
        if (resetVersion != Volatile.Read(ref _albumArtVersion))
            return;

        IsDecoding = false;
        IsDraggingSlider = false;
        AlbumCoverImage = null;
        BlurredAlbumCoverImage = null;
        IsLoselessAudio = false;
        IsHiResAudio = false;
        IsBitPerfect = false;
        AudioSourceFormat = string.Empty;
        SongTitle = NoFileOpenedTitle;
        ArtistName = UnknownArtist;
        ProgressValue = 0;
        ProgressMaximum = 100;
        CurrentTime = "0:00";
        TotalTime = "0:00";
        PlayPauseContent = PlayString;
        SpectrumData = new float[SpectrumSegmentCount];
        _spectrumBufferHasData = false;
        _disableAutoAdvance = false;
        _smtcService.UpdatePlaybackStatus(PlaybackState.Stopped);
        _smtcService.UpdateTextMetadata(NoFileOpenedTitle, UnknownArtist);
        DesktopTrayIcon.TaskBarToolTipText = "WpfMusicPlayer";
        Playlist.SetPlayingItem(null);
        Lyrics.ResetState();
    }

    private BitmapImage? TryLoadAlbumCover(byte[]? albumArt)
    {
        if (albumArt is not { Length: > 0 })
            return null;

        try
        {
            return LoadBitmapImageFromBytes(albumArt);
        }
        catch (Exception ex)
        {
            _logger.LogWarning(ex, "Failed to load album art");
            return null;
        }
    }

    private void QueueBlurredAlbumArtRender(byte[]? albumArtPng, int albumArtVersion)
    {
        if (albumArtPng is not { Length: > 0 })
        {
            BlurredAlbumCoverImage = null;
            return;
        }

        _ = Task.Run(() =>
        {
            try
            {
                var blurredImage = RenderBlurredAlbumArt(albumArtPng);
                _syncContext.Post(_ =>
                {
                    if (!_isDisposed && albumArtVersion == Volatile.Read(ref _albumArtVersion))
                        BlurredAlbumCoverImage = blurredImage;
                }, null);
            }
            catch (Exception ex)
            {
                _logger.LogWarning(ex, "Failed to pre-render blurred album art");
            }
        });
    }

    private static BitmapSource RenderBlurredAlbumArt(byte[] albumArtPng)
    {
        using var source = SKBitmap.Decode(albumArtPng)
            ?? throw new InvalidDataException("Album art image stream could not be decoded for background rendering.");

        var targetInfo = new SKImageInfo(
            BlurredAlbumArtPixelSize,
            BlurredAlbumArtPixelSize,
            SKColorType.Bgra8888,
            SKAlphaType.Premul);
        using var target = new SKBitmap(targetInfo);
        using var canvas = new SKCanvas(target);
        using var blur = SKImageFilter.CreateBlur(
            BlurredAlbumArtSigma,
            BlurredAlbumArtSigma,
            SKShaderTileMode.Clamp);
        using var paint = new SKPaint
        {
            IsAntialias = true,
            ImageFilter = blur
        };

        var sourceBounds = new SKRect(0, 0, source.Width, source.Height);
        var targetBounds = new SKRect(0, 0, target.Width, target.Height);
        var sampling = new SKSamplingOptions(SKFilterMode.Linear, SKMipmapMode.Linear);
        canvas.Clear(SKColors.Transparent);
        canvas.DrawBitmap(source, sourceBounds, targetBounds, sampling, paint);
        canvas.Flush();

        var bitmapSource = BitmapSource.Create(
            target.Width,
            target.Height,
            96,
            96,
            PixelFormats.Bgra32,
            null,
            target.GetPixels(),
            target.ByteCount,
            target.RowBytes);
        bitmapSource.Freeze();
        return bitmapSource;
    }

    private static byte[] DecodeAlbumArtToPng(byte[] encodedImage)
    {
        using var source = SKBitmap.Decode(encodedImage)
            ?? throw new InvalidDataException("Album art image stream could not be decoded.");

        var targetSize = GetAlbumArtPixelSize();
        var sourceRect = GetCenterCropRect(source.Width, source.Height);
        var sourceBounds = new SKRect(sourceRect.Left, sourceRect.Top, sourceRect.Right, sourceRect.Bottom);
        var targetBounds = new SKRect(0, 0, targetSize, targetSize);
        var sampling = new SKSamplingOptions(SKFilterMode.Linear, SKMipmapMode.Linear);
        var targetInfo = new SKImageInfo(targetSize, targetSize, SKColorType.Bgra8888, SKAlphaType.Premul);
        using var target = new SKBitmap(targetInfo);
        using (var canvas = new SKCanvas(target))
        using (var paint = new SKPaint { IsAntialias = true })
        {
            canvas.Clear(SKColors.Transparent);
            canvas.DrawBitmap(
                source,
                sourceBounds,
                targetBounds,
                sampling,
                paint);
        }

        using var image = SKImage.FromBitmap(target);
        using var data = image.Encode(SKEncodedImageFormat.Png, 90)
            ?? throw new InvalidDataException("Album art image stream could not be encoded.");
        return data.ToArray();
    }

    private static SKRectI GetCenterCropRect(int width, int height)
    {
        if (width <= 0 || height <= 0)
        {
            throw new InvalidDataException("Album art image has invalid dimensions.");
        }

        if (width == height)
        {
            return new SKRectI(0, 0, width, height);
        }

        if (width > height)
        {
            var crop = height;
            var left = (width - crop) / 2;
            return new SKRectI(left, 0, left + crop, height);
        }

        var top = (height - width) / 2;
        return new SKRectI(0, top, width, top + width);
    }

    private static int GetAlbumArtPixelSize()
    {
        var scale = Application.Current?.MainWindow is { } window
            ? VisualTreeHelper.GetDpi(window).DpiScaleX
            : 1.0;
        return Math.Max(1, (int)Math.Round(AlbumArtLogicalSize * scale));
    }

    private static BitmapImage LoadBitmapImageFromBytes(byte[] data)
    {
        using var memoryStream = new MemoryStream(data);
        var bitmapImage = new BitmapImage();
        bitmapImage.BeginInit();
        bitmapImage.CacheOption = BitmapCacheOption.OnLoad;
        bitmapImage.StreamSource = memoryStream;
        bitmapImage.EndInit();
        bitmapImage.Freeze();
        return bitmapImage;
    }

    private static async Task<string> ComputeFileMd5Async(string filePath)
    {
        await using var stream = new FileStream(
            filePath,
            FileMode.Open,
            FileAccess.Read,
            FileShare.Read,
            bufferSize: 1024 * 64,
            options: FileOptions.Asynchronous | FileOptions.SequentialScan);
        using var md5 = MD5.Create();
        var hashBytes = await md5.ComputeHashAsync(stream).ConfigureAwait(false);
        return Convert.ToHexStringLower(hashBytes);
    }

    private static string ResolveSongTitle(string? cachedTitle, string? embeddedTitle, string filePath)
    {
        if (IsValidSongTitle(cachedTitle))
            return cachedTitle!.Trim();
        if (IsValidSongTitle(embeddedTitle))
            return embeddedTitle!.Trim();

        var fileName = Path.GetFileNameWithoutExtension(filePath);
        return string.IsNullOrWhiteSpace(fileName) ? NoFileOpenedTitle : fileName;
    }

    private static bool IsValidSongTitle(string? title) =>
        !string.IsNullOrWhiteSpace(title) &&
        !title.Equals(UnknownTitle, StringComparison.OrdinalIgnoreCase);

    private static string ResolveArtistName(string? cachedArtist, string? embeddedArtist)
    {
        if (!string.IsNullOrWhiteSpace(cachedArtist))
            return cachedArtist.Trim();
        return string.IsNullOrWhiteSpace(embeddedArtist) ? UnknownArtist : embeddedArtist.Trim();
    }

    private static string FormatTime(double seconds)
    {
        var timeSpan = TimeSpan.FromSeconds(seconds);
        return $"{(int)timeSpan.TotalMinutes}:{timeSpan.Seconds:D2}";
    }

    public async Task OpenExternalPlaylist(string playlistPath)
    {
        await Playlist.OpenPlaylistAsync(playlistPath);
    }

    private sealed class PlayerLoadContext(
        int version,
        string filePath,
        string md5,
        byte[]? albumArtPng)
    {
        public int Version { get; } = version;
        public string FilePath { get; } = filePath;
        public string Md5 { get; } = md5;
        public byte[]? AlbumArtPng { get; set; } = albumArtPng;
        public bool IsCommitted { get; set; }
    }
}
