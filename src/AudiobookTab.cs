using System;
using System.Drawing;
using System.Globalization;
using System.IO;
using System.Text;
using System.Threading.Tasks;
using System.Windows.Forms;

internal sealed class AudiobookTab : TabPage
{
    private readonly TextBox audioPath = new TextBox { Dock = DockStyle.Fill, ReadOnly = true };
    private readonly TextBox subtitlePath = new TextBox { Dock = DockStyle.Fill, ReadOnly = true };
    private readonly TrackBar seek = new TrackBar {
        Dock = DockStyle.Fill, Maximum = 10000, TickStyle = TickStyle.None, Enabled = false,
        AutoSize = false, Height = 28, Margin = new Padding(3, 1, 3, 1)
    };
    private readonly Label time = new Label { AutoSize = true, Padding = new Padding(6, 7, 0, 0) };
    private readonly Label state = new Label { AutoSize = true, Padding = new Padding(4) };
    private readonly Label connection = new Label { AutoSize = true, Padding = new Padding(4) };
    private readonly Label preview = new Label {
        Dock = DockStyle.Fill, TextAlign = ContentAlignment.MiddleCenter, Padding = new Padding(20),
        BorderStyle = BorderStyle.FixedSingle, Font = new Font("Segoe UI", 12)
    };
    private readonly CheckBox overlay = new CheckBox { Text = "Enable audiobook in Fallout", AutoSize = true, Checked = true };
    private readonly NumericUpDown volume = new NumericUpDown { Minimum = 0, Maximum = 100, Value = 75, Width = 64 };
    private readonly NumericUpDown offset = new NumericUpDown {
        Minimum = -30, Maximum = 30, DecimalPlaces = 2, Increment = .10m, Width = 72
    };
    private readonly NumericUpDown horizontal = StyleNumber(-90, 90, 0, 5);
    private readonly NumericUpDown vertical = StyleNumber(-70, 90, 40, 5);
    private readonly NumericUpDown opacity = StyleNumber(0, 100, 100, 5);
    private readonly NumericUpDown lines = StyleNumber(1, 6, 4, 1);
    private readonly ComboBox subtitleLayout = new ComboBox { DropDownStyle = ComboBoxStyle.DropDownList, Width = 205 };
    private readonly Button play;
    private readonly Timer timer = new Timer { Interval = 16 };
    private readonly string settings;
    private readonly AudiobookLink link;
    private AudiobookLink labLink;
    private SubtitleStyle labStyle;
    private BookAudio player;
    private Task<BookAudio> opening;
    private AudiobookSubtitles subtitles;
    private string audio = "", srt = "", failure = "";
    private double position, duration;
    private bool opened, playing, scrubbing, updatingSeek, loading, disposed;
    private int? pausedAt;
    private DateTime lastSave;

    internal AudiobookTab(string root) : base("Audiobook")
    {
        Padding = new Padding(12);
        settings = Path.Combine(root, "Audiobook.txt");
        try { link = new AudiobookLink(); }
        catch (Exception error) { connection.Text = "VR connection unavailable · " + error.Message; }

        var layout = new TableLayoutPanel { ColumnCount = 1, Dock = DockStyle.Fill, AutoScroll = true };
        layout.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        Controls.Add(layout);
        Add(layout, FileRow("MP3", audioPath, "Browse…", ChooseAudio));
        Add(layout, FileRow("SRT", subtitlePath, "Browse…", ChooseSubtitles, ClearSubtitles));
        Add(layout, Row(overlay));
        play = MakeButton("Play", Toggle);
        Add(layout, Row(play, MakeButton("Back 15s", () => Jump(position - 15)),
            MakeButton("Forward 15s", () => Jump(position + 15)), time,
            new Label { Text = "Volume %", AutoSize = true, Padding = new Padding(12, 7, 0, 0) }, volume));
        Add(layout, seek);
        Add(layout, Row(FieldLabel("Delay (s)"), offset, FieldLabel("X"), horizontal,
            FieldLabel("Y"), vertical, FieldLabel("Opacity %"), opacity, FieldLabel("Box lines"), lines,
            MakeButton("Reset", () => ApplyStyle(new SubtitleStyle()))));
        subtitleLayout.Items.AddRange(SubtitleStyle.Layouts);
        subtitleLayout.SelectedIndex = 0;
        Add(layout, Row(FieldLabel("Subtitle layout"), subtitleLayout));
        Add(layout, state);
        Add(layout, connection);
        preview.MinimumSize = new Size(0, 42);
        layout.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
        layout.Controls.Add(preview, 0, layout.RowCount++);
        volume.ValueChanged += (sender, args) => {
            if (player != null) player.Volume((float)volume.Value / 100);
            if (!loading) Save();
        };
        overlay.CheckedChanged += (sender, args) => { if (!loading) { RefreshPlayback(); Save(); } };
        foreach (var setting in new[] { offset, horizontal, vertical, opacity, lines })
            setting.ValueChanged += (sender, args) => { if (!loading) { RefreshPlayback(); Save(); } };
        subtitleLayout.SelectedIndexChanged += (sender, args) => { if (!loading) { RefreshPlayback(); Save(); } };
        seek.MouseDown += (sender, args) => { scrubbing = opened; };
        seek.MouseUp += (sender, args) => { scrubbing = false; SeekFromTrack(); };
        seek.ValueChanged += (sender, args) => {
            if (updatingSeek || !opened) return;
            if (scrubbing) ShowTime(duration * seek.Value / seek.Maximum);
            else SeekFromTrack();
        };
        timer.Tick += (sender, args) => RefreshPlayback();
        LoadSettings();
        RefreshPlayback();
        timer.Start();
    }

    private static NumericUpDown StyleNumber(int low, int high, int value, int increment)
    {
        return new NumericUpDown { Minimum = low, Maximum = high, Value = value, Increment = increment, Width = 54 };
    }

    private static Label FieldLabel(string title)
    {
        return new Label { Text = title, AutoSize = true, Padding = new Padding(4, 5, 0, 0) };
    }

    internal SubtitleStyle Style
    {
        get { return new SubtitleStyle { X = (int)horizontal.Value, Y = (int)vertical.Value, Opacity = (int)opacity.Value,
            Lines = (int)lines.Value, Layout = subtitleLayout.SelectedIndex }; }
    }

    internal void ApplyStyle(SubtitleStyle style)
    {
        var wasLoading = loading;
        loading = true;
        horizontal.Value = style.X;
        vertical.Value = style.Y;
        opacity.Value = style.Opacity;
        lines.Value = style.Lines;
        subtitleLayout.SelectedIndex = style.Layout;
        loading = wasLoading;
        if (!loading) { RefreshPlayback(); Save(); }
    }

    internal void BeginLab()
    {
        labStyle = Style;
        labLink = new AudiobookLink(true);
        RefreshPlayback();
    }

    internal void EndLab()
    {
        if (labLink == null) return;
        labLink.Dispose();
        labLink = null;
        labStyle = null;
    }

    private static void Add(TableLayoutPanel layout, Control control)
    {
        layout.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        layout.Controls.Add(control, 0, layout.RowCount++);
    }

    private static Control Row(params Control[] controls)
    {
        var row = new FlowLayoutPanel { Dock = DockStyle.Fill, AutoSize = true, WrapContents = false, Margin = Padding.Empty };
        row.Controls.AddRange(controls);
        return row;
    }

    private Button MakeButton(string caption, Action action)
    {
        var button = new Button { Text = caption, AutoSize = true };
        button.Click += (sender, args) => {
            try { action(); }
            catch (Exception error) { failure = error.Message; RefreshPlayback(); }
        };
        return button;
    }

    private Control FileRow(string label, TextBox path, string caption, Action action, Action clear = null)
    {
        var row = new TableLayoutPanel { ColumnCount = clear == null ? 3 : 4, Height = 37, Dock = DockStyle.Fill, Margin = Padding.Empty };
        row.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 52));
        row.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        row.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 96));
        row.Controls.Add(new Label { Text = label, AutoSize = true, Padding = new Padding(3, 6, 0, 0) }, 0, 0);
        row.Controls.Add(path, 1, 0);
        row.Controls.Add(MakeButton(caption, action), 2, 0);
        if (clear != null)
        {
            row.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
            row.Controls.Add(MakeButton("Clear SRT", clear), 3, 0);
        }
        return row;
    }

    private void ChooseAudio()
    {
        using (var dialog = new OpenFileDialog { Filter = "Audiobook MP3 (*.mp3)|*.mp3", FileName = audio })
        {
            if (dialog.ShowDialog(this) != DialogResult.OK) return;
            var path = dialog.FileName;
            var match = Path.ChangeExtension(path, ".srt");
            bool sameBook = string.Equals(audio, path, StringComparison.OrdinalIgnoreCase);
            Open(path, File.Exists(match) ? match : sameBook ? srt : "", sameBook ? position : 0);
            if (!sameBook) offset.Value = 0;
            Save();
        }
    }

    private void ChooseSubtitles()
    {
        using (var dialog = new OpenFileDialog { Filter = "SubRip subtitles (*.srt)|*.srt", FileName = srt })
        {
            if (dialog.ShowDialog(this) != DialogResult.OK) return;
            var parsed = AudiobookSubtitles.Read(dialog.FileName);
            failure = "";
            subtitlePath.Text = srt = dialog.FileName;
            subtitles = parsed;
            RefreshPlayback();
            Save();
        }
    }

    private void ClearSubtitles()
    {
        if (srt.Length == 0) return;
        if (failure.StartsWith("SRT · ", StringComparison.Ordinal)) failure = "";
        subtitlePath.Text = srt = "";
        subtitles = null;
        RefreshPlayback();
        Save();
    }

    private void Open(string audioFile, string subtitleFile, double resume)
    {
        ClosePlayer();
        failure = "";
        position = resume;
        audioPath.Text = audio = audioFile;
        subtitlePath.Text = srt = subtitleFile;
        subtitles = null;
        if (srt.Length != 0)
        {
            try { subtitles = AudiobookSubtitles.Read(srt); }
            catch (Exception error) { failure = "SRT · " + error.Message; }
        }
        if (audio.Length == 0) return;
        if (!File.Exists(audio))
        {
            failure = "The selected MP3 is unavailable. Choose its current location.";
            return;
        }

        opening = Task.Run(() => BookAudio.Open(audioFile));
    }

    private void CompleteOpen()
    {
        if (opening == null || !opening.IsCompleted) return;
        var pending = opening;
        opening = null;
        player = pending.GetAwaiter().GetResult();
        player.Volume((float)volume.Value / 100);
        duration = player.Duration;
        opened = true;
        Jump(position < duration ? position : 0);
    }

    private void ClosePlayer()
    {
        if (opening != null) opening.ContinueWith(task => {
            if (task.Status == TaskStatus.RanToCompletion) task.Result.Dispose();
            else if (task.IsFaulted) GC.KeepAlive(task.Exception);
        }, TaskScheduler.Default);
        opening = null;
        if (player != null) player.Dispose();
        player = null;
        playing = opened = scrubbing = false;
        pausedAt = null;
        duration = 0;
    }

    private void Toggle()
    {
        if (!opened) return;
        if (playing)
        {
            player.Play(false);
            position = player.Position;
            pausedAt = Environment.TickCount;
        }
        else
        {
            if (duration > 0 && position >= duration - .03) Jump(0);
            player.Play(true);
            pausedAt = null;
        }
        playing = !playing;
        if (!playing) Save();
    }

    private void Jump(double seconds)
    {
        if (!opened) return;
        player.Seek(seconds);
        position = player.Position;
        if (playing) player.Play(true);
    }

    private void SeekFromTrack()
    {
        if (opened && duration > 0) Jump(duration * seek.Value / seek.Maximum);
    }

    private static string FormatTime(double seconds)
    {
        var span = TimeSpan.FromSeconds(Math.Max(0, seconds));
        return string.Format(CultureInfo.InvariantCulture, "{0}:{1:00}:{2:00}", (int)span.TotalHours, span.Minutes, span.Seconds);
    }

    private void ShowTime(double seconds)
    {
        time.Text = FormatTime(seconds) + " / " + FormatTime(duration);
    }

    private void RefreshPlayback()
    {
        try
        {
            CompleteOpen();
            if (playing)
            {
                player.Pump();
                position = player.Position;
                if (position >= duration) { Jump(0); Save(); }
            }
        }
        catch (Exception error)
        {
            failure = "MP3 · " + error.Message;
            ClosePlayer();
        }
        var style = Style;
        var cueTime = position - (double)offset.Value;
        var caption = opened && subtitles != null ? subtitles.At(cueTime, style.Lines, style.Layout) : "";
        const int FadeDelayMs = 5000;
        const int FadeDurationMs = 350;
        uint pausedMs = pausedAt.HasValue ? unchecked((uint)(Environment.TickCount - pausedAt.Value)) : 0;
        int fade = pausedMs <= FadeDelayMs ? 0 :
            (int)(Math.Min(pausedMs - FadeDelayMs, FadeDurationMs) * 100 / FadeDurationMs);
        string vrState;
        bool ready = overlay.Checked && opened;
        if (link != null)
        {
            if (link.Update(ready, caption, style, fade, out vrState))
            {
                try { Toggle(); }
                catch (Exception error) { failure = error.Message; }
            }
            connection.Text = overlay.Checked ? (vrState ?? "Waiting for Fallout · Keep StrideXR open · Left Menu plays or pauses") :
                "Fallout audiobook disabled";
        }
        if (labLink != null)
        {
            labStyle = labLink.PreviewStyle(labStyle);
            var labCaption = opened && subtitles != null ? subtitles.At(cueTime, labStyle.Lines, labStyle.Layout) : "";
            if (labLink.Update(opened, labCaption, labStyle, fade, out vrState))
            {
                try { Toggle(); }
                catch (Exception error) { failure = error.Message; }
            }
            connection.Text = vrState ?? "Waiting for VR Lab";
        }
        preview.Text = caption.Replace("\u0001", "");
        play.Enabled = opened;
        play.Text = playing ? "Pause" : "Play";
        seek.Enabled = opened && duration > 0;
        if (!scrubbing)
        {
            updatingSeek = true;
            seek.Value = duration > 0 ? (int)Math.Max(0, Math.Min(seek.Maximum, position / duration * seek.Maximum)) : 0;
            updatingSeek = false;
            ShowTime(position);
        }
        if (failure.Length != 0) state.Text = failure;
        else if (audio.Length == 0) state.Text = "Choose an MP3 to get started.";
        else if (!opened) state.Text = "Indexing MP3 frames…";
        else state.Text = (playing ? "Playing" : "Paused") + (subtitles == null
            ? " · Audio only"
            : " · " + subtitles.Count.ToString("N0") + " subtitle cues");
        if (playing && (DateTime.UtcNow - lastSave).TotalSeconds >= 5) Save();
    }

    private void LoadSettings()
    {
        loading = true;
        try
        {
            if (!File.Exists(settings)) return;
            var lines = File.ReadAllLines(settings);
            if (lines.Length < 5) return;
            double resume;
            int level;
            if (!double.TryParse(lines[2], NumberStyles.Float, CultureInfo.InvariantCulture, out resume) ||
                double.IsNaN(resume) || double.IsInfinity(resume) || resume < 0 || resume > 36000000) resume = 0;
            if (int.TryParse(lines[3], out level)) volume.Value = Math.Max(0, Math.Min(100, level));
            overlay.Checked = lines[4] == "1";
            if (lines.Length > 5) ApplyStyle(SubtitleStyle.Parse(lines[5]));
            decimal delay;
            if (lines.Length > 6 && decimal.TryParse(lines[6], NumberStyles.Number, CultureInfo.InvariantCulture, out delay))
                offset.Value = Math.Max(offset.Minimum, Math.Min(offset.Maximum, delay));
            Open(lines[0], lines[1], resume);
        }
        catch (Exception error) { failure = error.Message; }
        finally { loading = false; }
    }

    private void Save()
    {
        lastSave = DateTime.UtcNow;
        var temporary = settings + ".tmp";
        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(settings));
            File.WriteAllLines(temporary, new[] { audio, srt, position.ToString("R", CultureInfo.InvariantCulture),
                volume.Value.ToString(CultureInfo.InvariantCulture), overlay.Checked ? "1" : "0", Style.ToString(),
                offset.Value.ToString(CultureInfo.InvariantCulture) }, new UTF8Encoding(false));
            if (File.Exists(settings)) File.Replace(temporary, settings, null);
            else File.Move(temporary, settings);
        }
        catch (IOException error) { failure = "Could not save audiobook settings · " + error.Message; }
        catch (UnauthorizedAccessException error) { failure = "Could not save audiobook settings · " + error.Message; }
    }

    protected override void Dispose(bool disposing)
    {
        if (disposing && !disposed)
        {
            disposed = true;
            timer.Dispose();
            if (playing) position = player.Position;
            Save();
            ClosePlayer();
            EndLab();
            if (link != null) link.Dispose();
        }
        base.Dispose(disposing);
    }
}
