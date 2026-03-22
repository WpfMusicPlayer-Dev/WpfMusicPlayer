using MusicPlayerLibrary;
using System.Collections.ObjectModel;
using System.IO;
using System.Runtime;
using System.Windows;
using System.Windows.Input;
using System.Windows.Media.Imaging;
using WpfMusicPlayer.Helpers;
using WpfMusicPlayer.Services;

namespace WpfMusicPlayer.ViewModels;

public class MainViewModel : ViewModelBase, IDisposable
{
    private readonly IFileDialogService _fileDialogService;
    private readonly ISmtcService _smtcService;
    private readonly SynchronizationContext _syncContext;
    private MusicPlayer _musicPlayer;
    private string? _currentFilePath;
    private LrcFileController? _lrcFileController;
    private int _sampleRate;
    private GCLatencyMode _previousLatencyMode;

    public MainViewModel(IFileDialogService fileDialogService, ISmtcService smtcService)
    {
        _fileDialogService = fileDialogService;
        _smtcService = smtcService;
        _syncContext = SynchronizationContext.Current!;
        Equalizer = new EqualizerViewModel(ApplyEqualizerBand);
        _sampleRate = 48000; // Studio quality
        _musicPlayer = new MusicPlayer(_sampleRate);
        SubscribePlayerEvents();
        SubscribeSmtcEvents();

        PlayPauseCommand = new RelayCommand(OnPlayPause);
        OpenCommand = new RelayCommand(async () => await OnOpenAsync());
        PrevCommand = new RelayCommand(() => { });
        NextCommand = new RelayCommand(() => { });
        PlayModeCommand = new RelayCommand(() => { });
        PlaylistCommand = new RelayCommand(() => { });
        TranslateCommand = new RelayCommand(OnToggleTranslation, () => HasTranslationAvailable);
        RomanjiCommand = new RelayCommand(OnToggleRomanji, () => HasRomanjiAvailable);
    }

    public EqualizerViewModel Equalizer { get; }

    private void ApplyEqualizerBand(int index, int value)
    {
        _musicPlayer.SetEqualizerBand(index, value);
    }

    public string SongTitle
    {
        get;
        private set => SetProperty(ref field, value);
    } = "Unknown Title";

    public string ArtistName
    {
        get;
        private set => SetProperty(ref field, value);
    } = "Unknown Artist";

    public BitmapImage? AlbumCoverImage
    {
        get;
        private set => SetProperty(ref field, value);
    }

    public string CurrentTime
    {
        get;
        private set => SetProperty(ref field, value);
    } = "0:00";

    public string TotalTime
    {
        get;
        private set => SetProperty(ref field, value);
    } = "0:00";

    public double ProgressValue
    {
        get;
        set => SetProperty(ref field, value);
    }

    public double ProgressMaximum
    {
        get;
        private set => SetProperty(ref field, value);
    } = 100;

    public double Volume
    {
        get;
        set
        {
            if (SetProperty(ref field, value))
            {
                _musicPlayer.SetMasterVolume((float)value);
            }
        }
    } = 0.5;

    public string PlayPauseContent
    {
        get;
        private set => SetProperty(ref field, value);
    } = "\u25B6";

    public ObservableCollection<LyricLineViewModel> Lyrics { get; } = [];

    public int CurrentLyricIndex
    {
        get;
        private set => SetProperty(ref field, value);
    } = -1;

    public bool IsDraggingSlider { get; set; }

    public bool IsTranslationVisible
    {
        get;
        private set => SetProperty(ref field, value);
    } = true;

    public bool HasTranslationAvailable
    {
        get;
        private set => SetProperty(ref field, value);
    }

    public bool IsRomanjiVisible
    {
        get;
        private set => SetProperty(ref field, value);
    } = true;

    public bool HasRomanjiAvailable
    {
        get;
        private set => SetProperty(ref field, value);
    }

    public bool IsFileDialogOpen { get; set; }

    public ICommand PlayPauseCommand { get; }
    public ICommand OpenCommand { get; }
    public ICommand PrevCommand { get; }
    public ICommand NextCommand { get; }
    public ICommand PlayModeCommand { get; }
    public ICommand TranslateCommand { get; }
    public ICommand RomanjiCommand { get; }
    public ICommand PlaylistCommand { get; }

    public void OpenFile(string filePath)
    {
        _currentFilePath = filePath;
        _musicPlayer.Dispose();
        _musicPlayer = new MusicPlayer(_sampleRate);
        SubscribePlayerEvents();
        _musicPlayer.OpenFile(filePath);
    }

    public async void SeekToCurrentPosition()
    {
        if (!_musicPlayer.IsInitialized()) return;
        var timeInSec = _musicPlayer.GetMusicTimeLength();
        var targetTime = timeInSec * (float)ProgressValue / (float)ProgressMaximum;
        var isPlaying = _musicPlayer.IsPlaying();

        IsDraggingSlider = true;

        await Task.Run(() =>
        {
            _musicPlayer.SeekToPosition(targetTime, true);
        });

        if (isPlaying)
        {
            _musicPlayer.Start();
        }

        await Task.Delay(200);
        IsDraggingSlider = false;
        if (!isPlaying)
        {
            OnTimeChanged(targetTime);
        }
    }

    public async void SeekToLyric(LyricLineViewModel lyric)
    {
        if (!_musicPlayer.IsInitialized() || lyric.TimeMs < 0) return;

        var targetTimeSec = lyric.TimeMs / 1000f;
        var isPlaying = _musicPlayer.IsPlaying();

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
            OnTimeChanged(targetTimeSec);
        }
    }

    public void OnWindowClosed()
    {
        if (_musicPlayer.IsInitialized())
        {
            _musicPlayer.Stop();
        }
    }

    public void Dispose()
    {
        GCSettings.LatencyMode = _previousLatencyMode;
        _musicPlayer.Dispose();
        GC.SuppressFinalize(this);
    }

    public async Task SetSampleRate(int sampleRate)
    {
        if (sampleRate < 8000 // Telephone quality
            || sampleRate > 192000) // Above high-resolution audio
        {
            throw new ArgumentOutOfRangeException(nameof(sampleRate), "Sample rate must be between 8000 and 192000 Hz.");
        }
        _sampleRate = sampleRate;
        if (_currentFilePath != null)
        {
            OpenFile( _currentFilePath );
        }
    }

    private void SubscribePlayerEvents()
    {
        _musicPlayer.OnPlayerTimeChange += OnTimeChanged;
        _musicPlayer.OnPlayerFileInit += OnFileInit;
        _musicPlayer.OnPlayerAlbumArtInit += OnAlbumArtInit;
        _musicPlayer.OnPlayerStart += OnStart;
        _musicPlayer.OnPlayerPause += OnPause;
        _musicPlayer.OnPlayerStop += OnStop;
    }

    private void SubscribeSmtcEvents()
    {
        _smtcService.PlayRequested += () => _syncContext.Post(_ =>
        {
            if (!_musicPlayer.IsPlaying()) OnPlayPause();
        }, null);
        _smtcService.PauseRequested += () => _syncContext.Post(_ =>
        {
            if (_musicPlayer.IsPlaying()) OnPlayPause();
        }, null);
        _smtcService.NextRequested += () => _syncContext.Post(_ => NextCommand.Execute(null), null);
        _smtcService.PreviousRequested += () => _syncContext.Post(_ => PrevCommand.Execute(null), null);
    }

    private void OnFileInit()
    {
        _syncContext.Post(_ =>
        {
            var length = _musicPlayer.GetMusicTimeLength();
            ProgressMaximum = length;
            TotalTime = FormatTime(length);

            SongTitle = _musicPlayer.GetSongTitle() ?? "Unknown Title";
            ArtistName = _musicPlayer.GetSongArtist() ?? "Unknown Artist";

            // 坑：UpdateMetadata会清除缩略图，对非NCM文件，FileInit总是晚于AlbumArtInit，导致设置的缩略图被清空
            _smtcService.UpdateTextMetadata(SongTitle, ArtistName);
            _smtcService.UpdateTimeline(TimeSpan.Zero, TimeSpan.FromSeconds(length));

            _musicPlayer.SetMasterVolume((float)Volume);

            LoadLyrics();
        }, null);
    }

    private void OnAlbumArtInit(System.Drawing.Image? image)
    {
        _syncContext.Post(_ =>
        {
            try
            {
                AlbumCoverImage = image != null ? ConvertDrawingImageToWpfImage(image) : null;

                Stream? stream = null;
                if (image != null)
                {
                    stream = new MemoryStream();
                    image.Save(stream, System.Drawing.Imaging.ImageFormat.Png);
                    stream.Position = 0;
                }
                _smtcService.UpdateMetadata(SongTitle, ArtistName, stream);
                stream?.Dispose();
            }
            finally
            {
                image?.Dispose();
            }

        }, null);
    }

    private void OnTimeChanged(float time)
    {
        _syncContext.Post(_ =>
        {
            if (IsDraggingSlider) return;

            ProgressValue = time;
            CurrentTime = FormatTime(time);
            if (_lrcFileController == null) return;
            _lrcFileController.SetTimeStamp((int)(time * 1000));
            var newIndex = _lrcFileController.GetCurrentLrcNodeIndex();

            if (newIndex != CurrentLyricIndex && newIndex >= 0 && newIndex < Lyrics.Count)
            {
                if (CurrentLyricIndex >= 0 && CurrentLyricIndex < Lyrics.Count)
                {
                    Lyrics[CurrentLyricIndex].IsHighlighted = false;
                    Lyrics[CurrentLyricIndex].Progress = 0;
                }
                CurrentLyricIndex = newIndex;
                Lyrics[CurrentLyricIndex].IsHighlighted = true;
            }

            if (CurrentLyricIndex >= 0 && CurrentLyricIndex < Lyrics.Count
                && Lyrics[CurrentLyricIndex].IsProgressEnabled)
            {
                Lyrics[CurrentLyricIndex].Progress = _lrcFileController.GetLrcPercentage(CurrentLyricIndex);
            }
        }, null);
    }

    private void OnStart()
    {
        _previousLatencyMode = GCSettings.LatencyMode;
        GCSettings.LatencyMode = GCLatencyMode.SustainedLowLatency;
        _syncContext.Post(_ =>
        {
            PlayPauseContent = "\u23F8";
            _smtcService.UpdatePlaybackStatus(PlaybackState.Playing);
            _smtcService.UpdateTimeline(TimeSpan.FromSeconds(ProgressValue), TimeSpan.FromSeconds(ProgressMaximum));
        }, null);
    }

    private void OnPause()
    {
        GCSettings.LatencyMode = _previousLatencyMode;
        _syncContext.Post(_ =>
        {
            PlayPauseContent = "\u25B6";
            _smtcService.UpdatePlaybackStatus(PlaybackState.Paused);
            _smtcService.UpdateTimeline(TimeSpan.FromSeconds(ProgressValue), TimeSpan.FromSeconds(ProgressMaximum));
        }, null);
    }

    private void OnStop()
    {
        GCSettings.LatencyMode = _previousLatencyMode;
        _syncContext.Post(_ =>
        {
            PlayPauseContent = "\u25B6";
            ProgressValue = 0;
            CurrentTime = "0:00";
            _smtcService.UpdatePlaybackStatus(PlaybackState.Stopped);
            _smtcService.UpdateTimeline(TimeSpan.Zero, TimeSpan.FromSeconds(ProgressMaximum));

            if (CurrentLyricIndex < 0 || CurrentLyricIndex >= Lyrics.Count) return;
            Lyrics[CurrentLyricIndex].IsHighlighted = false;
            Lyrics[CurrentLyricIndex].Progress = 0;
            CurrentLyricIndex = 0;
            Lyrics[CurrentLyricIndex].IsHighlighted = true;
            if (_lrcFileController != null && CurrentLyricIndex >= 0 && CurrentLyricIndex < Lyrics.Count
                                       && Lyrics[CurrentLyricIndex].IsProgressEnabled)
            {
                Lyrics[CurrentLyricIndex].Progress = _lrcFileController.GetLrcPercentage(CurrentLyricIndex);
            }
        }, null);
    }

    private void OnToggleTranslation()
    {
        IsTranslationVisible = !IsTranslationVisible;
    }

    private void OnToggleRomanji()
    {
        IsRomanjiVisible = !IsRomanjiVisible;
    }

    private async void OnPlayPause()
    {
        if (!_musicPlayer.IsInitialized())
        {
            _syncContext.Post(_ => PlayPauseContent = "\u25B6", null);
            return;
        }

        if (_musicPlayer.IsPlaying())
        {
            _musicPlayer.Pause();
        }
        else
        {
            IsDraggingSlider = true;
            _musicPlayer.Start();
            await Task.Delay(200);
            IsDraggingSlider = false;
        }
    }

    private async Task OnOpenAsync()
    {
        if (IsFileDialogOpen == true) { return; }
        IsFileDialogOpen = true;
        var path = await _fileDialogService.PickMusicFileAsync();

        IsFileDialogOpen = false;
        if (path == null) return;

        try
        {
            await Task.Run(() => OpenFile(path));
            _syncContext.Post(_ => PlayPauseContent = "\u25B6", null);
        }
        catch (Exception ex)
        {
            WpfMessageBox.Show($"{ex.Message}\n{path}", "Error",
                WpfMessageBoxIcon.Error);
            // reset state
            AlbumCoverImage = null;
            SongTitle = "Unknown Title";
            ArtistName = "Unknown Artist";
            ProgressValue = 0;
            CurrentTime = "0:00";
            _smtcService.UpdatePlaybackStatus(PlaybackState.Stopped);
            _smtcService.UpdateTimeline(TimeSpan.Zero, TimeSpan.FromSeconds(ProgressMaximum));
            _smtcService.UpdateTextMetadata("Unknown Title", "Unknown Artist");
            _lrcFileController?.Dispose();
            _lrcFileController = null;
            Lyrics.Clear();
            CurrentLyricIndex = -1;
            HasTranslationAvailable = false;
            HasRomanjiAvailable = false;
        }
    }

    private void LoadLyrics()
    {
        var lyricsStr = _musicPlayer.GetID3Lyric();
        Lyrics.Clear();
        CurrentLyricIndex = -1;
        HasTranslationAvailable = false;
        HasRomanjiAvailable = false;

        if (!string.IsNullOrEmpty(lyricsStr))
        {
            ParseAndAddLocalLyric(lyricsStr);
            return;
        }

        var lrcPath = FindBestLrcFile();
        if (!string.IsNullOrEmpty(lrcPath))
        {
            try
            {
                var content = File.ReadAllText(lrcPath);
                ParseAndAddLocalLyric(content);
                return;
            }
            catch (Exception ex)
            {
                WpfMessageBox.Show($"Failed to load lrc: {ex.Message}", "Error", WpfMessageBoxIcon.Error);
            }
        }

        // Fallback to same filename
        var exactLrcPath = Path.ChangeExtension(_currentFilePath, ".lrc");
        if (File.Exists(exactLrcPath))
        {
            try
            {
                var content = File.ReadAllText(exactLrcPath);
                ParseAndAddLocalLyric(content);
                return;
            }
            catch (InvalidOperationException ex)
            {
                // ignored
                WpfMessageBox.Show(ex.Message, "Error", WpfMessageBoxIcon.Error);
            }
        }

        _lrcFileController = null;
        Lyrics.Add(new LyricLineViewModel("暂无歌词"));
    }

    private void ParseAndAddLocalLyric(string content)
    {
        _lrcFileController?.Dispose();
        _lrcFileController = new LrcFileController();

        _lrcFileController.ParseLrcStream(content);
        _lrcFileController.SetSongDuration(_musicPlayer.GetMusicTimeLength());
        if (!_lrcFileController.Valid()) return;

        var hasTranslation = _lrcFileController.IsAuxiliaryInfoEnabled(LrcAuxiliaryInfo.Translation);
        HasTranslationAvailable = hasTranslation;
        var hasRomanji = _lrcFileController.IsAuxiliaryInfoEnabled(LrcAuxiliaryInfo.Romanization);
        HasRomanjiAvailable = hasRomanji;

        for (var i = 0; i < _lrcFileController.GetLrcNodeCount(); ++i)
        {
            var lyricIndex = _lrcFileController.GetLrcLineAuxIndex(i, LrcAuxiliaryInfo.Lyric);
            var timeMs = _lrcFileController.GetLrcNodeTimeMs(i);
            var lyricText = _lrcFileController.GetLrcLineAt(i, lyricIndex);

            string? translation = null;
            string? romanji = null;
            if (hasTranslation)
            {
                var transIndex = _lrcFileController.GetLrcLineAuxIndex(i, LrcAuxiliaryInfo.Translation);
                if (transIndex >= 0)
                    translation = _lrcFileController.GetLrcLineAt(i, transIndex);
            }
            if (hasRomanji)
            {
                var romanjiIndex = _lrcFileController.GetLrcLineAuxIndex(i, LrcAuxiliaryInfo.Romanization);
                if (romanjiIndex >= 0)
                    romanji = _lrcFileController.GetLrcLineAt(i, romanjiIndex);
            }

            Lyrics.Add(new LyricLineViewModel(lyricText, timeMs, translation, romanji)
            {
                IsProgressEnabled = _lrcFileController.IsPercentageEnabled(i)
            });
        }
    }

    private string? FindBestLrcFile()
    {
        if (string.IsNullOrEmpty(_currentFilePath)) return null;

        var fileDir = Path.GetDirectoryName(_currentFilePath);
        if (string.IsNullOrEmpty(fileDir)) return null;

        var searchPaths = new List<string>
        {
            fileDir,
            Path.GetFullPath(Path.Combine(fileDir, "..")),
        };

        AddKnownPath(Environment.SpecialFolder.MyMusic);
        AddKnownPath(Environment.SpecialFolder.MyDocuments);

        var targetName = _musicPlayer.GetSongTitle();
        if (string.IsNullOrEmpty(targetName))
        {
            targetName = Path.GetFileNameWithoutExtension(_currentFilePath);
        }

        foreach (var dir in searchPaths.Where(Directory.Exists))
        {
            try
            {
                var lrcFiles = Directory.GetFiles(dir, "*.lrc");
                string? bestFile = null;
                var bestSimilarity = 0f;

                foreach (var file in lrcFiles)
                {
                    var fileName = Path.GetFileNameWithoutExtension(file);

                    if (fileName.Contains(targetName, StringComparison.OrdinalIgnoreCase))
                    {
                        return file;
                    }

                    var sim = CalculateJaccardSimilarity(fileName, targetName);
                    if (!(sim > 0.7f) || !(sim > bestSimilarity)) continue;
                    bestSimilarity = sim;
                    bestFile = file;
                }

                if (bestFile != null) return bestFile;
            }
            catch { }
        }

        return null;

        void AddKnownPath(Environment.SpecialFolder folder)
        {
            var path = Environment.GetFolderPath(folder);
            if (string.IsNullOrEmpty(path)) return;
            searchPaths.Add(path);
            searchPaths.Add(Path.Combine(path, "Lyrics"));
        }
    }

    private static float CalculateJaccardSimilarity(string str1, string str2)
    {
        var set1 = new HashSet<char>(str1);
        var set2 = new HashSet<char>(str2);

        var intersection = new HashSet<char>(set1);
        intersection.IntersectWith(set2);

        var union = new HashSet<char>(set1);
        union.UnionWith(set2);

        if (union.Count == 0) return 0f;
        return (float)intersection.Count / union.Count;
    }

    private static BitmapImage ConvertDrawingImageToWpfImage(System.Drawing.Image drawingImage)
    {
        using var memoryStream = new MemoryStream();
        drawingImage.Save(memoryStream, System.Drawing.Imaging.ImageFormat.Png);
        memoryStream.Position = 0;

        var bitmapImage = new BitmapImage();
        bitmapImage.BeginInit();
        bitmapImage.CacheOption = BitmapCacheOption.OnLoad;
        bitmapImage.StreamSource = memoryStream;
        bitmapImage.EndInit();
        bitmapImage.Freeze();

        return bitmapImage;
    }

    private static string FormatTime(float seconds)
    {
        var timeSpan = TimeSpan.FromSeconds(seconds);
        return $"{(int)timeSpan.TotalMinutes}:{timeSpan.Seconds:D2}";
    }
}
