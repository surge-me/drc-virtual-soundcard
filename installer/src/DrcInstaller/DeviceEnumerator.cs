using System.Runtime.InteropServices;
using Microsoft.Win32;

namespace DrcInstaller;

internal enum EDataFlow { eRender = 0, eCapture = 1, eAll = 2 }
internal enum ERole { eConsole = 0, eMultimedia = 1, eCommunications = 2 }
[Flags] internal enum DeviceState { ACTIVE = 0x1, DISABLED = 0x2, NOTPRESENT = 0x4, UNPLUGGED = 0x8, ALL = 0xF }

internal sealed class DeviceInfo
{
    public required string Guid { get; init; }        // {xxxxxxxx-....}（注册表子键名）
    public required string EndpointId { get; init; }  // {0.0.0.00000000}.{guid}
    public required string FriendlyName { get; init; }
    public bool IsDefault { get; init; }
    public override string ToString() => $"{Guid}  \"{FriendlyName}\"{(IsDefault ? "  [默认]" : "")}";
}

/// <summary>
/// MMDeviceEnumerator COM interop（活跃渲染设备枚举）+ 注册表友好名回读。
/// 只读；不修改任何注册表。
/// </summary>
internal static class DeviceEnumerator
{
    [ComImport, Guid("BCDE0395-E52F-467C-8E3D-C4579291692E")]
    private class MMDeviceEnumeratorComObject { }

    [ComImport, Guid("A95664D2-9614-4F35-A746-DE8DB63617E6"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface IMMDeviceEnumerator
    {
        [PreserveSig] int EnumAudioEndpoints(EDataFlow dataFlow, DeviceState dwStateMask, out IMMDeviceCollection ppDevices);
        [PreserveSig] int GetDefaultAudioEndpoint(EDataFlow dataFlow, ERole role, out IMMDevice ppEndpoint);
        [PreserveSig] int GetDevice([MarshalAs(UnmanagedType.LPWStr)] string pwstrId, out IMMDevice ppDevice);
        [PreserveSig] int RegisterEndpointNotificationCallback(IntPtr pClient);
        [PreserveSig] int UnregisterEndpointNotificationCallback(IntPtr pClient);
    }

    [ComImport, Guid("0BD7A1BE-7A1A-44DB-8397-CC5392387B5E"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface IMMDeviceCollection
    {
        [PreserveSig] int GetCount(out int pcDevices);
        [PreserveSig] int Item(int nDevice, out IMMDevice ppDevice);
    }

    [ComImport, Guid("D666063F-1587-4E43-81F1-B948E807363F"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface IMMDevice
    {
        [PreserveSig] int Activate(ref Guid iid, int dwClsCtx, IntPtr pActivationParams, out IntPtr ppInterface);
        [PreserveSig] int OpenPropertyStore(int stgmAccess, out IntPtr ppProperties);
        [PreserveSig] int GetId([MarshalAs(UnmanagedType.LPWStr)] out string ppstrId);
        [PreserveSig] int GetState(out int pdwState);
    }

    /// <summary>枚举活跃渲染设备（EDataFlow.eRender, ACTIVE），并标注默认设备。</summary>
    public static List<DeviceInfo> ListActiveRender()
    {
        var result = new List<DeviceInfo>();
        string? defaultId = TryGetDefaultEndpointId();

        var enumerator = (IMMDeviceEnumerator)new MMDeviceEnumeratorComObject();
        int hr = enumerator.EnumAudioEndpoints(EDataFlow.eRender, DeviceState.ACTIVE, out IMMDeviceCollection collection);
        if (hr != 0) throw new InvalidOperationException($"EnumAudioEndpoints 失败, HRESULT=0x{hr:X8}");

        hr = collection.GetCount(out int count);
        if (hr != 0) throw new InvalidOperationException($"IMMDeviceCollection.GetCount 失败, HRESULT=0x{hr:X8}");

        for (int i = 0; i < count; i++)
        {
            if (collection.Item(i, out IMMDevice device) != 0) continue;
            if (device.GetId(out string id) != 0) continue;
            string guid = ExtractDeviceGuid(id);
            result.Add(new DeviceInfo
            {
                Guid = guid,
                EndpointId = id,
                FriendlyName = ReadFriendlyName(guid) ?? id,
                IsDefault = defaultId != null && string.Equals(id, defaultId, StringComparison.OrdinalIgnoreCase),
            });
        }
        return result;
    }

    private static string? TryGetDefaultEndpointId()
    {
        try
        {
            var enumerator = (IMMDeviceEnumerator)new MMDeviceEnumeratorComObject();
            if (enumerator.GetDefaultAudioEndpoint(EDataFlow.eRender, ERole.eMultimedia, out IMMDevice dev) != 0) return null;
            return dev.GetId(out string id) == 0 ? id : null;
        }
        catch { return null; }
    }

    /// <summary>从 endpoint id `{0.0.0.00000000}.{guid}` 提取设备 GUID（含大括号，小写）。</summary>
    public static string ExtractDeviceGuid(string endpointId)
    {
        int open = endpointId.LastIndexOf('{');
        int close = endpointId.LastIndexOf('}');
        if (open >= 0 && close > open) return endpointId.Substring(open, close - open + 1).ToLowerInvariant();
        return endpointId.ToLowerInvariant();
    }

    /// <summary>从注册表 Properties 回读友好名（REG_SZ）。多候选键，失败返回 null。</summary>
    public static string? ReadFriendlyName(string deviceGuid)
    {
        // 候选顺序：Device_FriendlyName → DeviceInterface_FriendlyName → Device_DeviceDesc
        string[] candidates =
        {
            "{a45c254e-df1c-4efd-8020-67d146a850e0},14",
            "{b3f8fa53-0004-438e-9003-51a46e139bfc},6",
            "{a45c254e-df1c-4efd-8020-67d146a850e0},2",
        };
        try
        {
            using var baseKey = RegistryKey.OpenBaseKey(RegistryHive.LocalMachine, RegistryView.Registry64);
            using var props = baseKey.OpenSubKey($@"{Constants.RenderRoot}\{deviceGuid}\Properties", false);
            if (props == null) return null;
            foreach (var name in candidates)
                if (props.GetValue(name) is string s && s.Length > 0) return s;
            return null;
        }
        catch { return null; }
    }

    /// <summary>按 GUID / endpointId 匹配设备（忽略大小写与外围大括号）。</summary>
    public static DeviceInfo? Find(List<DeviceInfo> devices, string token)
    {
        string norm = token.Trim().Trim('{', '}').ToLowerInvariant();
        return devices.FirstOrDefault(d =>
            string.Equals(d.Guid.Trim('{', '}'), norm, StringComparison.OrdinalIgnoreCase) ||
            string.Equals(d.EndpointId, token, StringComparison.OrdinalIgnoreCase));
    }
}
