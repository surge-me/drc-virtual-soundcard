using System.ComponentModel;
using System.Diagnostics;
using System.Security.Principal;
using System.Text;
using Microsoft.Win32;

namespace DrcInstaller;

internal static class Program
{
    private sealed class Options
    {
        public string? Command;
        public string? Device;
        public string? Dll = "DrcApo.dll";
        public string? BackupDir;
        public bool Capture;
        public Constants.InstallMode Mode = Constants.InstallMode.LfxOnly;
    }

    private static int Main(string[] args)
    {
        Console.OutputEncoding = Encoding.UTF8;
        Options opt;
        try { opt = ParseArgs(args); }
        catch (Exception ex) { Console.Error.WriteLine($"[error] 参数错误: {ex.Message}"); PrintUsage(); return Constants.ExitUsage; }

        if (opt.Command == null) { PrintUsage(); return Constants.ExitUsage; }

        try
        {
            return opt.Command switch
            {
                "status" => CmdStatus(opt),
                "dry-run" => CmdDryRun(opt),
                "install" => CmdInstall(opt),
                "uninstall" => CmdUninstall(opt),
                "selftest" => CmdSelfTest(),
                "diag" => CmdDiag(opt),
                "diag-acl" => CmdDiagAcl(opt),
                _ => Unknown(opt.Command),
            };
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"[error] {ex.Message}");
            return Constants.ExitError;
        }
    }

    // ---------------- 命令实现 ----------------

    private static int CmdStatus(Options opt)
    {
        var dev = ResolveDevice(opt);
        string key = RegistryFx.FxHklmPath(dev.Guid, opt.Capture);
        var (installed, hits) = RegistryFx.Status(dev.Guid, opt.Capture);

        Console.WriteLine($"[status] 设备 {dev.Guid}  \"{dev.FriendlyName}\"");
        Console.WriteLine($"  键: {key}  (存在={(RegistryFx.FxKeyExists(dev.Guid, opt.Capture) ? "是" : "否")})");
        Console.WriteLine($"  接入状态: {(installed ? "已接入" : "未接入")}");
        if (hits.Count > 0) Console.WriteLine($"  命中值: {string.Join(", ", hits)}");
        Console.WriteLine($"  本项目 CLSID: {Constants.DrcApoClsid}");
        return Constants.ExitOk;
    }

    private static int CmdDryRun(Options opt)
    {
        var dev = ResolveDevice(opt);
        string key = RegistryFx.FxHklmPath(dev.Guid, opt.Capture);
        string backupPath = BackupPath(opt, dev.Guid);
        string aclPath = AclPath(opt, dev.Guid);
        var snap = RegistryFx.Snapshot(dev.Guid, opt.Capture);

        Console.WriteLine($"[dry-run] 目标设备 {dev.Guid}  \"{dev.FriendlyName}\"");
        Console.WriteLine($"  目标键: {key}");
        Console.WriteLine($"  现有 FxProperties 值数: {snap.Count}");
        Console.WriteLine($"  写入模式: {ModeText(opt.Mode)}");
        Console.WriteLine("  将写入（REG_SZ）:");
        foreach (var name in Constants.WriteValueNames(opt.Mode))
            Console.WriteLine($"    \"{name}\"  REG_SZ  \"{Constants.DrcApoClsid}\"   [{LabelOf(name)}]");
        if (Constants.WritesProcessingModes(opt.Mode))
            foreach (var m in ProcessingModeNames())
                Console.WriteLine($"    \"{m}\"  REG_MULTI_SZ  \"{Constants.DefaultProcessingMode}\"   [处理模式，仅当不存在时写]");
        var conflicts = Constants.ConflictingValueNames(opt.Mode).Where(n => snap.ContainsKey(n)).ToList();
        if (conflicts.Count > 0)
        {
            Console.WriteLine("  将删除（FX 槽位互斥，原值在备份中）:");
            foreach (var name in conflicts)
                Console.WriteLine($"    \"{name}\"   [{LabelOf(name)}]");
            var wrapped = conflicts.Where(n => Constants.ChildWrappedSlots.Contains(n) && snap.ContainsKey(n)).ToList();
            if (wrapped.Count > 0)
                Console.WriteLine($"  将移交原厂 CLSID → HKLM\\{Constants.ChildAposRoot}\\{dev.Guid}（{string.Join(", ", wrapped)}）⇒ DrcApo 运行时串联原厂音效");
        }
        Console.WriteLine("  写权限：最小访问掩码 KEY_READ|KEY_SET_VALUE；被拒则自动升级（取所有权 + 授予 Administrators 完全控制）");
        Console.WriteLine($"  备份-值:  {backupPath}  (已存在={(File.Exists(backupPath) ? "是" : "否")})");
        Console.WriteLine($"  备份-ACL: {aclPath}  (已存在={(File.Exists(aclPath) ? "是" : "否")})");
        Console.WriteLine($"  regsvr32 注册: {ResolveDll(opt)}");
        Console.WriteLine("  （dry-run：不写盘）");
        return Constants.ExitOk;
    }

    private static int CmdInstall(Options opt)
    {
        if (!IsAdmin()) { Console.Error.WriteLine("[error] 需要管理员权限（写 HKLM\\...\\MMDevices\\Audio）。请以管理员身份运行。"); return Constants.ExitNoAdmin; }

        var dev = ResolveDevice(opt);
        string backupPath = BackupPath(opt, dev.Guid);
        string aclPath = AclPath(opt, dev.Guid);
        string key = RegistryFx.FxHklmPath(dev.Guid, opt.Capture);

        Console.WriteLine($"[install] 设备 {dev.Guid}  \"{dev.FriendlyName}\"");
        Console.WriteLine($"  目标键: {key}");
        Console.WriteLine($"  写入模式: {ModeText(opt.Mode)}");

        // ① 值备份（.reg，文件名带模式区分，绝不覆盖既有备份）
        if (File.Exists(backupPath))
            Console.WriteLine($"  [备份-值] 已存在，跳过覆盖: {backupPath}");
        else
        {
            RegistryFx.BackupToRegFile(dev.Guid, opt.Capture, backupPath);
            Console.WriteLine($"  [备份-值] FxProperties 全量值 → {backupPath}");
        }

        // ② Owner + DACL 快照（SDDL）
        if (File.Exists(aclPath))
            Console.WriteLine($"  [备份-ACL] 已存在，跳过覆盖: {aclPath}");
        else
        {
            string sddl = RegistryFx.SnapshotSddl(dev.Guid, opt.Capture);
            File.WriteAllText(aclPath, sddl + Environment.NewLine, new UTF8Encoding(false));
            Console.WriteLine($"  [备份-ACL] Owner+DACL → {aclPath}");
            Console.WriteLine($"             {sddl}");
        }

        // ③ 写值（最小访问掩码；Access denied 才升级 ACL 后重试）
        try
        {
            ApplyValues(opt, dev.Guid);
        }
        catch (Win32Exception ex) when (ex.NativeErrorCode == NativeRegistry.ERROR_ACCESS_DENIED)
        {
            Console.WriteLine($"  [ACL] 直接写入被拒（{ex.Message}）⇒ 升级：取所有权 + 授予 Administrators 完全控制");
            RegistryFx.EscalateWritability(dev.Guid, opt.Capture, Console.WriteLine);
            ApplyValues(opt, dev.Guid);
        }

        string dll = ResolveDll(opt);
        if (File.Exists(dll)) RunRegsvr32(dll, unregister: false);
        else Console.WriteLine($"  [regsvr32] 跳过：DLL 不存在 ({dll})；如需注册请用 --dll <路径>");

        var (installed, hits) = RegistryFx.Status(dev.Guid, opt.Capture);
        Console.WriteLine($"  [校验] 接入状态: {(installed ? "已接入" : "未接入")}{(hits.Count > 0 ? "  命中: " + string.Join(", ", hits) : "")}");
        return installed ? Constants.ExitOk : Constants.ExitError;
    }

    /// <summary>以最小访问掩码写值；被拒抛 Win32Exception(ERROR_ACCESS_DENIED)。</summary>
    private static void ApplyValues(Options opt, string deviceGuid)
    {
        var snap = RegistryFx.Snapshot(deviceGuid, opt.Capture);
        IntPtr h = RegistryFx.OpenForWrite(deviceGuid, opt.Capture);
        try
        {
            foreach (var name in Constants.WriteValueNames(opt.Mode))
            {
                NativeRegistry.SetString(h, name, Constants.DrcApoClsid);
                Console.WriteLine($"  [写入] \"{name}\" = \"{Constants.DrcApoClsid}\" (REG_SZ)  [{LabelOf(name)}]");
            }
            if (Constants.WritesProcessingModes(opt.Mode))
                foreach (var m in ProcessingModeNames())
                {
                    if (snap.ContainsKey(m)) { Console.WriteLine($"  [模式] \"{m}\" 已存在，跳过"); continue; }
                    NativeRegistry.SetMultiString(h, m, new[] { Constants.DefaultProcessingMode });
                    Console.WriteLine($"  [模式] \"{m}\" = \"{Constants.DefaultProcessingMode}\" (REG_MULTI_SZ)");
                }
            // FX 槽位互斥（2026-10-15 实测）：冲突槽位不删 ⇒ 本 APO 零加载（已备份，可还原）
            // 删除前把原厂 SFX/MFX CLSID 移交 ChildApos ⇒ DrcApo 运行时 CoCreate 串联（原厂 → DRC）
            SaveChildApos(snap, deviceGuid);
            foreach (var name in Constants.ConflictingValueNames(opt.Mode))
            {
                if (!snap.ContainsKey(name)) continue;
                NativeRegistry.DeleteValue(h, name);
                Console.WriteLine($"  [删除] \"{name}\"  [{LabelOf(name)}]  (FX 槽位互斥；原值在备份中)");
            }
        }
        finally { NativeRegistry.RegCloseKey(h); }
    }

    /// <summary>
    /// 把设备 FxProperties 里的原厂 SFX/MFX CLSID（REG_SZ）拷贝到
    /// HKLM\{ChildAposRoot}\{设备GUID}\{槽位值名}，供 DrcApo::Initialize 读取并串联。
    /// 只拷字符串值；GFX 等其它被删槽位不在移交范围（GFX 为混音后效果，无法在 LFX 位置保序）。
    /// </summary>
    private static void SaveChildApos(IReadOnlyDictionary<string, RegValue> snap, string deviceGuid)
    {
        var saved = new List<string>();
        foreach (var name in Constants.ChildWrappedSlots)
        {
            if (!snap.TryGetValue(name, out var v) ||
                v.Kind != RegistryValueKind.String || v.Data is not string clsid ||
                string.IsNullOrWhiteSpace(clsid))
            {
                continue;
            }
            using (var k = Registry.LocalMachine.CreateSubKey(
                       $@"{Constants.ChildAposRoot}\{deviceGuid}", writable: true))
            {
                k.SetValue(name, clsid, RegistryValueKind.String);
            }
            saved.Add($"\"{name}\" = \"{clsid}\"");
        }
        Console.WriteLine(saved.Count > 0
            ? $"  [child] 原厂 APO 移交 → HKLM\\{Constants.ChildAposRoot}\\{deviceGuid}  ({string.Join("; ", saved)})"
            : "  [child] 无原厂 SFX/MFX 可移交（槽位本就为空）");
    }

    private static int CmdUninstall(Options opt)
    {
        if (!IsAdmin()) { Console.Error.WriteLine("[error] 需要管理员权限。请以管理员身份运行。"); return Constants.ExitNoAdmin; }

        var dev = ResolveDevice(opt);
        string? backupPath = ResolveBackupFile(opt, dev.Guid);
        string? aclFile = ResolveAclFile(opt, dev.Guid);

        Console.WriteLine($"[uninstall] 设备 {dev.Guid}  \"{dev.FriendlyName}\"");
        if (backupPath == null)
        {
            Console.Error.WriteLine($"  [error] 未找到值备份（{BackupPath(opt, dev.Guid)} 或 {dev.Guid}.reg）—— 拒绝无备份还原");
            return Constants.ExitError;
        }

        var backup = RegFile.Read(backupPath);
        Console.WriteLine($"  [备份-值] 读入 {backup.Count} 个值 ← {backupPath}");

        try
        {
            RestoreValues(opt, dev.Guid, backup);
        }
        catch (Win32Exception ex) when (ex.NativeErrorCode == NativeRegistry.ERROR_ACCESS_DENIED)
        {
            Console.WriteLine($"  [ACL] 还原被拒（{ex.Message}）⇒ 升级：取所有权 + 授予 Administrators 完全控制");
            RegistryFx.EscalateWritability(dev.Guid, opt.Capture, Console.WriteLine);
            RestoreValues(opt, dev.Guid, backup);
        }

        var diffs = RegistryFx.Diff(backup, RegistryFx.Snapshot(dev.Guid, opt.Capture));
        Console.WriteLine(diffs.Count == 0 ? "  [自检] 键值 diff = 空" : "  [自检] 键值 diff = 非空:\n    " + string.Join("\n    ", diffs));

        // 清理 ChildApos 设备子键（原厂 CLSID 已随 FxProperties 还原，移交区不再需要）
        try
        {
            Registry.LocalMachine.DeleteSubKeyTree($@"{Constants.ChildAposRoot}\{dev.Guid}", throwOnMissingSubKey: false);
            Console.WriteLine($"  [child] 已清理 HKLM\\{Constants.ChildAposRoot}\\{dev.Guid}");
        }
        catch (Exception ex) { Console.WriteLine($"  [child] 清理失败（不阻断卸载）: {ex.Message}"); }

        // Owner/DACL 还原（原厂 DACL 常不给 BA 的 WD/WO 且 Owner=服务 SID ⇒ 打开被拒；
        // 与值还原同款：ACCESS_DENIED ⇒ 升级（取所有权+授 BA 完全控制）后重试）
        if (aclFile != null)
        {
            string sddl = File.ReadAllText(aclFile).Trim();
            try
            {
                RegistryFx.RestoreSddl(dev.Guid, opt.Capture, sddl);
            }
            catch (Win32Exception ex) when (ex.NativeErrorCode == NativeRegistry.ERROR_ACCESS_DENIED)
            {
                Console.WriteLine($"  [ACL] 还原打开被拒（{ex.Message}）⇒ 升级：取所有权 + 授予 Administrators 完全控制");
                RegistryFx.EscalateWritability(dev.Guid, opt.Capture, Console.WriteLine);
                RegistryFx.RestoreSddl(dev.Guid, opt.Capture, sddl);
            }
            Console.WriteLine($"  [还原-ACL] Owner+DACL ← {aclFile}");
        }
        else Console.WriteLine("  [还原-ACL] 无 ACL 快照，跳过");

        string dll = ResolveDll(opt);
        if (File.Exists(dll)) RunRegsvr32(dll, unregister: true);
        else Console.WriteLine($"  [regsvr32] 跳过：DLL 不存在 ({dll})；如需反注册请用 --dll <路径>");

        return diffs.Count == 0 ? Constants.ExitOk : Constants.ExitError;
    }

    private static void RestoreValues(Options opt, string deviceGuid, IReadOnlyDictionary<string, RegValue> backup)
    {
        var current = RegistryFx.Snapshot(deviceGuid, opt.Capture);
        var removed = new List<string>();
        IntPtr h = RegistryFx.OpenForWrite(deviceGuid, opt.Capture);
        try
        {
            foreach (var kv in backup) NativeRegistry.SetRegValue(h, kv.Key, kv.Value);
            foreach (var name in current.Keys)
                if (!backup.ContainsKey(name)) { NativeRegistry.DeleteValue(h, name); removed.Add(name); }
        }
        finally { NativeRegistry.RegCloseKey(h); }
        Console.WriteLine($"  [还原-值] 写回 {backup.Count} 个；删除多余 {removed.Count} 个{(removed.Count > 0 ? "（" + string.Join(", ", removed) + "）" : "")}");
    }

    /// <summary>诊断：探测各访问掩码是否被 DACL 放行 + 一次 KEY_SET_VALUE 探针写/删（只读为主）。</summary>
    private static int CmdDiag(Options opt)
    {
        var dev = ResolveDevice(opt);
        string sub = RegistryFx.FxSubKey(dev.Guid, opt.Capture);
        Console.WriteLine($"[diag] 键: HKLM\\{sub}");
        Console.WriteLine($"  IsAdmin = {IsAdmin()}");

        (string Name, int Mask)[] probes =
        {
            ("KEY_READ", NativeRegistry.KEY_READ),
            ("KEY_QUERY_VALUE", NativeRegistry.KEY_QUERY_VALUE),
            ("KEY_SET_VALUE", NativeRegistry.KEY_SET_VALUE),
            ("KEY_READ|KEY_SET_VALUE", NativeRegistry.KEY_READ | NativeRegistry.KEY_SET_VALUE),
            ("KEY_WRITE", NativeRegistry.KEY_WRITE),
            ("KEY_ALL_ACCESS", NativeRegistry.KEY_ALL_ACCESS),
            ("READ_CONTROL", NativeRegistry.READ_CONTROL),
            ("WRITE_DAC", NativeRegistry.WRITE_DAC),
            ("WRITE_OWNER", NativeRegistry.WRITE_OWNER),
        };
        foreach (var (name, mask) in probes)
        {
            IntPtr h = NativeRegistry.TryOpen(sub, mask | NativeRegistry.KEY_WOW64_64KEY, out int rc);
            Console.WriteLine($"  open 0x{mask:X5} {name,-24} => {(h != IntPtr.Zero ? "OK" : $"DENIED (rc={rc})")}");
            if (h != IntPtr.Zero) NativeRegistry.RegCloseKey(h);
        }

        // 探针写：KEY_SET_VALUE 写一个无名值再删（不触碰任何既有值）
        const string probeName = "DrcProbeTemp";
        IntPtr ph = NativeRegistry.TryOpen(sub,
            NativeRegistry.KEY_READ | NativeRegistry.KEY_SET_VALUE | NativeRegistry.KEY_WOW64_64KEY, out int prc);
        if (ph == IntPtr.Zero)
        {
            Console.WriteLine($"  [探针写] 打不开 KEY_SET_VALUE (rc={prc})");
            return Constants.ExitError;
        }
        try
        {
            NativeRegistry.SetString(ph, probeName, "1");
            Console.WriteLine($"  [探针写] RegSetValueEx(\"{probeName}\") => OK");
            NativeRegistry.DeleteValue(ph, probeName);
            Console.WriteLine($"  [探针写] RegDeleteValue(\"{probeName}\") => OK（已清理）");
        }
        finally { NativeRegistry.RegCloseKey(ph); }

        Console.WriteLine("  Owner/DACL SDDL:");
        Console.WriteLine("    " + RegistryFx.SnapshotSddl(dev.Guid, opt.Capture));
        return Constants.ExitOk;
    }

    /// <summary>
    /// 诊断（破坏性自证）：升级 ACL（取所有权+授予完全控制）→ 验证 KEY_ALL_ACCESS 放行 → 还原 Owner/DACL → 逐字比对。
    /// 用于证明升级/还原机制可用且可逆；结束后键安全属性回到原样。
    /// </summary>
    private static int CmdDiagAcl(Options opt)
    {
        var dev = ResolveDevice(opt);
        string sub = RegistryFx.FxSubKey(dev.Guid, opt.Capture);
        string before = RegistryFx.SnapshotSddl(dev.Guid, opt.Capture);
        Console.WriteLine("[diag-acl] 升级前 SDDL:");
        Console.WriteLine("  " + before);

        RegistryFx.EscalateWritability(dev.Guid, opt.Capture, Console.WriteLine);

        string during = RegistryFx.SnapshotSddl(dev.Guid, opt.Capture);
        Console.WriteLine("[diag-acl] 升级后 SDDL:");
        Console.WriteLine("  " + during);
        IntPtr h = NativeRegistry.TryOpen(sub, NativeRegistry.KEY_ALL_ACCESS | NativeRegistry.KEY_WOW64_64KEY, out int rc);
        Console.WriteLine($"  KEY_ALL_ACCESS 打开 => {(h != IntPtr.Zero ? "OK" : $"DENIED (rc={rc})")}");
        if (h != IntPtr.Zero) NativeRegistry.RegCloseKey(h);

        Console.WriteLine("[diag-acl] 还原 Owner/DACL ...");
        RegistryFx.RestoreSddl(dev.Guid, opt.Capture, before);
        string after = RegistryFx.SnapshotSddl(dev.Guid, opt.Capture);
        Console.WriteLine("[diag-acl] 还原后 SDDL:");
        Console.WriteLine("  " + after);

        string nb = RegistrySecurity.NormalizeForCompare(before);
        string na = RegistrySecurity.NormalizeForCompare(after);
        bool same = string.Equals(nb, na, StringComparison.Ordinal);
        Console.WriteLine(same
            ? "[diag-acl] PASS — 还原后 Owner/Group + ACE 序列与升级前一致（仅 DACL 控制标志 AI 由系统重算，非语义差异）"
            : "[diag-acl] FAIL — Owner/Group 或 ACE 序列不一致");
        return same ? Constants.ExitOk : Constants.ExitError;
    }

    /// <summary>离线自测：.reg 写出 → 读回 → 逐值比对（不碰注册表，证明备份/还原往返正确）。</summary>
    private static int CmdSelfTest()
    {
        var sample = new Dictionary<string, RegValue>(StringComparer.OrdinalIgnoreCase)
        {
            ["{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},5"] = new(RegistryValueKind.String, "{C9453E73-8C5C-4463-9984-AF8BAB2F5447}"),
            ["{d3993a3f-99c2-4402-b5ec-a92a0367664b},5"] = new(RegistryValueKind.MultiString, new[] { "{C18E2F7E-933D-4965-B7D1-1EEF228D2AF3}" }),
            ["{9c119480-ddc2-4954-a150-5bd240d454ad},7"] = new(RegistryValueKind.Binary, new byte[] { 0x11, 0, 0, 0, 1, 0, 0, 0, 255, 255, 0, 0 }),
            ["DwordSample"] = new(RegistryValueKind.DWord, 1),
            ["QwordSample"] = new(RegistryValueKind.QWord, 1234567890123L),
            ["ExpandSample"] = new(RegistryValueKind.ExpandString, @"%SystemRoot%\x"),
        };

        string tmp = Path.Combine(Path.GetTempPath(), "drc-selftest.reg");
        RegFile.Write(tmp, "HKEY_LOCAL_MACHINE\\SOFTWARE\\_DRC_SELFTEST_", sample);
        var readBack = RegFile.Read(tmp);

        Console.WriteLine($"[selftest] .reg 往返: 写出 {sample.Count} 值 → {tmp}");
        var diffs = RegistryFx.Diff(sample, readBack);
        if (diffs.Count == 0)
        {
            Console.WriteLine("[selftest] PASS — 往返键值 diff = 空");
            return Constants.ExitOk;
        }
        Console.WriteLine("[selftest] FAIL — diff 非空:");
        foreach (var d in diffs) Console.WriteLine("    " + d);
        return Constants.ExitError;
    }

    private static int Unknown(string cmd)
    {
        Console.Error.WriteLine($"[error] 未知命令: {cmd}");
        PrintUsage();
        return Constants.ExitUsage;
    }

    // ---------------- 辅助 ----------------

    private static DeviceInfo ResolveDevice(Options opt)
    {
        var devices = DeviceEnumerator.ListActiveRender();
        if (devices.Count == 0) throw new InvalidOperationException("未发现活跃渲染设备（MMDeviceEnumerator 返回 0 个）。");

        if (!string.IsNullOrWhiteSpace(opt.Device))
        {
            var found = DeviceEnumerator.Find(devices, opt.Device!)
                ?? throw new InvalidOperationException($"未找到匹配设备: {opt.Device}（候选 {devices.Count} 个）");
            return found;
        }

        var def = devices.FirstOrDefault(d => d.IsDefault);
        if (def != null) return def;

        Console.WriteLine($"[info] 无默认渲染设备，改用首个活跃设备（共 {devices.Count} 个候选）:");
        foreach (var d in devices) Console.WriteLine($"       {d}");
        return devices[0];
    }

    private static string ResolveDll(Options opt)
    {
        if (string.IsNullOrWhiteSpace(opt.Dll)) return "DrcApo.dll";
        return Path.IsPathRooted(opt.Dll!) ? opt.Dll! : Path.GetFullPath(opt.Dll!);
    }

    private static string BackupDirOf(Options opt) => Path.GetFullPath(opt.BackupDir ?? DefaultBackupDir());

    /// <summary>值备份路径：{设备GUID}.{模式}.reg（带模式区分，避免覆盖他模式/旧版备份）。</summary>
    private static string BackupPath(Options opt, string deviceGuid) =>
        Path.Combine(BackupDirOf(opt), $"{deviceGuid}.{ModeKey(opt.Mode)}.reg");

    /// <summary>ACL 快照路径：{设备GUID}.{模式}.acl.txt（SDDL 文本）。</summary>
    private static string AclPath(Options opt, string deviceGuid) =>
        Path.Combine(BackupDirOf(opt), $"{deviceGuid}.{ModeKey(opt.Mode)}.acl.txt");

    private static string ModeKey(Constants.InstallMode m) => m switch
    {
        Constants.InstallMode.LfxOnly => "lfx-only",
        Constants.InstallMode.LfxGfx => "lfx-gfx",
        Constants.InstallMode.SfxMfx => "sfx-mfx",
        _ => "both",
    };

    /// <summary>卸载定位备份：模式专属 → 旧式 {guid}.reg → 目录内最新 {guid}.*.reg。</summary>
    private static string? ResolveBackupFile(Options opt, string deviceGuid)
    {
        string dir = BackupDirOf(opt);
        string exact = BackupPath(opt, deviceGuid);
        if (File.Exists(exact)) return exact;
        string legacy = Path.Combine(dir, $"{deviceGuid}.reg");
        if (File.Exists(legacy)) return legacy;
        return NewestMatch(dir, $"{deviceGuid}.*.reg");
    }

    private static string? ResolveAclFile(Options opt, string deviceGuid)
    {
        string dir = BackupDirOf(opt);
        string exact = AclPath(opt, deviceGuid);
        if (File.Exists(exact)) return exact;
        return NewestMatch(dir, $"{deviceGuid}.*.acl.txt");
    }

    private static string? NewestMatch(string dir, string pattern)
    {
        if (!Directory.Exists(dir)) return null;
        return Directory.GetFiles(dir, pattern).OrderByDescending(File.GetLastWriteTimeUtc).FirstOrDefault();
    }

    private static string DefaultBackupDir()
    {
        string cwd = Environment.CurrentDirectory;
        string installerDir = Path.Combine(cwd, "installer");
        return Directory.Exists(installerDir) ? Path.Combine(installerDir, "backup") : Path.Combine(cwd, "backup");
    }

    private static bool IsAdmin()
    {
        using var id = WindowsIdentity.GetCurrent();
        return new WindowsPrincipal(id).IsInRole(WindowsBuiltInRole.Administrator);
    }

    private static void RunRegsvr32(string dll, bool unregister)
    {
        string exe = Path.Combine(Environment.SystemDirectory, "regsvr32.exe");
        string args = unregister ? $"/s /u \"{dll}\"" : $"/s \"{dll}\"";
        Console.WriteLine($"  [regsvr32] {exe} {args}");
        try
        {
            var psi = new ProcessStartInfo(exe, args) { UseShellExecute = false };
            using var p = Process.Start(psi);
            p?.WaitForExit(30000);
            Console.WriteLine($"  [regsvr32] 退出码 = {p?.ExitCode}");
        }
        catch (Exception ex) { Console.WriteLine($"  [regsvr32] 失败: {ex.Message}"); }
    }

    private static Options ParseArgs(string[] args)
    {
        var o = new Options();
        for (int i = 0; i < args.Length; i++)
        {
            string a = args[i];
            switch (a)
            {
                case "--install": case "--uninstall": case "--dry-run": case "--status":
                case "--selftest": case "--diag": case "--diag-acl":
                    o.Command = a.TrimStart('-'); break;
                case "--device": o.Device = Next(args, ref i, a); break;
                case "--dll": o.Dll = Next(args, ref i, a); break;
                case "--backup-dir": o.BackupDir = Next(args, ref i, a); break;
                case "--mode": o.Mode = ParseMode(Next(args, ref i, a)); break;
                case "--capture": o.Capture = true; break;
                case "--render": o.Capture = false; break;
                case "--help": case "-h": o.Command = null; break;
                default:
                    if (a.StartsWith('-')) throw new ArgumentException($"未知参数 {a}");
                    break;
            }
        }
        return o;
    }

    private static Constants.InstallMode ParseMode(string v) => v.ToLowerInvariant() switch
    {
        "lfx-only" or "lfx1" => Constants.InstallMode.LfxOnly,
        "lfx-gfx" or "gfx" => Constants.InstallMode.LfxGfx,
        "sfx-mfx" or "sfx" or "mfx" => Constants.InstallMode.SfxMfx,
        "both" => Constants.InstallMode.Both,
        _ => throw new ArgumentException($"未知 --mode 取值: {v}（可选 lfx-only | lfx-gfx | sfx-mfx | both）"),
    };

    private static string ModeText(Constants.InstallMode m) => m switch
    {
        Constants.InstallMode.LfxOnly => "lfx-only（Win11 24H2 实测唯一可用；删除冲突槽位）",
        Constants.InstallMode.LfxGfx => "lfx-gfx（SPEC §5 历史；24H2 同 CLSID 双槽不可用）",
        Constants.InstallMode.SfxMfx => "sfx-mfx（24H2 实测被引擎弃图）",
        _ => "both（LFX/GFX + SFX/MFX）",
    };

    private static string LabelOf(string valueName) => valueName switch
    {
        Constants.PkeyLfx => "LFX(PKEY_FX_PreMixEffectClsid)",
        Constants.PkeyGfx => "GFX(PKEY_FX_PostMixEffectClsid)",
        Constants.PkeySfx => "SFX(PKEY_FX_StreamEffectClsid)",
        Constants.PkeyMfx => "MFX(PKEY_FX_ModeEffectClsid)",
        Constants.PkeyEfx => "EFX(PKEY_FX_EndpointEffectClsid)",
        _ => valueName,
    };

    private static IEnumerable<string> ProcessingModeNames() =>
        new[] { Constants.PkeySfxModes, Constants.PkeyMfxModes };

    private static string Next(string[] args, ref int i, string flag)
    {
        if (i + 1 >= args.Length) throw new ArgumentException($"{flag} 缺少取值");
        return args[++i];
    }

    private static void PrintUsage()
    {
        Console.WriteLine("DrcInstaller — DRC APO FX 链注册表注入/备份/卸载");
        Console.WriteLine($"  APO CLSID = {Constants.DrcApoClsid}（TASK-002 冻结）");
        Console.WriteLine();
        Console.WriteLine("用法: DrcInstaller <命令> [选项]");
        Console.WriteLine("命令:");
        Console.WriteLine("  --install      备份值+Owner/DACL → 写 CLSID（权限不足自动升级 ACL）→ 注册 DLL（需管理员）");
        Console.WriteLine("  --uninstall    按备份还原值 + 还原 Owner/DACL + regsvr32 /u（需管理员）");
        Console.WriteLine("  --dry-run      只打印将写入的键/值/类型，不落盘");
        Console.WriteLine("  --status       检测设备 FX 链是否已含本项目 CLSID（GUI「接入状态」同口径）");
        Console.WriteLine("  --selftest     离线自测 .reg 往返（不碰注册表）");
        Console.WriteLine("  --diag         诊断：探测各访问掩码放行情况 + Owner/DACL SDDL");
        Console.WriteLine("  --diag-acl     诊断（破坏性自证）：升级 ACL → 验证 → 还原 → 逐字比对");
        Console.WriteLine("选项:");
        Console.WriteLine("  --device <GUID>      指定设备（默认=默认渲染设备）；缺省自动枚举");
        Console.WriteLine("  --dll <路径>         DrcApo.dll 路径（默认 DrcApo.dll，供 regsvr32）");
        Console.WriteLine("  --backup-dir <目录>  备份目录（默认 installer/backup）");
        Console.WriteLine("  --mode <模式>        lfx-gfx(默认,SPEC §5) | sfx-mfx(Win8.1+ 引擎链) | both");
        Console.WriteLine("  --capture            对 Capture 设备操作（默认 Render）");
    }
}
