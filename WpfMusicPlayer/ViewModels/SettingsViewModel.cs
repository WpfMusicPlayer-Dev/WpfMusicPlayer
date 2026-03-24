using System.Runtime.CompilerServices;
using WpfMusicPlayer.Models;
using WpfMusicPlayer.Services;
using static WpfMusicPlayer.Models.ConfigData;

namespace WpfMusicPlayer.ViewModels;

public sealed class SettingChangedEventArgs(string settingName) : EventArgs
{
    public string SettingName { get; } = settingName;
}

public class SettingsViewModel : ViewModelBase
{
    private readonly IConfigProvider _configProvider;
    private bool _isLoading;

    public event EventHandler<SettingChangedEventArgs>? SettingChanged;

    public SettingsViewModel(IConfigProvider configProvider)
    {
        _configProvider = configProvider;
        LoadFromConfig();
    }

    // UI Settings
    public UISettings.ThemeMode SelectedTheme
    {
        get;
        set
        {
            if (SetProperty(ref field, value))
                ApplyToConfig();
        }
    }

    public UISettings.BackgroundMode SelectedBackground
    {
        get;
        set
        {
            if (SetProperty(ref field, value))
                ApplyToConfig();
        }
    }

    // Audio Settings
    public AudioSettings.ChannelType SelectedChannel
    {
        get;
        set
        {
            if (SetProperty(ref field, value))
                ApplyToConfig();
        }
    }

    public int SelectedSampleRate
    {
        get;
        set
        {
            if (SetProperty(ref field, value))
                ApplyToConfig();
        }
    }

    public UISettings.ThemeMode[] ThemeOptions { get; } =
        Enum.GetValues<UISettings.ThemeMode>();

    public UISettings.BackgroundMode[] BackgroundOptions { get; } =
        Enum.GetValues<UISettings.BackgroundMode>();

    public AudioSettings.ChannelType[] ChannelOptions { get; } =
        Enum.GetValues<AudioSettings.ChannelType>();

    public int[] SampleRateOptions { get; } = [44100, 48000, 88200, 96000, 192000];

    private void LoadFromConfig()
    {
        _isLoading = true;
        ref var config = ref _configProvider.GetConfig();
        SelectedTheme = config.UI.Theme;
        SelectedBackground = config.UI.Background;
        SelectedChannel = config.Audio.Channel;
        SelectedSampleRate = config.Audio.SampleRate;
        _isLoading = false;
    }

    private void ApplyToConfig([CallerMemberName] string? settingName = null)
    {
        if (_isLoading) return;
        ref var config = ref _configProvider.GetConfig();
        config.UI.Theme = SelectedTheme;
        config.UI.Background = SelectedBackground;
        config.Audio.Channel = SelectedChannel;
        config.Audio.SampleRate = SelectedSampleRate;
        _configProvider.WriteFile();
        OnSettingChanged(settingName!);
    }

    private void OnSettingChanged(string settingName)
    {
        SettingChanged?.Invoke(this, new SettingChangedEventArgs(settingName));
    }
}
