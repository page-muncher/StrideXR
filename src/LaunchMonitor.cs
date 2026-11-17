using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Threading;

internal sealed class Launch
{
    internal int Pid;
    internal string Exe;
    internal string Path;
    internal string Stage;
    internal string Capture;
    internal string Runtime;
    internal string Application;
    internal DateTime Time;
    internal bool Live;
    internal string Target { get { return string.IsNullOrEmpty(Path) ? Exe : Path; } }
    internal bool IsVr { get { return Stage == "OpenXR VR session"; } }
    internal Launch Snapshot() { return (Launch)MemberwiseClone(); }
}

internal sealed class LaunchMonitor : IDisposable
{
    private readonly object sync = new object();
    private readonly Dictionary<int, Launch> processes = new Dictionary<int, Launch>();
    private readonly string directory;
    private readonly int session = Process.GetCurrentProcess().SessionId;
    private Timer watcher;

    internal LaunchMonitor(string dataDirectory)
    {
        directory = Path.Combine(dataDirectory, "OpenXR");
    }

    internal void Start()
    {
        watcher = new Timer(state => ScanProcesses(), null, 0, 175);
    }

    private static string ReadPath(Process process)
    {
        try { return process.MainModule.FileName; }
        catch (Win32Exception) { return null; }
        catch (InvalidOperationException) { return null; }
    }

    private void ScanProcesses()
    {
        if (!Monitor.TryEnter(sync)) return;
        try
        {
            var active = new HashSet<int>();
            foreach (var process in Process.GetProcesses())
            using (process)
            {
                try
                {
                    Launch launch;
                    if (!processes.TryGetValue(process.Id, out launch) || !launch.Live)
                    {
                        if (process.SessionId != session) continue;
                        string path = ReadPath(process);
                        launch = new Launch {
                            Pid = process.Id, Time = process.StartTime.ToUniversalTime(), Path = path,
                            Exe = string.IsNullOrEmpty(path) ? process.ProcessName + ".exe" : Path.GetFileName(path)
                        };
                        processes[process.Id] = launch;
                    }
                    active.Add(process.Id);
                }
                catch (InvalidOperationException) { }
                catch (Win32Exception) { }
            }
            foreach (var launch in processes.Values) launch.Live = active.Contains(launch.Pid);
            foreach (var id in processes.Where(item => !item.Value.Live).OrderByDescending(item => item.Value.Time)
                .Skip(250).Select(item => item.Key).ToArray()) processes.Remove(id);
        }
        catch (Win32Exception) { }
        finally { Monitor.Exit(sync); }
    }

    internal Launch[] ReadLaunches()
    {
        Launch[] snapshot;
        lock (sync) snapshot = processes.Values.Select(launch => launch.Snapshot()).ToArray();
        var records = new List<Launch>();
        if (Directory.Exists(directory))
        {
            foreach (var file in new DirectoryInfo(directory).GetFiles("*.xr").OrderByDescending(f => f.LastWriteTimeUtc).Take(150))
            {
                try
                {
                    int pid;
                    if (!int.TryParse(Path.GetFileNameWithoutExtension(file.Name), out pid)) continue;
                    string[] lines;
                    using (var stream = new FileStream(file.FullName, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete))
                    using (var reader = new StreamReader(stream))
                        lines = reader.ReadToEnd().Replace("\r\n", "\n").Split('\n');
                    if (lines.Length < 6 || !Path.IsPathRooted(lines[0])) continue;
                    records.Add(new Launch {
                        Pid = pid, Path = lines[0], Exe = Path.GetFileName(lines[0]), Stage = lines[1],
                        Capture = lines[2], Runtime = lines[3], Application = lines[4], Time = file.LastWriteTimeUtc,
                        Live = snapshot.Any(p => p.Pid == pid && p.Live && p.Time <= file.LastWriteTimeUtc &&
                            string.Equals(p.Target, lines[0], StringComparison.OrdinalIgnoreCase))
                    });
                }
                catch (IOException) { }
                catch (UnauthorizedAccessException) { }
            }
        }
        return records.Concat(snapshot).Where(launch => !string.IsNullOrEmpty(launch.Exe) &&
                launch.Exe.EndsWith(".exe", StringComparison.OrdinalIgnoreCase))
            .OrderByDescending(launch => launch.IsVr).ThenByDescending(launch => launch.Stage != null)
            .ThenByDescending(launch => launch.Time)
            .GroupBy(launch => launch.Target, StringComparer.OrdinalIgnoreCase).Select(group => group.First()).ToArray();
    }

    public void Dispose()
    {
        if (watcher == null) return;
        watcher.Dispose();
        watcher = null;
    }
}
