using Application = System.Windows.Application;
using System.IO;
using System.Security.Cryptography;
using System.Windows;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Threading;

namespace KP70Studio;

public partial class App : Application
{
    public static string? EnginePath { get; private set; }

    static string EnsureEngine()
    {
        using var stream = typeof(App).Assembly.GetManifestResourceStream("KP70HapticsEngine.exe")
            ?? throw new IOException("程序包缺少触觉引擎。");
        var hash = SHA256.HashData(stream);
        stream.Position = 0;
        var dir = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
            "AudioRumble", "engines", Convert.ToHexString(hash));
        var target = Path.Combine(dir, "KP70HapticsEngine.exe");
        Directory.CreateDirectory(dir);
        bool Matches()
        {
            if (!File.Exists(target)) return false;
            using var file = File.Open(target, FileMode.Open, FileAccess.Read, FileShare.Read | FileShare.Delete);
            return SHA256.HashData(file).AsSpan().SequenceEqual(hash);
        }
        if (!Matches())
        {
            var temp = Path.Combine(dir, Guid.NewGuid().ToString("N") + ".tmp");
            try
            {
                using (var file = File.Create(temp)) stream.CopyTo(file);
                try { File.Move(temp, target, overwrite: true); }
                catch (IOException) when (Matches()) { }
            }
            finally { File.Delete(temp); }
        }
        return target;
    }
    protected override void OnStartup(StartupEventArgs e)
    {
        base.OnStartup(e);
        if (e.Args.Length >= 2 && e.Args[0] == "--preview")
        {
            DispatcherUnhandledException += (_, args) =>
            {
                File.WriteAllText(e.Args[1] + ".log", args.Exception.ToString());
                args.Handled = true;
                Shutdown(1);
            };
        }
        Palette.Apply("red");
        bool preview = e.Args.Length is >= 2 and <= 4 && e.Args[0] == "--preview";
        if (!preview)
        {
            try { EnginePath = EnsureEngine(); }
            catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
            {
                System.Windows.MessageBox.Show(ex.Message, "Audio Rumble", MessageBoxButton.OK, MessageBoxImage.Error);
                Shutdown(1);
                return;
            }
        }
        var window = new MainWindow(!preview);
        if (preview)
        {
            string page = e.Args.Length >= 3 ? e.Args[2] : "devices";
            if (e.Args.Length == 4) Palette.Apply(e.Args[3]);
            window.PreviewDevice(page == "tuning" ? "70" : page);
            Window surfaceWindow = page == "tuning" ? window.CreateTuningWindow() : window;
            surfaceWindow.WindowStartupLocation = WindowStartupLocation.Manual;
            surfaceWindow.ShowActivated = false;
            surfaceWindow.ShowInTaskbar = false;
            surfaceWindow.Left = -12000;
            surfaceWindow.Top = 0;
            surfaceWindow.Show();
            Dispatcher.BeginInvoke(DispatcherPriority.ApplicationIdle, new Action(() =>
            {
                if (surfaceWindow.Content is FrameworkElement surface)
                {
                    var bitmap = new RenderTargetBitmap((int)Math.Ceiling(surface.ActualWidth),
                        (int)Math.Ceiling(surface.ActualHeight), 96, 96, PixelFormats.Pbgra32);
                    bitmap.Render(surface);
                    var encoder = new PngBitmapEncoder();
                    encoder.Frames.Add(BitmapFrame.Create(bitmap));
                    using var file = File.Create(e.Args[1]);
                    encoder.Save(file);
                }
                Shutdown();
            }));
            return;
        }
        window.Show();
    }
}
