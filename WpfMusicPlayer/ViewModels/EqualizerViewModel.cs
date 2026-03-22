using System.Collections.ObjectModel;

namespace WpfMusicPlayer.ViewModels;

public class EqualizerPreset(string name, int[] values)
{
    public string Name { get; } = name;
    public int[] Values { get; } = values;
}

public class EqualizerViewModel : ViewModelBase
{
    private readonly Action<int, int>? _applyBand;
    private bool _suppressPresetSwitch;

    public EqualizerViewModel(Action<int, int>? applyBand = null)
    {
        _applyBand = applyBand;

        string[] labels = ["31", "62", "125", "250", "500", "1k", "2k", "4k", "8k", "16k"];
        for (var i = 0; i < 10; i++)
        {
            Bands.Add(new EqualizerBandViewModel(i, labels[i], OnBandValueChanged));
        }

        Presets =
        [
            new("默认",     [0,  0,  0,  0,  0,  0,  0,  0,  0,  0]),
            new("完美低音", [6,  4, -5,  2,  3,  4,  4,  5,  5,  6]),
            new("极致摇滚", [6,  4,  0, -2, -6,  1,  4,  6,  7,  9]),
            new("最毒人声", [4,  0,  1,  2,  3,  4,  5,  4,  3,  3])
        ];

        SelectedPreset = Presets[0];
    }

    public ObservableCollection<EqualizerBandViewModel> Bands { get; } = [];

    public List<EqualizerPreset> Presets { get; }

    public EqualizerPreset? SelectedPreset
    {
        get;
        set
        {
            if (!SetProperty(ref field, value) || value == null) return;
            ApplyPreset(value);
        }
    }

    private void ApplyPreset(EqualizerPreset preset)
    {
        _suppressPresetSwitch = true;
        for (var i = 0; i < 10 && i < preset.Values.Length; i++)
        {
            Bands[i].Value = preset.Values[i];
        }
        _suppressPresetSwitch = false;
    }

    private void OnBandValueChanged(int index, int value)
    {
        _applyBand?.Invoke(index, value);

        if (_suppressPresetSwitch) return;

        if (SelectedPreset != null && MatchesPreset(SelectedPreset)) return;

        _suppressPresetSwitch = true;
        SelectedPreset = null;
        _suppressPresetSwitch = false;
    }

    private bool MatchesPreset(EqualizerPreset preset)
    {
        for (var i = 0; i < 10; i++)
        {
            if (Bands[i].Value != preset.Values[i]) return false;
        }
        return true;
    }

    public void SyncFromPlayer(Func<int, int> getBand)
    {
        _suppressPresetSwitch = true;
        for (var i = 0; i < 10; i++)
        {
            Bands[i].Value = Math.Clamp(getBand(i), -12, 12);
        }
        _suppressPresetSwitch = false;

        foreach (var preset in Presets)
        {
            if (!MatchesPreset(preset)) continue;
            SelectedPreset = preset;
            return;
        }
        SelectedPreset = null;
    }
}
