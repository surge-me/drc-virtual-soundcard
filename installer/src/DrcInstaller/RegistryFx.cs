using Microsoft.Win32;

namespace DrcInstaller;

/// <summary>
/// FxProperties 注册表操作（64 位视图）。所有写入前必须已备份（由调用方保证）。
/// </summary>
internal static class RegistryFx
{
    public static string FxSubKey(string deviceGuid, bool capture) =>
        $@"{(capture ? Constants.CaptureRoot : Constants.RenderRoot)}\{deviceGuid}\FxProperties";

    public static string FxHklmPath(string deviceGuid, bool capture) =>
        "HKEY_LOCAL_MACHINE\\" + FxSubKey(deviceGuid, capture);

    private static RegistryKey OpenRoot() => RegistryKey.OpenBaseKey(RegistryHive.LocalMachine, RegistryView.Registry64);

    public static bool FxKeyExists(string deviceGuid, bool capture)
    {
        using var root = OpenRoot();
        using var key = root.OpenSubKey(FxSubKey(deviceGuid, capture), false);
        return key != null;
    }

    /// <summary>
    /// 以最小访问掩码打开 FxProperties 供写值（KEY_READ|KEY_SET_VALUE）。
    /// 不能用 RegistryKey.CreateSubKey/OpenSubKey(writable) —— 它们请求 KEY_ALL_ACCESS/KEY_WRITE，
    /// 而该键 DACL 仅给 Administrators「SetValue, ReadKey」⇒ Access denied（TASK-003A 根因）。
    /// </summary>
    public static IntPtr OpenForWrite(string deviceGuid, bool capture) =>
        NativeRegistry.Open(FxSubKey(deviceGuid, capture),
            NativeRegistry.KEY_READ | NativeRegistry.KEY_SET_VALUE | NativeRegistry.KEY_WOW64_64KEY);

    /// <summary>写权限不足时的升级路径：启用特权 → 取所有权 → 追加 Administrators 完全控制 ACE。</summary>
    public static void EscalateWritability(string deviceGuid, bool capture, Action<string> log)
    {
        string sub = FxSubKey(deviceGuid, capture);
        var privs = RegistrySecurity.EnablePrivileges(
            "SeTakeOwnershipPrivilege", "SeBackupPrivilege", "SeRestorePrivilege");
        foreach (var kv in privs) log($"  特权 {kv.Key}: {(kv.Value ? "已启用" : "不可用")}");
        log("  取所有权 → Owner = BUILTIN\\Administrators");
        RegistrySecurity.TakeOwnership(sub);
        log("  追加 ACE → BUILTIN\\Administrators 完全控制（保留原 DACL 其余条目）");
        RegistrySecurity.GrantAdministratorsFullControl(sub);
    }

    /// <summary>读取键 Owner+Group+DACL 的 SDDL 文本（快照用）。</summary>
    public static string SnapshotSddl(string deviceGuid, bool capture) => RegistrySecurity.SnapshotSddl(FxSubKey(deviceGuid, capture));

    /// <summary>把 SDDL 快照还原到键。</summary>
    public static void RestoreSddl(string deviceGuid, bool capture, string sddl) =>
        RegistrySecurity.RestoreSddl(FxSubKey(deviceGuid, capture), sddl);

    /// <summary>读取 FxProperties 全部值快照（名 → 类型 + 数据）。</summary>
    public static Dictionary<string, RegValue> Snapshot(string deviceGuid, bool capture)
    {
        var result = new Dictionary<string, RegValue>(StringComparer.OrdinalIgnoreCase);
        using var root = OpenRoot();
        using var key = root.OpenSubKey(FxSubKey(deviceGuid, capture), false);
        if (key == null) return result;
        foreach (var name in key.GetValueNames())
        {
            var kind = key.GetValueKind(name);
            object? data = key.GetValue(name, null, RegistryValueOptions.DoNotExpandEnvironmentNames);
            result[name] = new RegValue(kind, data);
        }
        return result;
    }

    /// <summary>导出该设备 FxProperties 全量值到 .reg（写盘）。</summary>
    public static void BackupToRegFile(string deviceGuid, bool capture, string regPath)
    {
        var snap = Snapshot(deviceGuid, capture);
        Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(regPath))!);
        RegFile.Write(regPath, FxHklmPath(deviceGuid, capture), snap);
    }

    /// <summary>比较两份快照，返回差异描述（空 = 完全一致）。</summary>
    public static List<string> Diff(IReadOnlyDictionary<string, RegValue> expected, IReadOnlyDictionary<string, RegValue> actual)
    {
        var diffs = new List<string>();
        foreach (var kv in expected)
        {
            if (!actual.TryGetValue(kv.Key, out var a)) { diffs.Add($"缺失: {kv.Key}"); continue; }
            if (a.Kind != kv.Value.Kind) { diffs.Add($"类型不同: {kv.Key} 期望 {kv.Value.Kind} 实际 {a.Kind}"); continue; }
            if (!DataEquals(kv.Value.Data, a.Data)) diffs.Add($"数据不同: {kv.Key}");
        }
        foreach (var name in actual.Keys)
            if (!expected.ContainsKey(name)) diffs.Add($"多余: {name}");
        return diffs;
    }

    private static bool DataEquals(object? a, object? b)
    {
        if (a == null || b == null) return a == b;
        if (a is byte[] ba && b is byte[] bb) return ba.AsSpan().SequenceEqual(bb);
        if (a is string[] sa && b is string[] sb) return sa.SequenceEqual(sb, StringComparer.Ordinal);
        return a.Equals(b);
    }

    /// <summary>
    /// 接入状态：FxProperties 中 LFX/GFX/SFX/MFX/EFX 是否有值 == 本项目 CLSID。
    /// 返回 (是否接入, 命中的值名列表)。
    /// </summary>
    public static (bool installed, List<string> hits) Status(string deviceGuid, bool capture)
    {
        var hits = new List<string>();
        var snap = Snapshot(deviceGuid, capture);
        foreach (var name in Constants.AllFxValueNames)
        {
            if (snap.TryGetValue(name, out var v) && v.Data is string s &&
                string.Equals(s.Trim(), Constants.DrcApoClsid, StringComparison.OrdinalIgnoreCase))
                hits.Add(name);
        }
        return (hits.Count > 0, hits);
    }
}
