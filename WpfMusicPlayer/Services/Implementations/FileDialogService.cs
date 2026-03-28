using System.Windows;
using WinRT.Interop;
using WpfMusicPlayer.Services.Abstractions;

namespace WpfMusicPlayer.Services.Implementations;

public class FileDialogService : IFileDialogService
{
    public async Task<string?> PickMusicFileAsync()
    {
        var picker = new Windows.Storage.Pickers.FileOpenPicker();
        picker.ViewMode = Windows.Storage.Pickers.PickerViewMode.Thumbnail;
        picker.SuggestedStartLocation = Windows.Storage.Pickers.PickerLocationId.MusicLibrary;
        picker.FileTypeFilter.Add(".mp3");
        picker.FileTypeFilter.Add(".flac");
        picker.FileTypeFilter.Add(".ncm");

        var hwnd = new System.Windows.Interop.WindowInteropHelper(Application.Current.MainWindow).Handle;
        InitializeWithWindow.Initialize(picker, hwnd);

        Windows.Storage.StorageFile file = await picker.PickSingleFileAsync();
        return file?.Path;
    }
}
