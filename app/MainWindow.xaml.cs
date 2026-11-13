using Application = System.Windows.Application;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.IO.MemoryMappedFiles;
using System.Runtime.InteropServices;
using System.Text.Json;
using System.Windows;
using System.Windows.Interop;
using Forms = System.Windows.Forms;

namespace KP70Studio;

public partial class MainWindow : Window
{
    record Profile(bool Shoulder = true, int ShoulderGain = 65, bool Grip = false,
        bool AudioGrip = false, int GripGain = 50, bool Trigger = false,
        int Base = 9, int Kick = 30, int Start = 12, int Frequency = 20,
        int Sensitivity = 100, int Suppression = 100, bool ShoulderPress = false,
        int? ShoulderSensitivity = null, int ShoulderThreshold = 2,
        int? GripSensitivity = null, int GripThreshold = 5,
        int? TriggerSensitivity = null, int TriggerThreshold = 20, bool TriggerPress = false,
        ChannelTuning?[]? Channels = null, string CloseAction = "ask");

    const int ChannelCount = 6, StudioSettingsSize = 89 * sizeof(int);
    readonly ChannelTuning?[] channels = new ChannelTuning?[ChannelCount];

    static readonly string ProfileDirectory = Path.Combine(Environment.GetFolderPath(
        Environment.SpecialFolder.LocalApplicationData), "KP70Studio");
    Process? worker;
    EventWaitHandle? stop;
    MemoryMappedFile? map;
    MemoryMappedViewAccessor? settings;
    Mutex? settingsLock;
    bool running, stopping, closing, padBound, applying;
    readonly bool persistProfile;
    string? error;

    [DllImport("dwmapi.dll")]
    static extern int DwmSetWindowAttribute(IntPtr window, int attribute, ref int value, int size);

    public MainWindow(bool persistProfile)
    {
        this.persistProfile = persistProfile;
        InitializeComponent();
        LoadAppOptions();
        if (persistProfile) Loaded += async (_, _) => await ScanDevices();
        SourceInitialized += (_, _) =>
        {
            int dark = 1;
            DwmSetWindowAttribute(new WindowInteropHelper(this).Handle, 20, ref dark, sizeof(int));
        };
        Closing += OnClosing;
        Closed += (_, _) => { tray?.Dispose(); tray = null; };
        if (persistProfile) SetupTray();
    }

    static string ProfilePath(Device device) => Path.Combine(ProfileDirectory, device.Pid + ".json");

    static Profile LoadProfile(Device device)
    {
        var fallback = new Profile(Grip: !device.Direct);
        try
        {
            string path = ProfilePath(device);
            if (device.Direct && !File.Exists(path)) path = Path.Combine(ProfileDirectory, "profile.json");
            return JsonSerializer.Deserialize<Profile>(File.ReadAllText(path)) ?? fallback;
        }
        catch (IOException) { return fallback; }
        catch (JsonException) { return fallback; }
        catch (UnauthorizedAccessException) { return fallback; }
    }

    void SaveProfile()
    {
        if (!persistProfile || selected is not Device device) return;
        try
        {
            Directory.CreateDirectory(ProfileDirectory);
            File.WriteAllText(ProfilePath(device), JsonSerializer.Serialize(Read()));
        }
        catch (IOException) { }
        catch (UnauthorizedAccessException) { }
    }

    void Apply(Profile p)
    {
        applying = true;
        Array.Clear(channels);
        if (p.Channels is { Length: ChannelCount }) p.Channels.CopyTo(channels, 0);
        ShoulderOn.IsChecked = p.Shoulder;
        ShoulderGain.Value = p.ShoulderGain;
        ShoulderPress.IsChecked = p.ShoulderPress;
        GripOn.IsChecked = p.Grip;
        GripAudio.IsChecked = p.AudioGrip;
        GripNative.IsChecked = !p.AudioGrip;
        GripGain.Value = p.GripGain;
        TriggerOn.IsChecked = p.Trigger;
        TriggerPress.IsChecked = p.TriggerPress;
        TriggerBase.Value = p.Base;
        TriggerKick.Value = p.Kick;
        TriggerStart.Value = p.Start;
        TriggerFrequency.Value = p.Frequency;
        ShoulderSensitivity.Value = p.ShoulderSensitivity ?? p.Sensitivity;
        ShoulderThreshold.Value = p.ShoulderThreshold;
        GripSensitivity.Value = p.GripSensitivity ?? p.Sensitivity;
        GripThreshold.Value = p.GripThreshold;
        TriggerSensitivity.Value = p.TriggerSensitivity ?? p.Sensitivity;
        TriggerThreshold.Value = p.TriggerThreshold;
        Suppression.Value = p.Suppression;
        applying = false;
    }

    Profile Read() => new(ShoulderOn.IsChecked == true, (int)ShoulderGain.Value,
        GripOn.IsChecked == true, GripAudio.IsChecked == true, (int)GripGain.Value,
        TriggerOn.IsChecked == true, (int)TriggerBase.Value, (int)TriggerKick.Value,
        (int)TriggerStart.Value, (int)TriggerFrequency.Value, 100, (int)Suppression.Value,
        ShoulderPress.IsChecked == true, (int)ShoulderSensitivity.Value, (int)ShoulderThreshold.Value,
        (int)GripSensitivity.Value, (int)GripThreshold.Value, (int)TriggerSensitivity.Value, (int)TriggerThreshold.Value,
        TriggerPress.IsChecked == true, (ChannelTuning?[])channels.Clone(), closeAction);

    static ChannelTuning CommonTuning(Profile p, int index) => (index / 2) switch
    {
        0 => new(p.ShoulderGain, p.ShoulderSensitivity ?? p.Sensitivity, p.ShoulderThreshold),
        1 => new(p.GripGain, p.GripSensitivity ?? p.Sensitivity, p.GripThreshold),
        _ => new(p.Kick, p.TriggerSensitivity ?? p.Sensitivity, p.TriggerThreshold,
            Base: p.Base, Kick: p.Kick, Start: p.Start, Frequency: p.Frequency)
    };

    int[] AvailableChannels() => Enumerable.Range(0, ChannelCount).Where(index => (index / 2) switch
    {
        0 => selected?.Shoulders == true,
        1 => true,
        _ => selected?.Direct == true
    }).ToArray();

    internal ChannelTuningWindow CreateTuningWindow()
    {
        var common = Enumerable.Range(0, ChannelCount).Select(i => CommonTuning(Read(), i)).ToArray();
        return new ChannelTuningWindow(Resources, AvailableChannels(), common, channels, selected?.Linear == true, () =>
        {
            WriteSettings();
        });
    }

    void EditChannels(object sender, RoutedEventArgs args)
    {
        if (selected is null) return;
        var dialog = CreateTuningWindow();
        dialog.Owner = this;
        dialog.ShowDialog();
        SaveProfile();
    }

    void SettingsChanged(object sender, RoutedEventArgs args)
    {
        if (!IsLoaded || applying) return;
        RefreshControls();
        WriteSettings();
        if (running && !stopping && error is null) RefreshRunningStatus();
    }

    void RefreshControls()
    {
        var p = Read();
        ShoulderGain.IsEnabled = p.Shoulder;
        ShoulderSensitivity.IsEnabled = ShoulderThreshold.IsEnabled = p.Shoulder;
        ShoulderPress.IsEnabled = p.Shoulder;
        GripGain.IsEnabled = GripSensitivity.IsEnabled = GripThreshold.IsEnabled = p.Grip && p.AudioGrip;
        bool triggerOn = p.Trigger;
        TriggerPress.IsEnabled = triggerOn;
        TriggerBase.IsEnabled = TriggerKick.IsEnabled = TriggerStart.IsEnabled = TriggerFrequency.IsEnabled = TriggerSensitivity.IsEnabled = TriggerThreshold.IsEnabled = triggerOn;
        GripTuning.Opacity = p.Grip && p.AudioGrip ? 1 : 0.35;
        GripHint.Text = p.Grip && p.AudioGrip
            ? "音频模式建议关掉游戏内的握把震动。" : "原生模式由游戏控制握把震动。";
    }

    void WriteSettings()
    {
        if (settings is null || settingsLock is null) return;
        var p = Read();
        bool direct = selected?.Direct == true;
        var values = new List<int>(StudioSettingsSize / sizeof(int))
        {
            selected?.Shoulders == true && p.Shoulder ? 1 : 0, p.Grip && p.AudioGrip ? 1 : 0,
            direct && p.Trigger ? 1 : 0, p.Base, p.Start, p.Frequency, p.Suppression,
            p.ShoulderPress ? 1 : 0, p.TriggerPress ? 1 : 0
        };
        for (int i = 0; i < ChannelCount; i++)
        {
            var tuning = channels[i] ?? CommonTuning(p, i);
            var curve = tuning.Curve;
            bool custom = curve is { Length: FrequencyCurve.BandCount };
            values.Add(tuning.Gain);
            values.Add(tuning.Sensitivity);
            values.Add(tuning.Threshold);
            values.Add(custom ? 1 : 0);
            for (int band = 0; band < FrequencyCurve.BandCount; band++)
                values.Add(custom ? curve![band] : 0);
        }
        for (int i = 0; i < ChannelCount; i++)
            values.Add(i < 2 ? (p.ShoulderPress ? 1 : 0) : (i >= 4 && p.TriggerPress ? 1 : 0));
        for (int side = 0; side < 2; side++)
            values.Add((channels[4 + side] ?? CommonTuning(p, 4 + side)).Base);
        for (int side = 0; side < 2; side++)
            values.Add((channels[4 + side] ?? CommonTuning(p, 4 + side)).Kick);
        for (int side = 0; side < 2; side++)
            values.Add((channels[4 + side] ?? CommonTuning(p, 4 + side)).Start);
        for (int side = 0; side < 2; side++)
            values.Add((channels[4 + side] ?? CommonTuning(p, 4 + side)).Frequency);
        try { if (!settingsLock.WaitOne(40)) return; }
        catch (AbandonedMutexException) { }
        try { settings.WriteArray(0, values.ToArray(), 0, values.Count); }
        finally { settingsLock.ReleaseMutex(); }
    }

    void ToggleRun(object sender, RoutedEventArgs args)
    {
        if (worker is not null) { StopWorker(); return; }
        if (selected is not Device device) return;
        try
        {
            string id = "KP70Studio_" + Guid.NewGuid().ToString("N");
            stop = new EventWaitHandle(false, EventResetMode.ManualReset, id + "_stop");
            map = MemoryMappedFile.CreateNew(id, StudioSettingsSize);
            settings = map.CreateViewAccessor();
            settingsLock = new Mutex(false, id + "_lock");
            WriteSettings();
            var info = EngineStartInfo();
            info.ArgumentList.Add("--studio");
            info.ArgumentList.Add(id);
            info.ArgumentList.Add(Environment.ProcessId.ToString());
            info.ArgumentList.Add(device.Pid);
            info.ArgumentList.Add(device.Path);
            var process = new Process { StartInfo = info, EnableRaisingEvents = true };
            worker = process;
            error = null;
            padBound = false;
            process.OutputDataReceived += (_, e) =>
            {
                if (e.Data is string line) Dispatcher.BeginInvoke(() => Receive(process, line));
            };
            process.ErrorDataReceived += (_, e) =>
            {
                if (e.Data is string line) Dispatcher.BeginInvoke(() => { if (worker == process) error = line; });
            };
            process.Exited += (_, _) => Task.Run(() =>
            {
                process.WaitForExit();
                Dispatcher.BeginInvoke(() => Finish(process));
            });
            process.Start();
            process.BeginOutputReadLine();
            process.BeginErrorReadLine();
            StatusPill.Text = "连接中";
            StatusLine.Text = "正在打开 Windows 音频与手柄输出…";
            RunButton.Content = "停止";
            BackButton.IsEnabled = false;
        }
        catch (Exception ex)
        {
            StatusPill.Text = "启动失败";
            StatusLine.Text = ex.Message;
            ReleaseSession();
        }
    }

    void StopWorker()
    {
        if (stopping) return;
        stopping = true;
        stop?.Set();
        RunButton.IsEnabled = false;
        StatusPill.Text = "停止中";
        StatusLine.Text = "正在清除本次运行产生的震动与阻力…";
    }

    void Receive(Process process, string line)
    {
        if (worker != process || stopping) return;
        if (line == "READY")
        {
            running = true;
            RefreshRunningStatus();
            return;
        }
        var parts = line.Split(' ');
        if (parts.Length != 11 || parts[0] != "STUDIO") return;
        var values = new int[10];
        for (int i = 0; i < values.Length; ++i)
            if (!int.TryParse(parts[i + 1], out values[i])) return;
        padBound = values[0] != 0;
        InputMeter.Value = values[1];
        InputValue.Text = $"{values[1]}%";
        ShoulderL.Value = values[2]; ShoulderR.Value = values[3];
        GripL.Value = values[4]; GripR.Value = values[5];
        TriggerL.Value = values[6]; TriggerR.Value = values[7];
        TriggerStatus.Text = $"左 {TriggerMode(values[8])}  ·  右 {TriggerMode(values[9])}";
        RefreshRunningStatus();
    }

    static string TriggerMode(int mode) => mode switch
    {
        1 => "基础阻力",
        2 => "反冲震动",
        3 => "音频振动",
        _ => "待机"
    };

    void RefreshRunningStatus()
    {
        var p = Read();
        bool needsBinding = p.Grip && p.AudioGrip || selected is { Direct: true } device && p.Trigger && (!device.Linear || p.TriggerPress) ||
            selected?.Shoulders == true && p.Shoulder && p.ShoulderPress;
        StatusPill.Text = needsBinding && !padBound ? "等待绑定" : "运行中";
        StatusLine.Text = needsBinding && !padBound
            ? "正在识别手柄。若未自动绑定，可同时按住鲲鹏的 LB + RB。"
            : p.Grip && p.AudioGrip ? "音频握把已启用。关闭游戏内震动可避免输出冲突。"
            : "采集 Windows 默认播放设备。握把原生震动由游戏控制。";
    }

    void Finish(Process process)
    {
        if (worker != process) return;
        bool failed = process.ExitCode != 0;
        StatusPill.Text = failed ? "已中断" : "已停止";
        StatusLine.Text = failed ? OutputError(error) : "已停止音频触觉。";
        ReleaseSession();
        if (closing) Close();
    }

    static string OutputError(string? message)
    {
        var fields = message?.Split(' ');
        if (fields is not { Length: 6 } || fields[0] != "CONTROL")
            return message ?? "输出已中断。请检查手柄连接后重试。";

        return fields[1] switch
        {
            "open" when fields[2] is "5" or "32" or "33" =>
                $"控制接口被占用或拒绝访问（Windows {fields[2]}）。请从托盘退出北通智控后重试。",
            "open" =>
                $"未打开控制接口（Windows {fields[2]}）。请重连手柄或接收器，返回设备页刷新。",
            "metadata" => $"读取控制接口信息失败（代码 {fields[2]}）。",
            "interface" => $"控制接口不匹配（HID {fields[3]}:{fields[4]}，输出 {fields[5]} 字节）。",
            "length" => $"控制接口的输出报告只有 {fields[5]} 字节，当前协议需要至少 64 字节。",
            _ => message!
        };
    }

    void ReleaseSession()
    {
        worker?.Dispose(); worker = null;
        settings?.Dispose(); settings = null;
        map?.Dispose(); map = null;
        settingsLock?.Dispose(); settingsLock = null;
        stop?.Dispose(); stop = null;
        running = stopping = false;
        RunButton.Content = "启动音频震动";
        RunButton.IsEnabled = true;
        BackButton.IsEnabled = true;
        InputValue.Text = "0%";
        TriggerStatus.Text = "左 待机  ·  右 待机";
        InputMeter.Value = ShoulderL.Value = ShoulderR.Value = GripL.Value = GripR.Value = TriggerL.Value = TriggerR.Value = 0;
    }

    Forms.NotifyIcon? tray;
    string closeAction = "ask";
    bool forceExit;

    Forms.ToolStripMenuItem? behaviorAsk, behaviorTray, behaviorExit;

    void SetupTray()
    {
        var stream = Application.GetResourceStream(new Uri("tray.ico", UriKind.Relative))?.Stream;
        tray = new Forms.NotifyIcon
        {
            Icon = stream is null ? System.Drawing.SystemIcons.Application : new System.Drawing.Icon(stream),
            Visible = true,
            Text = "Audio Rumble"
        };
        var menu = new Forms.ContextMenuStrip();
        menu.Items.Add("显示", null, (_, _) => RestoreFromTray());
        var behavior = new Forms.ToolStripMenuItem("关闭按钮行为");
        behaviorAsk = new Forms.ToolStripMenuItem("每次询问") { CheckOnClick = false };
        behaviorTray = new Forms.ToolStripMenuItem("最小化到托盘") { CheckOnClick = false };
        behaviorExit = new Forms.ToolStripMenuItem("直接退出") { CheckOnClick = false };
        behaviorAsk.Click += (_, _) => SetCloseAction("ask");
        behaviorTray.Click += (_, _) => SetCloseAction("tray");
        behaviorExit.Click += (_, _) => SetCloseAction("exit");
        behavior.DropDownItems.Add(behaviorAsk);
        behavior.DropDownItems.Add(behaviorTray);
        behavior.DropDownItems.Add(behaviorExit);
        menu.Items.Add(behavior);
        menu.Items.Add(new Forms.ToolStripSeparator());
        menu.Items.Add("退出", null, (_, _) => { forceExit = true; Close(); });
        tray.ContextMenuStrip = menu;
        tray.DoubleClick += (_, _) => RestoreFromTray();
        SyncBehaviorMenu();
    }

    void SetCloseAction(string value)
    {
        closeAction = value;
        SyncBehaviorMenu();
        SaveAppOptions();
    }

    void SyncBehaviorMenu()
    {
        if (behaviorAsk is null) return;
        behaviorAsk.Checked = closeAction == "ask";
        behaviorTray!.Checked = closeAction == "tray";
        behaviorExit!.Checked = closeAction == "exit";
    }
    void HideToTray()
    {
        Hide();
    }

    void RestoreFromTray()
    {
        Show();
        WindowState = WindowState.Normal;
        Activate();
    }

    (string Action, bool Remember) AskCloseAction()
    {
        var prompt = new ClosePrompt { Owner = this };
        prompt.ShowDialog();
        return (prompt.DialogResult == true ? prompt.Choice : "cancel", prompt.Remember);
    }
    void OnClosing(object? sender, CancelEventArgs args)
    {
        if (!persistProfile) return;
        string action = forceExit || closing ? "exit" : closeAction;
        if (action == "ask")
        {
            var choice = AskCloseAction();
            action = choice.Action;
            if (choice.Remember && action is "exit" or "tray") SetCloseAction(action);
        }
        if (action != "exit")
        {
            args.Cancel = true;
            if (action == "tray") { SaveProfile(); HideToTray(); }
            return;
        }
        if (worker is not null) { args.Cancel = closing = true; StopWorker(); return; }
        SaveProfile();
    }
}
