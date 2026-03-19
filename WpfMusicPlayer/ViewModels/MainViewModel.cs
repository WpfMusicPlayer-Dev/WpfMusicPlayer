using MusicPlayerLibrary;
using System.Collections.ObjectModel;
using System.IO;
using System.Windows;
using System.Windows.Input;
using System.Windows.Media.Imaging;
using WpfMusicPlayer.Services;

namespace WpfMusicPlayer.ViewModels;

public class MainViewModel : ViewModelBase, IDisposable
{
    private readonly IFileDialogService _fileDialogService;
    private readonly SynchronizationContext _syncContext;
    private MusicPlayer _musicPlayer;
    private string? _currentFilePath;

    public MainViewModel(IFileDialogService fileDialogService)
    {
        _fileDialogService = fileDialogService;
        _syncContext = SynchronizationContext.Current!;
        _musicPlayer = new MusicPlayer();
        SubscribePlayerEvents();

        PlayPauseCommand = new RelayCommand(OnPlayPause);
        OpenCommand = new RelayCommand(async () => await OnOpenAsync());
        PrevCommand = new RelayCommand(() => { });
        NextCommand = new RelayCommand(() => { });
        PlayModeCommand = new RelayCommand(() => { });
        TranslateCommand = new RelayCommand(() => { });
    }

    public string SongTitle
    {
        get;
        private set => SetProperty(ref field, value);
    } = "Song Title";

    public string ArtistName
    {
        get;
        private set => SetProperty(ref field, value);
    } = "Artist Name";

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
    } = "3:30";

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

    public ObservableCollection<string> Lyrics { get; } = [];

    public bool IsDraggingSlider { get; set; }

    public ICommand PlayPauseCommand { get; }
    public ICommand OpenCommand { get; }
    public ICommand PrevCommand { get; }
    public ICommand NextCommand { get; }
    public ICommand PlayModeCommand { get; }
    public ICommand TranslateCommand { get; }

    public void OpenFile(string filePath)
    {
        _currentFilePath = filePath;
        _musicPlayer.Dispose();
        _musicPlayer = new MusicPlayer();
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
        _musicPlayer.Dispose();
        GC.SuppressFinalize(this);
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

    private void OnFileInit()
    {
        _syncContext.Post(_ =>
        {
            var length = _musicPlayer.GetMusicTimeLength();
            ProgressMaximum = length;
            TotalTime = FormatTime(length);

            SongTitle = _musicPlayer.GetSongTitle() ?? "Unknown Title";
            ArtistName = _musicPlayer.GetSongArtist() ?? "Unknown Artist";

            _musicPlayer.SetMasterVolume((float)Volume);

            LoadLyrics();
        }, null);
    }

    private void OnAlbumArtInit(System.Drawing.Image? image)
    {
        _syncContext.Post(_ =>
        {
            AlbumCoverImage = image != null ? ConvertDrawingImageToWpfImage(image) : null;
        }, null);
    }

    private void OnTimeChanged(float time)
    {
        _syncContext.Post(_ =>
        {
            if (IsDraggingSlider) return;
            
            ProgressValue = time;
            CurrentTime = FormatTime(time);
        }, null);
    }

    private void OnStart()
    {
        _syncContext.Post(_ => PlayPauseContent = "\u23F8", null);
    }

    private void OnPause()
    {
        _syncContext.Post(_ => PlayPauseContent = "\u25B6", null);
    }

    private void OnStop()
    {
        _syncContext.Post(_ =>
        {
            PlayPauseContent = "\u25B6";
            ProgressValue = 0;
            CurrentTime = "0:00";
        }, null);
    }

    private void OnPlayPause()
    {
        if (!_musicPlayer.IsInitialized())
        {
            _syncContext.Post(_ => PlayPauseContent = "\u25B6", null);
            return;
        }
        
        if (_musicPlayer.IsPlaying())
        {
            _syncContext.Post(_ => PlayPauseContent = "\u25B6", null);
            _musicPlayer.Pause();
        }
        else
        {
            _syncContext.Post(_ => PlayPauseContent = "\u23F8", null);
            _musicPlayer.Start();
        }
    }

    private async Task OnOpenAsync()
    {
        var path = await _fileDialogService.PickMusicFileAsync();
        if (path == null) return;

        try
        {
            await Task.Run(() => OpenFile(path));
            _syncContext.Post(_ => PlayPauseContent = "\u25B6", null);

        }
        catch (ArgumentException ex)
        {
            Helpers.WpfMessageBox.Show($"{ex.Message}\n{path}", "Error",
                Helpers.WpfMessageBoxIcon.Error);
        }
    }

    private void LoadLyrics()
    {
        var lyricsStr = _musicPlayer.GetID3Lyric();
        Lyrics.Clear();

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
                System.Diagnostics.Debug.WriteLine($"Failed to load lrc: {ex.Message}");
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
            catch
            {
                // ignored
            }
        }

        Lyrics.Add("No lyrics available");
    }

    private void ParseAndAddLocalLyric(string content)
    {
        var lyricController = new LrcFileController();
        lyricController.ParseLrcStream(content);
        if (!lyricController.Valid()) return;
        for (var i = 0; i < lyricController.GetLrcNodeCount(); ++i)
        {
            var lyricIndex = lyricController.GetLrcLineAuxIndex(i, LrcAuxiliaryInfo.Lyric);
            Lyrics.Add(lyricController.GetLrcLineAt(i, lyricIndex));
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
