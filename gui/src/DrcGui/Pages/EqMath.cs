namespace DrcGui.Pages;

/// <summary>EQ 数学：peaking biquad 幅频（RBJ cookbook，Q=1.1，虚拟 Fs=48k）。</summary>
static class EqMath
{
    public const double Fs = 48000.0;
    public const double Q = 1.1;

    /// <summary>中心频率 f0、增益 gainDb 的 peaking 段在频率 f 处的幅值（dB）。</summary>
    public static double MagDb(double f0, double gainDb, double f)
    {
        if (Math.Abs(gainDb) < 0.001) return 0;
        double a = Math.Pow(10.0, gainDb / 40.0);
        double w0 = 2 * Math.PI * f0 / Fs;
        double alpha = Math.Sin(w0) / (2 * Q);
        double b0 = 1 + alpha * a, b1 = -2 * Math.Cos(w0), b2 = 1 - alpha * a;
        double a0 = 1 + alpha / a, a1 = -2 * Math.Cos(w0), a2 = 1 - alpha / a;

        double w = 2 * Math.PI * f / Fs;
        double c1 = Math.Cos(w), s1 = Math.Sin(w);
        double c2 = Math.Cos(2 * w), s2 = Math.Sin(2 * w);
        double nRe = b0 + b1 * c1 + b2 * c2, nIm = -(b1 * s1 + b2 * s2);
        double dRe = a0 + a1 * c1 + a2 * c2, dIm = -(a1 * s1 + a2 * s2);
        double den = dRe * dRe + dIm * dIm;
        double hRe = (nRe * dRe + nIm * dIm) / den;
        double hIm = (nIm * dRe - nRe * dIm) / den;
        return 20.0 * Math.Log10(Math.Sqrt(hRe * hRe + hIm * hIm) + 1e-30);
    }
}
