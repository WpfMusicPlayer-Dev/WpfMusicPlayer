// SPDX-License-Identifier: MIT

using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using Silk.NET.Direct2D;

namespace WpfMusicPlayer.Helpers;

internal static unsafe class DirectWriteNative
{
    private static readonly Guid FactoryIid = new("b859ee5a-d838-4b5b-a2e8-1adc7d93db48");

    public const int FontWeightNormal = 400;
    public const int FontWeightBold = 700;
    public const int FontStyleNormal = 0;
    public const int FontStretchNormal = 5;
    public const int TextAlignmentLeading = 0;
    public const int WordWrappingWrap = 0;

    [DllImport("dwrite.dll", ExactSpelling = true)]
    private static extern int DWriteCreateFactory(
        int factoryType,
        [In] ref Guid iid,
        out IntPtr factory);

    public static void* CreateFactory()
    {
        var iid = FactoryIid;
        var hr = DWriteCreateFactory(0, ref iid, out var factory);
        if (hr < 0)
            throw new COMException($"DWriteCreateFactory failed: 0x{hr:X8}", hr);
        if (factory == IntPtr.Zero)
            throw new InvalidOperationException("DWriteCreateFactory returned a null factory.");
        return (void*)factory;
    }

    public static uint Release(void* comObject)
    {
        if (comObject == null)
            return 0;

        var vtbl = *(void***)comObject;
        var release = (delegate* unmanaged[Stdcall]<void*, uint>)vtbl[2];
        return release(comObject);
    }

    public static IDWriteTextFormat* CreateTextFormat(
        void* factory,
        string fontFamily,
        float fontSize,
        bool bold,
        string locale)
    {
        var com = (IDWriteFactoryCom)Marshal.GetUniqueObjectForIUnknown((nint)factory);
        try
        {
            var hr = com.CreateTextFormat(
                fontFamily,
                IntPtr.Zero,
                bold ? FontWeightBold : FontWeightNormal,
                FontStyleNormal,
                FontStretchNormal,
                fontSize,
                locale,
                out var format);
            if (hr < 0)
                throw new COMException($"CreateTextFormat failed: 0x{hr:X8}", hr);

            var formatCom = (IDWriteTextFormatCom)Marshal.GetUniqueObjectForIUnknown(format);
            try
            {
                formatCom.SetTextAlignment(TextAlignmentLeading);
                formatCom.SetWordWrapping(WordWrappingWrap);
            }
            finally
            {
                Marshal.ReleaseComObject(formatCom);
            }

            return (IDWriteTextFormat*)format;
        }
        finally
        {
            Marshal.ReleaseComObject(com);
        }
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static void SafeRelease(ref void* comObject)
    {
        if (comObject == null)
            return;
        Release(comObject);
        comObject = null;
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static void SafeRelease(ref IDWriteTextFormat* format)
    {
        if (format == null)
            return;
        Release(format);
        format = null;
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static void SafeRelease(ref Silk.NET.DirectWrite.IDWriteTextLayout* layout)
    {
        if (layout == null)
            return;
        Release(layout);
        layout = null;
    }

    [ComImport]
    [Guid("b859ee5a-d838-4b5b-a2e8-1adc7d93db48")]
    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface IDWriteFactoryCom
    {
        [PreserveSig] int GetSystemFontCollection(out IntPtr fontCollection, int checkForUpdates);
        [PreserveSig] int CreateCustomFontCollection(IntPtr collectionLoader, IntPtr collectionKey, uint collectionKeySize, out IntPtr fontCollection);
        [PreserveSig] int RegisterFontCollectionLoader(IntPtr fontCollectionLoader);
        [PreserveSig] int UnregisterFontCollectionLoader(IntPtr fontCollectionLoader);
        [PreserveSig] int CreateFontFileReference([MarshalAs(UnmanagedType.LPWStr)] string filePath, IntPtr lastWriteTime, out IntPtr fontFile);
        [PreserveSig] int CreateCustomFontFileReference(IntPtr fontFileReferenceKey, uint keySize, IntPtr fontFileLoader, out IntPtr fontFile);
        [PreserveSig] int CreateFontFace(int fontFaceType, uint numberOfFiles, IntPtr fontFiles, uint faceIndex, int fontFaceSimulationFlags, out IntPtr fontFace);
        [PreserveSig] int CreateRenderingParams(out IntPtr renderingParams);
        [PreserveSig] int CreateMonitorRenderingParams(IntPtr monitor, out IntPtr renderingParams);
        [PreserveSig] int CreateCustomRenderingParams(float gamma, float enhancedContrast, float clearTypeLevel, int pixelGeometry, int renderingMode, out IntPtr renderingParams);
        [PreserveSig] int RegisterFontFileLoader(IntPtr fontFileLoader);
        [PreserveSig] int UnregisterFontFileLoader(IntPtr fontFileLoader);
        [PreserveSig] int CreateTextFormat(
            [MarshalAs(UnmanagedType.LPWStr)] string fontFamilyName,
            IntPtr fontCollection,
            int fontWeight,
            int fontStyle,
            int fontStretch,
            float fontSize,
            [MarshalAs(UnmanagedType.LPWStr)] string localeName,
            out IntPtr textFormat);
    }

    [ComImport]
    [Guid("9c906818-31d7-4fd3-a151-7c5e225db55a")]
    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface IDWriteTextFormatCom
    {
        [PreserveSig] int SetTextAlignment(int textAlignment);
        [PreserveSig] int SetParagraphAlignment(int paragraphAlignment);
        [PreserveSig] int SetWordWrapping(int wordWrapping);
    }
}
