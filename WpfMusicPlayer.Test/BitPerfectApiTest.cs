using MusicPlayerLibrary;

namespace WpfMusicPlayer.Test;

[TestClass]
[DoNotParallelize]
public sealed class BitPerfectApiTest
{
    [TestMethod]
    public void UnopenedAndClosedPlayers_AreNotBitPerfect()
    {
        using var player = new MusicPlayerManaged(48_000, 2, 16);
        Assert.IsFalse(player.IsBitPerfect());

        var path = CreatePcm16WaveFile(48_000);
        try
        {
            player.OpenFile(path);
            player.CloseFile();
            Assert.IsFalse(player.IsBitPerfect());
        }
        finally
        {
            File.Delete(path);
        }
    }

    [TestMethod]
    public void ExactPcmRoute_OnSharedFAudioNeverReportsBitPerfect()
    {
        var path = CreatePcm16WaveFile(48_000);
        try
        {
            using var player = new MusicPlayerManaged(48_000, 2, 16);
            player.OpenFile(path);

            Assert.IsFalse(
                player.IsBitPerfect(),
                $"Shared FAudio must not report bit-perfect. Source={player.GetAudioSourceFormat()}, Sink={player.GetDeviceOutputFormat()}");
            player.SetEqualizerBand(5, 6);
            Assert.IsFalse(player.IsBitPerfect());
            player.SetEqualizerBand(5, 0);
            Assert.IsFalse(player.IsBitPerfect());

			using var stopped = new ManualResetEventSlim();
			Exception? playbackError = null;
			player.OnPlayerStop = () => stopped.Set();
			player.OnPlayerError = exception =>
			{
				playbackError = exception;
				stopped.Set();
			};
			player.Start();
			Assert.IsTrue(
				stopped.Wait(TimeSpan.FromSeconds(10)),
				"Shared FAudio playback did not reach its natural EOS.");
			Assert.IsNull(playbackError, playbackError?.ToString());
			Assert.IsFalse(player.IsBitPerfect());
        }
        finally
        {
            File.Delete(path);
        }
    }

    [TestMethod]
    public void SampleRateConversion_IsNotBitPerfect()
    {
        var path = CreatePcm16WaveFile(44_100);
        try
        {
            using var player = new MusicPlayerManaged(48_000, 2, 16);
            player.OpenFile(path);
            Assert.IsFalse(player.IsBitPerfect());
        }
        finally
        {
            File.Delete(path);
        }
    }

    private static string CreatePcm16WaveFile(int sampleRate)
    {
        const short channelCount = 2;
        const short bitsPerSample = 16;
        var frameCount = sampleRate / 20;
        var blockAlign = (short)(channelCount * bitsPerSample / 8);
        var dataLength = frameCount * blockAlign;
        var path = Path.Combine(
            Path.GetTempPath(), $"wpfmusicplayer-bit-perfect-{Guid.NewGuid():N}.wav");

        using var stream = File.Create(path);
        using var writer = new BinaryWriter(stream);
        writer.Write("RIFF"u8);
        writer.Write(36 + dataLength);
        writer.Write("WAVE"u8);
        writer.Write("fmt "u8);
        writer.Write(16);
        writer.Write((short)1);
        writer.Write(channelCount);
        writer.Write(sampleRate);
        writer.Write(sampleRate * blockAlign);
        writer.Write(blockAlign);
        writer.Write(bitsPerSample);
        writer.Write("data"u8);
        writer.Write(dataLength);

        for (var frame = 0; frame < frameCount; frame++)
        {
            var sample = (short)(Math.Sin(
                2.0 * Math.PI * 440.0 * frame / sampleRate) * 4_000.0);
            writer.Write(sample);
            writer.Write(sample);
        }

        return path;
    }
}
