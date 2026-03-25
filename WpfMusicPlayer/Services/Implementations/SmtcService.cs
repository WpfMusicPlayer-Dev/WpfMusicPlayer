using System.IO;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Threading;
using MusicPlayerLibrary;
using Windows.Media;
using Windows.Storage.Streams;
using WinRT;
using WpfMusicPlayer.Services.Abstractions;

namespace WpfMusicPlayer.Services.Implementations;

public class SmtcService : ISmtcService
{
    private SystemMediaTransportControls? _smtc;
    private readonly Dispatcher _dispatcher = Application.Current?.Dispatcher ?? Dispatcher.CurrentDispatcher;
    private InMemoryRandomAccessStream? _thumbnailStream;

    public event Action? PlayRequested;
    public event Action? PauseRequested;
    public event Action? NextRequested;
    public event Action? PreviousRequested;

    public void Initialize(IntPtr windowHandle)
    {
        // Use native C++/CLI helper to bypass CsWinRT activation factory proxy
        var smtcPtr = SmtcInteropHelper.GetSmtcForWindow(windowHandle);
        if (smtcPtr == IntPtr.Zero) return;

        try
        {
            // CsWinRT projected types cannot be obtained via Marshal.GetObjectForIUnknown;
            // use CsWinRT's MarshalInterface<T>.FromAbi which performs the correct QI.
            _smtc = MarshalInterface<SystemMediaTransportControls>.FromAbi(smtcPtr);
        }
        finally
        {
            Marshal.Release(smtcPtr);
        }

        _smtc.IsEnabled = true;
        _smtc.IsPlayEnabled = true;
        _smtc.IsPauseEnabled = true;
        _smtc.IsNextEnabled = true;
        _smtc.IsPreviousEnabled = true;
        _smtc.ButtonPressed += OnButtonPressed;
    }

    private void OnButtonPressed(SystemMediaTransportControls sender,
        SystemMediaTransportControlsButtonPressedEventArgs args)
    {
        _dispatcher.Invoke(() =>
        {
            switch (args.Button)
            {
                case SystemMediaTransportControlsButton.Play:     PlayRequested?.Invoke();     break;
                case SystemMediaTransportControlsButton.Pause:    PauseRequested?.Invoke();    break;
                case SystemMediaTransportControlsButton.Next:     NextRequested?.Invoke();     break;
                case SystemMediaTransportControlsButton.Previous: PreviousRequested?.Invoke(); break;
            }
        });
    }

    public void UpdateTextMetadata(string title, string artist)
    {
        if (_smtc == null) return;

        var updater = _smtc.DisplayUpdater;
        updater.Type = MediaPlaybackType.Music;
        updater.MusicProperties.Title = title;
        updater.MusicProperties.Artist = artist;
        updater.Update();
    }

    public void UpdateMetadata(string title, string artist, Stream? albumArtStream)
    {
        if (_smtc == null) return;

        var updater = _smtc.DisplayUpdater;
        updater.Type = MediaPlaybackType.Music;
        updater.MusicProperties.Title = title;
        updater.MusicProperties.Artist = artist;

        try
        {
            _thumbnailStream?.Dispose();
            _thumbnailStream = null;

            if (albumArtStream != null)
            {
                // Read all bytes from the managed stream
                using var ms = new MemoryStream();
                albumArtStream.CopyTo(ms);
                var bytes = ms.ToArray();

                // Write into a WinRT InMemoryRandomAccessStream and keep it alive
                // so SMTC can read the thumbnail asynchronously after this method returns.
                var ras = new InMemoryRandomAccessStream();
                var writer = new DataWriter(ras.GetOutputStreamAt(0));
                writer.WriteBytes(bytes);
                writer.StoreAsync().GetResults();
                writer.DetachStream();
                writer.Dispose();
                ras.Seek(0);
                _thumbnailStream = ras;
                updater.Thumbnail = RandomAccessStreamReference.CreateFromStream(ras);
            }
            else
            {
                updater.Thumbnail = null;
            }
        }
        catch
        {
            updater.Thumbnail = null;
        }

        updater.Update();
    }

    public void UpdatePlaybackStatus(PlaybackState state)
    {
        if (_smtc == null) return;
        _smtc.PlaybackStatus = state switch
        {
            PlaybackState.Playing => MediaPlaybackStatus.Playing,
            PlaybackState.Paused  => MediaPlaybackStatus.Paused,
            PlaybackState.Stopped => MediaPlaybackStatus.Stopped,
            PlaybackState.Closed  => MediaPlaybackStatus.Closed,
            _ => _smtc.PlaybackStatus
        };
    }

    public void UpdateTimeline(TimeSpan position, TimeSpan duration)
    {
        if (_smtc == null) return;
        try
        {
            _smtc.UpdateTimelineProperties(new SystemMediaTransportControlsTimelineProperties
            {
                StartTime = TimeSpan.Zero,
                MinSeekTime = TimeSpan.Zero,
                Position = position,
                MaxSeekTime = duration,
                EndTime = duration
            });
        }
        catch
        {
            // Timeline not supported on this OS version
        }
    }

    public void Dispose()
    {
        if (_smtc == null) return;
        _smtc.IsEnabled = false;
        _smtc.ButtonPressed -= OnButtonPressed;
        _smtc = null;
        _thumbnailStream?.Dispose();
        _thumbnailStream = null;
        GC.SuppressFinalize(this);
    }
}

