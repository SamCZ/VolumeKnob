using System.Diagnostics;
using System.Globalization;
using System.Runtime.InteropServices;
using System.Text;
using Microsoft.Win32.SafeHandles;
using NAudio.CoreAudioApi;

namespace VolumeKnob;

/// <summary>One volume-mixer entry: all audio sessions of one process (exe), or System Sounds.</summary>
sealed record AppEntry(int Id, string Name, uint IconKey, int Volume, bool Muted);

/// <param name="MasterFixed">The device reports no adjustable volume range (hardware-controlled volume).</param>
sealed record MixerSnapshot(int MasterVolume, bool MasterMuted, bool MasterFixed, string OutputName, IReadOnlyList<AppEntry> Apps);

/// <summary>
/// Reads and writes the default render device's volume mixer through Core Audio (NAudio).
/// Not thread-safe: create and use it on a single MTA thread.
/// </summary>
sealed class AudioBackend : IDisposable
{
    public const uint IconNone = 0;
    public const uint IconSystem = 1;

    readonly MMDeviceEnumerator enumerator = new();
    MMDevice? device;
    string? deviceId;
    string deviceName = "";
    bool deviceFixedVolume;
    long nextDeviceCheckMs;
    AudioSessionManager? sessionManager;

    const int SessionReloadMs = 60_000; // safety pass; new sessions normally arrive by notification
    const int SessionReloadMinMs = 500;

    // The current device's sessions. Kept across polls: see ReloadSessions.
    sealed record CachedSession(AudioSessionControl Control, uint? Pid, string Key, string Name, uint IconKey);
    readonly List<CachedSession> cachedSessions = new();
    volatile bool sessionsDirty = true;
    long lastSessionReloadMs;
    long nextSessionReloadMs;

    // Group key (lower-case exe path, "#system", or "pid:N") -> stable id for the knob.
    readonly Dictionary<string, int> idByKey = new();
    int nextId = 1;

    // Sessions from the last poll, per app id; used to apply volume changes from the knob.
    Dictionary<int, List<AudioSessionControl>> sessionsById = new();

    readonly Dictionary<uint, ProcessInfo> processes = new();
    readonly Dictionary<uint, byte[]?> iconsByKey = new();
    readonly Dictionary<string, (uint Key, byte[]? Pixels)> iconsByPath = new(StringComparer.OrdinalIgnoreCase);

    sealed class ProcessInfo : IDisposable
    {
        public required SafeProcessHandle Handle;
        public string? Path;
        public string Name = "";
        public bool Alive => WaitForSingleObject(Handle, 0) == WaitTimeout;
        public void Dispose() => Handle.Dispose();
    }

    public MixerSnapshot Poll()
    {
        EnsureDevice();
        sessionsById = new Dictionary<int, List<AudioSessionControl>>();

        if (device == null)
            return new MixerSnapshot(0, false, true, "No output device", Array.Empty<AppEntry>());

        var endpoint = device.AudioEndpointVolume;
        int masterVolume = ToPercent(endpoint.MasterVolumeLevelScalar);
        bool masterMuted = endpoint.Mute;

        long now = Environment.TickCount64;
        if (now >= nextSessionReloadMs || (sessionsDirty && now >= lastSessionReloadMs + SessionReloadMinMs))
            ReloadSessions(now);

        var groups = new List<(string Key, string Name, uint IconKey, List<AudioSessionControl> Sessions)>();
        var groupIndex = new Dictionary<string, int>();

        for (int i = cachedSessions.Count - 1; i >= 0; i--)
        {
            var cached = cachedSessions[i];
            if (IsGone(cached))
            {
                cached.Control.Dispose();
                cachedSessions.RemoveAt(i);
            }
        }

        foreach (var cached in cachedSessions)
        {
            if (!groupIndex.TryGetValue(cached.Key, out int gi))
            {
                gi = groups.Count;
                groupIndex[cached.Key] = gi;
                groups.Add((cached.Key, cached.Name, cached.IconKey, new List<AudioSessionControl>()));
            }
            groups[gi].Sessions.Add(cached.Control);
        }

        // System Sounds first, like the Windows mixer; the rest in session order.
        groups.Sort((a, b) => (b.Key == "#system").CompareTo(a.Key == "#system"));

        var apps = new List<AppEntry>(groups.Count);
        foreach (var g in groups)
        {
            int volume;
            bool muted;
            try
            {
                var first = g.Sessions[0].SimpleAudioVolume;
                volume = ToPercent(first.Volume);
                muted = first.Mute;
            }
            catch (Exception e)
            {
                Log.Write($"session read failed: {e.Message}");
                sessionsDirty = true;
                continue;
            }

            if (!idByKey.TryGetValue(g.Key, out int id))
            {
                id = nextId++;
                idByKey[g.Key] = id;
            }
            apps.Add(new AppEntry(id, g.Name, g.IconKey, volume, muted));
            sessionsById[id] = g.Sessions;
        }

        // Forget ids of entries that left the mixer so a reopened app gets a fresh one.
        foreach (var gone in idByKey.Keys.Where(k => !groupIndex.ContainsKey(k)).ToList())
            idByKey.Remove(gone);
        PruneProcesses();

        return new MixerSnapshot(masterVolume, masterMuted, deviceFixedVolume, deviceName, apps);
    }

    /// <summary>
    /// Re-enumerates the device's sessions. RefreshSessions leaks native memory here and in the
    /// Windows audio service on every call, so this runs only when a session appears, plus a slow
    /// safety pass in case a notification is missed. Never call it from the regular poll path.
    /// </summary>
    void ReloadSessions(long now)
    {
        DropSessions();
        sessionsDirty = false; // cleared first so a session created during the reload isn't lost
        lastSessionReloadMs = now;
        nextSessionReloadMs = now + SessionReloadMs;

        try
        {
            var manager = device!.AudioSessionManager;
            manager.RefreshSessions();
            var sessions = manager.Sessions;
            for (int i = 0; i < sessions.Count; i++)
            {
                var cached = ReadSession(sessions[i]);
                if (cached != null) cachedSessions.Add(cached);
            }
        }
        catch
        {
            sessionsDirty = true;
            throw;
        }
    }

    /// <summary>Reads a session's identity; returns null (and disposes it) if it shouldn't be listed.</summary>
    CachedSession? ReadSession(AudioSessionControl session)
    {
        try
        {
            if (session.State == NAudio.CoreAudioApi.Interfaces.AudioSessionState.AudioSessionStateExpired)
            {
                session.Dispose();
                return null;
            }
            if (session.IsSystemSoundsSession)
                return new CachedSession(session, null, "#system", "System Sounds", IconSystem);

            uint pid = session.GetProcessID;
            var info = GetProcess(pid);
            if (info == null) // process exited; Windows hides these sessions too
            {
                session.Dispose();
                return null;
            }
            return new CachedSession(session, pid, info.Path?.ToLowerInvariant() ?? $"pid:{pid}",
                DisplayName(session, info), info.Path != null ? GetIconKey(info.Path) : IconNone);
        }
        catch (Exception e)
        {
            Log.Write($"session read failed: {e.Message}");
            session.Dispose();
            return null;
        }
    }

    bool IsGone(CachedSession cached)
    {
        try
        {
            return cached.Control.State == NAudio.CoreAudioApi.Interfaces.AudioSessionState.AudioSessionStateExpired
                || (cached.Pid is { } pid && GetProcess(pid) == null);
        }
        catch (Exception e)
        {
            Log.Write($"session read failed: {e.Message}");
            sessionsDirty = true;
            return true;
        }
    }

    void DropSessions()
    {
        foreach (var cached in cachedSessions) cached.Control.Dispose();
        cachedSessions.Clear();
    }

    void AttachDevice(MMDevice newDevice)
    {
        DetachDevice();
        device = newDevice;
        deviceId = newDevice.ID;
        deviceName = Sanitize(newDevice.FriendlyName, 60);
        sessionManager = newDevice.AudioSessionManager;
        sessionManager.OnSessionCreated += OnSessionCreated;
        sessionsDirty = true;
        nextSessionReloadMs = 0;
    }

    void DetachDevice()
    {
        DropSessions();
        sessionsById = new Dictionary<int, List<AudioSessionControl>>();
        if (sessionManager != null) sessionManager.OnSessionCreated -= OnSessionCreated;
        sessionManager = null;
        device?.Dispose();
        device = null;
        deviceId = null;
    }

    // Runs on a Core Audio thread.
    void OnSessionCreated(object sender, NAudio.CoreAudioApi.Interfaces.IAudioSessionControl newSession) => sessionsDirty = true;

    public void SetMaster(int volume, bool muted)
    {
        if (device == null) return;
        var endpoint = device.AudioEndpointVolume;
        endpoint.MasterVolumeLevelScalar = Math.Clamp(volume, 0, 100) / 100f;
        endpoint.Mute = muted;
    }

    public void SetApp(int id, int volume, bool muted)
    {
        if (!sessionsById.TryGetValue(id, out var sessions)) return;
        foreach (var s in sessions)
        {
            try
            {
                s.SimpleAudioVolume.Volume = Math.Clamp(volume, 0, 100) / 100f;
                s.SimpleAudioVolume.Mute = muted;
            }
            catch (Exception e) { Log.Write($"set app volume failed: {e.Message}"); }
        }
    }

    public byte[]? GetIcon(uint key) => iconsByKey.TryGetValue(key, out var pixels) ? pixels : null;

    void EnsureDevice()
    {
        long now = Environment.TickCount64;
        if (device != null && now < nextDeviceCheckMs) return;
        nextDeviceCheckMs = now + 1000;
        try
        {
            var current = enumerator.GetDefaultAudioEndpoint(DataFlow.Render, Role.Multimedia);
            if (current.ID == deviceId)
            {
                current.Dispose();
                return;
            }
            AttachDevice(current);
            deviceFixedVolume = IsFixedVolume(current);
            Log.Write($"output device: {current.FriendlyName}{(deviceFixedVolume ? " (fixed volume)" : "")}");
        }
        catch (Exception e)
        {
            if (device != null) Log.Write($"no default output device: {e.Message}");
            DetachDevice();
        }
    }

    /// <summary>
    /// Some devices (e.g. headsets whose volume lives on a base station) report a 0..0 dB range:
    /// Windows accepts volume writes but the level never moves.
    /// </summary>
    static bool IsFixedVolume(MMDevice dev)
    {
        try
        {
            var range = dev.AudioEndpointVolume.VolumeRange;
            return range.MaxDecibels - range.MinDecibels < 0.01f;
        }
        catch
        {
            return false;
        }
    }

    uint GetIconKey(string path)
    {
        if (!iconsByPath.TryGetValue(path, out var entry))
        {
            byte[]? pixels = IconExtractor.Extract(path);
            uint key = pixels == null ? IconNone : Fnv1a(path.ToLowerInvariant());
            entry = (key, pixels);
            iconsByPath[path] = entry;
            if (pixels != null) iconsByKey[key] = pixels;
        }
        return entry.Key;
    }

    static uint Fnv1a(string s)
    {
        uint h = 2166136261;
        foreach (char c in s)
        {
            h ^= c;
            h *= 16777619;
        }
        return h <= IconSystem ? h + 2 : h; // keep 0 and 1 reserved
    }

    ProcessInfo? GetProcess(uint pid)
    {
        if (processes.TryGetValue(pid, out var cached))
        {
            if (cached.Alive) return cached;
            cached.Dispose();
            processes.Remove(pid);
            return null;
        }

        var handle = OpenProcess(ProcessQueryLimitedInformation | Synchronize, false, pid);
        if (handle.IsInvalid)
        {
            handle.Dispose();
            return null;
        }
        var info = new ProcessInfo { Handle = handle };
        if (!info.Alive)
        {
            info.Dispose();
            return null;
        }

        var sb = new StringBuilder(1024);
        int size = sb.Capacity;
        if (QueryFullProcessImageName(handle, 0, sb, ref size)) info.Path = sb.ToString();

        info.Name = FriendlyProcessName(info.Path, pid);
        processes[pid] = info;
        return info;
    }

    void PruneProcesses()
    {
        foreach (var pid in processes.Where(p => !p.Value.Alive).Select(p => p.Key).ToList())
        {
            processes[pid].Dispose();
            processes.Remove(pid);
        }
    }

    static string FriendlyProcessName(string? path, uint pid)
    {
        if (path != null)
        {
            try
            {
                string? description = FileVersionInfo.GetVersionInfo(path).FileDescription;
                if (!string.IsNullOrWhiteSpace(description)) return description.Trim();
            }
            catch { }
            return Path.GetFileNameWithoutExtension(path);
        }
        try { return Process.GetProcessById((int)pid).ProcessName; } catch { return $"Process {pid}"; }
    }

    static string DisplayName(AudioSessionControl session, ProcessInfo info)
    {
        string name = info.Name;
        try
        {
            string display = session.DisplayName;
            if (!string.IsNullOrWhiteSpace(display) && !display.StartsWith('@')) name = display;
        }
        catch { }
        return Sanitize(name, 40);
    }

    /// <summary>The knob's fonts only cover ASCII: strip accents and drop anything else.</summary>
    public static string Sanitize(string text, int maxLength)
    {
        string decomposed = text.Normalize(NormalizationForm.FormD);
        var sb = new StringBuilder(decomposed.Length);
        foreach (char c in decomposed)
        {
            if (CharUnicodeInfo.GetUnicodeCategory(c) == UnicodeCategory.NonSpacingMark) continue;
            if (c >= 0x20 && c < 0x7F) sb.Append(c);
            else if (char.IsWhiteSpace(c)) sb.Append(' ');
        }
        string result = sb.ToString().Trim();
        if (result.Length > maxLength) result = result[..maxLength].TrimEnd();
        return result.Length == 0 ? "?" : result;
    }

    static int ToPercent(float scalar) => (int)Math.Round(Math.Clamp(scalar, 0f, 1f) * 100f);

    public void Dispose()
    {
        DetachDevice();
        foreach (var p in processes.Values) p.Dispose();
        enumerator.Dispose();
    }

    const uint ProcessQueryLimitedInformation = 0x1000;
    const uint Synchronize = 0x00100000;
    const uint WaitTimeout = 0x102;

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern SafeProcessHandle OpenProcess(uint access, bool inherit, uint pid);

    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern bool QueryFullProcessImageName(SafeProcessHandle process, int flags, StringBuilder name, ref int size);

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern uint WaitForSingleObject(SafeProcessHandle handle, uint milliseconds);
}
