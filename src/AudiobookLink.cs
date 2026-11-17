using System;
using System.IO.MemoryMappedFiles;
using System.Threading;

internal sealed class AudiobookLink : IDisposable
{
    private const string Name = @"Local\StrideXR-Audiobook-v3";
    private const int Capacity = 2048;
    private const int TextOffset = 32;
    private const int StyleOffset = TextOffset + 2 * Capacity;
    private readonly Mutex gate;
    private readonly MemoryMappedFile mapping;
    private readonly MemoryMappedViewAccessor view;
    private string caption = "";
    private bool ready;
    private int presses;
    private string lastStyle;
    private readonly bool lab;

    internal AudiobookLink(bool lab = false)
    {
        this.lab = lab;
        var name = lab ? Name + "-Lab" : Name;
        gate = new Mutex(false, name + "-Lock");
        try
        {
            mapping = MemoryMappedFile.CreateOrOpen(name, StyleOffset + 32);
            view = mapping.CreateViewAccessor();
            // A game still running may hold the mapping from the last player window.
            Enter();
            try
            {
                presses = view.ReadInt32(16);
                view.Write(0, 0);
                view.Write(24, 0);
                view.Write(28, 0);
                view.Write(12, 0);
                view.Write(4, unchecked(view.ReadInt32(4) + 1));
            }
            finally { gate.ReleaseMutex(); }
        }
        catch
        {
            if (view != null) view.Dispose();
            if (mapping != null) mapping.Dispose();
            gate.Dispose();
            throw;
        }
    }

    private bool Enter(int timeout = Timeout.Infinite)
    {
        try { return gate.WaitOne(timeout); }
        catch (AbandonedMutexException) { return true; }
    }

    internal SubtitleStyle PreviewStyle(SubtitleStyle fallback)
    {
        if (!Enter(0)) return fallback;
        try
        {
            if (!IsConnected() || (view.ReadInt32(12) & 16) == 0) return fallback;
            var text = string.Format(System.Globalization.CultureInfo.InvariantCulture, "0,40,100,{0},{1}",
                view.ReadInt32(StyleOffset + 20), view.ReadInt32(StyleOffset + 24));
            return SubtitleStyle.Parse(text);
        }
        finally { gate.ReleaseMutex(); }
    }

    private bool IsConnected()
    {
        return view.ReadInt32(12) != 0 && unchecked((uint)(Environment.TickCount - view.ReadInt32(8))) < 1800;
    }

    internal bool Update(bool active, string text, SubtitleStyle style, int fade, out string connection)
    {
        connection = null;
        if (!Enter(0)) return false;
        try
        {
            view.Write(20, Environment.TickCount);
            view.Write(28, fade);
            if (text.Length > Capacity) text = text.Substring(0, Capacity - 1) + "…";
            var styleKey = style.ToString();
            if (ready != active || caption != text || lastStyle != styleKey)
            {
                ready = active;
                caption = text;
                var characters = new ushort[caption.Length];
                for (int i = 0; i < caption.Length; ++i) characters[i] = caption[i];
                view.WriteArray(TextOffset, characters, 0, characters.Length);
                view.Write(24, characters.Length);
                view.Write(0, ready ? 1 : 0);
                int settings = StyleOffset;
                view.Write(settings, style.X);
                view.Write(settings + 4, style.Y);
                view.Write(settings + 8, style.Opacity);
                view.Write(settings + 12, style.Lines);
                view.Write(settings + 16, style.Layout);
                lastStyle = styleKey;
                view.Write(4, unchecked(view.ReadInt32(4) + 1));
            }
            int flags = view.ReadInt32(12);
            if (IsConnected())
            {
                if (lab) connection = "VR Lab connected · Left Menu plays or pauses";
                else if ((flags & 4) != 0) connection = "Fallout connected · overlay needs a D3D11 session";
                else if ((flags & 8) != 0) connection = "Fallout connected · subtitle layer unavailable";
                else if ((flags & 2) != 0) connection = "Fallout connected · left Menu active";
                else connection = "Fallout connected · waiting for left Menu input";
            }
            int count = view.ReadInt32(16);
            bool toggle = active && ((count ^ presses) & 1) != 0;
            presses = count;
            return toggle;
        }
        finally { gate.ReleaseMutex(); }
    }

    public void Dispose()
    {
        Enter();
        try
        {
            view.Write(0, 0);
            view.Write(24, 0);
            view.Write(28, 0);
            view.Write(4, unchecked(view.ReadInt32(4) + 1));
        }
        finally
        {
            gate.ReleaseMutex();
            view.Dispose();
            mapping.Dispose();
            gate.Dispose();
        }
    }
}
