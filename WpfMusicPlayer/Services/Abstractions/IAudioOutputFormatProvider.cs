// SPDX-License-Identifier: MIT

using WpfMusicPlayer.Models;
using static WpfMusicPlayer.Models.ConfigData;

namespace WpfMusicPlayer.Services.Abstractions;

public interface IAudioOutputFormatProvider
{
    SystemAudioOutputFormat GetSystemDefaultOutputFormat();
    IReadOnlyList<AudioOutputDeviceOption> GetAudioOutputDevices(
        AudioSettings.BackendType backend);
}
