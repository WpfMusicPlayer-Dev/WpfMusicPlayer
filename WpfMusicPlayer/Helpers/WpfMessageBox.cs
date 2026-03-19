using System.Windows;

namespace WpfMusicPlayer.Helpers;

public enum WpfMessageBoxIcon
{
    None,
    Information,
    Error,
    Warning
}

public partial class WpfMessageBox : Window
{
    private WpfMessageBox()
    {
        InitializeComponent();
    }

    /// <summary>
    /// Shows a modern-styled message dialog.
    /// </summary>
    public static void Show(string message, string title,
        WpfMessageBoxIcon icon = WpfMessageBoxIcon.None)
    {
        var owner = Application.Current.Windows
            .OfType<Window>()
            .FirstOrDefault(w => w.IsActive)
            ?? Application.Current.MainWindow;

        var dlg = new WpfMessageBox
        {
            TitleText =
            {
                Text = title
            },
            MessageText =
            {
                Text = message
            }
        };

        if (icon != WpfMessageBoxIcon.None)
        {
            dlg.IconText.Visibility = Visibility.Visible;

            // ReSharper disable once SwitchStatementHandlesSomeKnownEnumValuesWithDefault
            switch (icon)
            {
                case WpfMessageBoxIcon.Information:
                    dlg.IconText.Text = "\uE946"; // Info icon
                    dlg.IconText.Foreground = new System.Windows.Media.SolidColorBrush(
                        System.Windows.Media.Color.FromRgb(0x00, 0x78, 0xD7));
                    break;
                case WpfMessageBoxIcon.Error:
                    dlg.IconText.Text = "\uEA39"; // Error icon
                    dlg.IconText.Foreground = new System.Windows.Media.SolidColorBrush(
                        System.Windows.Media.Color.FromRgb(0xE8, 0x11, 0x23));
                    break;
                case WpfMessageBoxIcon.Warning:
                    dlg.IconText.Text = "\uE7BA"; // Warning icon
                    dlg.IconText.Foreground = new System.Windows.Media.SolidColorBrush(
                        System.Windows.Media.Color.FromRgb(0xFF, 0xB9, 0x00));
                    break;
                default:
                    throw new ArgumentOutOfRangeException(nameof(icon), icon, null);
            }
        }

        if (owner != null)
        {
            dlg.Owner = owner;
        }

        dlg.ShowDialog();
    }

    private void OkButton_Click(object sender, RoutedEventArgs e)
    {
        DialogResult = true;
        Close();
    }
}

