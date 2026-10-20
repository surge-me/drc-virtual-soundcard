using System.Diagnostics;
using Microsoft.Win32;

namespace DrcGui.Core;

/// <summary>
/// 设置页所需的 Windows 集成（非 IPC 通道，不涉及 GUI↔APO 数据流）：
/// ① 开机自启 = 写 HKCU\...\CurrentVersion\Run；② 音频端点枚举 = 只读 HKLM MMDevices。
/// </summary>
public static class WinIntegration
{
    const string RunKeyPath = @"Software\Microsoft\Windows\CurrentVersion\Run";
    const string RunValueName = "DrcGui";

    // 音频端点属性（MMDevices\...\Properties 下的值名）。实测（2026-10-15）：
    //   {a45c254e...},0  → 空
    //   {a45c254e...},2  → 端点描述（"扬声器" / "CABLE Input" / "Headphones"）
    //   {a45c254e...},14 → 空
    //   {b3f8fa53...},6  → 适配器名（"USB 耳机" / "VB-Audio Virtual Cable" / …）
    // 端点描述会重名（三个设备都是「扬声器」）⇒ 必须带适配器名才能区分。
    // 显示名 = 描述 + " (" + 适配器 + ")"，与 Windows 声音设置、以及 router 侧
    // PKEY_Device_FriendlyName 合成的完整名一致（router 的 --render 就按该串匹配）。
    const string PkeyEndpointDesc = "{a45c254e-df1c-4efd-8020-67d146a850e0},2";
    const string PkeyAdapterName = "{b3f8fa53-0004-438e-9003-51a46e139bfc},6";
    const int DeviceStateActive = 0x1;

    public static string ExePath =>
        Environment.ProcessPath ?? Process.GetCurrentProcess().MainModule?.FileName ?? string.Empty;

    /// <summary>开机自启是否已开启（HKCU Run 值存在）。</summary>
    public static bool IsAutoStartEnabled()
    {
        try
        {
            using var key = Registry.CurrentUser.OpenSubKey(RunKeyPath);
            return key?.GetValue(RunValueName) is string s && s.Length > 0;
        }
        catch { return false; }
    }

    /// <summary>开/关开机自启；返回是否成功。</summary>
    public static bool SetAutoStart(bool enabled)
    {
        try
        {
            using var key = Registry.CurrentUser.CreateSubKey(RunKeyPath, writable: true);
            if (key == null) return false;
            if (enabled)
            {
                if (ExePath.Length == 0) return false;
                key.SetValue(RunValueName, $"\"{ExePath}\"", RegistryValueKind.String);
            }
            else
            {
                key.DeleteValue(RunValueName, throwOnMissingValue: false);
            }
            return true;
        }
        catch { return false; }
    }

    /// <summary>枚举当前「活动」输出（Render）设备：返回 (设备GUID, 端点描述, 适配器名)。只读，不修改系统。</summary>
    public static List<(string Id, string Desc, string Adapter)> EnumerateRenderDevices() =>
        EnumerateDevices("Render");

    /// <summary>枚举当前「活动」输入（Capture）设备：返回 (设备GUID, 端点描述, 适配器名)。只读。</summary>
    public static List<(string Id, string Desc, string Adapter)> EnumerateCaptureDevices() =>
        EnumerateDevices("Capture");

    /// <summary>与 Windows 声音设置一致的完整显示名：「端点描述 (适配器名)」。</summary>
    public static string DisplayName(string desc, string adapter) =>
        string.IsNullOrWhiteSpace(adapter) ? desc : $"{desc} ({adapter})";

    static List<(string Id, string Desc, string Adapter)> EnumerateDevices(string kind)
    {
        var list = new List<(string, string, string)>();
        try
        {
            using var root = Registry.LocalMachine.OpenSubKey(
                @"SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\" + kind);
            if (root == null) return list;
            foreach (var gid in root.GetSubKeyNames())
            {
                using var dev = root.OpenSubKey(gid);
                if (dev == null) continue;
                if (dev.GetValue("DeviceState") is not int state || state != DeviceStateActive) continue;
                using var props = dev.OpenSubKey("Properties");
                var desc = props?.GetValue(PkeyEndpointDesc) as string ?? "";
                var adapter = props?.GetValue(PkeyAdapterName) as string ?? "";
                if (string.IsNullOrWhiteSpace(desc) && string.IsNullOrWhiteSpace(adapter))
                    desc = gid;   // 两项都读不到 ⇒ 至少让条目可辨认
                list.Add((gid, desc, adapter));
            }
        }
        catch { /* 读注册表失败 → 返回已收集部分（可能为空） */ }
        return list;
    }
}
