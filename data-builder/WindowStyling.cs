using System.Runtime.InteropServices;

namespace McsmVitaDataBuilder;

internal static class WindowStyling
{
    private const int ImmersiveDarkModeBefore20H1 = 19;
    private const int ImmersiveDarkMode = 20;
    private const int BorderColor = 34;
    private const int CaptionColor = 35;
    private const int TextColor = 36;

    public static void ApplyDarkTitleBar(Form form)
    {
        if (!OperatingSystem.IsWindowsVersionAtLeast(10, 0, 17763))
        {
            return;
        }

        int enabled = 1;
        int result = DwmSetWindowAttribute(
            form.Handle,
            ImmersiveDarkMode,
            ref enabled,
            Marshal.SizeOf<int>());
        if (result != 0)
        {
            _ = DwmSetWindowAttribute(
                form.Handle,
                ImmersiveDarkModeBefore20H1,
                ref enabled,
                Marshal.SizeOf<int>());
        }

        if (OperatingSystem.IsWindowsVersionAtLeast(10, 0, 22000))
        {
            int border = ColorTranslator.ToWin32(Color.FromArgb(50, 64, 86));
            int caption = ColorTranslator.ToWin32(Color.FromArgb(10, 15, 28));
            int text = ColorTranslator.ToWin32(Color.FromArgb(241, 245, 249));
            _ = DwmSetWindowAttribute(form.Handle, BorderColor, ref border, Marshal.SizeOf<int>());
            _ = DwmSetWindowAttribute(form.Handle, CaptionColor, ref caption, Marshal.SizeOf<int>());
            _ = DwmSetWindowAttribute(form.Handle, TextColor, ref text, Marshal.SizeOf<int>());
        }
    }

    [DllImport("dwmapi.dll")]
    private static extern int DwmSetWindowAttribute(
        IntPtr window,
        int attribute,
        ref int value,
        int valueSize);
}
