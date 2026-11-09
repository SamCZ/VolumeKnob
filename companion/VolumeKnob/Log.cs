namespace VolumeKnob;

/// <summary>Tiny thread-safe file log in %LOCALAPPDATA%\VolumeKnob\log.txt (truncated at startup when large).</summary>
static class Log
{
    static readonly object Gate = new();
    public static readonly string Directory = Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "VolumeKnob");
    public static readonly string FilePath = Path.Combine(Directory, "log.txt");

    static Log()
    {
        try
        {
            System.IO.Directory.CreateDirectory(Directory);
            var info = new FileInfo(FilePath);
            if (info.Exists && info.Length > 1_000_000) info.Delete();
        }
        catch { }
    }

    public static void Write(string message)
    {
        string line = $"{DateTime.Now:HH:mm:ss.fff} {message}";
        System.Diagnostics.Debug.WriteLine(line);
        lock (Gate)
        {
            try { File.AppendAllText(FilePath, line + Environment.NewLine); } catch { }
        }
    }
}
