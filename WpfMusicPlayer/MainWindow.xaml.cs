using System.Windows;
using System.Windows.Input;
using MusicPlayerLibrary;
using WpfMusicPlayer.Services;
using WpfMusicPlayer.ViewModels;

namespace WpfMusicPlayer
{
    /// <summary>
    /// Interaction logic for MainWindow.xaml
    /// </summary>
    public partial class MainWindow : Window
    {
        private MainViewModel ViewModel => (MainViewModel)DataContext;

        public MainWindow()
        {
            InitializeComponent();
            DataContext = new MainViewModel(new FileDialogService());
            AtlTraceRedirectManager.Init();
        }

        private void MainWindow_Closed(object sender, EventArgs e)
        {
            ViewModel.OnWindowClosed();
            ViewModel.Dispose();
        }

        private void MainWindow_Drop(object sender, DragEventArgs e)
        {
            if (e.Data.GetDataPresent(DataFormats.FileDrop))
            {
                string[] files = (string[])e.Data.GetData(DataFormats.FileDrop);
                if (files.Length > 0)
                {
                    ViewModel.OpenFile(files[0]);
                }
            }
        }

        private void ProgressSlider_PreviewMouseDown(object sender, MouseButtonEventArgs e)
        {
            ViewModel.IsDraggingSlider = true;
        }

        private void ProgressSlider_PreviewMouseUp(object sender, MouseButtonEventArgs e)
        {
            ViewModel.SeekToCurrentPosition();
        }
    }
}