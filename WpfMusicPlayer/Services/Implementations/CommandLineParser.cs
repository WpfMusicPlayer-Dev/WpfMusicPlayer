using System.Globalization;
using WpfMusicPlayer.Services.Abstractions;
using WpfMusicPlayer.ViewModels;

namespace WpfMusicPlayer.Services.Implementations;

internal class CommandLineParser : ICommandLineParser
{
    public string FilePath { get; }

    public float MusicCurrentTime { get; }

    public bool AutoStart { get; }

    public float Volume { get; }

    public ActiveView StartupView { get; }

    public string OpenedPlaylistPath { get; }

    public bool TranslationToggled { get; }

    public bool RomanjiToggled { get; }

    public int[] AppliedEqualizerSettings { get; }

    public CommandLineParser()
    {
        string[] args = Environment.GetCommandLineArgs();

        System.Diagnostics.Debug.WriteLine("Command-line arguments: " + string.Join(" ", args));

        FilePath = string.Empty;
        OpenedPlaylistPath = string.Empty;
        Volume = 0.5f;
        StartupView = ActiveView.Player;
        AppliedEqualizerSettings = [];

        for (int i = 1; i < args.Length; i++)
        {
            switch (args[i].ToLowerInvariant())
            {
                case "--file" when i + 1 < args.Length:
                    FilePath = args[++i];
                    break;

                case "--time" when i + 1 < args.Length:
                    if (float.TryParse(args[++i], CultureInfo.InvariantCulture, out float time))
                        MusicCurrentTime = time;
                    break;

                case "--autostart":
                    AutoStart = true;
                    break;

                case "--volume" when i + 1 < args.Length:
                    if (float.TryParse(args[++i], CultureInfo.InvariantCulture, out float vol))
                        Volume = vol;
                    break;

                case "--view" when i + 1 < args.Length:
                    if (Enum.TryParse(args[++i], true, out ActiveView view))
                        StartupView = view;
                    break;

                case "--playlist" when i + 1 < args.Length:
                    OpenedPlaylistPath = args[++i];
                    break;

                case "--translation":
                    TranslationToggled = true;
                    break;

                case "--romanji":
                    RomanjiToggled = true;
                    break;

                case "--equalizer" when i + 1 < args.Length:
                    AppliedEqualizerSettings = [.. args[++i]
                        .Split(',', StringSplitOptions.RemoveEmptyEntries)
                        .Select(s => int.TryParse(s.Trim(), CultureInfo.InvariantCulture, out int v) ? v : 0)];
                    break;

                default:
                    // 默认选项为直接输入文件路径
                    if (!args[i].StartsWith("--") && string.IsNullOrEmpty(FilePath))
                        FilePath = args[i];
                    break;
            }
        }
    }
}
