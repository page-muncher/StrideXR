using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Text;
using System.Text.RegularExpressions;

internal sealed class AudiobookSubtitles
{
    private const int ContextLines = 2;
    private static readonly Regex Timing = new Regex(
        @"^\s*(\d{1,4}:[0-5]\d:[0-5]\d[,.]\d{3})\s*-->\s*(\d{1,4}:[0-5]\d:[0-5]\d[,.]\d{3})(?:\s+.*)?$",
        RegexOptions.CultureInvariant);
    private static readonly Regex Markup = new Regex(@"</?(?:i|b|u|s|font)(?:\s+[^>]*)?>",
        RegexOptions.IgnoreCase | RegexOptions.CultureInvariant);

    private sealed class Cue
    {
        internal double Start, End, LatestEnd;
        internal int Sequence;
        internal string Text;
        internal string[] Wrapped;
    }

    private readonly List<Cue> cues = new List<Cue>();

    internal int Count { get { return cues.Count; } }

    private static double Seconds(string timestamp)
    {
        var fields = timestamp.Split(':');
        return int.Parse(fields[0], CultureInfo.InvariantCulture) * 3600 +
            int.Parse(fields[1], CultureInfo.InvariantCulture) * 60 +
            double.Parse(fields[2].Replace(',', '.'), CultureInfo.InvariantCulture);
    }

    internal static AudiobookSubtitles Read(string path)
    {
        string content;
        try { content = File.ReadAllText(path, new UTF8Encoding(false, true)); }
        catch (DecoderFallbackException) { content = File.ReadAllText(path, Encoding.GetEncoding(1252)); }

        var result = new AudiobookSubtitles();
        using (var reader = new StringReader(content))
        {
            string line;
            while ((line = reader.ReadLine()) != null)
            {
                var match = Timing.Match(line);
                if (!match.Success) continue;
                var text = new List<string>();
                while (!string.IsNullOrWhiteSpace(line = reader.ReadLine()))
                    text.Add(Markup.Replace(line.Trim(), ""));
                var start = Seconds(match.Groups[1].Value);
                var end = Seconds(match.Groups[2].Value);
                var caption = System.Net.WebUtility.HtmlDecode(string.Join("\n", text));
                if (end > start && !string.IsNullOrWhiteSpace(caption))
                    result.cues.Add(new Cue { Start = start, End = end, Text = caption, Sequence = result.Count });
            }
        }
        if (result.Count == 0) throw new InvalidDataException("The SRT has no readable subtitle cues.");
        result.cues.Sort((a, b) => a.Start == b.Start ? a.Sequence.CompareTo(b.Sequence) : a.Start.CompareTo(b.Start));
        double latestEnd = 0;
        foreach (var cue in result.cues) cue.LatestEnd = latestEnd = Math.Max(latestEnd, cue.End);
        return result;
    }

    internal string At(double seconds, int lines, int layout)
    {
        int low = 0, high = cues.Count;
        while (low < high)
        {
            int middle = low + (high - low) / 2;
            if (cues[middle].Start <= seconds) low = middle + 1;
            else high = middle;
        }
        // Prepare the upcoming text before its timestamp, including in layouts that hide it.
        if (low < cues.Count) Rows(cues[low]);

        int active = low - 1;
        while (active >= 0 && cues[active].LatestEnd > seconds && cues[active].End <= seconds) --active;
        bool before = layout == 1 || layout == 3 || layout == 4;
        bool after = layout == 2 || layout == 3;
        var result = new List<string>(lines + 2 * ContextLines);
        if (active < 0 || cues[active].End <= seconds)
        {
            if (before) AddPrevious(result, low - 1);
            result.Add(" "); // Keep a blank highlighted row while there is no current cue.
            if (after) AddNext(result, low, result.Count + ContextLines);
            return string.Join("\n", result);
        }

        var cue = cues[active];
        var wrapped = Rows(cue);
        int pages = (wrapped.Length + lines - 1) / lines;
        int page = Math.Min(pages - 1, (int)((seconds - cue.Start) / (cue.End - cue.Start) * pages));
        int first = page * lines;
        int end = Math.Min(first + lines, wrapped.Length);
        if (before)
        {
            for (int j = first - 1; j >= 0 && result.Count < ContextLines; --j)
                result.Insert(0, "\u0001" + wrapped[j]);
            AddPrevious(result, active - 1);
        }
        for (int j = first; j < end; ++j) result.Add(wrapped[j]);
        if (after)
        {
            int budget = result.Count + ContextLines;
            for (int j = end; j < wrapped.Length && result.Count < budget; ++j) result.Add("\u0001" + wrapped[j]);
            AddNext(result, active + 1, budget);
        }
        return string.Join("\n", result);
    }

    private void AddNext(List<string> result, int index, int budget)
    {
        for (int i = index; i < cues.Count && result.Count < budget; ++i)
            foreach (var row in Rows(cues[i]))
            {
                if (result.Count == budget) break;
                result.Add("\u0001" + row);
            }
    }

    private static string[] Rows(Cue cue)
    {
        if (cue.Wrapped == null) cue.Wrapped = Wrap(cue.Text);
        return cue.Wrapped;
    }

    private void AddPrevious(List<string> result, int index)
    {
        for (int i = index; i >= 0 && result.Count < ContextLines; --i)
        {
            var rows = Rows(cues[i]);
            for (int j = rows.Length - 1; j >= 0 && result.Count < ContextLines; --j) result.Insert(0, "\u0001" + rows[j]);
        }
    }

    private static string[] Wrap(string text)
    {
        const int width = 1100;
        var result = new List<string>();
        using (var font = new System.Drawing.Font("Segoe UI Semibold", 39, System.Drawing.GraphicsUnit.Pixel))
        {
            foreach (var paragraph in text.Split('\n'))
            {
                var line = "";
                foreach (var word in paragraph.Split(new[] { ' ' }, StringSplitOptions.RemoveEmptyEntries))
                {
                    var next = line.Length == 0 ? word : line + " " + word;
                    if (line.Length != 0 && System.Windows.Forms.TextRenderer.MeasureText(next, font,
                        new System.Drawing.Size(int.MaxValue, int.MaxValue), System.Windows.Forms.TextFormatFlags.NoPrefix).Width > width)
                    {
                        result.Add(line);
                        line = word;
                    }
                    else line = next;
                }
                if (line.Length != 0) result.Add(line);
            }
        }
        return result.ToArray();
    }
}
