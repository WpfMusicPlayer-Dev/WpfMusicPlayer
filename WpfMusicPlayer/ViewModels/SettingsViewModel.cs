// SPDX-License-Identifier: MIT

using System.Collections.ObjectModel;
using System.Runtime.CompilerServices;
using System.Windows;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using WpfMusicPlayer.Helpers;
using WpfMusicPlayer.Models;
using WpfMusicPlayer.Services.Abstractions;
using static WpfMusicPlayer.Models.ConfigData;

namespace WpfMusicPlayer.ViewModels;

public sealed class SettingChangedEventArgs(string settingName) : EventArgs
{
    public string SettingName { get; } = settingName;
}

public sealed record BitDepthOption(AudioSettings.BitDepthType Value, string DisplayName)
{
    public override string ToString() => DisplayName;
}

public sealed record AudioBackendOption(AudioSettings.BackendType Value, string DisplayName)
{
    public override string ToString() => DisplayName;
}

public partial class SettingsViewModel : ObservableObject
{
    public const string AudioOutputSettingsChangeName = "AudioOutputSettings";
    private const string DefaultOutputDeviceDisplayName = "系统默认";
    private const string UnavailableOutputDeviceSuffix = "（当前不可用）";

    private readonly IConfigProvider _configProvider;
    private readonly IAudioOutputFormatProvider _audioOutputFormatProvider;
    private bool _isLoading;

    public event EventHandler<SettingChangedEventArgs>? SettingChanged;

    public SettingsViewModel(
        IConfigProvider configProvider,
        IAudioOutputFormatProvider audioOutputFormatProvider)
    {
        _configProvider = configProvider;
        _audioOutputFormatProvider = audioOutputFormatProvider;
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
    public AudioSettings.BackendType SelectedBackend
    {
        get;
        set
        {
            if (!SetProperty(ref field, value))
                return;

            OnPropertyChanged(nameof(IsOutputFormatSelectionEnabled));
            if (_isLoading)
                return;

            var preferredDeviceId = SelectedOutputDeviceId;
            _isLoading = true;
            try
            {
                RefreshOutputDeviceOptions(value, preferredDeviceId);
            }
            finally
            {
                _isLoading = false;
            }

            ApplyToConfig(AudioOutputSettingsChangeName);
        }
    }

    public string SelectedOutputDeviceId
    {
        get;
        set
        {
            value ??= string.Empty;
            if (SetProperty(ref field, value))
                ApplyToConfig();
        }
    } = string.Empty;

    public bool IsOutputFormatSelectionEnabled =>
        SelectedBackend == AudioSettings.BackendType.FAudio;

    public AudioSettings.ChannelType SelectedChannel
    {
        get;
        set
        {
            if (!_isLoading && !IsOutputFormatSelectionEnabled)
                return;
            if (SetProperty(ref field, value))
                ApplyToConfig();
        }
    }

    public int SelectedSampleRate
    {
        get;
        set
        {
            if (!_isLoading && !IsOutputFormatSelectionEnabled)
                return;
            if (SetProperty(ref field, value))
                ApplyToConfig();
        }
    }

    public AudioSettings.BitDepthType SelectedBitDepth
    {
        get;
        set
        {
            if (!_isLoading && !IsOutputFormatSelectionEnabled)
                return;
            if (SetProperty(ref field, value))
                ApplyToConfig();
        }
    }
    
    public bool SelectedDesktopLyricEnabled
    {
        get;
        set
        {
            if (SetProperty(ref field, value))
                ApplyToConfig();
        }
    }

    public double SelectedDesktopLyricFontSize
    {
        get;
        set
        {
            if (SetProperty(ref field, value))
                ApplyToConfig();
        }
    }

    public double SelectedDesktopLyricAuxFontSize
    {
        get;
        set
        {
            if (SetProperty(ref field, value))
                ApplyToConfig();
        }
    }
    
    public bool SelectedDesktopLyricIsAuxInfoCustomizable
    {
        get;
        set
        {
            if (SetProperty(ref field, value))
                ApplyToConfig();
        }
    }
    
    public double SelectedVolume 
    {
        get;
        set
        {
            if (SetProperty(ref field, value))
                ApplyToConfig();
        }
    }

    public Visibility Windows10WarningVisibility => OsVersionHelper.IsWindows11() ? Visibility.Collapsed : Visibility.Visible;

    public UISettings.ThemeMode[] ThemeOptions { get; } =
        Enum.GetValues<UISettings.ThemeMode>();

    public UISettings.BackgroundMode[] BackgroundOptions { get; } =
        Enum.GetValues<UISettings.BackgroundMode>();

#if WINDOWS
    public AudioBackendOption[] BackendOptions { get; } =
    [
        new(AudioSettings.BackendType.FAudio, "FAudio（共享模式）"),
        new(AudioSettings.BackendType.WasapiExclusive, "WASAPI（独占模式）")
    ];
#else
    public AudioBackendOption[] BackendOptions { get; } =
    [
        new(AudioSettings.BackendType.FAudio, "FAudio（共享模式）")
    ];
#endif

    public ObservableCollection<AudioOutputDeviceOption> OutputDeviceOptions { get; } = [];

    public AudioSettings.ChannelType[] ChannelOptions { get; } =
        Enum.GetValues<AudioSettings.ChannelType>()
            .Where(option => option != AudioSettings.ChannelType.System)
            .ToArray();

    public BitDepthOption[] BitDepthOptions { get; } =
    [
        new(AudioSettings.BitDepthType.Bit16, "16bit"),
        new(AudioSettings.BitDepthType.Bit24, "24bit"),
        new(AudioSettings.BitDepthType.Bit32, "32bit")
    ];

    public ObservableCollection<int> SampleRateOptions { get; } =
        [8000, 11025, 16000, 22050, 44100, 48000, 88200, 96000, 192000];

    [RelayCommand]
    private void ApplySystemOutputSettings()
    {
        if (!IsOutputFormatSelectionEnabled)
            return;

        WpfMusicPlayer.Models.SystemAudioOutputFormat systemFormat;
        try
        {
            systemFormat = _audioOutputFormatProvider.GetSystemDefaultOutputFormat();
        }
        catch (Exception exception)
        {
            WpfMessageBox.Show(
                $"无法读取系统默认输出格式。\n{exception.Message}",
                "应用系统输出设置",
                WpfMessageBoxIcon.Error);
            return;
        }

        if (SelectedChannel == systemFormat.Channel &&
            SelectedBitDepth == systemFormat.BitDepth &&
            SelectedSampleRate == systemFormat.SampleRate)
        {
            return;
        }

        _isLoading = true;
        try
        {
            EnsureSampleRateOption(systemFormat.SampleRate);
            SelectedChannel = systemFormat.Channel;
            SelectedBitDepth = systemFormat.BitDepth;
            SelectedSampleRate = systemFormat.SampleRate;
        }
        finally
        {
            _isLoading = false;
        }

        ApplyToConfig(AudioOutputSettingsChangeName);
    }

    private void LoadFromConfig()
    {
        _isLoading = true;
        try
        {
            ref var config = ref _configProvider.GetConfig();
            SelectedBackend = config.Audio.Backend;
            RefreshOutputDeviceOptions(
                SelectedBackend,
                config.Audio.OutputDeviceId ?? string.Empty);
            EnsureSampleRateOption(config.Audio.SampleRate);
            SelectedTheme = config.UI.Theme;
            SelectedBackground = config.UI.Background;
            SelectedChannel = config.Audio.Channel;
            SelectedBitDepth = config.Audio.BitDepth;
            SelectedSampleRate = config.Audio.SampleRate;
            SelectedVolume = config.Audio.Volume;
            SelectedDesktopLyricEnabled = config.DesktopLyric.IsDesktopLyricEnabled;
            SelectedDesktopLyricFontSize = config.DesktopLyric.DesktopLyricFontSize;
            SelectedDesktopLyricIsAuxInfoCustomizable = config.DesktopLyric.IsDesktopLyricAuxCustomizable;
            SelectedDesktopLyricAuxFontSize = config.DesktopLyric.DesktopLyricAuxFontSize;
        }
        finally
        {
            _isLoading = false;
        }
    }

    private void EnsureSampleRateOption(int sampleRate)
    {
        if (sampleRate <= 0 || SampleRateOptions.Contains(sampleRate))
            return;

        var index = 0;
        while (index < SampleRateOptions.Count && SampleRateOptions[index] < sampleRate)
            index++;
        SampleRateOptions.Insert(index, sampleRate);
    }

    public string GetOutputDeviceDisplayName(string? deviceId)
    {
        if (string.IsNullOrEmpty(deviceId))
            return DefaultOutputDeviceDisplayName;

        return OutputDeviceOptions.FirstOrDefault(option =>
                string.Equals(option.Id, deviceId, StringComparison.OrdinalIgnoreCase))
            ?.DisplayName ?? deviceId;
    }

    private void RefreshOutputDeviceOptions(
        AudioSettings.BackendType backend,
        string? preferredDeviceId)
    {
        OutputDeviceOptions.Clear();
        OutputDeviceOptions.Add(new AudioOutputDeviceOption(
            string.Empty,
            DefaultOutputDeviceDisplayName,
            true));

        foreach (var device in _audioOutputFormatProvider.GetAudioOutputDevices(backend))
        {
            if (string.IsNullOrWhiteSpace(device.Id) ||
                OutputDeviceOptions.Any(option => string.Equals(
                    option.Id,
                    device.Id,
                    StringComparison.OrdinalIgnoreCase)))
            {
                continue;
            }

            var displayName = device.IsDefault
                ? $"{device.DisplayName}（当前默认）"
                : device.DisplayName;
            OutputDeviceOptions.Add(device with { DisplayName = displayName });
        }

        var selectedOption = OutputDeviceOptions.FirstOrDefault(option =>
            string.Equals(
                option.Id,
                preferredDeviceId,
                StringComparison.OrdinalIgnoreCase));

        // An empty enumeration can mean that the endpoint is temporarily
        // unavailable or that enumeration itself failed. Keep the persisted ID
        // visible and selected until the user explicitly chooses System Default.
        if (selectedOption is null && !string.IsNullOrWhiteSpace(preferredDeviceId))
        {
            selectedOption = new AudioOutputDeviceOption(
                preferredDeviceId,
                $"{preferredDeviceId}{UnavailableOutputDeviceSuffix}",
                false);
            OutputDeviceOptions.Add(selectedOption);
        }

        SelectedOutputDeviceId = selectedOption?.Id ?? string.Empty;
    }

    private void ApplyToConfig([CallerMemberName] string? settingName = null)
    {
        if (_isLoading) return;
        ref var config = ref _configProvider.GetConfig();
        config.UI.Theme = SelectedTheme;
        config.UI.Background = SelectedBackground;
        config.Audio.Backend = SelectedBackend;
        config.Audio.OutputDeviceId = SelectedOutputDeviceId;
        config.Audio.Channel = SelectedChannel;
        config.Audio.BitDepth = SelectedBitDepth;
        config.Audio.SampleRate = SelectedSampleRate;
        config.Audio.Volume = SelectedVolume;
        config.DesktopLyric.IsDesktopLyricEnabled = SelectedDesktopLyricEnabled;
        config.DesktopLyric.DesktopLyricFontSize = SelectedDesktopLyricFontSize;
        config.DesktopLyric.IsDesktopLyricAuxCustomizable = SelectedDesktopLyricIsAuxInfoCustomizable;
        config.DesktopLyric.DesktopLyricAuxFontSize = SelectedDesktopLyricAuxFontSize;
        _configProvider.WriteFile();
        OnSettingChanged(settingName!);
    }

    private void OnSettingChanged(string settingName)
    {
        SettingChanged?.Invoke(this, new SettingChangedEventArgs(settingName));
    }
}
