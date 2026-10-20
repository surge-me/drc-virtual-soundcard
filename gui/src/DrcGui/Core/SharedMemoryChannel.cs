using System.Runtime.InteropServices;

namespace DrcGui.Core;

/// <summary>
/// GUI 侧共享内存通道：布局照 SPEC §4（参数区偏移 0 / 电平区偏移 512，小端，手工偏移读写）。
/// GUI 只 OpenFileMapping（不创建）；不存在 = 未接管。参数写入按 seqlock 流程：seq++ → 写 → seq++。
/// </summary>
public sealed class SharedMemoryChannel : IDisposable
{
    // SPEC §4（2026-10-15 e2e 订正）：audiodg=会话0 与用户会话的 Local 命名空间互不可见 ⇒ 必须 Global。
    public const string MapName = @"Global\DrcApoSharedMem_v1";
    public const uint DrcMagic = 0x43524421u; // "DRC!"

    const uint FILE_MAP_ALL_ACCESS = 0x000F001F;
    const int MeterBase = 512;

    IntPtr _map = IntPtr.Zero;
    IntPtr _view = IntPtr.Zero;

    public bool IsOpen => _view != IntPtr.Zero;

    public readonly record struct ParamData(
        float CeilingDb, float FloorDb, float MaxBoostDb,
        float LookaheadMs, float ReleaseMs, float[] EqGainDb, bool Bypass);

    /// <summary>
    /// 电平快照。口径照 SPEC §4.1：<c>InPeakDb</c>/<c>OutPeakDb</c> 为 **dBFS**（≤0，0=满刻度），
    /// 由线上下发值 <c>inPeakDb_x100</c> 经 <c>dBFS = -value/100</c> 换算；
    /// <c>GainReductionDb</c> 为限幅增益衰减量（≥0）。
    /// </summary>
    public readonly record struct MeterData(
        uint SampleRate, uint Channels, double GainReductionDb,
        double InPeakDb, double OutPeakDb, double LatencyMs, ulong Frames);

    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    private static extern IntPtr OpenFileMapping(uint desiredAccess, bool inherit, string name);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr MapViewOfFile(IntPtr fileMappingObject, uint desiredAccess,
        uint fileOffsetHigh, uint fileOffsetLow, UIntPtr numberOfBytesToMap);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool UnmapViewOfFile(IntPtr baseAddress);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool CloseHandle(IntPtr handle);

    /// <summary>尝试打开 APO 创建的共享内存；不存在（APO 未接管）返回 false。</summary>
    public bool TryOpen()
    {
        if (IsOpen) return true;
        _map = OpenFileMapping(FILE_MAP_ALL_ACCESS, false, MapName);
        if (_map == IntPtr.Zero) return false;
        _view = MapViewOfFile(_map, FILE_MAP_ALL_ACCESS, 0, 0, UIntPtr.Zero);
        if (_view == IntPtr.Zero)
        {
            CloseHandle(_map);
            _map = IntPtr.Zero;
            return false;
        }
        return true;
    }

    /// <summary>seqlock 写参数区（GUI 是唯一写者）。</summary>
    public void WriteParams(in ParamData p)
    {
        if (!IsOpen) return;
        int seq = Marshal.ReadInt32(_view, 4);
        WriteU32(_view, 4, (uint)seq + 1u);          // seq++（奇 = 写入中）
        WriteU32(_view, 0, DrcMagic);
        WriteU32(_view, 8, 1u);                       // DRC_PARAM_VERSION
        WriteU32(_view, 12, p.Bypass ? 1u : 0u);      // flags bit0 = bypass
        WriteF32(_view, 16, p.CeilingDb);
        WriteF32(_view, 20, p.FloorDb);
        WriteF32(_view, 24, p.MaxBoostDb);
        WriteF32(_view, 28, p.LookaheadMs);
        WriteF32(_view, 32, p.ReleaseMs);
        for (int i = 0; i < 10; i++)
            WriteF32(_view, 36 + i * 4, p.EqGainDb[i]);
        WriteU32(_view, 4, (uint)seq + 2u);          // seq++（偶 = 稳定）
    }

    /// <summary>读电平区（seqlock 读：奇 seq 或前后不一致则丢弃本帧）。</summary>
    public bool TryReadMeter(out MeterData m)
    {
        m = default;
        if (!IsOpen) return false;
        uint s1 = ReadU32(_view, MeterBase + 4);
        if ((s1 & 1u) != 0) return false;
        if (ReadU32(_view, MeterBase) != DrcMagic) return false;
        if (ReadU32(_view, MeterBase + 8) != 1u) return false;   // DRC_METER_VERSION
        uint sr = ReadU32(_view, MeterBase + 12);
        uint ch = ReadU32(_view, MeterBase + 16);
        double gr = ReadU32(_view, MeterBase + 20) / 100.0;          // 衰减量 ≥0（dB）
        double inP = -ReadU32(_view, MeterBase + 24) / 100.0;        // SPEC §4.1：dBFS = -value/100
        double outP = -ReadU32(_view, MeterBase + 28) / 100.0;       // 同上口径（0=0dBFS、5000=-50dBFS）
        double lat = ReadU32(_view, MeterBase + 32) / 10.0;
        ulong frames = ReadU64(_view, MeterBase + 36);
        uint s2 = ReadU32(_view, MeterBase + 4);
        if (s2 != s1) return false;
        m = new MeterData(sr, ch, gr, inP, outP, lat, frames);
        return true;
    }

    public void Dispose()
    {
        if (_view != IntPtr.Zero) { UnmapViewOfFile(_view); _view = IntPtr.Zero; }
        if (_map != IntPtr.Zero) { CloseHandle(_map); _map = IntPtr.Zero; }
    }

    static uint ReadU32(IntPtr b, int off) => (uint)Marshal.ReadInt32(b, off);
    static ulong ReadU64(IntPtr b, int off) => (ulong)Marshal.ReadInt64(b, off);
    static void WriteU32(IntPtr b, int off, uint v) => Marshal.WriteInt32(b, off, (int)v);
    static float ReadF32(IntPtr b, int off) => BitConverter.Int32BitsToSingle(Marshal.ReadInt32(b, off));
    static void WriteF32(IntPtr b, int off, float v) => Marshal.WriteInt32(b, off, BitConverter.SingleToInt32Bits(v));
}
