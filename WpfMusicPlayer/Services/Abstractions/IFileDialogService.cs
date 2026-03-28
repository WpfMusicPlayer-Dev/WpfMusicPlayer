namespace WpfMusicPlayer.Services.Abstractions;

public interface IFileDialogService
{
    Task<string?> PickMusicFileAsync();
}
