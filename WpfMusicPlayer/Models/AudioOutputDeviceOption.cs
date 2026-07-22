// SPDX-License-Identifier: MIT

namespace WpfMusicPlayer.Models;

public sealed record AudioOutputDeviceOption(
    string Id,
    string DisplayName,
    bool IsDefault)
{
    public override string ToString() => DisplayName;
}
