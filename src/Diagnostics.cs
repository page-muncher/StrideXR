using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Text;
using System.Threading;
using Microsoft.Win32;

internal sealed class DiagnosticsSession : IDisposable
{
    private const string OpenXR = @"SOFTWARE\Khronos\OpenXR\1";
    private readonly string locator;
    private readonly string runtimeLog;
    private readonly DateTime started = DateTime.UtcNow;
    private readonly Dictionary<int, string> observations = new Dictionary<int, string>();
    private EventWaitHandle signal;
    private DateTime runtimeWritten;
    private long runtimeLength;
    internal string Folder { get; private set; }
    internal bool Expired { get { return DateTime.UtcNow - started > TimeSpan.FromMinutes(5); } }

    internal DiagnosticsSession(string root, string runtimeLog = null)
    {
        this.runtimeLog = runtimeLog ?? Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData),
            "Virtual Desktop", "OpenXR.log");
        string id = Guid.NewGuid().ToString("N");
        Folder = Path.Combine(root, "Diagnostics", id);
        locator = Path.Combine(root, "DebugCapture.ini");
        Directory.CreateDirectory(Folder);
        try
        {
            Write("StrideXR " + Install.Version + " diagnostic capture");
            foreach (var view in new[] { RegistryView.Registry32, RegistryView.Registry64 })
            {
                using (var machine = RegistryKey.OpenBaseKey(RegistryHive.LocalMachine, view))
                using (var key = machine.OpenSubKey(OpenXR))
                    Write(view + " runtime = " + (key == null ? "unregistered" : key.GetValue("ActiveRuntime", "unregistered")));
                using (var user = RegistryKey.OpenBaseKey(RegistryHive.CurrentUser, view))
                using (var key = user.OpenSubKey(OpenXR + @"\ApiLayers\Implicit"))
                    if (key != null)
                        foreach (var value in key.GetValueNames().Where(value => Path.GetFileName(value).StartsWith("StrideXR", StringComparison.OrdinalIgnoreCase)))
                            Write(view + " layer = " + value + " | registry flag=" + key.GetValue(value));
            }
            signal = new EventWaitHandle(false, EventResetMode.ManualReset, @"Local\StrideXR-Debug-" + id);
            File.WriteAllText(locator, "[Capture]\r\nId=" + id + "\r\n", Encoding.Unicode);
        }
        catch
        {
            if (signal != null) signal.Dispose();
            throw;
        }
    }

    internal void Observe(IEnumerable<Launch> launches, IEnumerable<string> targets)
    {
        var names = new HashSet<string>(targets.Select(Path.GetFileName), StringComparer.OrdinalIgnoreCase);
        foreach (var launch in launches.Where(item => item.Time >= started && (item.Stage != null || names.Contains(item.Exe))))
        {
            string modules = launch.Live ? StrideModule(launch.Pid) : "process exited";
            string state = launch.Target + " | " + ImageArchitecture(launch.Path) + " | " + (launch.Stage ?? "Windows launch") + " | " + modules;
            string previous;
            if (observations.TryGetValue(launch.Pid, out previous) && previous == state) continue;
            observations[launch.Pid] = state;
            Write("PID " + launch.Pid + " | " + state);
        }

        var log = new FileInfo(runtimeLog);
        if (!log.Exists || log.LastWriteTimeUtc < started ||
            (log.LastWriteTimeUtc == runtimeWritten && log.Length == runtimeLength)) return;
        try
        {
            using (var stream = new FileStream(log.FullName, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete))
            {
                stream.Seek(Math.Max(0, stream.Length - 96 * 1024), SeekOrigin.Begin);
                using (var reader = new StreamReader(stream))
                    File.WriteAllText(Path.Combine(Folder, "VDXR.log"), reader.ReadToEnd());
            }
            if (runtimeWritten == default(DateTime)) Write("Captured Virtual Desktop OpenXR log.");
            runtimeWritten = log.LastWriteTimeUtc;
            runtimeLength = log.Length;
        }
        catch (IOException) { /* A new VR launch may replace the log. Retry on the next update. */ }
        catch (UnauthorizedAccessException) { }
    }

    private static string StrideModule(int pid)
    {
        try
        {
            using (var process = Process.GetProcessById(pid))
            {
                var layer = process.Modules.Cast<ProcessModule>().FirstOrDefault(module =>
                    module.ModuleName.Equals("StrideXR.dll", StringComparison.OrdinalIgnoreCase) ||
                    module.ModuleName.Equals("StrideXR-x86.dll", StringComparison.OrdinalIgnoreCase));
                return layer == null ? "StrideXR module not loaded" : "loaded " + layer.FileName;
            }
        }
        catch (ArgumentException) { return "process exited"; }
        catch (InvalidOperationException) { return "process exited"; }
        catch (Win32Exception error) { return "module inspection unavailable (" + error.NativeErrorCode + ")"; }
    }

    private static string ImageArchitecture(string path)
    {
        if (string.IsNullOrEmpty(path)) return "architecture unknown";
        try
        {
            using (var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete))
            using (var reader = new BinaryReader(stream))
            {
                if (reader.ReadUInt16() != 0x5a4d || stream.Length < 64) return "architecture unknown";
                stream.Position = 0x3c;
                uint offset = reader.ReadUInt32();
                if (offset > stream.Length - 6) return "architecture unknown";
                stream.Position = offset;
                if (reader.ReadUInt32() != 0x4550) return "architecture unknown";
                switch (reader.ReadUInt16())
                {
                    case 0x14c: return "x86";
                    case 0x8664: return "x64";
                    case 0xaa64: return "arm64";
                    default: return "architecture unknown";
                }
            }
        }
        catch (IOException) { return "architecture unknown"; }
        catch (UnauthorizedAccessException) { return "architecture unknown"; }
    }

    private void Write(string text)
    {
        File.AppendAllText(Path.Combine(Folder, "Capture.txt"), DateTime.UtcNow.ToString("O") + " " + text + Environment.NewLine);
    }

    public void Dispose()
    {
        if (signal == null) return;
        signal.Dispose();
        signal = null;
        File.Delete(locator);
        Write("Capture stopped.");
    }
}
