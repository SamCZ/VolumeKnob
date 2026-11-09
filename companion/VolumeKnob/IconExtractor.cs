using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;

namespace VolumeKnob;

/// <summary>Extracts an exe's icon as Size x Size ARGB8888 pixels in B,G,R,A byte order (straight alpha).</summary>
static class IconExtractor
{
    public const int Size = 64;

    public static byte[]? Extract(string exePath)
    {
        try
        {
            using Bitmap? source = Load(exePath);
            if (source == null) return null;

            using var canvas = new Bitmap(Size, Size, PixelFormat.Format32bppArgb);
            using (var g = Graphics.FromImage(canvas))
            {
                g.Clear(Color.Transparent);
                g.CompositingMode = CompositingMode.SourceOver;
                g.CompositingQuality = CompositingQuality.HighQuality;
                g.InterpolationMode = InterpolationMode.HighQualityBicubic;
                g.PixelOffsetMode = PixelOffsetMode.HighQuality;
                float scale = Math.Min((float)Size / source.Width, (float)Size / source.Height);
                float w = source.Width * scale, h = source.Height * scale;
                g.DrawImage(source, (Size - w) / 2f, (Size - h) / 2f, w, h);
            }

            var data = canvas.LockBits(new Rectangle(0, 0, Size, Size), ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
            try
            {
                var pixels = new byte[Size * Size * 4];
                for (int y = 0; y < Size; y++)
                    Marshal.Copy(data.Scan0 + y * data.Stride, pixels, y * Size * 4, Size * 4);
                return IsBlank(pixels) ? null : pixels;
            }
            finally
            {
                canvas.UnlockBits(data);
            }
        }
        catch (Exception e)
        {
            Log.Write($"icon extract failed for {exePath}: {e.Message}");
            return null;
        }
    }

    static Bitmap? Load(string exePath)
    {
        // Prefer a real large icon from the exe's resources; fall back to the shell association icon (32px).
        using Icon? large = Icon.ExtractIcon(exePath, 0, Size);
        if (large != null) return large.ToBitmap();
        using Icon? assoc = Icon.ExtractAssociatedIcon(exePath);
        return assoc?.ToBitmap();
    }

    static bool IsBlank(byte[] pixels)
    {
        for (int i = 3; i < pixels.Length; i += 4)
            if (pixels[i] > 16) return false;
        return true;
    }
}
