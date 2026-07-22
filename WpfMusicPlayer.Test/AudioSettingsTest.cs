// SPDX-License-Identifier: MIT

using System.Xml.Serialization;
using WpfMusicPlayer.Models;
using WpfMusicPlayer.Services.Abstractions;
using WpfMusicPlayer.Services.Implementations;
using WpfMusicPlayer.ViewModels;
using static WpfMusicPlayer.Models.ConfigData;
using static WpfMusicPlayer.Services.Implementations.ConfigProvider;

namespace WpfMusicPlayer.Test;

[TestClass]
public sealed class SettingsViewModelTest
{
    [TestMethod]
    public void BackendOptions_ContainOnlyCompileTimeAvailableBackends()
    {
        var configProvider = new FakeConfigProvider(CreateAudioConfig(
            48_000,
            AudioSettings.ChannelType.Stereo,
            AudioSettings.BitDepthType.Bit24));
        var viewModel = new SettingsViewModel(
            configProvider,
            new FakeAudioOutputFormatProvider(new SystemAudioOutputFormat(
                48_000,
                AudioSettings.ChannelType.Stereo,
                AudioSettings.BitDepthType.Bit24)));

        Assert.IsTrue(viewModel.BackendOptions.Any(option =>
            option.Value == AudioSettings.BackendType.FAudio));
#if WINDOWS
        Assert.IsTrue(viewModel.BackendOptions.Any(option =>
            option.Value == AudioSettings.BackendType.WasapiExclusive));
#else
        Assert.HasCount(1, viewModel.BackendOptions);
#endif
    }

    [TestMethod]
    public void AudioOptions_HideLegacySystemValues()
    {
        var configProvider = new FakeConfigProvider(CreateAudioConfig(
            48_000,
            AudioSettings.ChannelType.Stereo,
            AudioSettings.BitDepthType.Bit24));
        var audioOutputFormatProvider = new FakeAudioOutputFormatProvider(
            new SystemAudioOutputFormat(
                48_000,
                AudioSettings.ChannelType.Stereo,
                AudioSettings.BitDepthType.Bit24));

        var viewModel = new SettingsViewModel(
            configProvider,
            audioOutputFormatProvider);

        Assert.IsFalse(viewModel.ChannelOptions.Contains(
            AudioSettings.ChannelType.System));
        Assert.IsFalse(viewModel.BitDepthOptions.Any(option =>
            option.Value == AudioSettings.BitDepthType.System));
    }

    [TestMethod]
    public void ApplySystemOutputSettings_UpdatesAllValuesWithOneWriteAndOneAggregateEvent()
    {
        var configProvider = new FakeConfigProvider(CreateAudioConfig(
            48_000,
            AudioSettings.ChannelType.Stereo,
            AudioSettings.BitDepthType.Bit16));
        var systemFormat = new SystemAudioOutputFormat(
            176_400,
            AudioSettings.ChannelType.Surround51,
            AudioSettings.BitDepthType.Bit24);
        var audioOutputFormatProvider = new FakeAudioOutputFormatProvider(systemFormat);
        var viewModel = new SettingsViewModel(
            configProvider,
            audioOutputFormatProvider);
        var eventCount = 0;
        SettingChangedEventArgs? observedEvent = null;
        SystemAudioOutputFormat? valuesObservedByHandler = null;
        viewModel.SettingChanged += (_, eventArgs) =>
        {
            eventCount++;
            observedEvent = eventArgs;
            valuesObservedByHandler = new SystemAudioOutputFormat(
                viewModel.SelectedSampleRate,
                viewModel.SelectedChannel,
                viewModel.SelectedBitDepth);
        };

        viewModel.ApplySystemOutputSettingsCommand.Execute(null);

        Assert.AreEqual(1, audioOutputFormatProvider.QueryCount);
        Assert.AreEqual(1, configProvider.WriteCount);
        Assert.AreEqual(1, eventCount);
        Assert.IsNotNull(observedEvent);
        Assert.AreEqual(
            SettingsViewModel.AudioOutputSettingsChangeName,
            observedEvent!.SettingName);
        Assert.IsTrue(valuesObservedByHandler.HasValue);
        Assert.AreEqual(systemFormat, valuesObservedByHandler.Value);
        Assert.IsTrue(configProvider.LastWrittenAudioFormat.HasValue);
        Assert.AreEqual(systemFormat, configProvider.LastWrittenAudioFormat.Value);
        Assert.AreEqual(systemFormat.SampleRate, viewModel.SelectedSampleRate);
        Assert.AreEqual(systemFormat.Channel, viewModel.SelectedChannel);
        Assert.AreEqual(systemFormat.BitDepth, viewModel.SelectedBitDepth);
        Assert.Contains(systemFormat.SampleRate, viewModel.SampleRateOptions);

        ref var persistedConfig = ref configProvider.GetConfig();
        Assert.AreEqual(systemFormat.SampleRate, persistedConfig.Audio.SampleRate);
        Assert.AreEqual(systemFormat.Channel, persistedConfig.Audio.Channel);
        Assert.AreEqual(systemFormat.BitDepth, persistedConfig.Audio.BitDepth);
    }

    [TestMethod]
    public void ApplySystemOutputSettings_WhenValuesMatch_DoesNotWriteOrRaiseEvent()
    {
        var systemFormat = new SystemAudioOutputFormat(
            176_400,
            AudioSettings.ChannelType.Surround71,
            AudioSettings.BitDepthType.Bit32);
        var configProvider = new FakeConfigProvider(CreateAudioConfig(
            systemFormat.SampleRate,
            systemFormat.Channel,
            systemFormat.BitDepth));
        var audioOutputFormatProvider = new FakeAudioOutputFormatProvider(systemFormat);
        var viewModel = new SettingsViewModel(
            configProvider,
            audioOutputFormatProvider);
        var eventCount = 0;
        viewModel.SettingChanged += (_, _) => eventCount++;

        viewModel.ApplySystemOutputSettingsCommand.Execute(null);

        Assert.AreEqual(1, audioOutputFormatProvider.QueryCount);
        Assert.AreEqual(0, configProvider.WriteCount);
        Assert.AreEqual(0, eventCount);
        Assert.Contains(systemFormat.SampleRate, viewModel.SampleRateOptions);
    }

#if WINDOWS
    [TestMethod]
    public void WasapiSelection_DisablesAndPreservesManagedFormatSettings()
    {
        var configProvider = new FakeConfigProvider(CreateAudioConfig(
            48_000,
            AudioSettings.ChannelType.Stereo,
            AudioSettings.BitDepthType.Bit24));
        var audioOutputFormatProvider = new FakeAudioOutputFormatProvider(
            new SystemAudioOutputFormat(
                96_000,
                AudioSettings.ChannelType.Surround71,
                AudioSettings.BitDepthType.Bit32));
        var viewModel = new SettingsViewModel(
            configProvider,
            audioOutputFormatProvider);
        SettingChangedEventArgs? observedEvent = null;
        viewModel.SettingChanged += (_, eventArgs) => observedEvent = eventArgs;

        viewModel.SelectedBackend = AudioSettings.BackendType.WasapiExclusive;

        Assert.IsFalse(viewModel.IsOutputFormatSelectionEnabled);
        Assert.AreEqual(1, configProvider.WriteCount);
        Assert.AreEqual(
            SettingsViewModel.AudioOutputSettingsChangeName,
            observedEvent?.SettingName);

        viewModel.SelectedSampleRate = 96_000;
        viewModel.SelectedChannel = AudioSettings.ChannelType.Surround71;
        viewModel.SelectedBitDepth = AudioSettings.BitDepthType.Bit32;
        viewModel.ApplySystemOutputSettingsCommand.Execute(null);

        Assert.AreEqual(48_000, viewModel.SelectedSampleRate);
        Assert.AreEqual(AudioSettings.ChannelType.Stereo, viewModel.SelectedChannel);
        Assert.AreEqual(AudioSettings.BitDepthType.Bit24, viewModel.SelectedBitDepth);
        Assert.AreEqual(0, audioOutputFormatProvider.QueryCount);
        Assert.AreEqual(1, configProvider.WriteCount);
        ref var persistedConfig = ref configProvider.GetConfig();
        Assert.AreEqual(
            AudioSettings.BackendType.WasapiExclusive,
            persistedConfig.Audio.Backend);
        Assert.AreEqual(48_000, persistedConfig.Audio.SampleRate);
        Assert.AreEqual(AudioSettings.ChannelType.Stereo, persistedConfig.Audio.Channel);
        Assert.AreEqual(AudioSettings.BitDepthType.Bit24, persistedConfig.Audio.BitDepth);
    }

    [TestMethod]
    public void BackendChange_RefreshesDevicesAndKeepsAvailableStableId()
    {
        var config = CreateAudioConfig(
            48_000,
            AudioSettings.ChannelType.Stereo,
            AudioSettings.BitDepthType.Bit24);
        config.Audio.OutputDeviceId = "endpoint-b";
        var configProvider = new FakeConfigProvider(config);
        AudioOutputDeviceOption[] devices =
        [
            new("endpoint-a", "Speakers", true),
            new("endpoint-b", "USB DAC", false)
        ];
        var audioOutputFormatProvider = new FakeAudioOutputFormatProvider(
            new SystemAudioOutputFormat(
                48_000,
                AudioSettings.ChannelType.Stereo,
                AudioSettings.BitDepthType.Bit24),
            devices);
        var viewModel = new SettingsViewModel(
            configProvider,
            audioOutputFormatProvider);

        viewModel.SelectedBackend = AudioSettings.BackendType.WasapiExclusive;

        Assert.AreEqual(2, audioOutputFormatProvider.DeviceQueryCount);
        Assert.AreEqual("endpoint-b", viewModel.SelectedOutputDeviceId);
        Assert.AreEqual("endpoint-b", configProvider.LastWrittenOutputDeviceId);
        Assert.AreEqual(
            AudioSettings.BackendType.WasapiExclusive,
            configProvider.LastWrittenBackend);
        Assert.AreEqual(1, configProvider.WriteCount);
    }

    [TestMethod]
    public void EqualizerSampleRate_WasapiUsesResolvedRateAndFAudioKeepsConfiguredRate()
    {
        Assert.AreEqual(
            192_000,
            MainViewModel.ResolveEqualizerSampleRate(
                AudioSettings.BackendType.WasapiExclusive,
                8_000,
                192_000));
        Assert.AreEqual(
            8_000,
            MainViewModel.ResolveEqualizerSampleRate(
                AudioSettings.BackendType.WasapiExclusive,
                8_000,
                0));
        Assert.AreEqual(
            8_000,
            MainViewModel.ResolveEqualizerSampleRate(
                AudioSettings.BackendType.FAudio,
                8_000,
                192_000));
    }
#endif

    [TestMethod]
    public void UnavailableSavedDevice_RemainsSelectedUntilUserChoosesSystemDefault()
    {
        var config = CreateAudioConfig(
            48_000,
            AudioSettings.ChannelType.Stereo,
            AudioSettings.BitDepthType.Bit24);
        config.Audio.OutputDeviceId = "endpoint-temporarily-unavailable";
        var configProvider = new FakeConfigProvider(config);
        var viewModel = new SettingsViewModel(
            configProvider,
            new FakeAudioOutputFormatProvider(new SystemAudioOutputFormat(
                48_000,
                AudioSettings.ChannelType.Stereo,
                AudioSettings.BitDepthType.Bit24)));

        Assert.AreEqual(
            "endpoint-temporarily-unavailable",
            viewModel.SelectedOutputDeviceId);
        Assert.IsTrue(viewModel.OutputDeviceOptions.Any(option =>
            option.Id.Length == 0));
        Assert.IsTrue(viewModel.OutputDeviceOptions.Any(option =>
            option.Id == "endpoint-temporarily-unavailable" &&
            option.DisplayName.Contains("当前不可用", StringComparison.Ordinal)));
        Assert.AreEqual(0, configProvider.WriteCount);
        Assert.AreEqual(
            "endpoint-temporarily-unavailable",
            configProvider.GetConfig().Audio.OutputDeviceId);

        viewModel.SelectedOutputDeviceId = string.Empty;

        Assert.AreEqual(1, configProvider.WriteCount);
        Assert.AreEqual(string.Empty, configProvider.GetConfig().Audio.OutputDeviceId);
    }

    [TestMethod]
    public void OutputDeviceSelection_PersistsStableDeviceId()
    {
        var configProvider = new FakeConfigProvider(CreateAudioConfig(
            48_000,
            AudioSettings.ChannelType.Stereo,
            AudioSettings.BitDepthType.Bit24));
        AudioOutputDeviceOption[] devices =
        [
            new("endpoint-a", "Speakers", true),
            new("endpoint-b", "USB DAC", false)
        ];
        var audioOutputFormatProvider = new FakeAudioOutputFormatProvider(
            new SystemAudioOutputFormat(
                48_000,
                AudioSettings.ChannelType.Stereo,
                AudioSettings.BitDepthType.Bit24),
            devices);
        var viewModel = new SettingsViewModel(
            configProvider,
            audioOutputFormatProvider);
        SettingChangedEventArgs? observedEvent = null;
        viewModel.SettingChanged += (_, eventArgs) => observedEvent = eventArgs;

        viewModel.SelectedOutputDeviceId = "endpoint-b";

        Assert.AreEqual(1, configProvider.WriteCount);
        Assert.AreEqual(
            nameof(SettingsViewModel.SelectedOutputDeviceId),
            observedEvent?.SettingName);
        Assert.AreEqual("endpoint-b", configProvider.LastWrittenOutputDeviceId);
        Assert.AreEqual("USB DAC", viewModel.GetOutputDeviceDisplayName("endpoint-b"));
        Assert.IsTrue(viewModel.OutputDeviceOptions.Any(option =>
            option.Id.Length == 0));
        Assert.AreEqual(1, audioOutputFormatProvider.DeviceQueryCount);
        Assert.AreEqual(
            AudioSettings.BackendType.FAudio,
            audioOutputFormatProvider.LastDeviceBackend);
    }

    private static ConfigData CreateAudioConfig(
        int sampleRate,
        AudioSettings.ChannelType channel,
        AudioSettings.BitDepthType bitDepth) =>
        new()
        {
            Audio = new AudioSettings
            {
                SampleRate = sampleRate,
                Channel = channel,
                BitDepth = bitDepth
            }
        };
}

[TestClass]
[DoNotParallelize]
public sealed class ConfigProviderAudioMigrationTest
{
    [TestMethod]
    public void Startup_CreatesNewConfigWithExplicitSystemOutputFormat()
    {
        var systemFormat = new SystemAudioOutputFormat(
            96_000,
            AudioSettings.ChannelType.Surround71,
            AudioSettings.BitDepthType.Bit24);
        var audioOutputFormatProvider = new FakeAudioOutputFormatProvider(systemFormat);
        var fileName = $"audio-settings-test-{Guid.NewGuid():N}.xml";
        var filePath = GetConfigFilePath(fileName);
        ConfigProvider? provider = null;
        try
        {
            provider = new ConfigProvider(audioOutputFormatProvider, fileName);

            ref var createdConfig = ref provider.GetConfig();
            Assert.AreEqual(systemFormat.SampleRate, createdConfig.Audio.SampleRate);
            Assert.AreEqual(systemFormat.Channel, createdConfig.Audio.Channel);
            Assert.AreEqual(systemFormat.BitDepth, createdConfig.Audio.BitDepth);
            Assert.AreEqual(AudioSettings.BackendType.FAudio, createdConfig.Audio.Backend);
            Assert.AreEqual(string.Empty, createdConfig.Audio.OutputDeviceId);
            Assert.AreEqual(1, audioOutputFormatProvider.QueryCount);

            var persistedXml = File.ReadAllText(filePath);
            Assert.IsFalse(persistedXml.Contains(
                "<channel-type>system</channel-type>",
                StringComparison.Ordinal));
            Assert.IsFalse(persistedXml.Contains(
                "<bit-depth>system</bit-depth>",
                StringComparison.Ordinal));
        }
        finally
        {
            if (provider is not null)
                GC.SuppressFinalize(provider);
            File.Delete(filePath);
        }
    }

    [TestMethod]
    public void Startup_MigratesLegacySystemValuesAndPreservesExplicitSampleRate()
    {
        var legacyConfig = CreateAudioConfig(
            44_100,
            AudioSettings.ChannelType.System,
            AudioSettings.BitDepthType.System);
        var systemFormat = new SystemAudioOutputFormat(
            96_000,
            AudioSettings.ChannelType.Surround51,
            AudioSettings.BitDepthType.Bit32);
        var audioOutputFormatProvider = new FakeAudioOutputFormatProvider(systemFormat);

        WithConfigFile(legacyConfig, fileName =>
        {
            var provider = new ConfigProvider(audioOutputFormatProvider, fileName);
            try
            {
                ref var migratedConfig = ref provider.GetConfig();
                Assert.AreEqual(systemFormat.Channel, migratedConfig.Audio.Channel);
                Assert.AreEqual(systemFormat.BitDepth, migratedConfig.Audio.BitDepth);
                Assert.AreEqual(44_100, migratedConfig.Audio.SampleRate);
                Assert.AreEqual(1, audioOutputFormatProvider.QueryCount);

                var persistedXml = File.ReadAllText(GetConfigFilePath(fileName));
                Assert.IsFalse(persistedXml.Contains(
                    "<channel-type>system</channel-type>",
                    StringComparison.Ordinal));
                Assert.IsFalse(persistedXml.Contains(
                    "<bit-depth>system</bit-depth>",
                    StringComparison.Ordinal));
            }
            finally
            {
                GC.SuppressFinalize(provider);
            }
        });
    }

    [TestMethod]
    public void Startup_PreservesExplicitAudioValuesWithoutQueryingSystemFormat()
    {
        var explicitConfig = CreateAudioConfig(
            88_200,
            AudioSettings.ChannelType.Mono,
            AudioSettings.BitDepthType.Bit24);
        var audioOutputFormatProvider = new FakeAudioOutputFormatProvider(
            new SystemAudioOutputFormat(
                48_000,
                AudioSettings.ChannelType.Stereo,
                AudioSettings.BitDepthType.Bit16));

        WithConfigFile(explicitConfig, fileName =>
        {
            var provider = new ConfigProvider(audioOutputFormatProvider, fileName);
            try
            {
                ref var loadedConfig = ref provider.GetConfig();
                Assert.AreEqual(88_200, loadedConfig.Audio.SampleRate);
                Assert.AreEqual(AudioSettings.ChannelType.Mono, loadedConfig.Audio.Channel);
                Assert.AreEqual(AudioSettings.BitDepthType.Bit24, loadedConfig.Audio.BitDepth);
                Assert.AreEqual(0, audioOutputFormatProvider.QueryCount);
            }
            finally
            {
                GC.SuppressFinalize(provider);
            }
        });
    }

    private static ConfigData CreateAudioConfig(
        int sampleRate,
        AudioSettings.ChannelType channel,
        AudioSettings.BitDepthType bitDepth) =>
        new()
        {
            Audio = new AudioSettings
            {
                SampleRate = sampleRate,
                Channel = channel,
                BitDepth = bitDepth
            }
        };

    private static void WithConfigFile(
        ConfigData config,
        Action<string> assertion)
    {
        var fileName = $"audio-settings-test-{Guid.NewGuid():N}.xml";
        var filePath = GetConfigFilePath(fileName);
        try
        {
            using (var file = File.Create(filePath))
            {
                new XmlSerializer(typeof(ConfigData)).Serialize(file, config);
            }

            assertion(fileName);
        }
        finally
        {
            File.Delete(filePath);
        }
    }

    private static string GetConfigFilePath(string fileName)
    {
        var assemblyDirectory = Path.GetDirectoryName(
            typeof(ConfigProvider).Assembly.Location);
        Assert.IsNotNull(assemblyDirectory);
        return Path.Combine(assemblyDirectory, fileName);
    }
}

internal sealed class FakeAudioOutputFormatProvider(
    SystemAudioOutputFormat format,
    IReadOnlyList<AudioOutputDeviceOption>? outputDevices = null)
    : IAudioOutputFormatProvider
{
    public int QueryCount { get; private set; }
    public int DeviceQueryCount { get; private set; }
    public AudioSettings.BackendType? LastDeviceBackend { get; private set; }

    public SystemAudioOutputFormat GetSystemDefaultOutputFormat()
    {
        QueryCount++;
        return format;
    }

    public IReadOnlyList<AudioOutputDeviceOption> GetAudioOutputDevices(
        AudioSettings.BackendType backend)
    {
        DeviceQueryCount++;
        LastDeviceBackend = backend;
        return outputDevices ?? [];
    }
}

internal sealed class FakeConfigProvider(ConfigData config) : IConfigProvider
{
    private ConfigData _config = config;

    public int WriteCount { get; private set; }
    public SystemAudioOutputFormat? LastWrittenAudioFormat { get; private set; }
    public AudioSettings.BackendType? LastWrittenBackend { get; private set; }
    public string? LastWrittenOutputDeviceId { get; private set; }

    public ErrorCode CreateConfigFile(string ConfigFileName = "config.xml") =>
        ErrorCode.NoError;

    public ErrorCode Reload(string ConfigFileName = "config.xml") =>
        ErrorCode.NoError;

    public ErrorCode WriteFile(string ConfigFileName = "config.xml")
    {
        WriteCount++;
        LastWrittenAudioFormat = new SystemAudioOutputFormat(
            _config.Audio.SampleRate,
            _config.Audio.Channel,
            _config.Audio.BitDepth);
        LastWrittenBackend = _config.Audio.Backend;
        LastWrittenOutputDeviceId = _config.Audio.OutputDeviceId;
        return ErrorCode.NoError;
    }

    public ref ConfigData GetConfig() => ref _config;
}
