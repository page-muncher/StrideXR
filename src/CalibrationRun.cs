using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Text;

internal sealed class CalibrationRun : IDisposable
{
    private readonly string directory;
    private readonly Process process;
    internal bool Finished { get { return process.HasExited; } }
    internal bool Canceled { get; private set; }
    internal bool IsScene { get; private set; }

    internal CalibrationRun(Profile profile, SubtitleStyle style = null)
    {
        IsScene = style != null;
        var exe = Install.PrepareCalibration();
        directory = Path.Combine(Install.Root, "CalibrationRuns", Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(directory);
        var input = new StringBuilder("[Calibration]\r\n");
        foreach (var setting in profile.Values)
            if (setting.Key != "LaunchFolder") input.AppendLine(setting.Key + "=" + setting.Value);
        if (style != null)
        {
            input.AppendLine("[Subtitles]");
            input.AppendFormat(CultureInfo.InvariantCulture, "X={0}\r\nY={1}\r\nOpacity={2}\r\nLines={3}\r\nLayout={4}\r\n",
                style.X, style.Y, style.Opacity, style.Lines, style.Layout);
        }
        File.WriteAllText(Path.Combine(directory, "Input.ini"), input.ToString(), Encoding.Unicode);

        var start = new ProcessStartInfo(exe, "\"" + directory + "\"" + (IsScene ? " --scene" : "")) {
            UseShellExecute = false, CreateNoWindow = true, WorkingDirectory = Path.GetDirectoryName(exe)
        };
        start.EnvironmentVariables["DISABLE_STRIDEXR"] = "1";
        start.EnvironmentVariables["DISABLE_STRIDEXR_X86"] = "1";
        process = Process.Start(start);
        if (process == null) throw new InvalidOperationException("Could not start the calibration app.");
    }

    internal Dictionary<string, decimal> ReadResult()
    {
        CheckError();
        var result = Path.Combine(directory, "Result.ini");
        if (!File.Exists(result)) return null;
        var values = new Dictionary<string, decimal>();
        foreach (var line in File.ReadLines(result))
        {
            var pair = line.Split(new[] { '=' }, 2);
            decimal number;
            if (pair.Length == 2 && decimal.TryParse(pair[1], NumberStyles.Number, CultureInfo.InvariantCulture, out number))
                values[pair[0]] = number;
        }
        decimal start, slow, medium, fast;
        if (!values.TryGetValue("StartSpeed", out start) || !values.TryGetValue("SlowSpeed", out slow) ||
            !values.TryGetValue("MidSpeed", out medium) || !values.TryGetValue("FullSpeed", out fast) ||
            start < .03m || slow < start || medium <= slow || fast <= medium || fast > 3.20m || fast < start + .10m)
            throw new InvalidDataException("The calibration measurements are incomplete or out of order.");
        return values;
    }

    internal SubtitleStyle ReadStyle()
    {
        CheckError();
        var result = Path.Combine(directory, "SubtitleStyle.txt");
        return File.Exists(result) ? SubtitleStyle.Parse(File.ReadAllText(result).Trim()) : null;
    }

    private void CheckError()
    {
        var error = Path.Combine(directory, "Error.txt");
        if (File.Exists(error)) throw new InvalidOperationException(File.ReadAllText(error).Trim());
    }

    internal void Cancel()
    {
        if (process.HasExited) return;
        Canceled = true;
        File.WriteAllText(Path.Combine(directory, "Cancel"), "");
    }

    public void Dispose()
    {
        try
        {
            if (!process.HasExited)
            {
                Cancel();
                if (!process.WaitForExit(750))
                {
                    try { process.Kill(); }
                    catch (InvalidOperationException) { }
                }
            }
        }
        finally { process.Dispose(); }
    }
}
