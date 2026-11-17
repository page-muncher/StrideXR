using System;
using System.Globalization;

internal sealed class SubtitleStyle
{
    internal static readonly string[] Layouts = { "Current", "Previous + current", "Current + next", "Previous + current + next", "History" };
    internal int X, Y = 40, Opacity = 100, Lines = 4, Layout;

    internal static SubtitleStyle Parse(string value)
    {
        var style = new SubtitleStyle();
        var fields = value.Split(',');
        var numbers = new int[5];
        if (fields.Length != 4 && fields.Length != 5) return style;
        for (int i = 0; i < fields.Length; ++i)
            if (!int.TryParse(fields[i], NumberStyles.Integer, CultureInfo.InvariantCulture, out numbers[i])) return style;
        style.X = Math.Max(-90, Math.Min(90, numbers[0]));
        style.Y = Math.Max(-70, Math.Min(90, numbers[1]));
        style.Opacity = Math.Max(0, Math.Min(100, numbers[2]));
        style.Lines = Math.Max(1, Math.Min(6, numbers[3]));
        style.Layout = Math.Max(0, Math.Min(4, numbers[4]));
        return style;
    }

    public override string ToString()
    {
        return string.Format(CultureInfo.InvariantCulture, "{0},{1},{2},{3},{4}", X, Y, Opacity, Lines, Layout);
    }
}
