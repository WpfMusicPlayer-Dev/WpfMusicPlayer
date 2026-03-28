using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;

namespace WpfMusicPlayer.Helpers;

internal static class OsVersionHelper
{

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
}
