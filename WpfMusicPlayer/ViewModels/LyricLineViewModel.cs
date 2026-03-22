namespace WpfMusicPlayer.ViewModels;

public class LyricLineViewModel(string text, int timeMs = -1, string? translation = null, string? romanji = null) : ViewModelBase
{
    public string Text { get; } = text;

    public int TimeMs { get; } = timeMs;

    public string? Translation { get; } = translation;

    public string? Romanji { get; } = romanji;

    public bool HasTranslation => Translation != null;

    public bool HasRomanji => Romanji != null;

    public bool IsHighlighted
    {
        get;
        set => SetProperty(ref field, value);
    }

    public bool IsProgressEnabled { get; init; }

    public double Progress
    {
        get;
        set => SetProperty(ref field, value);
    }
}

