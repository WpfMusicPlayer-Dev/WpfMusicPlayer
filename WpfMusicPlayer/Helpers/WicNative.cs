// SPDX-License-Identifier: MIT

using System.Runtime.InteropServices;

namespace WpfMusicPlayer.Helpers;

internal static unsafe class WicNative
{
    private static readonly Guid ClsidImagingFactory = new("cacaf262-9370-4615-a13b-9f5539da4c0a");
    private static readonly Guid IidImagingFactory = new("ec5ec8a9-c395-4314-9c77-54d7a935ff70");
    private static readonly Guid PixelFormat32bppPbgra = new("6fddc324-4e03-4bfe-b185-3d77768dc910");

    private const uint ClsctxInprocServer = 1;
    private const int BitmapCacheOnLoad = 1;

    [DllImport("ole32.dll")]
    private static extern int CoCreateInstance(Guid* clsid, IntPtr outer, uint clsContext, Guid* iid, void** ppv);

    public static void* CreateFactory()
    {
        var clsid = ClsidImagingFactory;
        var iid = IidImagingFactory;
        void* factory;
        var hr = CoCreateInstance(&clsid, IntPtr.Zero, ClsctxInprocServer, &iid, &factory);
        if (hr < 0)
            throw new COMException($"CoCreateInstance(WIC) failed: 0x{hr:X8}", hr);
        if (factory == null)
            throw new InvalidOperationException("WIC factory was null.");
        return factory;
    }

    public static void* CreateBitmap(void* factory, uint width, uint height)
    {
        var format = PixelFormat32bppPbgra;
        void* bitmap;
        var vtbl = *(void***)factory;
        var create = (delegate* unmanaged[Stdcall]<void*, uint, uint, Guid*, int, void**, int>)vtbl[17];
        var hr = create(factory, width, height, &format, BitmapCacheOnLoad, &bitmap);
        if (hr < 0)
            throw new COMException($"IWICImagingFactory.CreateBitmap failed: 0x{hr:X8}", hr);
        return bitmap;
    }

    public static void CopyPixels(void* bitmap, uint stride, uint bufferSize, byte* buffer)
    {
        var vtbl = *(void***)bitmap;
        var copy = (delegate* unmanaged[Stdcall]<void*, void*, uint, uint, byte*, int>)vtbl[7];
        var hr = copy(bitmap, null, stride, bufferSize, buffer);
        if (hr < 0)
            throw new COMException($"IWICBitmap.CopyPixels failed: 0x{hr:X8}", hr);
    }
}
