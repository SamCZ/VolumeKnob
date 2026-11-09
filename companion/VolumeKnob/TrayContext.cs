using System.Diagnostics;
using System.Drawing;
using System.Drawing.Drawing2D;

namespace VolumeKnob;

/// <summary>Notification-area icon with status and a small menu; the work happens in <see cref="Engine"/>.</summary>
sealed class TrayContext : ApplicationContext
{
    readonly Engine engine;
    readonly NotifyIcon tray;
    readonly ToolStripMenuItem statusItem;
    readonly ToolStripMenuItem startupItem;
    readonly Icon iconConnected = MakeIcon(Color.FromArgb(45, 156, 255));
    readonly Icon iconDisconnected = MakeIcon(Color.FromArgb(142, 142, 147));
    readonly SynchronizationContext ui;

    public TrayContext(string? forcedPort)
    {
        ui = SynchronizationContext.Current ?? new WindowsFormsSynchronizationContext();

        statusItem = new ToolStripMenuItem("Starting...") { Enabled = false };
        startupItem = new ToolStripMenuItem("Start with Windows", null, (_, _) => ToggleStartup())
        {
            Checked = Settings.StartWithWindows,
        };
        var menu = new ContextMenuStrip();
        menu.Items.Add(statusItem);
        menu.Items.Add(new ToolStripSeparator());
        menu.Items.Add(startupItem);
        menu.Items.Add("Forget paired knob", null, (_, _) => engine!.Unpair());
        menu.Items.Add("Dump knob state to log", null, (_, _) => engine!.RequestDump());
        menu.Items.Add("Open log", null, (_, _) => OpenLog());
        menu.Items.Add(new ToolStripSeparator());
        menu.Items.Add("Exit", null, (_, _) => ExitThread());

        tray = new NotifyIcon
        {
            Icon = iconDisconnected,
            Text = "Volume Knob",
            ContextMenuStrip = menu,
            Visible = true,
        };

        Log.Write("companion started");
        engine = new Engine(Settings.Load(), forcedPort);
        engine.StatusChanged += () => ui.Post(_ => UpdateStatus(), null);
        UpdateStatus();
    }

    void UpdateStatus()
    {
        statusItem.Text = engine.Status;
        tray.Icon = engine.Connected ? iconConnected : iconDisconnected;
        string text = $"Volume Knob - {engine.Status}";
        tray.Text = text.Length > 63 ? text[..63] : text;
    }

    void ToggleStartup()
    {
        try
        {
            Settings.StartWithWindows = !Settings.StartWithWindows;
            startupItem.Checked = Settings.StartWithWindows;
        }
        catch (Exception e)
        {
            Log.Write($"startup toggle failed: {e.Message}");
        }
    }

    static void OpenLog()
    {
        try { Process.Start(new ProcessStartInfo(Log.FilePath) { UseShellExecute = true }); }
        catch (Exception e) { Log.Write($"open log failed: {e.Message}"); }
    }

    static Icon MakeIcon(Color accent)
    {
        using var bmp = new Bitmap(32, 32);
        using (var g = Graphics.FromImage(bmp))
        {
            g.SmoothingMode = SmoothingMode.AntiAlias;
            g.Clear(Color.Transparent);
            using var ring = new Pen(accent, 4f);
            g.DrawArc(ring, 4, 4, 24, 24, 135, 270);
            using var dot = new SolidBrush(Color.White);
            g.FillEllipse(dot, 12, 12, 8, 8);
        }
        return Icon.FromHandle(bmp.GetHicon());
    }

    protected override void ExitThreadCore()
    {
        tray.Visible = false;
        engine.Dispose();
        tray.Dispose();
        Log.Write("companion exited");
        base.ExitThreadCore();
    }
}
