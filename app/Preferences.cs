using System.IO;
using System.Text.Json;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using Brush = System.Windows.Media.Brush;
using Color = System.Windows.Media.Color;
using MessageBox = System.Windows.MessageBox;
using TextBox = System.Windows.Controls.TextBox;
using Button = System.Windows.Controls.Button;
using ComboBoxItem = System.Windows.Controls.ComboBoxItem;

namespace KP70Studio;

public partial class MainWindow
{
    record AppOptions(string Theme = "red", string CloseAction = "ask");
    record NamedPreset(string Name, Profile Settings);
    static string OptionsPath => Path.Combine(ProfileDirectory, "app-options.json");
    string theme = "red";
    bool selectingPreset;
    List<NamedPreset> presets = [];
    Profile? workingSettings;

    void LoadAppOptions()
    {
        var options = persistProfile ? ReadJson<AppOptions>(OptionsPath) : null;
        closeAction = options?.CloseAction is "tray" or "exit" ? options.CloseAction : "ask";
        string requested = options?.Theme ?? "red";
        ThemePicker.SelectedIndex = requested switch { "dark" => 1, "light" => 2, _ => 0 };
    }

    void SaveAppOptions()
    {
        if (!persistProfile) return;
        try { StoreJson(OptionsPath, new AppOptions(theme, closeAction)); }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        { MessageBox.Show(this, "保存设置失败。", "设置"); }
    }

    void ThemeChanged(object sender, SelectionChangedEventArgs args)
    {
        if (ThemePicker.SelectedItem is not ComboBoxItem { Tag: string name }) return;
        theme = name;
        Palette.Apply(name);
        if (IsLoaded) SaveAppOptions();
    }

    static T? ReadJson<T>(string path)
    {
        try { return JsonSerializer.Deserialize<T>(File.ReadAllText(path)); }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or JsonException) { return default; }
    }

    static void StoreJson<T>(string path, T value)
    {
        var folder = Path.GetDirectoryName(path)!;
        string temp = Path.Combine(folder, Guid.NewGuid().ToString("N") + ".tmp");
        try
        {
            Directory.CreateDirectory(folder);
            File.WriteAllText(temp, JsonSerializer.Serialize(value));
            File.Move(temp, path, true);
        }
        finally { File.Delete(temp); }
    }

    string PresetsPath => Path.Combine(ProfileDirectory, selected!.Pid + ".presets.json");
    int PresetIndex => PresetList.SelectedIndex - 1;

    void LoadPresets()
    {
        presets = persistProfile ? ReadJson<List<NamedPreset>>(PresetsPath) ?? [] : [];
        workingSettings = Read();
        ShowPresets(-1);
    }

    void ShowPresets(int index)
    {
        selectingPreset = true;
        PresetList.ItemsSource = new[] { new NamedPreset("当前设置", workingSettings ?? Read()) }.Concat(presets).ToArray();
        PresetList.SelectedIndex = index + 1;
        selectingPreset = false;
        SavePresetButton.IsEnabled = RenamePresetButton.IsEnabled = DeletePresetButton.IsEnabled = index >= 0;
    }

    void PresetChanged(object sender, SelectionChangedEventArgs args)
    {
        if (selectingPreset || selected is null) return;
        int index = PresetIndex;
        SavePresetButton.IsEnabled = RenamePresetButton.IsEnabled = DeletePresetButton.IsEnabled = index >= 0;
        if (PresetList.SelectedItem is not NamedPreset preset) return;
        if (args.RemovedItems.Count > 0 && args.RemovedItems[0] is NamedPreset { Name: "当前设置" })
            workingSettings = Read();
        Apply(index < 0 ? workingSettings ?? preset.Settings : preset.Settings);
        RefreshControls();
        WriteSettings();
        if (running && !stopping) RefreshRunningStatus();
        SaveProfile();
    }

    void ChangePresets(List<NamedPreset> updated, int index)
    {
        try { StoreJson(PresetsPath, updated); }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        {
            MessageBox.Show(this, "保存预设失败。", "预设");
            return;
        }
        presets = updated;
        ShowPresets(index);
        SaveProfile();
    }

    void SavePreset(object sender, RoutedEventArgs args)
    {
        int index = PresetIndex;
        if (index < 0) return;
        var updated = presets.ToList();
        updated[index] = updated[index] with { Settings = Read() };
        ChangePresets(updated, index);
    }

    void AddPreset(object sender, RoutedEventArgs args)
    {
        var name = AskPresetName("另存为", "", -1);
        if (name is not null) ChangePresets([.. presets, new(name, Read())], presets.Count);
    }

    void RenamePreset(object sender, RoutedEventArgs args)
    {
        int index = PresetIndex;
        if (index < 0) return;
        var name = AskPresetName("重命名", presets[index].Name, index);
        if (name is null) return;
        var updated = presets.ToList();
        updated[index] = updated[index] with { Name = name };
        ChangePresets(updated, index);
    }

    void DeletePreset(object sender, RoutedEventArgs args)
    {
        int index = PresetIndex;
        if (index < 0) return;
        var dialog = CreatePresetDialog("删除预设", "删除", "DangerAction", out var panel, out var confirm);
        panel.Children.Insert(0, new TextBlock { Text = $"删除「{presets[index].Name}」？", TextWrapping = TextWrapping.Wrap });
        confirm.Click += (_, _) => dialog.DialogResult = true;
        if (dialog.ShowDialog() != true) return;
        var updated = presets.ToList();
        updated.RemoveAt(index);
        workingSettings = Read();
        ChangePresets(updated, -1);
    }

    string? AskPresetName(string title, string initial, int except)
    {
        var dialog = CreatePresetDialog(title, "确定", "Action", out var panel, out var ok);
        var input = new TextBox { Text = initial, MaxLength = 40, Margin = new Thickness(0, 8, 0, 0),
            Style = (Style)Resources["PresetInput"] };
        System.Windows.Automation.AutomationProperties.SetName(input, "预设名称");
        var status = new TextBlock { Margin = new Thickness(0, 7, 0, 0), Visibility = Visibility.Collapsed };
        status.SetResourceReference(TextBlock.ForegroundProperty, "Danger");
        panel.Children.Insert(0, new TextBlock { Text = "预设名称" });
        panel.Children.Insert(1, input);
        panel.Children.Insert(2, status);
        dialog.Loaded += (_, _) => { input.Focus(); input.SelectAll(); };
        ok.Click += (_, _) =>
        {
            string name = input.Text.Trim();
            bool duplicate = name == "当前设置" || presets.Where((_, i) => i != except)
                .Any(p => p.Name.Equals(name, StringComparison.OrdinalIgnoreCase));
            if (name.Length == 0 || duplicate)
            {
                status.Text = duplicate ? "这个名称已存在。" : "请输入名称。";
                status.Visibility = Visibility.Visible;
                return;
            }
            dialog.DialogResult = true;
        };
        return dialog.ShowDialog() == true ? input.Text.Trim() : null;
    }

    Window CreatePresetDialog(string title, string action, string style, out StackPanel panel, out Button confirm)
    {
        var dialog = new Window
        {
            Owner = this, Title = title, Width = 350, SizeToContent = SizeToContent.Height,
            WindowStartupLocation = WindowStartupLocation.CenterOwner, ResizeMode = ResizeMode.NoResize,
            ShowInTaskbar = false
        };
        dialog.SetResourceReference(BackgroundProperty, "Surface");
        dialog.SetResourceReference(ForegroundProperty, "Text");
        panel = new StackPanel();
        var buttons = new StackPanel { Orientation = System.Windows.Controls.Orientation.Horizontal,
            HorizontalAlignment = System.Windows.HorizontalAlignment.Right, Margin = new Thickness(0, 16, 0, 0) };
        var cancel = new Button { Content = "取消", IsCancel = true, Style = (Style)Resources["SecondaryAction"] };
        confirm = new Button { Content = action, IsDefault = style != "DangerAction",
            Margin = new Thickness(8, 0, 0, 0), Style = (Style)Resources[style] };
        buttons.Children.Add(cancel);
        buttons.Children.Add(confirm);
        panel.Children.Add(buttons);
        dialog.Content = new Border { Margin = new Thickness(12), Child = panel, Style = (Style)Resources["Card"] };
        return dialog;
    }
}

internal static class Palette
{
    static readonly string[] Keys = ["Surface", "Panel", "Inset", "Footer", "Hero", "Hover", "Selected",
        "Border", "Track", "Accent", "Muted", "Knob", "Text", "OnAccent"];

    public static Brush Brush(string key) => (Brush)System.Windows.Application.Current.Resources[key];

    public static void Apply(string theme)
    {
        string colors = theme switch
        {
            "dark" => "0C1217 141E26 0E181F 101A21 1B3837 1B2C33 284D47 293942 34434D 6FE0BF A7B8BE E2FFF5 EAF3F3 0D2622",
            "light" => "F1F4F8 FFFFFF E8EDF5 F7F9FC E5EEF9 E2EAF4 DCEAFE CBD6E3 B1C0D2 285CA6 4B5C70 FFFFFF 192C43 FFFFFF",
            _ => "F6F4F4 FFFFFF F4EDEE FFFFFF FAE5E8 F4E3E6 F9DDE3 E0D2D4 C7AFB3 B81538 66575B FFFFFF 262126 FFFFFF"
        };
        var values = colors.Split(' ');
        var resources = System.Windows.Application.Current.Resources;
        for (int i = 0; i < Keys.Length; i++)
        {
            var color = (Color)System.Windows.Media.ColorConverter.ConvertFromString("#" + values[i])!;
            resources[Keys[i]] = new SolidColorBrush(color);
        }
        resources["Danger"] = new SolidColorBrush(theme == "dark" ? Color.FromRgb(255, 149, 159) : Color.FromRgb(166, 26, 49));
        resources["DangerFill"] = new SolidColorBrush(Color.FromRgb(166, 26, 49));
    }
}
