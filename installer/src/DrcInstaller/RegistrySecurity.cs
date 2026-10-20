using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;

namespace DrcInstaller;

/// <summary>
/// 注册表键 Owner/DACL 的原生处理（TASK-003A）。
/// 手法参考 Equalizer APO `RegistryHelper::takeOwnership` / `makeWritable`：
///   ① 启用 SeTakeOwnership/SeBackup/SeRestore（AdjustTokenPrivileges，作用于**进程令牌** ⇒ 持句柄线程同样生效）；
///   ② RegOpenKeyEx(WRITE_OWNER) + RegSetKeySecurity(OWNER) 设 Owner = BUILTIN\Administrators；
///   ③ 取所有权后 owner 隐含 READ_CONTROL|WRITE_DAC ⇒ RegOpenKeyEx(WRITE_DAC) + RegSetKeySecurity(DACL) 追加完全控制 ACE。
/// Owner/DACL 快照以 SDDL 文本落盘，卸载时原样还原。
/// </summary>
internal static class RegistrySecurity
{
    private const int TOKEN_ADJUST_PRIVILEGES = 0x0020;
    private const int TOKEN_QUERY = 0x0008;
    private const uint SE_PRIVILEGE_ENABLED = 0x00000002;
    private const int ERROR_NOT_ALL_ASSIGNED = 1300;
    private const int SDDL_REVISION_1 = 1;

    /// <summary>BUILTIN\Administrators 完全控制 + 容器/对象继承（SDDL ACE）。</summary>
    private const string AdminFullControlAce = "(A;OICI;KA;;;BA)";

    [StructLayout(LayoutKind.Sequential)]
    private struct LUID { public uint LowPart; public int HighPart; }

    [StructLayout(LayoutKind.Sequential)]
    private struct TOKEN_PRIVILEGES { public int PrivilegeCount; public LUID Luid; public uint Attributes; }

    [DllImport("advapi32.dll", SetLastError = true)]
    private static extern bool OpenProcessToken(IntPtr h, int access, out IntPtr token);

    [DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool LookupPrivilegeValue(string? system, string name, out LUID luid);

    [DllImport("advapi32.dll", SetLastError = true)]
    private static extern bool AdjustTokenPrivileges(IntPtr token, bool disableAll, ref TOKEN_PRIVILEGES state,
        int bufLen, IntPtr prev, IntPtr retLen);

    [DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool ConvertSecurityDescriptorToStringSecurityDescriptor(byte[] sd, int revision, int secInfo,
        out IntPtr outStr, out int outLen);

    [DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool ConvertStringSecurityDescriptorToSecurityDescriptor(string sddl, int revision,
        out IntPtr pSd, out int sdLen);

    [DllImport("advapi32.dll", SetLastError = true)]
    private static extern int GetSecurityDescriptorLength(IntPtr pSd);

    [DllImport("kernel32.dll")]
    private static extern IntPtr GetCurrentProcess();

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool CloseHandle(IntPtr h);

    [DllImport("kernel32.dll")]
    private static extern IntPtr LocalFree(IntPtr h);

    // ---------------- 特权 ----------------

    /// <summary>启用特权（进程令牌）。返回逐项 (名称 → 是否已启用/持有)。</summary>
    public static Dictionary<string, bool> EnablePrivileges(params string[] names)
    {
        var result = new Dictionary<string, bool>();
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, out IntPtr token))
            throw new Win32Exception(Marshal.GetLastWin32Error(), "OpenProcessToken 失败");

        try
        {
            foreach (var name in names)
            {
                if (!LookupPrivilegeValue(null, name, out LUID luid))
                {
                    result[name] = false;
                    continue;
                }
                var tp = new TOKEN_PRIVILEGES
                {
                    PrivilegeCount = 1,
                    Luid = luid,
                    Attributes = SE_PRIVILEGE_ENABLED,
                };
                bool ok = AdjustTokenPrivileges(token, false, ref tp, 0, IntPtr.Zero, IntPtr.Zero);
                int err = Marshal.GetLastWin32Error();
                result[name] = ok && err != ERROR_NOT_ALL_ASSIGNED;
            }
        }
        finally { CloseHandle(token); }
        return result;
    }

    // ---------------- SDDL 快照 / 还原 ----------------

    /// <summary>读取键 Owner+Group+DACL 的 SDDL 文本（需 READ_CONTROL）。</summary>
    public static string SnapshotSddl(string subKey)
    {
        int secInfo = NativeRegistry.OWNER_SECURITY_INFORMATION | NativeRegistry.GROUP_SECURITY_INFORMATION
                    | NativeRegistry.DACL_SECURITY_INFORMATION;

        IntPtr h = NativeRegistry.Open(subKey, NativeRegistry.READ_CONTROL | NativeRegistry.KEY_WOW64_64KEY);
        try
        {
            int size = 0;
            NativeRegistry.RegGetKeySecurity(h, secInfo, null, ref size);
            if (size <= 0) throw new Win32Exception(Marshal.GetLastWin32Error(), "RegGetKeySecurity 取长度失败");
            var sd = new byte[size];
            int rc = NativeRegistry.RegGetKeySecurity(h, secInfo, sd, ref size);
            if (rc != NativeRegistry.ERROR_SUCCESS) throw new Win32Exception(rc, "RegGetKeySecurity 读取安全描述符失败");

            if (!ConvertSecurityDescriptorToStringSecurityDescriptor(sd, SDDL_REVISION_1, secInfo, out IntPtr str, out _))
                throw new Win32Exception(Marshal.GetLastWin32Error(), "安全描述符转 SDDL 失败");
            try { return Marshal.PtrToStringUni(str) ?? string.Empty; }
            finally { LocalFree(str); }
        }
        finally { NativeRegistry.RegCloseKey(h); }
    }

    /// <summary>把 SDDL 文本还原到键（Owner+Group+DACL）。需 SeRestore/SeTakeOwnership。</summary>
    public static void RestoreSddl(string subKey, string sddl)
    {
        EnablePrivileges("SeRestorePrivilege", "SeTakeOwnershipPrivilege", "SeBackupPrivilege");

        int secInfo = NativeRegistry.OWNER_SECURITY_INFORMATION | NativeRegistry.GROUP_SECURITY_INFORMATION
                    | NativeRegistry.DACL_SECURITY_INFORMATION;

        IntPtr h = NativeRegistry.Open(subKey,
            NativeRegistry.WRITE_OWNER | NativeRegistry.WRITE_DAC | NativeRegistry.KEY_WOW64_64KEY);
        try
        {
            if (!ConvertStringSecurityDescriptorToSecurityDescriptor(sddl, SDDL_REVISION_1, out IntPtr pSd, out _))
                throw new Win32Exception(Marshal.GetLastWin32Error(), "SDDL 转安全描述符失败");
            try
            {
                int len = GetSecurityDescriptorLength(pSd);
                var sd = new byte[len];
                Marshal.Copy(pSd, sd, 0, len);
                int rc = NativeRegistry.RegSetKeySecurity(h, secInfo, sd);
                if (rc != NativeRegistry.ERROR_SUCCESS) throw new Win32Exception(rc, "RegSetKeySecurity 还原失败");
            }
            finally { LocalFree(pSd); }
        }
        finally { NativeRegistry.RegCloseKey(h); }
    }

    // ---------------- 取所有权 / 授予完全控制 ----------------

    /// <summary>启用 SeTakeOwnership 后把键 Owner 设为 BUILTIN\Administrators。</summary>
    public static void TakeOwnership(string subKey)
    {
        var privs = EnablePrivileges("SeTakeOwnershipPrivilege");
        if (!privs.GetValueOrDefault("SeTakeOwnershipPrivilege"))
            throw new InvalidOperationException("SeTakeOwnershipPrivilege 不可用（需以管理员身份运行）");

        IntPtr h = NativeRegistry.Open(subKey, NativeRegistry.WRITE_OWNER | NativeRegistry.KEY_WOW64_64KEY);
        try
        {
            byte[] sd = SddlToBytes("O:BAG:BAD:" + AdminFullControlAce);
            int rc = NativeRegistry.RegSetKeySecurity(h, NativeRegistry.OWNER_SECURITY_INFORMATION, sd);
            if (rc != NativeRegistry.ERROR_SUCCESS) throw new Win32Exception(rc, "RegSetKeySecurity(OWNER) 失败");
        }
        finally { NativeRegistry.RegCloseKey(h); }
    }

    /// <summary>在当前 DACL 基础上追加 BUILTIN\Administrators 完全控制 ACE（需 WRITE_DAC）。</summary>
    public static void GrantAdministratorsFullControl(string subKey)
    {
        string current = SnapshotSddl(subKey);
        string newSddl = BuildSddlWithAdminAce(current);

        IntPtr h = NativeRegistry.Open(subKey, NativeRegistry.WRITE_DAC | NativeRegistry.KEY_WOW64_64KEY);
        try
        {
            byte[] sd = SddlToBytes(newSddl);
            int rc = NativeRegistry.RegSetKeySecurity(h, NativeRegistry.DACL_SECURITY_INFORMATION, sd);
            if (rc != NativeRegistry.ERROR_SUCCESS) throw new Win32Exception(rc, "RegSetKeySecurity(DACL) 失败");
        }
        finally { NativeRegistry.RegCloseKey(h); }
    }

    /// <summary>把 Administrators 完全控制 ACE 前插到原 DACL 之前（保留 O/G/S 段与原有 ACE）。</summary>
    internal static string BuildSddlWithAdminAce(string sddl)
    {
        var sec = SplitSddl(sddl);
        string aces = AcesOf(sec.GetValueOrDefault('D'));
        string newDacl = "D:PAI" + AdminFullControlAce + aces;
        return sec.GetValueOrDefault('O', "") + sec.GetValueOrDefault('G', "") + newDacl + sec.GetValueOrDefault('S', "");
    }

    /// <summary>
    /// 规范化 SDDL 供逐字比对：保留 Owner/Group + DACL/SACL 的 **ACE 序列**，剔除 DACL/SACL 控制标志
    /// （AI/P/AR 等为系统重算的元信息，RegSetKeySecurity 不回写 ⇒ 非语义差异）。
    /// </summary>
    public static string NormalizeForCompare(string sddl)
    {
        var sec = SplitSddl(sddl);
        return sec.GetValueOrDefault('O', "") + "|" + sec.GetValueOrDefault('G', "")
             + "|D:" + AcesOf(sec.GetValueOrDefault('D'))
             + "|S:" + AcesOf(sec.GetValueOrDefault('S'));
    }

    private static Dictionary<char, string> SplitSddl(string sddl)
    {
        var result = new Dictionary<char, string>();
        int depth = 0, start = -1;
        char cur = '\0';
        for (int i = 0; i < sddl.Length; i++)
        {
            char c = sddl[i];
            if (c == '(') depth++;
            else if (c == ')') depth--;
            else if (depth == 0 && i + 1 < sddl.Length && sddl[i + 1] == ':' && "OGDS".IndexOf(c) >= 0)
            {
                if (cur != '\0') result[cur] = sddl.Substring(start, i - start);
                cur = c; start = i;
            }
        }
        if (cur != '\0') result[cur] = sddl.Substring(start);
        return result;
    }

    private static string AcesOf(string? section)
    {
        if (string.IsNullOrEmpty(section)) return string.Empty;
        int idx = section.IndexOf('(');
        return idx < 0 ? string.Empty : section.Substring(idx);
    }

    private static byte[] SddlToBytes(string sddl)
    {
        if (!ConvertStringSecurityDescriptorToSecurityDescriptor(sddl, SDDL_REVISION_1, out IntPtr pSd, out _))
            throw new Win32Exception(Marshal.GetLastWin32Error(), "SDDL 转安全描述符失败");
        try
        {
            int len = GetSecurityDescriptorLength(pSd);
            var sd = new byte[len];
            Marshal.Copy(pSd, sd, 0, len);
            return sd;
        }
        finally { LocalFree(pSd); }
    }
}
