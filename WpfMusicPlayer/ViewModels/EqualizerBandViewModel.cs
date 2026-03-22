namespace WpfMusicPlayer.ViewModels;

public class EqualizerBandViewModel(int index, string label, Action<int, int> onValueChanged) : ViewModelBase
{
    private readonly Action<int, int> _onValueChanged = onValueChanged;
    private readonly int _index = index;

    public string Label { get; } = label;

    public int Value
    {
        get;
        set
        {
            value = Math.Clamp(value, -12, 12);
            if (SetProperty(ref field, value))
            {
                OnPropertyChanged(nameof(DisplayValue));
                _onValueChanged(_index, value);
            }
        }
    }

    public string DisplayValue => Value >= 0 ? $"+{Value}" : Value.ToString();
}
