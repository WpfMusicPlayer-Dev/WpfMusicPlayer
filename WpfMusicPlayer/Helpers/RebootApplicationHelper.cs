using System.Diagnostics;
using System.Globalization;
using System.Linq;
using System.Windows;
using WpfMusicPlayer.ViewModels;

namespace WpfMusicPlayer.Helpers;

internal static class RebootApplicationHelper
{
    public static void RebootApplication()
    {
        if (Application.Current.MainWindow?.DataContext is not MainViewModel vm)
            return;

        var args = BuildCommandLineArgs(vm);
        var exePath = Environment.ProcessPath;

        if (exePath is not null)
        {
            Process.Start(exePath, args);
        }

        Application.Current.Shutdown();
    }

    private static string BuildCommandLineArgs(MainViewModel vm)
    {
        var parts = new List<string>();

        var currentItem = vm.PlaylistItems.FirstOrDefault(p => p.IsPlaying);
        if (currentItem is not null)
        {
            parts.Add("--file");
            parts.Add($"\"{currentItem.FilePath}\"");

            parts.Add("--time");
            parts.Add(((float)vm.ProgressValue).ToString(CultureInfo.InvariantCulture));

            // "\u23F8" is the pause icon, shown when music is playing
            if (vm.IsMusicPlaying)
                parts.Add("--autostart");
        }

        parts.Add("--volume");
        parts.Add(((float)vm.Volume).ToString(CultureInfo.InvariantCulture));

        parts.Add("--view");
        parts.Add(vm.ActiveView.ToString());

        if (vm.IsTranslationVisible)
            parts.Add("--translation");

        if (vm.IsRomanjiVisible)
            parts.Add("--romanji");

        var eqValues = vm.Equalizer.Bands.Select(b => b.Value.ToString(CultureInfo.InvariantCulture));
        parts.Add("--equalizer");
        parts.Add(string.Join(",", eqValues));

        return string.Join(" ", parts);
    }
}
