using System;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Interop;
using System.Windows.Media;

namespace WpfMusicPlayer.Helpers;

internal static class GaussianBlueHelper
{

    [DllImport("user32.dll")]
    private static extern int SetWindowCompositionAttribute(IntPtr hwnd, ref WindowCompositionAttributeData data);

    [DllImport("dwmapi.dll")]
    private static extern int DwmExtendFrameIntoClientArea(IntPtr hwnd, ref Margins margins);

    [DllImport("dwmapi.dll")]
    private static extern int DwmSetWindowAttribute(IntPtr hwnd, int attr, ref int value, int size);

    [StructLayout(LayoutKind.Sequential)]
    private struct Margins
    {
        public int Left, Right, Top, Bottom;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct AccentPolicy
    {
        public int AccentState;
        public int AccentFlags;
        public uint GradientColor;
        public int AnimationId;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct WindowCompositionAttributeData
    {
        public int Attribute;
        public IntPtr Data;
        public int SizeOfData;
    }

    private const int WCA_ACCENT_POLICY = 19;
    private const int ACCENT_ENABLE_GRADIENT = 1;
    private const int ACCENT_ENABLE_BLURBEHIND = 3;

    private const int DWMWA_SYSTEMBACKDROP_TYPE = 38;
    private const int DWMWA_USE_IMMERSIVE_DARK_MODE = 20;

    public enum DwmSystemBackdropType
    {
        None = 1,
        Acrylic = 3
    }

    [DllImport("ntdll.dll")]
    static extern int RtlGetVersion(ref OSVERSIONINFOEX versionInfo);

    [StructLayout(LayoutKind.Sequential)]
    public struct OSVERSIONINFOEX
    {
        public int dwOSVersionInfoSize;
        public int dwMajorVersion;
        public int dwMinorVersion;
        public int dwBuildNumber;
        public int dwPlatformId;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)]
        public string szCSDVersion;
    }

    public static bool IsWindows11()
    {
        var v = new OSVERSIONINFOEX();
        v.dwOSVersionInfoSize = Marshal.SizeOf(v);
        RtlGetVersion(ref v);
        return v is { dwMajorVersion: 10, dwBuildNumber: >= 22000 };
    }

    public static void EnableDarkMode(Window window)
    {
        var hwnd = GetHwnd(window);
        if (hwnd == IntPtr.Zero) return;

        var dark = 1;
        DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, ref dark, sizeof(int));
    }

    public static void EnableAcrylic(Window window, uint tintColor = 0xCC222222)
    {
        if (IsWindows11())
            Win11_ApplyBackdrop(window, DwmSystemBackdropType.Acrylic);
        else
            Win10_ApplyBlur(window, tintColor);
    }

    public static void EnableSolid(Window window)
    {
        if (IsWindows11())
            Win11_ApplyBackdrop(window, DwmSystemBackdropType.None);
        else
            Win10_ApplySolid(window);
    }

    public static void EnableImageBlur(Window window)
    {
        if (IsWindows11())
            Win11_ApplyBackdrop(window, DwmSystemBackdropType.None);
        else
            Win10_ApplyImageBlur(window);
    }

    private static void Win10_ApplyBlur(Window window, uint tintColor)
    {
        var hwnd = GetHwnd(window);
        if (hwnd == IntPtr.Zero) return;

        ExtendFrame(hwnd);

        var accent = new AccentPolicy
        {
            AccentState = ACCENT_ENABLE_BLURBEHIND,
            AccentFlags = 2,
            GradientColor = tintColor
        };

        ApplyAccent(hwnd, accent);
    }

    private static void Win10_ApplySolid(Window window)
    {
        var hwnd = GetHwnd(window);
        if (hwnd == IntPtr.Zero) return;

        ExtendFrame(hwnd);

        var accent = new AccentPolicy
        {
            AccentState = ACCENT_ENABLE_BLURBEHIND,
            AccentFlags = 2,
            GradientColor = 0xFF000000
        };

        ApplyAccent(hwnd, accent);
    }

    private static void Win10_ApplyImageBlur(Window window)
    {
        var hwnd = GetHwnd(window);
        if (hwnd == IntPtr.Zero) return;

        ExtendFrame(hwnd);

        var accent = new AccentPolicy
        {
            AccentState = ACCENT_ENABLE_GRADIENT,
            AccentFlags = 0,
            GradientColor = 0x00000000
        };

        ApplyAccent(hwnd, accent);
    }
    private static void Win11_ApplyBackdrop(Window window, DwmSystemBackdropType type)
    {
        var hwnd = GetHwnd(window);
        if (hwnd == IntPtr.Zero) return;

        var backdrop = (int)type;
        DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, ref backdrop, sizeof(int));
    }

    private static IntPtr GetHwnd(Window window)
    {
        var src = (HwndSource?)PresentationSource.FromVisual(window);
        return src?.Handle ?? IntPtr.Zero;
    }

    private static void ExtendFrame(IntPtr hwnd)
    {
        var margins = new Margins { Left = -1, Right = -1, Top = -1, Bottom = -1 };
        DwmExtendFrameIntoClientArea(hwnd, ref margins);
    }

    private static void ApplyAccent(IntPtr hwnd, AccentPolicy accent)
    {
        var size = Marshal.SizeOf<AccentPolicy>();
        var ptr = Marshal.AllocHGlobal(size);

        try
        {
            Marshal.StructureToPtr(accent, ptr, false);

            var data = new WindowCompositionAttributeData
            {
                Attribute = WCA_ACCENT_POLICY,
                Data = ptr,
                SizeOfData = size
            };

            SetWindowCompositionAttribute(hwnd, ref data);
        }
        finally
        {
            Marshal.FreeHGlobal(ptr);
        }
    }
}
