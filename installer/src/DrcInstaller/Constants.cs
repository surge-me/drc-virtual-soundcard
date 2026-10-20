namespace DrcInstaller;

/// <summary>
/// 冻结常量。CLSID 来源 = docs/devlogs/2026-10-15-apo-com.md（TASK-002 冻结，禁止再生成）。
/// 注册值名/类型来源 = installer/notes-apo-registry.md（Equalizer APO 源码 + Windows SDK 头 + MS Learn）。
/// </summary>
internal static class Constants
{
    /// <summary>本项目 APO COM CLSID（TASK-002 冻结，唯一来源）。</summary>
    public const string DrcApoClsid = "{02E42793-63F4-49A3-B547-95B3250D57E4}";

    public const string DrcApoDisplayName = "DRC Dynamic Range Compander APO";

    // ---- FxProperties 值名（PKEY_FX_*，基 GUID {d04e05a6-594b-4fb6-a80d-01af5eed7d1d}）----
    // 见 notes-apo-registry.md C2/C3/C4；类型一律 REG_SZ（C6）。
    public const string PkeyLfx = "{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},1"; // LFX  = PKEY_FX_PreMixEffectClsid
    public const string PkeyGfx = "{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},2"; // GFX  = PKEY_FX_PostMixEffectClsid
    public const string PkeySfx = "{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},5"; // SFX  = PKEY_FX_StreamEffectClsid
    public const string PkeyMfx = "{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},6"; // MFX  = PKEY_FX_ModeEffectClsid
    public const string PkeyEfx = "{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},7"; // EFX  = PKEY_FX_EndpointEffectClsid

    /// <summary>处理模式值（伴随 SFX/MFX/EFX；REG_MULTI_SZ）。来源见 notes C12。</summary>
    public const string PkeySfxModes = "{d3993a3f-99c2-4402-b5ec-a92a0367664b},5";
    public const string PkeyMfxModes = "{d3993a3f-99c2-4402-b5ec-a92a0367664b},6";
    public const string DefaultProcessingMode = "{C18E2F7E-933D-4965-B7D1-1EEF228D2AF3}";

    /// <summary>--status 检测范围（LFX/GFX/SFX/MFX/EFX，兼容不同安装手法）。</summary>
    public static readonly string[] AllFxValueNames = { PkeyLfx, PkeyGfx, PkeySfx, PkeyMfx, PkeyEfx };

    /// <summary>
    /// 写入模式（见 notes-apo-registry.md）。
    /// lfx-only = Win11 24H2 实测唯一可用（2026-10-15 devlog：SFX/MFX 路径被引擎静默弃图；
    ///            同 CLSID 占 LFX+GFX 双槽 → 引擎循环重建；与原厂 SFX/MFX 共存 → 互斥零加载）。
    ///            安装时须删除冲突槽位（EQAPO 同款行为，备份先行）。
    /// lfx-gfx = SPEC §5 历史默认（24H2 上同 CLSID 双槽不可用）；sfx-mfx = 24H2 上被弃图；both = 两者都写。
    /// </summary>
    public enum InstallMode { LfxOnly, LfxGfx, SfxMfx, Both }

    public static string[] WriteValueNames(InstallMode mode) => mode switch
    {
        InstallMode.LfxOnly => new[] { PkeyLfx },
        InstallMode.LfxGfx => new[] { PkeyLfx, PkeyGfx },
        InstallMode.SfxMfx => new[] { PkeySfx, PkeyMfx },
        _ => new[] { PkeyLfx, PkeyGfx, PkeySfx, PkeyMfx },
    };

    /// <summary>
    /// 安装时需删除的冲突槽位（互斥性，2026-10-15 实测：与原厂 SFX/MFX 共存 ⇒ 本 APO 零加载）。
    /// 调用方必须已做全量备份（Snapshot/BackupToRegFile）。
    /// </summary>
    public static string[] ConflictingValueNames(InstallMode mode) => mode switch
    {
        InstallMode.LfxOnly or InstallMode.LfxGfx => new[] { PkeyGfx, PkeySfx, PkeyMfx },
        InstallMode.SfxMfx => new[] { PkeyLfx, PkeyGfx },
        _ => Array.Empty<string>(),
    };

    public static bool WritesProcessingModes(InstallMode mode) => mode is InstallMode.SfxMfx or InstallMode.Both;

    // ---- 注册表路径 ----
    public const string RenderRoot = @"SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render";
    public const string CaptureRoot = @"SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Capture";

    /// <summary>
    /// 原厂 APO 移交区：安装时把被删除的原厂 SFX/MFX CLSID 存到这里（按设备 GUID 分子键，
    /// 值名 = 槽位值名），DrcApo 在 Initialize 时读出并 CoCreate 串联（child APO 模式，
    /// EQAPO 同款）⇒ 处理顺序 = 原厂 SFX → 原厂 MFX → DRC，原厂音效不丢。
    /// 依据：gfx_test.py（2026-10-15）实测 GFX 与原厂 SFX/MFX 共存零加载 ⇒ 集合级互斥成立，
    /// 唯一两全解 = 包装器串联。
    /// </summary>
    public const string ChildAposRoot = @"SOFTWARE\DrcApo\ChildApos";

    /// <summary>会被移交 ChildApos 的槽位（均为混音前效果，顺序 = SFX → MFX → 本 APO）。</summary>
    public static readonly string[] ChildWrappedSlots = { PkeySfx, PkeyMfx };

    public const string PkeyDeviceFriendlyName = "{a45c254e-df1c-4efd-8020-67d146a850e0},14";

    // ---- 退出码 ----
    public const int ExitOk = 0;
    public const int ExitNoAdmin = 2;
    public const int ExitUsage = 3;
    public const int ExitError = 4;
}
