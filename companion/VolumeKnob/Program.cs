namespace VolumeKnob;

static class Program
{
    /// <summary>
    /// Tray companion for the CrowPanel volume knob.
    /// Usage: VolumeKnob.exe [--port COMx]   (default: auto-detect the knob)
    /// </summary>
    [STAThread]
    static void Main(string[] args)
    {
        using var mutex = new Mutex(true, @"Local\VolumeKnobCompanion", out bool firstInstance);
        if (!firstInstance) return;

        string? port = null;
        for (int i = 0; i < args.Length - 1; i++)
            if (args[i] is "--port" or "-p") port = args[i + 1].ToUpperInvariant();

        ApplicationConfiguration.Initialize();
        Application.Run(new TrayContext(port));
    }
}
