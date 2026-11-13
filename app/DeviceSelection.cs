using System.ComponentModel;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Windows;

namespace KP70Studio;

public partial class MainWindow
{
    sealed record Device(string Pid, string Transport, string Access, bool Input, string Path, bool Shoulders)
    {
        public string Name => Pid switch
        {
            "5159" => "鲲鹏 20D",
            "5194" or "5195" => "鲲鹏 40E",
            "5173" or "5174" => "鲲鹏 70C",
            "518A" or "518B" => "鲲鹏 70C1",
            "518C" or "518D" => "鲲鹏 70C2",
            "519B" or "519C" => "鲲鹏 70C3",
            "519D" or "519E" => "鲲鹏 70C4",
            "512F" or "5130" => "鲲鹏 70A",
            "516D" or "516E" => "鲲鹏 70A1",
            "515F" or "5160" => "鲲鹏 70B",
            _ => "北通手柄"
        };
        public bool Direct => Access != "grip";
        public bool Linear => Direct && Pid is "5194" or "5195";
        public string Details => $"20BC:{Pid}  ·  " + (Transport switch
        {
            "receiver" => "USB 接收器",
            "bluetooth" => "蓝牙",
            "usb" => "USB",
            _ => "连接方式未识别"
        });
        public string Status => Access == "open" ? "控制接口暂不可写"
            : Access is "metadata" or "interface" or "length" ? "控制接口需要检查"
            : !Input ? "已检测到设备，等待 XInput 输入"
            : Linear ? "握把震动 · 扳机震动"
            : Shoulders ? "肩键震动 · 握把震动 · 扳机力反馈"
            : Direct ? "握把震动 · 扳机力反馈"
            : "握把震动";
    }

    Device? selected;
    bool scanning;

    static ProcessStartInfo EngineStartInfo() => new(App.EnginePath!)
    {
        UseShellExecute = false, CreateNoWindow = true,
        RedirectStandardOutput = true, RedirectStandardError = true
    };

    async Task ScanDevices()
    {
        if (scanning) return;
        scanning = true;
        RefreshButton.IsEnabled = false;
        DeviceMessage.Text = "正在查找北通手柄…";
        try
        {
            var info = EngineStartInfo();
            info.ArgumentList.Add("--devices");
            using var process = Process.Start(info) ?? throw new IOException("设备扫描未能启动。");
            using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(5));
            var output = process.StandardOutput.ReadToEndAsync(timeout.Token);
            var stderr = process.StandardError.ReadToEndAsync(timeout.Token);
            try { await process.WaitForExitAsync(timeout.Token); }
            catch (OperationCanceledException)
            {
                if (!process.HasExited) process.Kill();
                throw new IOException("设备扫描超时，请点击刷新设备重试。");
            }
            if (process.ExitCode != 0) throw new IOException("设备扫描失败，请点击刷新设备重试。" + await stderr);
            var devices = new List<Device>();
            foreach (var line in (await output).Split('\n'))
            {
                var fields = line.TrimEnd('\r').Split('\t');
                if (fields.Length != 7 || fields[0] != "DEVICE" ||
                    !ushort.TryParse(fields[1], NumberStyles.HexNumber, CultureInfo.InvariantCulture, out _)) continue;
                devices.Add(new(fields[1], fields[2], fields[3], fields[4] == "1", fields[5], fields[6] == "1"));
            }
            DeviceList.ItemsSource = devices;
            DeviceMessage.Text = devices.Count == 0
                ? "没有发现北通手柄。接上 USB 或接收器，开启手柄后刷新。"
                : $"检测到 {devices.Count} 台设备。请选择要控制的手柄。";
            if (VendorAppRunning()) DeviceMessage.Text += " 北通智控正在运行，输出异常时可从托盘退出后重试。";
        }
        catch (Exception ex) when (ex is IOException or Win32Exception or InvalidOperationException)
        {
            DeviceList.ItemsSource = null;
            DeviceMessage.Text = ex.Message;
        }
        finally
        {
            scanning = false;
            RefreshButton.IsEnabled = true;
        }
    }

    static bool VendorAppRunning()
    {
        bool running = false;
        foreach (string name in new[] { "aipan70", "newAIPan", "betopgame" })
        {
            foreach (var process in Process.GetProcessesByName(name))
            {
                running = true;
                process.Dispose();
            }
        }
        return running;
    }

    async void RefreshDevices(object sender, RoutedEventArgs args) => await ScanDevices();

    void OpenDevice(object sender, RoutedEventArgs args)
    {
        if (sender is FrameworkElement { DataContext: Device device }) ShowSettings(device);
    }

    void ShowSettings(Device device)
    {
        selected = device;
        DeviceTitle.Text = device.Name;
        ShoulderCard.Visibility = device.Shoulders ? Visibility.Visible : Visibility.Collapsed;
        TriggerCard.Visibility = ResponseCard.Visibility =
            device.Direct ? Visibility.Visible : Visibility.Collapsed;
        ForceBase.Visibility = ForceFrequencyAndStart.Visibility = device.Linear ? Visibility.Collapsed : Visibility.Visible;
        TriggerPressRow.Visibility = device.Linear ? Visibility.Visible : Visibility.Collapsed;
        TriggerStrengthLabel.Text = device.Linear ? "输出强度" : "反冲强度";
        TriggerKick.Maximum = device.Linear ? 100 : 60;
        PanelCards.MaxWidth = device.Direct ? double.PositiveInfinity : 650;
        Apply(persistProfile ? LoadProfile(device) : new Profile(Grip: true, AudioGrip: !device.Direct));
        LoadPresets();
        RefreshControls();
        StatusPill.Text = "待启动";
        StatusLine.Text = device.Details + "  ·  " + device.Status;
        DevicePage.Visibility = Visibility.Collapsed;
        SettingsPage.Visibility = SettingsFooter.Visibility = Visibility.Visible;
        SettingsPage.ScrollToTop();
    }

    async void BackToDevices(object sender, RoutedEventArgs args)
    {
        if (worker is not null) return;
        SaveProfile();
        selected = null;
        SettingsPage.Visibility = SettingsFooter.Visibility = Visibility.Collapsed;
        DevicePage.Visibility = Visibility.Visible;
        await ScanDevices();
    }

    public void PreviewDevice(string mode)
    {
        var devices = new Device[]
        {
            new("5159", "receiver", "grip", true, "-", false),
            new("519C", "receiver", "ready", true, "-", true),
            new("5130", "receiver", "ready", true, "-", false),
            new("5195", "receiver", "ready", true, "-", false)
        };
        if (mode == "20") ShowSettings(devices[0]);
        else if (mode == "70") ShowSettings(devices[1]);
        else if (mode == "70a") ShowSettings(devices[2]);
        else if (mode == "40e") ShowSettings(devices[3]);
        else
        {
            DeviceMessage.Text = $"检测到 {devices.Length} 台设备。请选择要控制的手柄。";
            DeviceList.ItemsSource = devices;
        }
    }
}
