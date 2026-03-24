using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Interop;
using System.Windows.Media;
using Windows.UI;

namespace WpfMusicPlayer.Helpers;

internal static class GaussianBlueHelper
{
    [DllImport("user32.dll")]
    private static extern int SetWindowCompositionAttribute(IntPtr hwnd, ref WindowCompositionAttributeData data);

    [DllImport("user32.dll")]
    private static extern bool SetWindowPos(IntPtr hWnd, IntPtr hWndInsertAfter,
        int X, int Y, int cx, int cy, uint uFlags);

    [DllImport("dwmapi.dll")]
    private static extern int DwmExtendFrameIntoClientArea(IntPtr hwnd, ref Margins margins);

    [DllImport("dwmapi.dll")]
    private static extern int DwmSetWindowAttribute(IntPtr hwnd, int attr, ref int value, int size);

    [StructLayout(LayoutKind.Sequential)]
    private struct Margins
    {
        public int Left;
        public int Right;
        public int Top;
        public int Bottom;
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
    private const int ACCENT_ENABLE_ACRYLICBLURBEHIND = 4;
    private const int DWMWA_USE_IMMERSIVE_DARK_MODE = 20;
    private const uint SWP_NOMOVE = 0x0002;
    private const uint SWP_NOSIZE = 0x0001;
    private const uint SWP_NOZORDER = 0x0004;
    private const uint SWP_FRAMECHANGED = 0x0020;

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
        return v.dwMajorVersion == 10 && v.dwBuildNumber >= 22000;
    }
    public enum DwmSystemBackdropType
    {
        Auto = 0,
        None = 1,
        Mica = 2,
        Acrylic = 3,
        Tabbed = 4
    }

    public static void EnableDarkMode(Window window, bool enableAcrylic = false, uint tintColor = 0xCC222222)
    {
        var hwndSource = (HwndSource?)PresentationSource.FromVisual(window);
        if (hwndSource?.CompositionTarget == null)
            return;

        var hwnd = hwndSource.Handle;

        // 启用深色模式
        // 该API在Windows 10 2004+测试通过，Windows 11官方支持
        int darkMode = 1;
        DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, ref darkMode, sizeof(int));

        hwndSource.CompositionTarget.BackgroundColor = Colors.Transparent;
    }

    public static void EnableAcrylic(Window window, uint tintColor = 0xCC222222)
    {
        var hwndSource = (HwndSource?)PresentationSource.FromVisual(window);
        if (hwndSource?.CompositionTarget == null)
            return;

        var hwnd = hwndSource.Handle;

        hwndSource.CompositionTarget.BackgroundColor = Colors.Transparent;
        var margins = new Margins { Left = -1, Right = -1, Top = -1, Bottom = -1 };
        DwmExtendFrameIntoClientArea(hwnd, ref margins);

        // Win11: ACCENT_ENABLE_ACRYLICBLURBEHIND (4) -> Acrylic
        // Win10: ACCENT_ENABLE_BLURBEHIND (3) -> Gaussian Blur
        var accent = new AccentPolicy
        {
            AccentState = IsWindows11() ? ACCENT_ENABLE_ACRYLICBLURBEHIND : ACCENT_ENABLE_BLURBEHIND,
            AccentFlags = 2,
            GradientColor = tintColor
        };

        ApplyAccent(hwnd, accent);
    }

    public static void EnableSolid(Window window)
    {
        var hwndSource = (HwndSource?)PresentationSource.FromVisual(window);
        if (hwndSource?.CompositionTarget == null)
            return;

        var hwnd = hwndSource.Handle;

        var margins = new Margins { Left = -1, Right = -1, Top = -1, Bottom = -1 };
        DwmExtendFrameIntoClientArea(hwnd, ref margins);

        hwndSource.CompositionTarget.BackgroundColor = Colors.Transparent;

        // 模糊点满，反正看起来像纯黑就对了
        // 不要用ACCENT_DISABLED 会直接切断DWM管线！！！！！！！！
        var accent = new AccentPolicy
        {
            AccentState = ACCENT_ENABLE_BLURBEHIND,
            AccentFlags = 2,
            GradientColor = 0xFF000000,
            AnimationId = 0
        };

        ApplyAccent(hwnd, accent);
    }

    public static void EnableImageBlur(Window window)
    {
        var hwndSource = (HwndSource?)PresentationSource.FromVisual(window);
        if (hwndSource?.CompositionTarget == null)
            return;

        var hwnd = hwndSource.Handle;

        var margins = new Margins { Left = -1, Right = -1, Top = -1, Bottom = -1 };
        DwmExtendFrameIntoClientArea(hwnd, ref margins);

        hwndSource.CompositionTarget.BackgroundColor = Colors.Transparent;

        var accent = new AccentPolicy
        {
            AccentState = ACCENT_ENABLE_GRADIENT,
            AccentFlags = 0,
            GradientColor = 0x00000000, // 完全透明
            AnimationId = 0
        };

        ApplyAccent(hwnd, accent);
    }

    private static void ApplyAccent(nint hwnd, AccentPolicy accent)
    {
        var accentSize = Marshal.SizeOf<AccentPolicy>();
        var accentPtr = Marshal.AllocHGlobal(accentSize);
        try
        {
            Marshal.StructureToPtr(accent, accentPtr, false);

            var data = new WindowCompositionAttributeData
            {
                Attribute = WCA_ACCENT_POLICY,
                Data = accentPtr,
                SizeOfData = accentSize
            };

            SetWindowCompositionAttribute(hwnd, ref data);
        }
        finally
        {
            Marshal.FreeHGlobal(accentPtr);
        }
    }
}
