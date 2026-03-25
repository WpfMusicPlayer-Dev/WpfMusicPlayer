using WpfMusicPlayer.Models;

namespace WpfMusicPlayer.Services.Abstractions;

public interface ISongDatabaseService : IDisposable
{
    SongRecord? FindByMd5(string md5);
    void Upsert(SongRecord record);
    void IncrementPlayCount(string md5);
    List<SongRecord> GetAllSongs();
    List<SongRecord> GetTopPlayed(int count);
}
