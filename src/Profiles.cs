using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text;

internal sealed class Profile
{
    internal string Id;
    internal string Name;
    internal readonly List<string> Targets = new List<string>();
    internal readonly Dictionary<string, string> Values = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);

    internal Profile(string id, string name)
    {
        Id = id;
        Name = name;
    }

    internal string Value(string key, string fallback = "")
    {
        string value;
        return Values.TryGetValue(key, out value) ? value : fallback;
    }

    public override string ToString() { return Name; }
}

internal sealed class ProfileStore
{
    internal readonly List<Profile> Profiles = new List<Profile>();
    internal readonly HashSet<string> HiddenExecutables = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
    internal string DefaultId = "zelda";
    internal bool Global;
    internal string CaptureId = "";
    private readonly string path;

    internal ProfileStore(string directory, string legacy)
    {
        path = Path.Combine(directory, "Profiles.ini");
        if (!File.Exists(path))
        {
            Migrate(legacy);
            return;
        }

        var sections = ReadIni(path);
        Dictionary<string, string> header;
        if (!sections.TryGetValue("Profiles", out header)) throw new InvalidDataException("Profiles.ini is missing its profile list.");
        Global = Read(header, "Global") == "1";
        DefaultId = Read(header, "Default", "zelda");
        Dictionary<string, string> hidden;
        if (sections.TryGetValue("Hidden", out hidden))
            HiddenExecutables.UnionWith(hidden.Values.Where(value => value.EndsWith(".exe", StringComparison.OrdinalIgnoreCase)));
        foreach (var id in Read(header, "Ids").Split(new[] { ';' }, StringSplitOptions.RemoveEmptyEntries))
        {
            Dictionary<string, string> values;
            if (!sections.TryGetValue("Profile:" + id, out values)) continue;
            var profile = new Profile(id, Read(values, "Name", id));
            int count;
            int.TryParse(Read(values, "TargetCount"), out count);
            for (int i = 0; i < Math.Min(count, 256); ++i)
            {
                string target = Read(values, "Target" + i);
                if (target.Length > 0) profile.Targets.Add(target);
            }
            foreach (var key in values.Keys)
                if (key != "Name" && !key.StartsWith("Target", StringComparison.OrdinalIgnoreCase))
                    profile.Values[key] = values[key];
            Profiles.Add(profile);
        }
        if (Profiles.Count == 0) throw new InvalidDataException("Profiles.ini contains no profiles.");
        if (Profiles.All(profile => profile.Id != DefaultId)) DefaultId = Profiles[0].Id;
    }

    private void Migrate(string legacy)
    {
        var zelda = new Profile("zelda", "The Legend of Zelda");
        zelda.Targets.Add("soh.exe");
        zelda.Values["Thumbrests"] = "Both";
        zelda.Values["StartSpeed"] = "0.20";
        zelda.Values["FullSpeed"] = "3.00";
        zelda.Values["MinimumStick"] = "0.151";
        zelda.Values["MaximumStick"] = "0.53";
        Profiles.Add(zelda);

        var sections = ReadIni(legacy);
        Dictionary<string, string> values;
        if (!sections.TryGetValue("StrideXR", out values)) return;
        var targets = Read(values, "Games", "Dishonored2.exe").Split(';').Select(value => value.Trim())
            .Where(value => value.Length > 0).ToArray();
        if (targets.Contains("soh.exe", StringComparer.OrdinalIgnoreCase)) CopySettings(values, zelda);

        var other = targets.Where(target => !string.Equals(target, "soh.exe", StringComparison.OrdinalIgnoreCase)).ToArray();
        if (other.Length == 0) return;
        var imported = new Profile("imported", other.Length == 1 && other[0].Equals("Dishonored2.exe", StringComparison.OrdinalIgnoreCase)
            ? "Dishonored 2" : "Imported settings");
        imported.Targets.AddRange(other.Where(target => target != "*"));
        CopySettings(values, imported);
        if (other.Contains("*"))
        {
            Global = true;
            DefaultId = imported.Id;
        }
        Profiles.Add(imported);
    }

    private static void CopySettings(Dictionary<string, string> source, Profile profile)
    {
        foreach (var item in source)
            if (item.Key != "Games" && item.Key != "Enabled") profile.Values[item.Key] = item.Value;
    }

    internal void Assign(Profile profile, string target)
    {
        if (profile.Targets.Contains(target, StringComparer.OrdinalIgnoreCase)) return;
        if (profile.Targets.Count >= 256) throw new InvalidOperationException("This profile already has 256 executables.");
        foreach (var item in Profiles) item.Targets.RemoveAll(value => value.Equals(target, StringComparison.OrdinalIgnoreCase));
        profile.Targets.Add(target);
    }

    internal static string ValidateName(string text)
    {
        var name = text.Trim();
        if (name.Length == 0 || name.Length > 100 || name.Any(c => char.IsControl(c) || c == '"'))
            throw new ArgumentException("Enter a profile name up to 100 characters without quotation marks.");
        return name;
    }

    internal void Rename(Profile profile, string text)
    {
        var previous = profile.Name;
        profile.Name = ValidateName(text);
        try { Save(); }
        catch { profile.Name = previous; throw; }
    }

    internal void SetLaunchFolder(Profile profile, string path)
    {
        string previous;
        bool hadFolder = profile.Values.TryGetValue("LaunchFolder", out previous);
        profile.Values["LaunchFolder"] = path;
        try { Save(); }
        catch
        {
            if (hadFolder) profile.Values["LaunchFolder"] = previous;
            else profile.Values.Remove("LaunchFolder");
            throw;
        }
    }

    internal void Delete(Profile profile)
    {
        if (Profiles.Count == 1) throw new InvalidOperationException("Keep at least one profile.");
        var index = Profiles.IndexOf(profile);
        if (index < 0) return;
        var previousDefault = DefaultId;
        Profiles.RemoveAt(index);
        if (DefaultId == profile.Id)
            DefaultId = (Profiles.FirstOrDefault(item => item.Id == "zelda") ?? Profiles[0]).Id;
        try { Save(); }
        catch
        {
            Profiles.Insert(index, profile);
            DefaultId = previousDefault;
            throw;
        }
    }

    internal void Save()
    {
        Directory.CreateDirectory(Path.GetDirectoryName(path));
        var text = new StringBuilder("[Profiles]\r\n");
        text.AppendLine("Ids=\"" + string.Join(";", Profiles.Select(profile => profile.Id)) + "\"");
        text.AppendLine("Default=" + DefaultId);
        text.AppendLine("Global=" + (Global ? "1" : "0"));
        text.AppendLine("Capture=" + CaptureId);
        if (HiddenExecutables.Count > 0)
        {
            text.AppendLine();
            text.AppendLine("[Hidden]");
            int index = 0;
            foreach (var executable in HiddenExecutables.OrderBy(value => value, StringComparer.OrdinalIgnoreCase))
                text.AppendLine("Exe" + index++ + "=\"" + executable + "\"");
        }
        foreach (var profile in Profiles)
        {
            text.AppendLine();
            text.AppendLine("[Profile:" + profile.Id + "]");
            text.AppendLine("Name=\"" + profile.Name + "\"");
            text.AppendLine("TargetCount=" + profile.Targets.Count);
            for (int i = 0; i < profile.Targets.Count; ++i)
                text.AppendLine("Target" + i + "=\"" + profile.Targets[i] + "\"");
            foreach (var value in profile.Values) text.AppendLine(value.Key + "=" + value.Value);
        }
        var temporary = path + "." + Guid.NewGuid().ToString("N") + ".tmp";
        try
        {
            File.WriteAllText(temporary, text.ToString(), Encoding.Unicode);
            if (File.Exists(path)) File.Replace(temporary, path, null);
            else File.Move(temporary, path);
        }
        finally { File.Delete(temporary); }
    }

    private static string Read(Dictionary<string, string> values, string key, string fallback = "")
    {
        string result;
        return values.TryGetValue(key, out result) ? result : fallback;
    }

    private static Dictionary<string, Dictionary<string, string>> ReadIni(string path)
    {
        var sections = new Dictionary<string, Dictionary<string, string>>(StringComparer.OrdinalIgnoreCase);
        if (!File.Exists(path)) return sections;
        Dictionary<string, string> values = null;
        foreach (var raw in File.ReadLines(path))
        {
            var line = raw.Trim();
            if (line.StartsWith("[") && line.EndsWith("]"))
            {
                values = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
                sections[line.Substring(1, line.Length - 2)] = values;
            }
            else if (values != null && !line.StartsWith(";") && !line.StartsWith("#"))
            {
                int split = line.IndexOf('=');
                if (split > 0) values[line.Substring(0, split).Trim()] = line.Substring(split + 1).Trim().Trim('"');
            }
        }
        return sections;
    }
}
