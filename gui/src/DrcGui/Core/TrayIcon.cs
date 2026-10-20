using System.Runtime.InteropServices;

namespace DrcGui.Core;

/// <summary>
/// 托盘（Shell_NotifyIcon interop 自实现，禁第三方托盘库）：
/// 左键单击/双击 = 打开主界面；右键 = 菜单；图标随 bypass 切换红/灰。
/// </summary>
public sealed class TrayIcon : IDisposable
{
    const uint WmAppTray = 0x8000 + 0x0001;
    const uint NimAdd = 0, NimModify = 1, NimDelete = 2;
    const uint NifMessage = 0x1, NifIcon = 0x2, NifTip = 0x4;
    const uint WmLbuttonUp = 0x0202, WmLbuttonDblclk = 0x0203, WmRbuttonUp = 0x0205;

    public event Action? OpenRequested;
    public event Action? MenuRequested;

    IntPtr _iconNormal = IntPtr.Zero;
    IntPtr _iconBypass = IntPtr.Zero;
    IntPtr _hWnd = IntPtr.Zero;
    bool _added;
    bool _bypass;

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct NOTIFYICONDATAW
    {
        public uint cbSize;          // 首成员：Shell_NotifyIcon 必填（= sizeof(NOTIFYICONDATAW)）
        public IntPtr hWnd;
        public uint uID;
        public uint uFlags;
        public uint uCallbackMessage;
        public IntPtr hIcon;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string szTip;
        public uint dwState;
        public uint dwStateMask;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 256)] public string szInfo;
        public uint uVersion;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 64)] public string szInfoTitle;
        public uint dwInfoFlags;
        public Guid guidItem;
        public IntPtr hBalloonIcon;
    }

    [DllImport("shell32.dll", CharSet = CharSet.Unicode)]
    private static extern bool Shell_NotifyIconW(uint dwMessage, ref NOTIFYICONDATAW lpData);

    [DllImport("user32.dll")]
    private static extern bool DestroyIcon(IntPtr hIcon);

    public void Init(System.Windows.Interop.HwndSource source)
    {
        _hWnd = source.Handle;
        source.AddHook(WndProc);
        _iconNormal = IconFactory.CreateWaveIcon(false);
        _iconBypass = IconFactory.CreateWaveIcon(true);
        var d = MakeData();
        _added = Shell_NotifyIconW(NimAdd, ref d);   // 如实记录结果，失败不谎报
    }

    /// <summary>托盘图标是否已成功注册（供自动化核验）。</summary>
    public bool IsRegistered => _added;

    string _tip = "DRC 动态范围控制";

    /// <summary>更新托盘提示（快捷键改参数后让用户不用开窗口也能确认当前值）。</summary>
    public void SetTooltip(string text)
    {
        _tip = text;
        if (!_added) return;
        var d = MakeData();
        Shell_NotifyIconW(NimModify, ref d);
    }

    public void SetBypass(bool bypass)
    {
        if (_bypass == bypass) return;
        _bypass = bypass;
        if (!_added) return;
        var d = MakeData();
        Shell_NotifyIconW(NimModify, ref d);
    }

    IntPtr WndProc(IntPtr hwnd, int msg, IntPtr wParam, IntPtr lParam, ref bool handled)
    {
        if (msg != WmAppTray) return IntPtr.Zero;
        uint evt = unchecked((uint)lParam.ToInt64());
        if (evt == WmLbuttonUp || evt == WmLbuttonDblclk)
            OpenRequested?.Invoke();
        else if (evt == WmRbuttonUp)
            MenuRequested?.Invoke();
        handled = true;
        return IntPtr.Zero;
    }

    NOTIFYICONDATAW MakeData() => new()
    {
        cbSize = (uint)Marshal.SizeOf<NOTIFYICONDATAW>(),
        hWnd = _hWnd,
        uID = 1,
        uFlags = NifMessage | NifIcon | NifTip,
        uCallbackMessage = WmAppTray,
        hIcon = _bypass ? _iconBypass : _iconNormal,
        szTip = _tip,
    };

    public void Dispose()
    {
        if (_added)
        {
            var d = MakeData();
            Shell_NotifyIconW(NimDelete, ref d);
            _added = false;
        }
        if (_iconNormal != IntPtr.Zero) DestroyIcon(_iconNormal);
        if (_iconBypass != IntPtr.Zero) DestroyIcon(_iconBypass);
        _iconNormal = _iconBypass = IntPtr.Zero;
    }
}
