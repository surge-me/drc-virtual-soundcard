using System.Runtime.InteropServices;

namespace DrcGui.Core;

/// <summary>
/// 托盘图标代码绘制（禁二进制资源）：32×32 32bpp 像素手工绘制 → 组图标资源映像
/// （BITMAPINFOHEADER + XOR + AND）→ CreateIconFromResourceEx 得 HICON。
///
/// 造型 = 应用图标（<c>Assets/app.ico</c>，由 <c>scripts/make_icon.py</c> 生成）同一语言：
/// **一条正弦波被顶部天花板线削平**（限幅语义）。托盘底透明（任务栏深浅主题下均可见），
/// 正常 = 亮红 #E83C3C / 旁路 = 灰 #8C8C96。
/// 几何参数与 make_icon.py 的小尺寸档一致（f=1.0 / amp=0.27 / ceil=0.205 / y0=0.47）。
/// </summary>
static class IconFactory
{
    [DllImport("user32.dll")]
    private static extern IntPtr CreateIconFromResourceEx(byte[] presbits, uint dwResSize,
        bool fIcon, uint dwVer, int cx, int cy, uint flags);

    [DllImport("user32.dll")]
    private static extern bool DestroyIcon(IntPtr hIcon);

    const int S = 32;                     // 托盘图标尺寸
    const double F = 1.0, AMP = 0.27, CEIL = 0.205, Y0 = 0.47, X0 = 0.13, X1 = 0.87;
    const double HalfWidth = 0.095;       // 线宽/2（相对 32px）

    public static IntPtr CreateWaveIcon(bool bypass)
    {
        var px = new byte[S * S * 4];     // 屏幕坐标 BGRA，写前翻转为 DIB 自下而上行序
        // 正常 #E83C3C 亮红 / 旁路 #8C8C96 灰。底透明：任务栏深色不吞图标。
        byte fR = bypass ? (byte)140 : (byte)232;
        byte fG = bypass ? (byte)140 : (byte)60;
        byte fB = bypass ? (byte)150 : (byte)60;

        for (int y = 0; y < S; y++)
        {
            for (int x = 0; x < S; x++)
            {
                if (!OnWave(x, y)) continue;
                int di = ((S - 1 - y) * S + x) * 4;
                px[di + 0] = fB;
                px[di + 1] = fG;
                px[di + 2] = fR;
                px[di + 3] = 255;
            }
        }

        int xorLen = S * S * 4;
        int andStride = ((S + 31) / 32) * 4;
        int andLen = andStride * S;      // AND 掩码全 0（32bpp 由 alpha 决定）

        // CreateIconFromResourceEx 要的是「图标资源映像」= BITMAPINFOHEADER + XOR 位 + AND 掩码，
        // 而**不是** .ico 文件容器（ICONDIR/ICONDIRENTRY）。传容器会被读成非法 biSize 而返回 NULL。
        var res = new byte[40 + xorLen + andLen];
        WriteI32(res, 0, 40);            // biSize
        WriteI32(res, 4, S);             // biWidth
        WriteI32(res, 8, S * 2);         // biHeight = XOR + AND 两倍高
        res[12] = 1;                     // biPlanes
        res[14] = 32;                    // biBitCount
        Array.Copy(px, 0, res, 40, xorLen);

        return CreateIconFromResourceEx(res, (uint)res.Length, true, 0x00030000, S, S, 0);
    }

    /// <summary>像素是否落在「被削平的正弦」描边上（含圆头）。</summary>
    static bool OnWave(double px, double py)
    {
        const int N = 160;
        double lo = Y0 - CEIL;
        double rr = HalfWidth * S * HalfWidth * S;   // 半径平方（像素域）
        for (int i = 0; i <= N; i++)
        {
            double t = (double)i / N;
            double cx = (X0 + (X1 - X0) * t) * S;
            double cy = Math.Max(Y0 - AMP * Math.Sin(2 * Math.PI * F * t), lo) * S;
            double dx = px - cx, dy = py - cy;
            if (dx * dx + dy * dy <= rr) return true;
        }
        return false;
    }

    static void WriteI32(byte[] a, int off, int v)
    {
        a[off] = (byte)v; a[off + 1] = (byte)(v >> 8);
        a[off + 2] = (byte)(v >> 16); a[off + 3] = (byte)(v >> 24);
    }
}
