using System;
using System.Collections.Generic;
using System.Text;
using WpfMusicPlayer.ViewModels;

namespace WpfMusicPlayer.Services.Abstractions;

public interface ICommandLineParser
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
}
