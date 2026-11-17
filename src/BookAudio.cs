using System;
using System.IO;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

internal sealed class BookAudio : SafeHandleZeroOrMinusOneIsInvalid
{
    private const string Library = "StrideXRAudio.dll";
    private static readonly Lazy<IntPtr> module = new Lazy<IntPtr>(() => {
        var path = Install.PrepareAudio();
        var handle = LoadLibraryEx(path, IntPtr.Zero, 0x900);
        if (handle == IntPtr.Zero) throw new IOException("Could not load the audiobook audio engine.",
            new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error()));
        return handle;
    });

    private BookAudio() : base(true) { }

    internal static BookAudio Open(string path)
    {
        if (module.Value == IntPtr.Zero) throw new IOException("Audiobook audio engine unavailable.");
        var book = BookOpen(path);
        if (book.IsInvalid)
        {
            book.Dispose();
            throw new InvalidDataException("Could not index this MP3. Check that it is a readable MP3 file.");
        }
        return book;
    }

    internal double Duration { get { return BookDuration(this); } }
    internal double Position { get { return BookPosition(this); } }
    internal void Seek(double position) { Check(BookSeek(this, position), "Could not seek to this position."); }
    internal void Play(bool playing) { Check(BookPlaying(this, playing ? 1 : 0), "Could not change playback. Check the Windows audio output."); }
    internal void Pump() { Check(BookPump(this), "Playback stopped while decoding the MP3."); }
    internal void Volume(float value) { BookVolume(this, value); }

    private static void Check(int result, string message)
    {
        if (result == 0) throw new IOException(message);
    }

    protected override bool ReleaseHandle() { BookClose(handle); return true; }

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr LoadLibraryEx(string file, IntPtr reserved, uint flags);
    [DllImport(Library, CharSet = CharSet.Unicode, ExactSpelling = true)]
    private static extern BookAudio BookOpen(string path);
    [DllImport(Library)] private static extern void BookClose(IntPtr player);
    [DllImport(Library)] private static extern double BookDuration(BookAudio player);
    [DllImport(Library)] private static extern double BookPosition(BookAudio player);
    [DllImport(Library)] private static extern int BookSeek(BookAudio player, double seconds);
    [DllImport(Library)] private static extern int BookPlaying(BookAudio player, int value);
    [DllImport(Library)] private static extern int BookPump(BookAudio player);
    [DllImport(Library)] private static extern void BookVolume(BookAudio player, float value);
}
