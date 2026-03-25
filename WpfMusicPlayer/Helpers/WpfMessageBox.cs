using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;

namespace WpfMusicPlayer.Helpers;

public enum WpfMessageBoxIcon
{
    None,
    Information,
    Error,
    Warning,
    Question
}

public enum WpfMessageBoxResult
{
    None,
    OK,
    Cancel,
    Yes,
    No
}

public enum WpfMessageBoxButton
{
    OK,
    OKCancel,
    YesNo,
    YesNoCancel
}

public partial class WpfMessageBox : Window
{
    public WpfMessageBoxResult Result { get; private set; } = WpfMessageBoxResult.None;

    private WpfMessageBox()
    {
        InitializeComponent();
    }

    // AfxMessageBox和MessageBox.Show丑死了
    // 写个简单的凑合着用用
    public static void Show(string message, string title,
        WpfMessageBoxIcon icon = WpfMessageBoxIcon.None)
    {
        Show(message, title, WpfMessageBoxButton.OK, icon);
    }

    public static WpfMessageBoxResult Show(string message, string title,
        WpfMessageBoxButton buttons, WpfMessageBoxIcon icon = WpfMessageBoxIcon.None)
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

        dlg.BuildButtons(buttons);

        if (icon != WpfMessageBoxIcon.None)
        {
            dlg.IconText.Visibility = Visibility.Visible;

            // ReSharper disable once SwitchStatementHandlesSomeKnownEnumValuesWithDefault
            switch (icon)
            {
                case WpfMessageBoxIcon.Information:
                    dlg.IconText.Text = "\uE946"; // Info icon
                    dlg.IconText.Foreground = new SolidColorBrush(
                        Color.FromRgb(0x00, 0x78, 0xD7));
                    break;
                case WpfMessageBoxIcon.Error:
                    dlg.IconText.Text = "\uEA39"; // Error icon
                    dlg.IconText.Foreground = new SolidColorBrush(
                        Color.FromRgb(0xE8, 0x11, 0x23));
                    break;
                case WpfMessageBoxIcon.Warning:
                    dlg.IconText.Text = "\uE7BA"; // Warning icon
                    dlg.IconText.Foreground = new SolidColorBrush(
                        Color.FromRgb(0xFF, 0xB9, 0x00));
                    break;
                case WpfMessageBoxIcon.Question:
                    dlg.IconText.Text = "\uE9CE"; // Question icon
                    dlg.IconText.Foreground = new SolidColorBrush(
                        Color.FromRgb(0x44, 0x44, 0x44));
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
        return dlg.Result;
    }

    private void BuildButtons(WpfMessageBoxButton buttons)
    {
        var accentBrush = new SolidColorBrush(Color.FromRgb(0x00, 0x78, 0xD7));
        var neutralBrush = new SolidColorBrush(Color.FromRgb(0x44, 0x44, 0x44));
        var style = (Style)FindResource("DialogButtonStyle");

        switch (buttons)
        {
            case WpfMessageBoxButton.OK:
                AddButton("OK", WpfMessageBoxResult.OK, accentBrush, style, isDefault: true);
                break;
            case WpfMessageBoxButton.OKCancel:
                AddButton("取消", WpfMessageBoxResult.Cancel, neutralBrush, style, isCancel: true);
                AddButton("OK", WpfMessageBoxResult.OK, accentBrush, style, isDefault: true);
                break;
            case WpfMessageBoxButton.YesNo:
                AddButton("否", WpfMessageBoxResult.No, neutralBrush, style, isCancel: true);
                AddButton("是", WpfMessageBoxResult.Yes, accentBrush, style, isDefault: true);
                break;
            case WpfMessageBoxButton.YesNoCancel:
                AddButton("取消", WpfMessageBoxResult.Cancel, neutralBrush, style, isCancel: true);
                AddButton("否", WpfMessageBoxResult.No, neutralBrush, style);
                AddButton("是", WpfMessageBoxResult.Yes, accentBrush, style, isDefault: true);
                break;
        }
    }

    private void AddButton(string content, WpfMessageBoxResult result,
        Brush background, Style style,
        bool isDefault = false, bool isCancel = false)
    {
        var btn = new Button
        {
            Content = content,
            Background = background,
            Foreground = Brushes.White,
            Style = style,
            IsDefault = isDefault,
            IsCancel = isCancel,
            Margin = new Thickness(8, 0, 0, 0)
        };

        btn.Click += (_, _) =>
        {
            Result = result;
            DialogResult = true;
            Close();
        };

        ButtonPanel.Children.Add(btn);
    }
}

