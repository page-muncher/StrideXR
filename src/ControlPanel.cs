using System;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Threading;
using System.Windows.Forms;
using Microsoft.Win32;

internal static class Install
{
    internal const string Version = "1.6.3";
    private const string RegistryPath = @"SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit";
    internal static readonly string Root = Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "StrideXR");
    private static readonly string VersionDir = Path.Combine(Root, Version);
    private static readonly string[] ManifestNames = { "StrideXR.json", "StrideXR-x86.json" };
    internal static readonly string Ini = Path.Combine(Root, "StrideXR.ini");
    private static readonly string Shortcut = Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.Programs), "StrideXR.lnk");

    private static RegistryKey RegistryRoot()
    {
        return RegistryKey.OpenBaseKey(RegistryHive.CurrentUser, RegistryView.Registry64);
    }

    private static bool OwnedManifest(string value)
    {
        return value.StartsWith(Root + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase) &&
               ManifestNames.Contains(Path.GetFileName(value), StringComparer.OrdinalIgnoreCase);
    }

    internal static bool Enabled()
    {
        using (var root = RegistryRoot())
        using (var key = root.OpenSubKey(RegistryPath))
            return key != null && ManifestNames.All(name => {
                string manifest = Path.Combine(VersionDir, name);
                return object.Equals(key.GetValue(manifest), 0) && File.Exists(manifest);
            });
    }

    private static void Extract(string name, string path)
    {
        using (var source = Assembly.GetExecutingAssembly().GetManifestResourceStream(name))
        using (var buffer = new MemoryStream())
        {
            source.CopyTo(buffer);
            var bytes = buffer.ToArray();
            if (File.Exists(path) && File.ReadAllBytes(path).SequenceEqual(bytes)) return;
            File.WriteAllBytes(path, bytes);
        }
    }

    internal static string PrepareCalibration()
    {
        var folder = Path.Combine(VersionDir, "Calibration");
        Directory.CreateDirectory(folder);
        foreach (var name in new[] { "StrideXRCalibrate.exe", "openxr_loader.dll", "OpenXR-MIT.txt", "OpenXR-loader-LICENSE.txt", "JsonCpp-LICENSE.txt" })
            Extract(name, Path.Combine(folder, name));
        return Path.Combine(folder, "StrideXRCalibrate.exe");
    }

    internal static string PrepareAudio()
    {
        Directory.CreateDirectory(VersionDir);
        var path = Path.Combine(VersionDir, "StrideXRAudio.dll");
        Extract("StrideXRAudio.dll", path);
        return path;
    }

    internal static void Enable()
    {
        Directory.CreateDirectory(VersionDir);
        Extract("StrideXR.dll", Path.Combine(VersionDir, "StrideXR.dll"));
        Extract("StrideXR-x86.dll", Path.Combine(VersionDir, "StrideXR-x86.dll"));
        PrepareAudio();
        foreach (var name in ManifestNames) Extract(name, Path.Combine(VersionDir, name));
        if (!File.Exists(Ini)) Extract("StrideXR.ini", Ini);
        if (!File.Exists(Path.Combine(Root, "Profiles.ini"))) new ProfileStore(Root, Ini).Save();
        var exe = Path.Combine(VersionDir, "StrideXR.exe");
        var source = Assembly.GetExecutingAssembly().Location;
        if (!string.Equals(source, exe, StringComparison.OrdinalIgnoreCase))
        {
            var bytes = File.ReadAllBytes(source);
            if (!File.Exists(exe) || !File.ReadAllBytes(exe).SequenceEqual(bytes)) File.WriteAllBytes(exe, bytes);
        }
        using (var root = RegistryRoot())
        using (var key = root.CreateSubKey(RegistryPath))
        {
            var manifests = ManifestNames.Select(name => Path.Combine(VersionDir, name)).ToArray();
            foreach (var name in key.GetValueNames().Where(OwnedManifest))
                if (!manifests.Contains(name, StringComparer.OrdinalIgnoreCase)) key.DeleteValue(name, false);
            foreach (var manifest in manifests) key.SetValue(manifest, 0, RegistryValueKind.DWord);
        }
        var shellType = Type.GetTypeFromProgID("WScript.Shell");
        if (shellType != null)
        {
            dynamic shell = Activator.CreateInstance(shellType);
            dynamic link = shell.CreateShortcut(Shortcut);
            link.TargetPath = exe;
            link.WorkingDirectory = VersionDir;
            link.Description = "StrideXR OpenXR locomotion and audiobook player";
            link.Save();
        }
    }

    internal static void Disable(bool unregister)
    {
        using (var root = RegistryRoot())
        using (var key = root.OpenSubKey(RegistryPath, true))
            if (key != null)
                foreach (var name in key.GetValueNames().Where(OwnedManifest))
                    if (unregister) key.DeleteValue(name, false);
                    else key.SetValue(name, 1, RegistryValueKind.DWord);
        if (unregister && File.Exists(Shortcut)) File.Delete(Shortcut);
    }
}

internal static class Program
{
    [STAThread]
    private static int Main(string[] args)
    {
        if (args.Length != 0)
        {
            try
            {
                if (args.Length != 1) return 2;
                switch (args[0])
                {
                    case "--install": Install.Enable(); return 0;
                    case "--disable": Install.Disable(false); return 0;
                    case "--uninstall": Install.Disable(true); return 0;
                    default: return 2;
                }
            }
            catch (Exception e)
            {
                Console.Error.WriteLine(e);
                return 1;
            }
        }
        bool owns;
        using (var single = new Mutex(true, @"Local\StrideXR-ControlPanel", out owns))
        {
            if (!owns)
            {
                MessageBox.Show("StrideXR is already open.", "StrideXR");
                return 0;
            }
            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);
            Application.Run(new ControlPanel());
        }
        return 0;
    }
}
