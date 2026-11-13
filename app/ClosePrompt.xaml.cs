using System.Windows;

namespace KP70Studio;

public partial class ClosePrompt : Window
{
    internal ClosePrompt()
    {
        InitializeComponent();
        MinimizeButton.Click += (_, _) => { Choice = "tray"; DialogResult = true; };
        ExitButton.Click += (_, _) => { Choice = "exit"; DialogResult = true; };
    }

    internal string Choice { get; private set; } = "tray";

    internal bool Remember => RememberBox.IsChecked == true;
}
