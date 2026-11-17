using System;
using System.Collections;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Threading;
using System.Windows.Forms;

internal sealed class ControlPanel : Form
{
    private readonly ProfileStore store = new ProfileStore(Install.Root, Install.Ini);
    private readonly LaunchMonitor launches = new LaunchMonitor(Install.Root);
    private readonly ComboBox profiles = new ComboBox { DropDownStyle = ComboBoxStyle.DropDownList, Width = 230 };
    private readonly TextBox name = new TextBox();
    private readonly ComboBox gate = new ComboBox { DropDownStyle = ComboBoxStyle.DropDownList };
    private readonly Dictionary<string, NumericUpDown> tuning = new Dictionary<string, NumericUpDown>();
    private readonly Dictionary<string, decimal> defaults = new Dictionary<string, decimal>();
    private readonly CheckBox global = new CheckBox { Text = "Use the default profile for unassigned OpenXR games", AutoSize = true };
    private readonly CheckBox enabledBox = new CheckBox { Text = "Enabled", AutoSize = true, Margin = new Padding(3, 8, 12, 3) };
    private readonly CheckBox reverse = new CheckBox { Text = "Reverse with both controllers upside down by your head", AutoSize = true };
    private readonly Label gameFolder = new Label { AutoSize = false, AutoEllipsis = true, Dock = DockStyle.Fill, Height = 25 };
    private readonly ListBox targets = new ListBox { Dock = DockStyle.Fill, HorizontalScrollbar = true };
    private readonly TextBox filter = new TextBox { Width = 165 };
    private readonly CheckBox showHidden = new CheckBox { Text = "Show hidden", AutoSize = true, Margin = new Padding(8, 7, 3, 3) };
    private Button hideSelected;
    private readonly ListView games = new ListView {
        View = View.Details, FullRowSelect = true, MultiSelect = false, HideSelection = false, Dock = DockStyle.Fill
    };
    private readonly TextBox status = new TextBox {
        Dock = DockStyle.Fill, Multiline = true, ReadOnly = true, ScrollBars = ScrollBars.Vertical, Font = new Font("Consolas", 10)
    };
    private readonly Label banner = new Label { AutoSize = true };
    private readonly Label notice = new Label { Dock = DockStyle.Fill, AutoSize = true };
    private readonly Label debugStatus = new Label { AutoSize = true, Dock = DockStyle.Top };
    private readonly Label calibrationStatus = new Label { AutoSize = true, Dock = DockStyle.Top };
    private readonly CheckBox keepFast = new CheckBox { Text = "Keep the selected profile’s current full-speed setting", AutoSize = true, Checked = true };
    private Button applyCalibration;
    private readonly TabControl tabs = new DarkTheme.Navigation { Dock = DockStyle.Fill };
    private readonly AudiobookTab audiobook;
    private readonly System.Windows.Forms.Timer timer = new System.Windows.Forms.Timer { Interval = 650 };
    private Launch[] current = new Launch[0];
    private int sortColumn = -1;
    private bool descending;
    private EventWaitHandle captureEvent;
    private DateTime captureSince;
    private DiagnosticsSession diagnostics;
    private string lastDebugFolder;
    private CalibrationRun calibration;
    private Dictionary<string, decimal> measured;
    private bool updatingEnabledBox;
    private Profile Selected { get { return (Profile)profiles.SelectedItem; } }

    internal ControlPanel()
    {
        Text = "StrideXR " + Install.Version;
        Font = new Font("Segoe UI", 10);
        AutoScaleMode = AutoScaleMode.Font;
        StartPosition = FormStartPosition.CenterScreen;
        ClientSize = new Size(930, 690);
        MinimumSize = new Size(815, 630);

        var layout = Table(1);
        layout.Dock = DockStyle.Fill;
        layout.Padding = new Padding(16);
        layout.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        layout.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        layout.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        layout.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
        layout.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        layout.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        Controls.Add(layout);
        banner.Font = new Font("Segoe UI Semibold", 19);
        layout.Controls.Add(banner, 0, 0);
        layout.Controls.Add(Row(new Label { Text = "Profile", AutoSize = true, Padding = new Padding(0, 6, 0, 0) },
            profiles, Button("New profile", NewProfile), Button("Rename", RenameProfile), Button("Delete", DeleteProfile),
            Button("Set as default", SetDefault)), 0, 1);
        layout.Controls.Add(global, 0, 2);
        layout.Controls.Add(tabs, 0, 3);
        layout.Controls.Add(notice, 0, 4);
        layout.Controls.Add(Row(enabledBox,
            Button("Open data folder", () => { Directory.CreateDirectory(Install.Root); Process.Start("explorer.exe", "\"" + Install.Root + "\""); })), 0, 5);

        BuildGamesTab();
        BuildTuningTab();
        audiobook = new AudiobookTab(Install.Root);
        tabs.TabPages.Add(audiobook);
        BuildCalibrationTab();
        var monitor = new TabPage("Input monitor") { Padding = new Padding(10) };
        monitor.Controls.Add(status);
        tabs.TabPages.Add(monitor);
        BuildDiagnosticsTab();
        DarkTheme.Apply(this);

        global.Checked = store.Global;
        enabledBox.Checked = Install.Enabled();
        enabledBox.CheckedChanged += (sender, args) => {
            if (!updatingEnabledBox) Run(UpdateInstallation);
        };
        global.CheckedChanged += (sender, args) => Run(() => {
            store.Global = global.Checked;
            store.Save();
            SetNotice("Scope saved. Relaunch any open VR game to apply it.");
        });
        profiles.SelectedIndexChanged += (sender, args) => ShowProfile();
        ReloadProfiles(store.DefaultId);
        SetNotice("Click Refresh to list running and recent launches. OpenXR sessions appear first.");
        Shown += (sender, args) => { launches.Start(); RefreshActivity(); timer.Start(); };
        timer.Tick += (sender, args) => RefreshActivity();
        FormClosed += (sender, args) => {
            timer.Dispose();
            launches.Dispose();
            if (captureEvent != null) captureEvent.Dispose();
            if (diagnostics != null) Run(StopDiagnostics);
            if (calibration != null) Run(CancelCalibration);
        };
    }

    private static TableLayoutPanel Table(int columns)
    {
        return new TableLayoutPanel { ColumnCount = columns, GrowStyle = TableLayoutPanelGrowStyle.AddRows };
    }

    private static FlowLayoutPanel Row(params Control[] controls)
    {
        var row = new FlowLayoutPanel { AutoSize = true, WrapContents = false, Dock = DockStyle.Fill };
        row.Controls.AddRange(controls);
        return row;
    }

    private Button Button(string caption, Action action)
    {
        var button = new Button { Text = caption, AutoSize = true };
        button.Click += (sender, args) => Run(action);
        return button;
    }

    private void Run(Action action)
    {
        try { action(); }
        catch (Exception error) { SetNotice(error.Message); }
    }

    private void BuildGamesTab()
    {
        var page = new TabPage("Games") { Padding = new Padding(10) };
        var layout = Table(1);
        layout.Dock = DockStyle.Fill;
        layout.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        layout.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        layout.RowStyles.Add(new RowStyle(SizeType.Percent, 72));
        layout.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        layout.RowStyles.Add(new RowStyle(SizeType.Percent, 28));
        layout.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        layout.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        layout.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        page.Controls.Add(layout);
        layout.Controls.Add(Row(Button("Refresh", RefreshLaunchList),
            new Label { Text = "Filter", AutoSize = true, Padding = new Padding(0, 6, 0, 0) },
            filter, showHidden), 0, 0);
        hideSelected = Button("Hide selected", ToggleSelectedVisibility);
        layout.Controls.Add(Row(hideSelected, Button("Hide all listed", HideListed), Button("Detect next game", ArmSelected), Button("Cancel detection", () => {
                CancelCapture();
                SetNotice("Automatic detection canceled. You can still pick an executable below.");
            })), 0, 1);
        games.Columns.Add("Executable", 180);
        games.Columns.Add("Activity", 175);
        games.Columns.Add("OpenXR name or executable path", 400);
        games.ColumnClick += (sender, args) => SortLaunches(args.Column);
        games.ShowItemToolTips = true;
        layout.Controls.Add(games, 0, 2);
        layout.Controls.Add(new Label { Text = "Executables assigned to this profile", AutoSize = true }, 0, 3);
        layout.Controls.Add(targets, 0, 4);
        layout.Controls.Add(Row(Button("Assign selected EXE", AssignSelected), Button("Browse for EXE", Browse),
            Button("Remove assignment", RemoveTarget)), 0, 5);
        layout.Controls.Add(gameFolder, 0, 6);
        layout.Controls.Add(Row(Button("Open game folder", OpenGameFolder), Button("Choose game folder", ChooseGameFolder)), 0, 7);
        filter.TextChanged += (sender, args) => ShowLaunches();
        showHidden.CheckedChanged += (sender, args) => ShowLaunches();
        games.SelectedIndexChanged += (sender, args) => UpdateHideButton();
        UpdateHideButton();
        tabs.TabPages.Add(page);
    }

    private void BuildTuningTab()
    {
        var page = new TabPage("Swing settings") { Padding = new Padding(10) };
        var layout = Table(1);
        layout.Dock = DockStyle.Fill;
        layout.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
        layout.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        var scroller = new Panel { Dock = DockStyle.Fill, AutoScroll = true };
        page.Controls.Add(layout);
        layout.Controls.Add(scroller, 0, 0);
        layout.Controls.Add(Row(Button("Save profile", SaveProfile)), 0, 1);
        var grid = Table(2);
        grid.Dock = DockStyle.Top;
        grid.AutoSize = true;
        grid.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 270));
        grid.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        scroller.Controls.Add(grid);
        name.Width = 260;
        gate.Items.AddRange(new object[] { "Both", "Either", "Left", "Right" });
        Field(grid, "Profile name", name);
        Field(grid, "Thumbrests to touch", gate);
        Field(grid, "Reverse waggle", reverse);
        Number(grid, "Start swing threshold (m/s)", "StartSpeed", .03m, 3.10m, .24m);
        Number(grid, "Slow arm speed (m/s, 0 = auto)", "SlowSpeed", 0m, 3.20m, 0m);
        Number(grid, "Medium arm speed (m/s, 0 = auto)", "MidSpeed", 0m, 3.20m, 0m);
        Number(grid, "Fast arm speed (m/s)", "FullSpeed", .15m, 3.20m, 1.10m);
        Number(grid, "Minimum stick input (0–1)", "MinimumStick", .05m, 1m, .38m);
        Number(grid, "Medium stick input (0 = auto)", "MidStick", 0m, 1m, 0m);
        Number(grid, "Maximum stick input (0–1)", "MaximumStick", .05m, 1m, 1m);
        Number(grid, "Base peak hold (s)", "PeakHold", .15m, 2m, .467m);
        Number(grid, "Slowdown smoothing (s)", "Slowdown", 0m, 1m, .10m);
        Number(grid, "Pause before stopping (s)", "StopDelay", 0m, 2m, .50m);
        Number(grid, "Stop fade (s)", "StopFade", 0m, 2m, .33m);
        Number(grid, "Top acceleration (stick ranges/s)", "TopAcceleration", .10m, 5m, .80m);
        Number(grid, "Swing speed smoothing (s)", "Smoothing", .01m, .50m, .085m);
        Number(grid, "Start confirmation (s)", "StartConfirmation", 0m, .50m, .06m);
        tabs.TabPages.Add(page);
    }

    private void BuildCalibrationTab()
    {
        var page = new TabPage("VR Lab") { Padding = new Padding(16) };
        var layout = Table(1);
        layout.Dock = DockStyle.Fill;
        page.Controls.Add(layout);
        layout.Controls.Add(new Label {
            Text = "Close your VR game, then open the 3D test scene to try forward and reverse motion and set up your subtitle panel.",
            MaximumSize = new Size(660, 0), AutoSize = true, Padding = new Padding(3, 3, 3, 7)
        }, 0, 0);
        layout.Controls.Add(Row(Button("Open 3D test scene", () => StartCalibration(true)),
            Button("Start calibration", () => StartCalibration(false)), Button("Close VR session", CancelCalibration)), 0, 1);
        calibrationStatus.Text = "In the headset, press A to record each pace. Press B to repeat a step.";
        layout.Controls.Add(calibrationStatus, 0, 2);
        layout.Controls.Add(keepFast, 0, 3);
        applyCalibration = Button("Apply to selected profile", ApplyCalibration);
        applyCalibration.Enabled = false;
        layout.Controls.Add(Row(applyCalibration), 0, 4);
        tabs.TabPages.Add(page);
    }

    private void StartCalibration(bool scene)
    {
        if (calibration != null) throw new InvalidOperationException("Calibration is already running.");
        if (scene) audiobook.BeginLab();
        try { calibration = new CalibrationRun(Selected, scene ? audiobook.Style : null); }
        catch { if (scene) audiobook.EndLab(); throw; }
        measured = null;
        applyCalibration.Enabled = false;
        calibrationStatus.Text = scene ? "3D test scene opened. In VR, press B to save the subtitle panel and exit." :
            "Calibration opened. Put on your headset and follow the slow, medium, and fast prompts.";
    }

    private void CancelCalibration()
    {
        if (calibration == null) return;
        var run = calibration;
        calibration = null;
        run.Dispose();
        if (run.IsScene) audiobook.EndLab();
        calibrationStatus.Text = run.IsScene ? "Test scene closed." : "Calibration canceled.";
    }

    private void FinishCalibration()
    {
        if (calibration == null || !calibration.Finished) return;
        var run = calibration;
        calibration = null;
        if (run.IsScene) audiobook.EndLab();
        using (run)
        {
            try
            {
                if (run.IsScene)
                {
                    var style = run.ReadStyle();
                    if (style != null) audiobook.ApplyStyle(style);
                    calibrationStatus.Text = style == null ? "Test scene closed." : "Subtitle panel settings saved.";
                    return;
                }
                measured = run.ReadResult();
                if (measured == null) { calibrationStatus.Text = "Calibration closed before measurements were saved."; return; }
                calibrationStatus.Text = string.Format(CultureInfo.CurrentCulture,
                    "Measured arm speed   Slow {0:0.00}   Medium {1:0.00}   Fast {2:0.00} m/s",
                    measured["SlowSpeed"], measured["MidSpeed"], measured["FullSpeed"]);
                applyCalibration.Enabled = true;
            }
            catch (Exception error) { calibrationStatus.Text = error.Message; }
        }
    }

    private void ApplyCalibration()
    {
        if (measured == null) return;
        var profile = Selected;
        decimal fullSpeed;
        if (!decimal.TryParse(profile.Value("FullSpeed"), NumberStyles.Number, CultureInfo.InvariantCulture, out fullSpeed))
            fullSpeed = defaults["FullSpeed"];
        if (keepFast.Checked && (measured["MidSpeed"] + .01m >= fullSpeed || measured["StartSpeed"] + .10m > fullSpeed))
            throw new ArgumentException("The measured medium pace is too close to the current fast pace. Clear the keep-fast option or adjust the fast pace.");
        var previous = new Dictionary<string, string>(profile.Values);
        foreach (var item in measured)
            if (item.Key != "FullSpeed" || !keepFast.Checked)
                profile.Values[item.Key] = item.Value.ToString("0.###", CultureInfo.InvariantCulture);
        try { store.Save(); }
        catch
        {
            profile.Values.Clear();
            foreach (var item in previous) profile.Values[item.Key] = item.Value;
            throw;
        }
        ShowProfile();
        SetNotice("Calibration saved to " + profile.Name + ". Relaunch any open VR game.");
    }

    private void BuildDiagnosticsTab()
    {
        var page = new TabPage("Diagnostics") { Padding = new Padding(12) };
        var layout = Table(1);
        layout.Dock = DockStyle.Fill;
        page.Controls.Add(layout);
        layout.Controls.Add(new Label {
            Text = "Start capture before launching the game. Try arm swing in VR, then stop capture.",
            AutoSize = true, Padding = new Padding(3, 3, 3, 10)
        }, 0, 0);
        layout.Controls.Add(Row(Button("Start capture", StartDiagnostics), Button("Stop capture", StopDiagnostics),
            Button("Open capture folder", OpenDiagnostics)), 0, 1);
        debugStatus.Text = "Captures stay on this PC and stop automatically after five minutes.";
        layout.Controls.Add(debugStatus, 0, 2);
        tabs.TabPages.Add(page);
    }

    private void StartDiagnostics()
    {
        if (diagnostics != null) throw new InvalidOperationException("A capture is already running.");
        diagnostics = new DiagnosticsSession(Install.Root);
        lastDebugFolder = diagnostics.Folder;
        debugStatus.Text = "Capturing. Launch the VR game now.";
    }

    private void StopDiagnostics()
    {
        if (diagnostics == null) return;
        var recording = diagnostics;
        diagnostics = null;
        recording.Dispose();
        debugStatus.Text = "Capture saved. Open the capture folder to view the logs.";
    }

    private void OpenDiagnostics()
    {
        string folder = lastDebugFolder ?? Path.Combine(Install.Root, "Diagnostics");
        Directory.CreateDirectory(folder);
        Process.Start("explorer.exe", "\"" + folder + "\"");
    }

    private static void Field(TableLayoutPanel grid, string caption, Control control)
    {
        grid.Controls.Add(new Label { Text = caption, AutoSize = true, Anchor = AnchorStyles.Left });
        grid.Controls.Add(control);
    }

    private void Number(TableLayoutPanel grid, string caption, string key, decimal low, decimal high, decimal initial)
    {
        var control = new NumericUpDown {
            Minimum = low, Maximum = high, Value = initial, DecimalPlaces = 3, Increment = .01m, Width = 110
        };
        defaults.Add(key, initial);
        tuning.Add(key, control);
        Field(grid, caption, control);
    }

    private void ReloadProfiles(string id)
    {
        profiles.Items.Clear();
        profiles.Items.AddRange(store.Profiles.ToArray());
        profiles.SelectedItem = store.Profiles.First(profile => profile.Id == id);
    }

    private void ShowProfile()
    {
        if (Selected == null) return;
        name.Text = Selected.Name;
        var thumbrests = Selected.Value("Thumbrests", "Both");
        gate.SelectedItem = gate.Items.Contains(thumbrests) ? thumbrests : "Both";
        reverse.Checked = Selected.Value("ReverseWaggle", "1") != "0";
        foreach (var item in tuning)
        {
            decimal value;
            if (!decimal.TryParse(Selected.Value(item.Key), NumberStyles.Number, CultureInfo.InvariantCulture, out value))
                value = defaults[item.Key];
            item.Value.Value = Math.Min(item.Value.Maximum, Math.Max(item.Value.Minimum, value));
        }
        ShowTargets();
        var folder = Selected.Value("LaunchFolder");
        gameFolder.Text = folder.Length > 0 ? folder : "Choose a folder for this game’s launcher or shortcuts.";
    }

    private void ShowTargets()
    {
        targets.Items.Clear();
        targets.Items.AddRange(Selected.Targets.ToArray());
    }

    private void SaveProfile()
    {
        var label = ProfileStore.ValidateName(name.Text);
        if (tuning["FullSpeed"].Value < tuning["StartSpeed"].Value + .10m)
            throw new ArgumentException("Full speed must be at least 0.10 m/s above start speed.");
        if (tuning["MaximumStick"].Value < tuning["MinimumStick"].Value)
            throw new ArgumentException("Maximum stick must be at least as high as minimum stick.");
        var start = tuning["StartSpeed"].Value;
        var slow = tuning["SlowSpeed"].Value;
        var medium = tuning["MidSpeed"].Value;
        var fast = tuning["FullSpeed"].Value;
        var mediumStick = tuning["MidStick"].Value;
        if (slow > 0 && (slow < start || slow >= fast) || medium > 0 && (medium <= Math.Max(start, slow) || medium >= fast))
            throw new ArgumentException("Set slow, medium, and fast speeds in increasing order. Use zero for an automatic slow or medium point.");
        if (mediumStick > 0 && (mediumStick < tuning["MinimumStick"].Value || mediumStick > tuning["MaximumStick"].Value))
            throw new ArgumentException("Set medium stick within the minimum and maximum, or use zero for the midpoint.");
        var profile = Selected;
        profile.Name = label;
        profile.Values["Thumbrests"] = (string)gate.SelectedItem;
        profile.Values["ReverseWaggle"] = reverse.Checked ? "1" : "0";
        foreach (var item in tuning) profile.Values[item.Key] = item.Value.Value.ToString("0.###", CultureInfo.InvariantCulture);
        store.Save();
        ReloadProfiles(profile.Id);
        SetNotice("Profile saved. Relaunch any open VR game to apply its settings.");
    }

    private void SetDefault()
    {
        store.DefaultId = Selected.Id;
        store.Save();
        SetNotice(Selected.Name + " is the default profile.");
    }

    private void UpdateInstallation()
    {
        try
        {
            if (enabledBox.Checked) Install.Enable();
            else { CancelCapture(); Install.Disable(false); }
            SetNotice(enabledBox.Checked ? "Enabled. Relaunch any open VR game." : "Disabled. Relaunch any open VR game.");
        }
        finally { ReflectInstallation(); }
    }

    private void ReflectInstallation()
    {
        var enabled = Install.Enabled();
        updatingEnabledBox = true;
        try { enabledBox.Checked = enabled; }
        finally { updatingEnabledBox = false; }
        banner.Text = (enabled ? "StrideXR enabled" : "StrideXR disabled") + "   |   Default: " +
            store.Profiles.First(profile => profile.Id == store.DefaultId).Name;
    }

    private void ChooseGameFolder()
    {
        var profile = Selected;
        using (var dialog = new FolderBrowserDialog { Description = "Choose a folder with this game’s launcher or shortcuts.", ShowNewFolderButton = false })
        {
            var existing = profile.Value("LaunchFolder");
            if (Directory.Exists(existing)) dialog.SelectedPath = existing;
            if (dialog.ShowDialog(this) != DialogResult.OK) return;
            store.SetLaunchFolder(profile, dialog.SelectedPath);
        }
        ShowProfile();
        SetNotice("Game folder saved for " + profile.Name + ".");
    }

    private void OpenGameFolder()
    {
        var folder = Selected.Value("LaunchFolder");
        if (folder.Length == 0)
            folder = Selected.Targets.Where(Path.IsPathRooted).Select(Path.GetDirectoryName).FirstOrDefault(Directory.Exists);
        if (!Directory.Exists(folder)) throw new DirectoryNotFoundException("Choose an existing game folder for this profile.");
        Process.Start("explorer.exe", "\"" + folder + "\"");
    }

    private void NewProfile()
    {
        CancelCapture();
        var profile = new Profile("p" + Guid.NewGuid().ToString("N"), "New profile");
        store.Profiles.Add(profile);
        store.Save();
        ReloadProfiles(profile.Id);
        tabs.SelectedIndex = 0;
        ArmSelected();
    }

    private void RenameProfile()
    {
        var profile = Selected;
        using (var dialog = new Form {
            Text = "Rename profile", StartPosition = FormStartPosition.CenterParent, FormBorderStyle = FormBorderStyle.FixedDialog,
            MinimizeBox = false, MaximizeBox = false, ShowInTaskbar = false, ClientSize = new Size(390, 105), Font = Font
        })
        {
            var entry = new TextBox { Text = profile.Name, MaxLength = 100, Dock = DockStyle.Top };
            var apply = new Button { Text = "Rename", AutoSize = true };
            var cancel = new Button { Text = "Cancel", AutoSize = true, DialogResult = DialogResult.Cancel };
            var layout = Table(1);
            layout.Dock = DockStyle.Fill;
            layout.Padding = new Padding(12);
            layout.Controls.Add(entry, 0, 0);
            layout.Controls.Add(Row(apply, cancel), 0, 1);
            dialog.Controls.Add(layout);
            dialog.AcceptButton = apply;
            dialog.CancelButton = cancel;
            DarkTheme.Apply(dialog);
            apply.Click += (sender, args) => {
                try { store.Rename(profile, entry.Text); dialog.DialogResult = DialogResult.OK; }
                catch (Exception error) { MessageBox.Show(dialog, error.Message, "Rename profile", MessageBoxButtons.OK, MessageBoxIcon.Warning); }
            };
            if (dialog.ShowDialog(this) != DialogResult.OK) return;
        }
        ReloadProfiles(profile.Id);
        SetNotice("Profile renamed to " + profile.Name + ".");
    }

    private void DeleteProfile()
    {
        var profile = Selected;
        if (store.Profiles.Count == 1) throw new InvalidOperationException("Keep at least one profile.");
        if (MessageBox.Show(this, "Delete “" + profile.Name + "” and remove its executable assignments?", "Delete profile",
            MessageBoxButtons.YesNo, MessageBoxIcon.Question, MessageBoxDefaultButton.Button2) != DialogResult.Yes) return;
        if (store.CaptureId == profile.Id) CancelCapture();
        store.Delete(profile);
        ReloadProfiles(store.DefaultId);
        SetNotice("Profile deleted. Relaunch any affected VR game.");
    }

    private void ArmSelected()
    {
        if (!Install.Enabled())
        {
            SetNotice("Enable StrideXR to detect the next OpenXR game. You can also assign an EXE from the launch list.");
            return;
        }
        CancelCapture();
        captureSince = DateTime.UtcNow;
        captureEvent = new EventWaitHandle(false, EventResetMode.ManualReset, @"Local\StrideXR-" + Selected.Id);
        store.CaptureId = Selected.Id;
        try { store.Save(); }
        catch
        {
            captureEvent.Dispose();
            captureEvent = null;
            store.CaptureId = "";
            throw;
        }
        SetNotice("Waiting for the next unassigned OpenXR VR session. Launch the game with StrideXR open.");
    }

    private void CancelCapture()
    {
        if (captureEvent == null) return;
        captureEvent.Dispose();
        captureEvent = null;
        store.CaptureId = "";
        store.Save();
    }

    private void AssignSelected()
    {
        if (games.SelectedItems.Count == 0) throw new ArgumentException("Select an executable in the launch list.");
        Assign(((Launch)games.SelectedItems[0].Tag).Target);
    }

    private void Browse()
    {
        using (var dialog = new OpenFileDialog { Filter = "Windows executables (*.exe)|*.exe", CheckFileExists = true })
            if (dialog.ShowDialog(this) == DialogResult.OK) Assign(dialog.FileName);
    }

    private void Assign(string target)
    {
        CancelCapture();
        store.Assign(Selected, target);
        store.Save();
        ShowTargets();
        SetNotice("Assigned " + Path.GetFileName(target) + ". Relaunch the game if it is open.");
    }

    private void RemoveTarget()
    {
        if (targets.SelectedItem == null) throw new ArgumentException("Select an assigned executable first.");
        Selected.Targets.Remove((string)targets.SelectedItem);
        store.Save();
        ShowTargets();
        SetNotice("Assignment removed. Relaunch any open VR game to apply the change.");
    }

    private void SetNotice(string message) { notice.Text = message; }

    private void RefreshActivity()
    {
        try
        {
            ReflectInstallation();
            FinishCalibration();
            var observed = captureEvent != null || diagnostics != null ? launches.ReadLaunches() : null;
            if (diagnostics != null)
            {
                diagnostics.Observe(observed, store.Profiles.SelectMany(profile => profile.Targets));
                if (diagnostics.Expired) StopDiagnostics();
            }
            if (captureEvent != null)
            {
                var found = observed.Where(launch => launch.IsVr && launch.Capture == store.CaptureId && launch.Time >= captureSince)
                    .OrderBy(launch => launch.Time).FirstOrDefault();
                if (found != null)
                {
                    var profile = store.Profiles.First(item => item.Id == store.CaptureId);
                    if (profile.Name == "New profile") profile.Name = string.IsNullOrWhiteSpace(found.Application)
                        ? Path.GetFileNameWithoutExtension(found.Exe) : new string(found.Application.Where(c => !char.IsControl(c) && c != '"').Take(100).ToArray());
                    if (string.IsNullOrWhiteSpace(profile.Name)) profile.Name = Path.GetFileNameWithoutExtension(found.Exe);
                    store.Assign(profile, found.Target);
                    CancelCapture();
                    ReloadProfiles(profile.Id);
                    SetNotice("Detected and assigned " + found.Exe + " to " + profile.Name + ".");
                }
            }
            RefreshStatus();
        }
        catch (IOException error) { SetNotice(error.Message); }
        catch (UnauthorizedAccessException) { SetNotice("The StrideXR data folder is inaccessible."); }
    }

    private void RefreshLaunchList()
    {
        current = launches.ReadLaunches();
        ShowLaunches();
        if (captureEvent == null) SetNotice("Launch list refreshed.");
    }

    private void UpdateHideButton()
    {
        var selected = games.SelectedItems.Count > 0 ? (Launch)games.SelectedItems[0].Tag : null;
        hideSelected.Enabled = selected != null;
        hideSelected.Text = selected != null && store.HiddenExecutables.Contains(selected.Exe) ? "Unhide selected" : "Hide selected";
    }

    private void ToggleSelectedVisibility()
    {
        if (games.SelectedItems.Count == 0) return;
        var executable = ((Launch)games.SelectedItems[0].Tag).Exe;
        bool hiding = store.HiddenExecutables.Add(executable);
        if (!hiding) store.HiddenExecutables.Remove(executable);
        try { store.Save(); }
        catch
        {
            if (hiding) store.HiddenExecutables.Remove(executable);
            else store.HiddenExecutables.Add(executable);
            throw;
        }
        ShowLaunches();
    }

    private void HideListed()
    {
        var names = games.Items.Cast<ListViewItem>().Select(row => ((Launch)row.Tag).Exe)
            .Distinct(StringComparer.OrdinalIgnoreCase).Where(exe => !store.HiddenExecutables.Contains(exe)).ToArray();
        if (names.Length == 0) return;
        store.HiddenExecutables.UnionWith(names);
        try { store.Save(); }
        catch
        {
            store.HiddenExecutables.ExceptWith(names);
            throw;
        }
        if (showHidden.Checked) showHidden.Checked = false;
        else ShowLaunches();
        SetNotice("Hidden " + names.Length + " executable names. Launch the game and click Refresh.");
    }

    private sealed class LaunchColumnComparer : IComparer
    {
        private readonly int column;
        private readonly bool descending;
        internal LaunchColumnComparer(int column, bool descending) { this.column = column; this.descending = descending; }

        public int Compare(object first, object second)
        {
            var a = (ListViewItem)first;
            var b = (ListViewItem)second;
            int primary = StringComparer.CurrentCultureIgnoreCase.Compare(a.SubItems[column].Text, b.SubItems[column].Text);
            return primary != 0 ? (descending ? -primary : primary) : StringComparer.CurrentCultureIgnoreCase.Compare(a.Text, b.Text);
        }
    }

    private void SortLaunches(int column)
    {
        descending = sortColumn == column && !descending;
        sortColumn = column;
        foreach (ColumnHeader header in games.Columns) header.Text = header.Text.TrimEnd(' ', '▲', '▼');
        games.Columns[column].Text += descending ? " ▼" : " ▲";
        games.ListViewItemSorter = new LaunchColumnComparer(column, descending);
    }

    private void ShowLaunches()
    {
        string query = filter.Text.Trim();
        var entries = current.AsEnumerable();
        if (showHidden.Checked)
        {
            var absent = store.HiddenExecutables.Except(current.Select(item => item.Exe), StringComparer.OrdinalIgnoreCase)
                .OrderBy(exe => exe, StringComparer.OrdinalIgnoreCase);
            entries = entries.Concat(absent.Select(exe => new Launch { Exe = exe }));
        }
        else entries = entries.Where(item => !store.HiddenExecutables.Contains(item.Exe));

        var visible = entries.Where(item => item.Target.IndexOf(query, StringComparison.OrdinalIgnoreCase) >= 0 ||
                (item.Application ?? "").IndexOf(query, StringComparison.OrdinalIgnoreCase) >= 0);
        string selected = games.SelectedItems.Count > 0 ? ((Launch)games.SelectedItems[0].Tag).Target : "";
        games.BeginUpdate();
        try
        {
            games.Items.Clear();
            foreach (var item in visible)
            {
                bool hidden = store.HiddenExecutables.Contains(item.Exe);
                bool inSnapshot = item.Time != default(DateTime);
                string activity = inSnapshot ? item.Stage ?? (item.Live ? "Running process" : "Recent launch") : "Outside snapshot";
                if (item.Stage != null && !item.Live) activity += " (recent)";
                if (hidden) activity = "Hidden · " + activity;
                string description = string.IsNullOrWhiteSpace(item.Application) ? item.Path ?? "" : item.Application;
                var row = new ListViewItem(new[] { item.Exe, activity, description }) {
                    Tag = item, ToolTipText = item.Target + (inSnapshot ? "\r\n" + item.Time.ToLocalTime().ToString("g") : "") +
                        (string.IsNullOrEmpty(item.Runtime) ? "" : "\r\n" + item.Runtime)
                };
                games.Items.Add(row);
                if (string.Equals(item.Target, selected, StringComparison.OrdinalIgnoreCase)) row.Selected = true;
            }
        }
        finally { games.EndUpdate(); }
        UpdateHideButton();
    }

    private void RefreshStatus()
    {
        var latest = Directory.Exists(Install.Root) ? new DirectoryInfo(Install.Root).GetFiles("*.status")
            .OrderByDescending(file => file.LastWriteTimeUtc).FirstOrDefault() : null;
        if (latest == null)
        {
            status.Text = "Start an assigned OpenXR game with your headset connected.";
            return;
        }
        bool live = (DateTime.UtcNow - latest.LastWriteTimeUtc).TotalSeconds < 3;
        try
        {
            using (var file = new FileStream(latest.FullName, FileMode.Open, FileAccess.Read, FileShare.ReadWrite))
            using (var reader = new StreamReader(file))
            {
                var text = (live ? "Live or recently updated" : "Last session") + "   |   " + latest.Name +
                    Environment.NewLine + reader.ReadToEnd().Replace("\r\n", "\n").Replace("\n", "\r\n");
                if (status.Text != text) status.Text = text;
            }
        }
        catch (IOException) { /* Retry if the game is updating its status. */ }
    }
}
