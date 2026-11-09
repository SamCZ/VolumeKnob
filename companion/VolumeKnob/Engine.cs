using System.Globalization;

namespace VolumeKnob;

/// <summary>
/// Owns the audio backend and the device link on one MTA thread. Polls the mixer, sends the
/// knob only what changed, applies the knob's commands, and serves icon requests one at a time.
/// </summary>
sealed class Engine : IDisposable
{
    const int PollConnectedMs = 80;
    const int PollIdleMs = 1000;
    const int ReconnectMs = 3000;
    const int PingMs = 1000;
    const int IconAckTimeoutMs = 3000;

    readonly AutoResetEvent wake = new(false);
    readonly DeviceLink link;
    readonly Thread thread;
    volatile bool stopping;

    AudioBackend? audio;
    MixerSnapshot? snapshot;
    bool needSync;

    // What the knob currently believes; diffs are computed against this.
    (int Volume, bool Muted, bool Fixed, string Output)? sentMaster;
    Dictionary<int, AppEntry> sentApps = new();

    // Icon requests are paced: the next icon goes out only after the knob acked the previous one.
    readonly Queue<uint> iconQueue = new();
    uint? iconInFlight;
    long iconSentMs;

    public event Action? StatusChanged;
    public string Status { get; private set; } = "Starting...";
    public bool Connected => link.IsConnected;

    public Engine(Settings settings, string? forcedPort)
    {
        link = new DeviceLink(wake, settings, forcedPort);
        thread = new Thread(Run) { IsBackground = true, Name = "knob-engine" };
        thread.SetApartmentState(ApartmentState.MTA);
        thread.Start();
    }

    void SetStatus(string status)
    {
        if (status == Status) return;
        Status = status;
        Log.Write($"status: {status}");
        StatusChanged?.Invoke();
    }

    void Run()
    {
        try
        {
            audio = new AudioBackend();
        }
        catch (Exception e)
        {
            Log.Write($"audio init failed: {e}");
            SetStatus("Audio unavailable");
            return;
        }

        long nextPoll = 0, nextConnect = 0;
        bool wasConnected = false;

        while (!stopping)
        {
            long now = Environment.TickCount64;

            if (!link.IsConnected)
            {
                if (wasConnected)
                {
                    wasConnected = false;
                    ResetDeviceState();
                }
                if (now >= nextConnect)
                {
                    if (link.TryConnect())
                    {
                        wasConnected = true;
                        needSync = true;
                        SetStatus($"Connected to {link.PairedSerial ?? "knob"} ({link.PortName})");
                    }
                    else
                    {
                        SetStatus(link.PairedSerial is { } serial
                            ? $"Waiting for knob {serial}"
                            : "Pairing: listening for a knob...");
                    }
                    nextConnect = Environment.TickCount64 + ReconnectMs;
                }
            }

            while (link.TryDequeue(out string line)) HandleLine(line);

            now = Environment.TickCount64;
            if (now >= nextPoll)
            {
                try
                {
                    snapshot = audio.Poll();
                }
                catch (Exception e)
                {
                    Log.Write($"poll failed: {e.Message}");
                }
                nextPoll = now + (link.IsConnected ? PollConnectedMs : PollIdleMs);

                if (link.IsConnected && snapshot != null)
                {
                    if (needSync) SendFullSync(snapshot);
                    else SendDiff(snapshot);
                }
            }

            if (link.IsConnected)
            {
                PumpIcons();
                if (pendingDump)
                {
                    pendingDump = false;
                    link.Send("D");
                }
                if (Environment.TickCount64 - link.LastSendMs >= PingMs) link.Send("P");
            }

            wake.WaitOne(15);
        }

        link.Send("X");
        link.Dispose();
        audio.Dispose();
    }

    void ResetDeviceState()
    {
        sentMaster = null;
        sentApps = new Dictionary<int, AppEntry>();
        iconQueue.Clear();
        iconInFlight = null;
    }

    void HandleLine(string line)
    {
        string[] f = line.Split(' ');
        try
        {
            switch (f[0])
            {
                case "!HELLO":
                case "!VOLKNOB":
                    needSync = true; // knob rebooted or re-handshook
                    break;
                case "g":
                {
                    int vol = int.Parse(f[1], CultureInfo.InvariantCulture);
                    bool mute = f[2] == "1";
                    Log.Write($"knob: master {vol}{(mute ? " muted" : "")}");
                    audio!.SetMaster(vol, mute);
                    if (sentMaster is { } m) sentMaster = (vol, mute, m.Fixed, m.Output);
                    break;
                }
                case "a":
                {
                    int id = int.Parse(f[1], CultureInfo.InvariantCulture);
                    int vol = int.Parse(f[2], CultureInfo.InvariantCulture);
                    bool mute = f[3] == "1";
                    Log.Write($"knob: app {id} {vol}{(mute ? " muted" : "")}");
                    audio!.SetApp(id, vol, mute);
                    if (sentApps.TryGetValue(id, out var app)) sentApps[id] = app with { Volume = vol, Muted = mute };
                    break;
                }
                case "q":
                {
                    uint key = uint.Parse(f[1], NumberStyles.HexNumber, CultureInfo.InvariantCulture);
                    if (!iconQueue.Contains(key) && iconInFlight != key) iconQueue.Enqueue(key);
                    break;
                }
                case "k":
                    iconInFlight = null;
                    break;
                case "r":
                    // Knob ignored updates while the user was turning it: resend all values.
                    if (sentMaster is { } sm) sentMaster = (-1, sm.Muted, sm.Fixed, sm.Output);
                    foreach (var id in sentApps.Keys.ToList()) sentApps[id] = sentApps[id] with { Volume = -1 };
                    break;
                default:
                    if (line.StartsWith('#')) Log.Write($"knob {line}");
                    break;
            }
        }
        catch (Exception e)
        {
            Log.Write($"bad line from knob '{Truncate(line)}': {e.Message}");
        }
    }

    void PumpIcons()
    {
        if (iconInFlight != null)
        {
            if (Environment.TickCount64 - iconSentMs < IconAckTimeoutMs) return;
            Log.Write($"icon {iconInFlight:x8} not acked, moving on");
            iconInFlight = null;
        }
        while (iconQueue.Count > 0)
        {
            uint key = iconQueue.Dequeue();
            byte[]? pixels = audio!.GetIcon(key);
            if (pixels == null) continue;
            link.Send($"I {key:x8} {IconExtractor.Size} {IconExtractor.Size} {Convert.ToBase64String(pixels)}");
            iconInFlight = key;
            iconSentMs = Environment.TickCount64;
            Log.Write($"sent icon {key:x8}");
            return;
        }
    }

    void SendFullSync(MixerSnapshot snap)
    {
        needSync = false;
        iconQueue.Clear();
        iconInFlight = null;
        link.Send("S");
        link.Send($"O {snap.OutputName}");
        link.Send(MasterLine(snap));
        foreach (var app in snap.Apps) link.Send(AddLine(app));
        link.Send("E");
        sentMaster = (snap.MasterVolume, snap.MasterMuted, snap.MasterFixed, snap.OutputName);
        sentApps = snap.Apps.ToDictionary(a => a.Id);
        Log.Write($"full sync: master {snap.MasterVolume}{(snap.MasterMuted ? " muted" : "")}, {snap.Apps.Count} apps: "
                  + string.Join(", ", snap.Apps.Select(a => $"{a.Id}:{a.Name}={a.Volume}")));
    }

    void SendDiff(MixerSnapshot snap)
    {
        if (sentMaster is not { } m || m.Output != snap.OutputName)
            link.Send($"O {snap.OutputName}");
        if (sentMaster is not { } m2 || m2.Volume != snap.MasterVolume || m2.Muted != snap.MasterMuted || m2.Fixed != snap.MasterFixed)
            link.Send(MasterLine(snap));
        sentMaster = (snap.MasterVolume, snap.MasterMuted, snap.MasterFixed, snap.OutputName);

        var current = snap.Apps.ToDictionary(a => a.Id);
        foreach (var id in sentApps.Keys)
        {
            if (current.ContainsKey(id)) continue;
            link.Send($"R {id}");
            Log.Write($"removed app {id} ({sentApps[id].Name})");
        }
        foreach (var app in snap.Apps)
        {
            if (!sentApps.TryGetValue(app.Id, out var old))
            {
                link.Send(AddLine(app));
                Log.Write($"added app {app.Id} ({app.Name}) icon {app.IconKey:x8}");
            }
            else if (old.Name != app.Name || old.IconKey != app.IconKey)
                link.Send(AddLine(app));
            else if (old.Volume != app.Volume || old.Muted != app.Muted)
                link.Send($"V {app.Id} {app.Volume} {B(app.Muted)}");
        }
        sentApps = current;
    }

    static string MasterLine(MixerSnapshot s) => $"G {s.MasterVolume} {B(s.MasterMuted)} {B(s.MasterFixed)}";
    static string AddLine(AppEntry a) => $"A {a.Id} {a.IconKey:x8} {a.Volume} {B(a.Muted)} {a.Name}";
    static int B(bool v) => v ? 1 : 0;
    static string Truncate(string s) => s.Length > 80 ? s[..80] + "..." : s;

    /// <summary>Ask the knob for its state dump; the answer lands in the log.</summary>
    /// <summary>Forget the paired knob and pair with the next one that announces itself.</summary>
    public void Unpair() => link.Unpair();

    public void RequestDump()
    {
        // Send from the engine thread to keep writes single-threaded.
        pendingDump = true;
        wake.Set();
    }
    volatile bool pendingDump;

    public void Dispose()
    {
        stopping = true;
        wake.Set();
        thread.Join(3000);
    }
}
