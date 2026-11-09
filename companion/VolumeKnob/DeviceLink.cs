using System.Collections.Concurrent;
using System.IO.Ports;
using System.Management;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.RegularExpressions;

namespace VolumeKnob;

/// <summary>
/// Serial connection to the knob over its ESP32-S3 native USB port (VID 303A / PID 1001).
///
/// Finding the knob without disturbing other serial devices:
///  - Paired: Windows reports each ESP32 port's USB serial number (the chip MAC). Only the port whose
///    serial matches the paired knob is opened, whatever its COM number.
///  - Not paired yet: ESP32 ports are opened read-only and we listen for the knob's own announcement
///    ("!VOLKNOB 1 &lt;MAC&gt;", sent every second while no companion is connected). Nothing is ever written
///    to a device that has not identified itself as the knob.
/// DTR/RTS stay low so no board is ever reset.
/// </summary>
sealed partial class DeviceLink : IDisposable
{
    const int ListenMs = 1600;

    readonly ConcurrentQueue<string> incoming = new();
    readonly AutoResetEvent wake;
    readonly string? forcedPort;
    readonly Settings settings;
    readonly object settingsGate = new();
    SerialPort? port;
    Thread? reader;
    volatile bool connected;

    public DeviceLink(AutoResetEvent wake, Settings settings, string? forcedPort)
    {
        this.wake = wake;
        this.settings = settings;
        this.forcedPort = forcedPort;
    }

    public bool IsConnected => connected;
    public string? PortName => connected ? port?.PortName : null;
    public long LastSendMs { get; private set; }

    public string? PairedSerial
    {
        get { lock (settingsGate) return settings.PairedSerial; }
    }

    public bool TryDequeue(out string line) => incoming.TryDequeue(out line!);

    /// <summary>Forget the paired knob; the next connect attempt pairs again by listening.</summary>
    public void Unpair()
    {
        lock (settingsGate)
        {
            settings.PairedSerial = null;
            settings.Save();
        }
        Log.Write("pairing cleared");
        connected = false;
        wake.Set();
    }

    /// <summary>Try to find and open the knob. Returns true when connected.</summary>
    public bool TryConnect()
    {
        Close();

        if (forcedPort != null)
            return TryOpen(forcedPort, usbSerial: null, mayWrite: true);

        string? paired = PairedSerial;
        var ports = EspPorts();
        if (paired != null)
        {
            foreach (var p in ports.Where(p => string.Equals(p.Serial, paired, StringComparison.OrdinalIgnoreCase)))
                if (TryOpen(p.Port, p.Serial, mayWrite: true)) return true;
            return false; // knob unplugged; never touch the other boards
        }

        foreach (var p in ports)
            if (TryOpen(p.Port, p.Serial, mayWrite: false)) return true;
        return false;
    }

    sealed record EspPort(string Port, string? Serial);

    /// <summary>COM ports of ESP32 USB Serial/JTAG devices with their USB serial numbers.</summary>
    static List<EspPort> EspPorts()
    {
        var result = new List<EspPort>();
        try
        {
            using var searcher = new ManagementObjectSearcher(
                "SELECT Name, PNPDeviceID FROM Win32_PnPEntity WHERE PNPDeviceID LIKE 'USB\\\\VID_303A&PID_1001%'");
            foreach (var obj in searcher.Get())
            {
                var match = ComRegex().Match(obj["Name"]?.ToString() ?? "");
                if (!match.Success) continue;
                result.Add(new EspPort(match.Groups[1].Value, UsbSerialOf(obj["PNPDeviceID"]?.ToString())));
            }
        }
        catch (Exception e)
        {
            Log.Write($"port scan failed: {e.Message}");
        }
        return result;
    }

    [GeneratedRegex(@"\((COM\d+)\)")]
    private static partial Regex ComRegex();

    /// <summary>
    /// The COM port is interface 0 of a composite device (USB\VID_303A&amp;PID_1001&amp;MI_00\...); its parent's
    /// instance id ends with the device's USB serial number, which for ESP32 USB Serial/JTAG is the chip MAC.
    /// </summary>
    static string? UsbSerialOf(string? pnpDeviceId)
    {
        if (pnpDeviceId == null) return null;
        if (CM_Locate_DevNodeW(out uint node, pnpDeviceId, 0) != 0) return null;
        if (CM_Get_Parent(out uint parent, node, 0) != 0) return null;
        var buffer = new char[256];
        if (CM_Get_Device_IDW(parent, buffer, buffer.Length, 0) != 0) return null;
        string parentId = new string(buffer).TrimEnd('\0');
        int slash = parentId.LastIndexOf('\\');
        return slash >= 0 ? parentId[(slash + 1)..] : null;
    }

    bool TryOpen(string name, string? usbSerial, bool mayWrite)
    {
        var sp = new SerialPort(name, 115200)
        {
            DtrEnable = false,
            RtsEnable = false,
            NewLine = "\n",
            Encoding = Encoding.ASCII,
            ReadTimeout = 200,
            WriteTimeout = 2000,
            WriteBufferSize = 64 * 1024,
            ReadBufferSize = 64 * 1024,
        };
        try
        {
            sp.Open();
            if (mayWrite) sp.Write("?\n");
            long deadline = Environment.TickCount64 + ListenMs;
            while (Environment.TickCount64 < deadline)
            {
                string line;
                try { line = sp.ReadLine().TrimEnd('\r'); }
                catch (TimeoutException) { continue; }
                if (!line.StartsWith("!VOLKNOB")) continue;

                string? announced = line.Split(' ').ElementAtOrDefault(2);
                string id = usbSerial ?? announced ?? name;
                lock (settingsGate)
                {
                    if (forcedPort == null && settings.PairedSerial == null)
                    {
                        settings.PairedSerial = id;
                        settings.Save();
                        Log.Write($"paired with knob {id} (announced {announced ?? "?"}) on {name}");
                    }
                }
                port = sp;
                connected = true;
                LastSendMs = Environment.TickCount64;
                reader = new Thread(ReadLoop) { IsBackground = true, Name = "knob-reader" };
                reader.Start();
                Log.Write($"connected to knob {id} on {name}");
                return true;
            }
            Log.Write($"{name} ({usbSerial ?? "no serial"}): not a knob");
        }
        catch (Exception e)
        {
            Log.Write($"{name}: {e.Message}");
        }
        try { sp.Dispose(); } catch { }
        return false;
    }

    void ReadLoop()
    {
        var sp = port;
        while (connected && sp != null)
        {
            try
            {
                string line = sp.ReadLine().TrimEnd('\r');
                if (line.Length == 0) continue;
                incoming.Enqueue(line);
                wake.Set();
            }
            catch (TimeoutException) { }
            catch (Exception e)
            {
                if (connected) Log.Write($"read failed: {e.Message}");
                connected = false;
                wake.Set();
            }
        }
    }

    public void Send(string line)
    {
        var sp = port;
        if (!connected || sp == null) return;
        try
        {
            sp.Write(line + "\n");
            LastSendMs = Environment.TickCount64;
        }
        catch (Exception e)
        {
            Log.Write($"write failed: {e.Message}");
            connected = false;
        }
    }

    public void Close()
    {
        connected = false;
        var sp = port;
        port = null;
        if (sp != null)
        {
            try { sp.Close(); } catch { }
            try { sp.Dispose(); } catch { }
        }
        reader?.Join(1000);
        reader = null;
        incoming.Clear();
    }

    public void Dispose() => Close();

    [DllImport("cfgmgr32.dll", CharSet = CharSet.Unicode)]
    static extern int CM_Locate_DevNodeW(out uint devInst, string deviceId, uint flags);

    [DllImport("cfgmgr32.dll")]
    static extern int CM_Get_Parent(out uint parent, uint devInst, uint flags);

    [DllImport("cfgmgr32.dll", CharSet = CharSet.Unicode)]
    static extern int CM_Get_Device_IDW(uint devInst, char[] buffer, int bufferLen, uint flags);
}
