using System;
using System.Drawing;
using System.Runtime.InteropServices;
using System.Windows.Forms;

internal static class DarkTheme
{
    private static readonly Color Background = Color.FromArgb(18, 21, 26);
    private static readonly Color Surface = Color.FromArgb(25, 29, 35);
    private static readonly Color Field = Color.FromArgb(32, 38, 45);
    private static readonly Color Border = Color.FromArgb(58, 69, 79);
    private static readonly Color Text = Color.FromArgb(230, 236, 239);
    private static readonly Color Muted = Color.FromArgb(166, 181, 191);
    private static readonly Color Accent = Color.FromArgb(126, 224, 202);

    internal static void Apply(Control control)
    {
        control.ForeColor = Text;
        control.BackColor = control is Form || control is Navigation ? Background : Surface;
        if (control is Form)
        {
            control.HandleCreated += (sender, args) => DarkTitle(control);
            if (control.IsHandleCreated) DarkTitle(control);
        }
        var button = control as Button;
        if (button != null)
        {
            bool primary = button.Text == "Play" || button.Text == "Save profile" || button.Text == "Open 3D test scene";
            button.FlatStyle = FlatStyle.Flat;
            button.FlatAppearance.BorderColor = primary ? Accent : Border;
            button.FlatAppearance.MouseOverBackColor = Color.FromArgb(48, 62, 67);
            button.FlatAppearance.MouseDownBackColor = Color.FromArgb(52, 77, 76);
            button.Padding = new Padding(4, 2, 4, 2);
            button.BackColor = primary ? Color.FromArgb(29, 57, 57) : Field;
            button.ForeColor = primary ? Accent : Text;
        }
        var text = control as TextBox;
        if (text != null)
        {
            text.BorderStyle = BorderStyle.FixedSingle;
            text.BackColor = Field;
        }
        var combo = control as ComboBox;
        if (combo != null)
        {
            combo.FlatStyle = FlatStyle.Flat;
            combo.BackColor = Field;
            if (combo.DropDownStyle == ComboBoxStyle.DropDownList)
            {
                combo.DrawMode = DrawMode.OwnerDrawFixed;
                combo.DrawItem += (sender, args) => {
                    bool selected = (args.State & DrawItemState.Selected) != 0;
                    using (var brush = new SolidBrush(selected ? Color.FromArgb(40, 69, 71) : Field))
                        args.Graphics.FillRectangle(brush, args.Bounds);
                    if (args.Index >= 0)
                        TextRenderer.DrawText(args.Graphics, combo.GetItemText(combo.Items[args.Index]), combo.Font,
                            args.Bounds, selected ? Accent : Text, TextFormatFlags.VerticalCenter | TextFormatFlags.EndEllipsis);
                    args.DrawFocusRectangle();
                };
            }
        }
        var number = control as NumericUpDown;
        if (number != null)
        {
            number.BorderStyle = BorderStyle.FixedSingle;
            number.BackColor = Field;
        }
        var list = control as ListView;
        if (list != null)
        {
            list.BorderStyle = BorderStyle.FixedSingle;
            list.BackColor = Field;
            list.OwnerDraw = true;
            list.DrawColumnHeader += (sender, args) => {
                using (var brush = new SolidBrush(Surface)) args.Graphics.FillRectangle(brush, args.Bounds);
                using (var pen = new Pen(Border)) args.Graphics.DrawLine(pen, args.Bounds.Left, args.Bounds.Bottom - 1,
                    args.Bounds.Right, args.Bounds.Bottom - 1);
                TextRenderer.DrawText(args.Graphics, args.Header.Text, list.Font, Rectangle.Inflate(args.Bounds, -7, 0),
                    Muted, TextFormatFlags.Left | TextFormatFlags.VerticalCenter | TextFormatFlags.EndEllipsis);
            };
            list.DrawSubItem += (sender, args) => {
                using (var brush = new SolidBrush(args.Item.Selected ? Color.FromArgb(40, 69, 71) : Field))
                    args.Graphics.FillRectangle(brush, args.Bounds);
                TextRenderer.DrawText(args.Graphics, args.SubItem.Text, list.Font, Rectangle.Inflate(args.Bounds, -7, 0),
                    args.Item.Selected ? Accent : Text, TextFormatFlags.Left | TextFormatFlags.VerticalCenter |
                    TextFormatFlags.EndEllipsis | TextFormatFlags.NoPrefix);
                if (args.Item.Focused && list.Focused && args.ColumnIndex == 0) args.DrawFocusRectangle(args.Bounds);
            };
        }
        var listBox = control as ListBox;
        if (listBox != null)
        {
            listBox.BorderStyle = BorderStyle.FixedSingle;
            listBox.BackColor = Field;
        }
        if (control is CheckBox) ((CheckBox)control).FlatStyle = FlatStyle.Flat;
        if (control is TabPage) ((TabPage)control).UseVisualStyleBackColor = false;
        foreach (Control child in control.Controls) Apply(child);
    }

    private static void DarkTitle(Control window)
    {
        int enabled = 1;
        if (DwmSetWindowAttribute(window.Handle, 20, ref enabled, sizeof(int)) != 0)
            DwmSetWindowAttribute(window.Handle, 19, ref enabled, sizeof(int));
    }

    [DllImport("dwmapi.dll")]
    private static extern int DwmSetWindowAttribute(IntPtr window, int attribute, ref int value, int size);

    internal sealed class Navigation : TabControl
    {
        internal Navigation()
        {
            DrawMode = TabDrawMode.OwnerDrawFixed;
            SizeMode = TabSizeMode.Fixed;
            ItemSize = new Size(109, 40);
            SetStyle(ControlStyles.OptimizedDoubleBuffer, true);
        }

        protected override void OnDrawItem(DrawItemEventArgs e)
        {
            bool active = e.Index == SelectedIndex;
            var rect = GetTabRect(e.Index);
            using (var fill = new SolidBrush(active ? Surface : Background))
                e.Graphics.FillRectangle(fill, rect);
            if (active)
                using (var line = new SolidBrush(Accent)) e.Graphics.FillRectangle(line, rect.Left + 10, rect.Bottom - 3, rect.Width - 20, 2);
            TextRenderer.DrawText(e.Graphics, TabPages[e.Index].Text, Font, rect, active ? Accent : Muted,
                TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter | TextFormatFlags.NoPrefix);
        }

        protected override void WndProc(ref Message message)
        {
            if (message.Msg == 0x14)
            {
                using (var graphics = Graphics.FromHdc(message.WParam)) graphics.Clear(Background);
                message.Result = new IntPtr(1);
                return;
            }
            base.WndProc(ref message);
        }
    }
}
