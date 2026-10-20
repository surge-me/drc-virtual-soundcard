using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;
using Microsoft.Win32;

namespace DrcInstaller;

/// <summary>
/// 原生注册表 API（advapi32）。用于以**最小访问掩码**打开 MMDevices 键并写值——
/// .NET 的 RegistryKey.CreateSubKey 会请求 KEY_ALL_ACCESS，而该键 DACL 只给
/// BUILTIN\Administrators「SetValue, ReadKey」⇒ 被 Access denied（TASK-003A 根因）。
/// 参考手法：Equalizer APO `RegistryHelper`（openKey/writeValue/takeOwnership/makeWritable）。
/// </summary>
internal static class NativeRegistry
{
    public static readonly IntPtr HKEY_LOCAL_MACHINE = new(unchecked((int)0x80000002));

    public const int KEY_QUERY_VALUE = 0x0001;
    public const int KEY_SET_VALUE = 0x0002;
    public const int KEY_CREATE_SUB_KEY = 0x0004;
    public const int KEY_ENUMERATE_SUB_KEYS = 0x0008;
    public const int KEY_NOTIFY = 0x0010;
    public const int READ_CONTROL = 0x00020000;
    public const int WRITE_DAC = 0x00040000;
    public const int WRITE_OWNER = 0x00080000;
    public const int KEY_READ = 0x00020019;
    public const int KEY_WRITE = 0x00020006;
    public const int KEY_ALL_ACCESS = 0x000F003F;
    public const int KEY_WOW64_64KEY = 0x00000100;

    public const int ERROR_SUCCESS = 0;
    public const int ERROR_FILE_NOT_FOUND = 2;
    public const int ERROR_ACCESS_DENIED = 5;

    public const int REG_SZ = 1, REG_EXPAND_SZ = 2, REG_BINARY = 3, REG_DWORD = 4, REG_MULTI_SZ = 7, REG_QWORD = 11;

    public const int OWNER_SECURITY_INFORMATION = 0x1;
    public const int GROUP_SECURITY_INFORMATION = 0x2;
    public const int DACL_SECURITY_INFORMATION = 0x4;

    [DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern int RegOpenKeyEx(IntPtr hKey, string lpSubKey, int ulOptions, int samDesired, out IntPtr phkResult);

    [DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern int RegCreateKeyEx(IntPtr hKey, string lpSubKey, int reserved, string? lpClass, int dwOptions,
        int samDesired, IntPtr lpSecurityAttributes, out IntPtr phkResult, out int lpdwDisposition);

    [DllImport("advapi32.dll", SetLastError = true)]
    public static extern int RegCloseKey(IntPtr hKey);

    [DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern int RegSetValueEx(IntPtr hKey, string lpValueName, int reserved, int dwType, byte[] lpData, int cbData);

    [DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern int RegDeleteValue(IntPtr hKey, string lpValueName);

    [DllImport("advapi32.dll", SetLastError = true)]
    public static extern int RegGetKeySecurity(IntPtr hKey, int securityInformation, byte[]? pSecurityDescriptor, ref int lpcbSecurityDescriptor);

    [DllImport("advapi32.dll", SetLastError = true)]
    public static extern int RegSetKeySecurity(IntPtr hKey, int securityInformation, byte[] pSecurityDescriptor);

    // ---------------- 便捷封装 ----------------

    /// <summary>以指定访问掩码打开键；失败抛 Win32Exception（含错误码）。</summary>
    public static IntPtr Open(string subKey, int samDesired)
    {
        int rc = RegOpenKeyEx(HKEY_LOCAL_MACHINE, subKey, 0, samDesired, out IntPtr h);
        if (rc != ERROR_SUCCESS)
            throw new Win32Exception(rc, $"RegOpenKeyEx 失败: HKLM\\{subKey}  samDesired=0x{samDesired:X5}");
        return h;
    }

    /// <summary>尝试打开；失败返回 IntPtr.Zero 并输出错误码。</summary>
    public static IntPtr TryOpen(string subKey, int samDesired, out int rc)
    {
        rc = RegOpenKeyEx(HKEY_LOCAL_MACHINE, subKey, 0, samDesired, out IntPtr h);
        return rc == ERROR_SUCCESS ? h : IntPtr.Zero;
    }

    public static void SetRaw(IntPtr h, string name, int regType, byte[] data)
    {
        int rc = RegSetValueEx(h, name, 0, regType, data, data.Length);
        if (rc != ERROR_SUCCESS) throw new Win32Exception(rc, $"RegSetValueEx 失败: \"{name}\"");
    }

    public static void SetString(IntPtr h, string name, string value) =>
        SetRaw(h, name, REG_SZ, Encoding.Unicode.GetBytes(value + "\0"));

    public static void SetMultiString(IntPtr h, string name, string[] items)
    {
        var sb = new StringBuilder();
        foreach (var s in items) sb.Append(s).Append('\0');
        sb.Append('\0');
        SetRaw(h, name, REG_MULTI_SZ, Encoding.Unicode.GetBytes(sb.ToString()));
    }

    /// <summary>按 RegValue（.reg 快照中的类型+数据）写回一个值。</summary>
    public static void SetRegValue(IntPtr h, string name, RegValue v)
    {
        switch (v.Kind)
        {
            case RegistryValueKind.String:
                SetString(h, name, v.Data as string ?? string.Empty); break;
            case RegistryValueKind.ExpandString:
                SetRaw(h, name, REG_EXPAND_SZ, Encoding.Unicode.GetBytes((v.Data as string ?? string.Empty) + "\0")); break;
            case RegistryValueKind.DWord:
                SetRaw(h, name, REG_DWORD, BitConverter.GetBytes(Convert.ToInt32(v.Data))); break;
            case RegistryValueKind.QWord:
                SetRaw(h, name, REG_QWORD, BitConverter.GetBytes(Convert.ToInt64(v.Data))); break;
            case RegistryValueKind.MultiString:
                SetMultiString(h, name, (string[])(v.Data ?? Array.Empty<string>())); break;
            case RegistryValueKind.Binary:
                SetRaw(h, name, REG_BINARY, (byte[])(v.Data ?? Array.Empty<byte>())); break;
            default:
                SetRaw(h, name, REG_BINARY, Array.Empty<byte>()); break;
        }
    }

    public static void DeleteValue(IntPtr h, string name)
    {
        int rc = RegDeleteValue(h, name);
        if (rc != ERROR_SUCCESS && rc != ERROR_FILE_NOT_FOUND)
            throw new Win32Exception(rc, $"RegDeleteValue 失败: \"{name}\"");
    }
}
