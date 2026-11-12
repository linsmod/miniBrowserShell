# Verifies the UpdateLayeredWindow rule used by statusBarRender: the DIB must
# stay selected into the memory DC while it is presented. Hidden window only -
# nothing is shown, moved or focused.
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

public static class LayTest
{
    [StructLayout(LayoutKind.Sequential)]
    public struct BITMAPINFOHEADER
    {
        public int biSize, biWidth, biHeight;
        public short biPlanes, biBitCount;
        public int biCompression, biSizeImage, biXPelsPerMeter, biYPelsPerMeter;
        public int biClrUsed, biClrImportant;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct BITMAPINFO
    {
        public BITMAPINFOHEADER bmiHeader;
        public int bmiColors;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct BLENDFUNCTION
    {
        public byte BlendOp, BlendFlags, SourceConstantAlpha, AlphaFormat;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct SIZE { public int cx, cy; }

    [StructLayout(LayoutKind.Sequential)]
    public struct POINT { public int x, y; }

    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern IntPtr CreateWindowExW(int ex, string cls, string title,
        int style, int x, int y, int w, int h, IntPtr parent, IntPtr menu, IntPtr inst, IntPtr param);

    [DllImport("user32.dll")]
    public static extern bool DestroyWindow(IntPtr hwnd);

    [DllImport("user32.dll", SetLastError = true)]
    public static extern bool UpdateLayeredWindow(IntPtr hwnd, IntPtr dst, ref POINT dstPt,
        ref SIZE size, IntPtr src, ref POINT srcPt, int colorKey, ref BLENDFUNCTION blend, int flags);

    [DllImport("gdi32.dll")]
    public static extern IntPtr CreateCompatibleDC(IntPtr hdc);

    [DllImport("gdi32.dll")]
    public static extern IntPtr CreateDIBSection(IntPtr dc, ref BITMAPINFO info, int usage,
        out IntPtr bits, IntPtr section, int offset);

    [DllImport("gdi32.dll")]
    public static extern IntPtr SelectObject(IntPtr dc, IntPtr obj);

    [DllImport("gdi32.dll")]
    public static extern bool DeleteObject(IntPtr obj);

    [DllImport("gdi32.dll")]
    public static extern bool DeleteDC(IntPtr dc);

    static int[] Run(bool keepSelected)
    {
        IntPtr hwnd = CreateWindowExW(0x00080000 | 0x00000080, "STATIC", "",
            unchecked((int)0x80000000), 0, 0, 1, 1, IntPtr.Zero, IntPtr.Zero,
            IntPtr.Zero, IntPtr.Zero);
        if (hwnd == IntPtr.Zero)
            return new int[] { -1, Marshal.GetLastWin32Error() };

        BITMAPINFO info = new BITMAPINFO();
        info.bmiHeader.biSize = Marshal.SizeOf(typeof(BITMAPINFOHEADER));
        info.bmiHeader.biWidth = 120;
        info.bmiHeader.biHeight = -30;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;

        IntPtr dc = CreateCompatibleDC(IntPtr.Zero);
        IntPtr bits;
        IntPtr bmp = CreateDIBSection(dc, ref info, 0, out bits, IntPtr.Zero, 0);
        IntPtr old = SelectObject(dc, bmp);

        byte[] px = new byte[120 * 30 * 4];
        for (int i = 0; i < px.Length; i += 4) { px[i] = 0x40; px[i + 3] = 0xFF; }
        Marshal.Copy(px, 0, bits, px.Length);

        if (!keepSelected)
            SelectObject(dc, old);   /* the bug: present with no surface */

        POINT src = new POINT();
        SIZE size = new SIZE { cx = 120, cy = 30 };
        POINT dst = new POINT { x = 0, y = 0 };
        BLENDFUNCTION blend = new BLENDFUNCTION {
            BlendOp = 0, BlendFlags = 0, SourceConstantAlpha = 255, AlphaFormat = 1 };

        bool ok = UpdateLayeredWindow(hwnd, IntPtr.Zero, ref dst, ref size, dc, ref src, 0, ref blend, 2);
        int err = Marshal.GetLastWin32Error();

        if (keepSelected)
            SelectObject(dc, old);   /* restore the DC before deleting the DIB */
        DeleteObject(bmp);
        DeleteDC(dc);
        DestroyWindow(hwnd);
        return new int[] { ok ? 1 : 0, err };
    }

    public static string Check()
    {
        int[] selected = Run(true);
        int[] deselected = Run(false);
        return string.Format("selected={0} err={1} | deselected={2} err={3}",
            selected[0], selected[1], deselected[0], deselected[1]);
    }
}
'@

[LayTest]::Check()
