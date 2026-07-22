// SPDX-License-Identifier: MIT

using MusicPlayerLibrary;
using Microsoft.Extensions.Logging;
using WpfMusicPlayer.Models;
using WpfMusicPlayer.Services.Abstractions;
using static WpfMusicPlayer.Models.ConfigData;

namespace WpfMusicPlayer.Services.Implementations;

public sealed class AudioOutputFormatProvider(
    ILogger<AudioOutputFormatProvider> logger) : IAudioOutputFormatProvider
{
    private static readonly SystemAudioOutputFormat FallbackFormat = new(
        48_000,
        AudioSettings.ChannelType.Stereo,
        AudioSettings.BitDepthType.Bit32);

    public SystemAudioOutputFormat GetSystemDefaultOutputFormat()
    {
        int channelTypeId;
        int sampleRate;
        int bitDepthValue;
        try
        {
            MusicPlayerManaged.GetSystemDefaultOutputFormat(
                out channelTypeId,
                out sampleRate,
                out bitDepthValue);
        }
        catch (Exception exception)
        {
            logger.LogWarning(
                exception,
                "Unable to query the system output format; using the safe fallback {SampleRate} Hz, {Channel}, {BitDepth}",
                FallbackFormat.SampleRate,
                FallbackFormat.Channel,
                FallbackFormat.BitDepth);
            return FallbackFormat;
        }

        var channel = (AudioSettings.ChannelType)channelTypeId;
        if (!Enum.IsDefined(channel) || channel == AudioSettings.ChannelType.System)
        {
            logger.LogWarning(
                "The system output channel layout is not supported (type id: {ChannelTypeId}); using stereo",
                channelTypeId);
            channel = FallbackFormat.Channel;
        }

        var bitDepth = (AudioSettings.BitDepthType)bitDepthValue;
        if (!Enum.IsDefined(bitDepth) || bitDepth == AudioSettings.BitDepthType.System)
        {
            logger.LogWarning(
                "The system output bit depth is not supported ({BitDepth}-bit); using {FallbackBitDepth}",
                bitDepthValue,
                FallbackFormat.BitDepth);
            bitDepth = FallbackFormat.BitDepth;
        }

        if (sampleRate <= 0)
        {
            logger.LogWarning(
                "The system output sample rate is invalid ({SampleRate} Hz); using {FallbackSampleRate} Hz",
                sampleRate,
                FallbackFormat.SampleRate);
            sampleRate = FallbackFormat.SampleRate;
        }

        return new SystemAudioOutputFormat(sampleRate, channel, bitDepth);
    }

    public IReadOnlyList<AudioOutputDeviceOption> GetAudioOutputDevices(
        AudioSettings.BackendType backend)
    {
        try
        {
            var nativeDevices = MusicPlayerManaged.GetAudioOutputDevices((int)backend);
            if (nativeDevices is null || nativeDevices.Length == 0)
                return [];

            var devices = new List<AudioOutputDeviceOption>(nativeDevices.Length);
            var knownIds = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            foreach (var nativeDevice in nativeDevices)
            {
                if (nativeDevice is null || string.IsNullOrWhiteSpace(nativeDevice.Id) ||
                    !knownIds.Add(nativeDevice.Id))
                {
                    continue;
                }

                var displayName = string.IsNullOrWhiteSpace(nativeDevice.DisplayName)
                    ? nativeDevice.Id
                    : nativeDevice.DisplayName;
                devices.Add(new AudioOutputDeviceOption(
                    nativeDevice.Id,
                    displayName,
                    nativeDevice.IsDefault));
            }
            return devices;
        }
        catch (Exception exception)
        {
            logger.LogWarning(
                exception,
                "Unable to enumerate output devices for audio backend {AudioBackend}",
                backend);
            return [];
        }
    }
}
