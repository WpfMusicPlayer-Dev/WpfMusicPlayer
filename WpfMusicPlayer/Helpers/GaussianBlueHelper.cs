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
    private const int ACCENT_ENABLE_BLURBEHIND = 3;
    private const int DWMWA_USE_IMMERSIVE_DARK_MODE = 20;

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
        OSVERSIONINFOEX v = new OSVERSIONINFOEX();
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

    public static void EnableBlur(Window window, uint tintColor = 0xCC222222)
    {
        var hwndSource = (HwndSource?)PresentationSource.FromVisual(window);
        if (hwndSource?.CompositionTarget == null)
            return;

        var hwnd = hwndSource.Handle;

        // 深色模式
        int darkMode = 1;
        DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, ref darkMode, sizeof(int));

        hwndSource.CompositionTarget.BackgroundColor = Colors.Transparent;

        // 去除标题栏
        var margins = new Margins { Left = -1, Right = -1, Top = -1, Bottom = -1 };
        DwmExtendFrameIntoClientArea(hwnd, ref margins);
        bool enableAcrylic = false;
        if (!enableAcrylic) return; 
        if (IsWindows11())
        {
            // Windows 11: Apply Acrylic (Mica doesn't work at all)
            const int DWMWA_SYSTEMBACKDROP_TYPE = 38;
            int backdrop = (int)DwmSystemBackdropType.Acrylic;
            DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, ref backdrop, sizeof(int));
        } else
        {
            // Windows 10: Apply Gaussian Blur
            // (Acrylic doesn't work on my Win10 PC)
            var accent = new AccentPolicy
            {
                AccentState = ACCENT_ENABLE_BLURBEHIND,
                AccentFlags = 2,
                GradientColor = tintColor
            };

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
}
