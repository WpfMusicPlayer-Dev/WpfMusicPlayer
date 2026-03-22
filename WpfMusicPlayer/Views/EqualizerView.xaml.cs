using System.Windows.Controls;
using System.Windows.Input;

namespace WpfMusicPlayer.Views;

public partial class EqualizerView : UserControl
{
    public EqualizerView()
    {
        InitializeComponent();
    }

    private void ValueTextBox_GotFocus(object sender, System.Windows.RoutedEventArgs e)
    {
        if (sender is TextBox tb)
            tb.SelectAll();
    }

    private void ValueTextBox_KeyDown(object sender, KeyEventArgs e)
    {
        if (e.Key != Key.Enter) return;
        if (sender is TextBox tb)
        {
            var binding = tb.GetBindingExpression(TextBox.TextProperty);
            binding?.UpdateSource();
            Keyboard.ClearFocus();
        }
    }
}
