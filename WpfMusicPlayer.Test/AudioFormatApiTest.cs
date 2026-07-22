using MusicPlayerLibrary;

namespace WpfMusicPlayer.Test;

[TestClass]
public sealed class AudioFormatApiTest
{
    [TestMethod]
    public void ManagedFormatter_ProducesHumanReadableAudioMetadata()
    {
        Assert.AreEqual("44.1 kHz", MusicPlayerManaged.FormatAudioSampleRate(44_100));
        Assert.AreEqual("11.025 kHz", MusicPlayerManaged.FormatAudioSampleRate(11_025));
        Assert.AreEqual("800 Hz", MusicPlayerManaged.FormatAudioSampleRate(800));
        Assert.AreEqual("Unknown sample rate", MusicPlayerManaged.FormatAudioSampleRate(0));

        Assert.AreEqual("System", MusicPlayerManaged.FormatAudioChannelType(0));
        Assert.AreEqual("Mono", MusicPlayerManaged.FormatAudioChannelType(1));
        Assert.AreEqual("Stereo", MusicPlayerManaged.FormatAudioChannelType(2));
        Assert.AreEqual("Surround 5.1", MusicPlayerManaged.FormatAudioChannelType(3));
        Assert.AreEqual("Surround 7.1", MusicPlayerManaged.FormatAudioChannelType(4));
        Assert.AreEqual("Unknown channel layout", MusicPlayerManaged.FormatAudioChannelType(-1));

        Assert.AreEqual("System", MusicPlayerManaged.FormatAudioBitDepth(0));
        Assert.AreEqual("24-bit", MusicPlayerManaged.FormatAudioBitDepth(24));
        Assert.AreEqual("Unknown bit depth", MusicPlayerManaged.FormatAudioBitDepth(-1));
        Assert.AreEqual(
            "192 kHz / 32-bit / Surround 5.1, 120.00 kbps",
            MusicPlayerManaged.FormatAudioFormat(3, 192_000, 32, 120000));
    }

    [TestMethod]
    [DoNotParallelize]
    public void ManagedQueries_FormatNativeStructMetadata()
    {
        using var player = new MusicPlayerManaged(192_000, 3, 24);

        Assert.IsNull(player.GetAudioSourceFormat());
        Assert.AreEqual(
            "192 kHz / 24-bit / Surround 5.1",
            player.GetDeviceOutputFormat());
        Assert.AreEqual(192_000, player.GetDeviceOutputSampleRate());
    }
}
