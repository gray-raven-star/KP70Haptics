using System.Windows;
using System.Windows.Controls;

namespace KP70Studio;

internal sealed record ChannelTuning(int Gain, int Sensitivity, int Threshold, int[]? Curve = null,
    int Base = 9, int Kick = 30, int Start = 12, int Frequency = 20, double[][]? Points = null);

public partial class ChannelTuningWindow : Window
{
    static readonly string[] Names = ["左肩键", "右肩键", "左握把", "右握把", "左扳机", "右扳机"];
    record Choice(int Index, string Name);

    readonly ChannelTuning[] common;
    readonly ChannelTuning?[] overrides;
    readonly bool linear;
    readonly Action changed;
    bool loading;

    internal ChannelTuningWindow(ResourceDictionary styles, int[] available, ChannelTuning[] common,
        ChannelTuning?[] overrides, bool linear, Action changed)
    {
        this.common = common;
        this.overrides = overrides;
        this.linear = linear;
        this.changed = changed;
        InitializeComponent();
        Resources.MergedDictionaries.Add(styles);
        Curve.Edited += OnCurveEdited;
        ChannelPicker.ItemsSource = available.Select(i => new Choice(i, Names[i])).ToArray();
        ChannelPicker.SelectedIndex = 0;
    }

    static int[] SuggestedCurve(int index) => index is 2 or 3
        ? [65, 90, 55, 10, 0, 0, 0]
        : [0, 0, 0, 15, 70, 90, 65];

    void ChannelChanged(object sender, SelectionChangedEventArgs args)
    {
        if (ChannelPicker.SelectedItem is Choice choice) LoadChannel(choice.Index);
    }

    void LoadChannel(int index)
    {
        var value = overrides[index] ?? common[index];
        loading = true;
        Gain.Maximum = index >= 4 && !linear ? 60 : 100;
        Threshold.Minimum = index >= 4 ? 1 : 0;
        Gain.Value = value.Gain;
        Sensitivity.Value = value.Sensitivity;
        Threshold.Value = value.Threshold;
        bool force = index >= 4 && !linear;
        TriggerPanel.Visibility = force ? Visibility.Visible : Visibility.Collapsed;
        Gain.Parent.SetCurrentValue(UIElement.VisibilityProperty, force ? Visibility.Collapsed : Visibility.Visible);
        TrgBase.Value = value.Base;
        TrgKick.Value = value.Kick;
        TrgStart.Value = value.Start;
        TrgFrequency.Value = value.Frequency;
        bool custom = value.Curve is { Length: FrequencyCurve.BandCount };
        Curve.SetValues(custom ? value.Curve! : SuggestedCurve(index), custom, value.Points);
        loading = false;
        RefreshState(index);
    }

    void RefreshState(int index)
    {
        ResetChannel.IsEnabled = overrides[index] is not null;
        ResetCurve.IsEnabled = overrides[index]?.Curve is { Length: FrequencyCurve.BandCount };
        ChannelState.Text = ResetChannel.IsEnabled ? "当前通道独立设置" : "当前通道跟随主界面";
        CurveState.Text = ResetCurve.IsEnabled ? "自定义曲线生效" : "沿用原有频段，拖动任一点启用自定义";
    }

    ChannelTuning Read(int[]? curve) => new((int)Gain.Value, (int)Sensitivity.Value, (int)Threshold.Value, curve,
        (int)TrgBase.Value, (int)TrgKick.Value, (int)TrgStart.Value, (int)TrgFrequency.Value,
        curve is null ? null : Curve.GetPoints());

    void Commit(int[]? curve)
    {
        if (ChannelPicker?.SelectedItem is not Choice choice) return;
        overrides[choice.Index] = Read(curve ?? overrides[choice.Index]?.Curve);
        RefreshState(choice.Index);
        changed();
    }

    void ScalarChanged(object sender, RoutedPropertyChangedEventArgs<double> args)
    {
        if (loading) return;
        Commit(null);
    }
    void TriggerChanged(object sender, RoutedPropertyChangedEventArgs<double> args)
    {
        if (loading) return;
        Commit(null);
    }

    void OnCurveEdited(int[] values)
    {
        if (ChannelPicker.SelectedItem is not Choice choice) return;
        overrides[choice.Index] = Read(values);
        RefreshState(choice.Index);
        changed();
    }

    void RestoreCurve(object sender, RoutedEventArgs args)
    {
        if (ChannelPicker.SelectedItem is not Choice choice) return;
        var value = Read(null);
        overrides[choice.Index] = value == common[choice.Index] ? null : value;
        Curve.SetValues(SuggestedCurve(choice.Index), false);
        RefreshState(choice.Index);
        changed();
    }

    void RestoreChannel(object sender, RoutedEventArgs args)
    {
        if (ChannelPicker.SelectedItem is not Choice choice) return;
        overrides[choice.Index] = null;
        LoadChannel(choice.Index);
        changed();
    }
}
