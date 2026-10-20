using System.Runtime.InteropServices;
using System.Windows.Interop;

namespace DrcGui.Core;

/// <summary>可被全局快捷键触发的动作（TASK-013 P1）。</summary>
public enum HotkeyAction
{
    Bypass,
    CeilingUp,
    CeilingDown,
    FloorUp,
    FloorDown,
}

/// <summary>
/// 全局快捷键（RegisterHotKey，挂在主窗口 HWND 上）：
/// 窗口 Hide() 到托盘后 HWND 仍在、消息循环仍在，WM_HOTKEY 照常送达 ⇒ 不用前台也能改参数。
/// 默认键位：Ctrl+Alt+B 旁路 · Ctrl+Alt+↑/↓ 峰值上限 ±0.5dB · Ctrl+Alt+Shift+↑/↓ 噪声下限 ±0.5dB。
/// 注册失败（被游戏/其它软件占用）必须如实记录到 <see cref="Status"/>，不许静默失败。
/// </summary>
public sealed class Hotkeys : IDisposable
{
    const int WmHotkey = 0x0312;
    const uint ModAlt = 0x0001, ModControl = 0x0002, ModShift = 0x0004, ModNoRepeat = 0x4000;

    readonly Dictionary<int, HotkeyAction> _ids = new();
    HwndSource? _src;
    int _ok, _total;

    public event Action<HotkeyAction>? Triggered;

    /// <summary>注册结果摘要（单行，托盘/日志用）。</summary>
    public string Status { get; private set; } = "未注册";

    /// <summary>逐条明细（设置页逐行显示）：组合键 / 动作 / 是否注册成功。</summary>
    public IReadOnlyList<(string Combo, string Action, bool Ok)> Entries { get; private set; }
        = Array.Empty<(string, string, bool)>();

    public void Init(HwndSource source)
    {
        _src = source;
        source.AddHook(WndProc);
    }

    public void RegisterAll()
    {
        if (_src == null) { Status = "未注册（无窗口句柄）"; return; }
        var table = new (HotkeyAction Action, uint Mods, uint Vk)[]
        {
            (HotkeyAction.Bypass,      ModControl | ModAlt, 0x42),                        // B
            (HotkeyAction.CeilingUp,   ModControl | ModAlt, 0x26),                        // ↑
            (HotkeyAction.CeilingDown, ModControl | ModAlt, 0x28),                        // ↓
            (HotkeyAction.FloorUp,     ModControl | ModAlt | ModShift, 0x26),             // Shift+↑
            (HotkeyAction.FloorDown,   ModControl | ModAlt | ModShift, 0x28),             // Shift+↓
        };
        _ok = 0; _total = table.Length;
        int id = 0x4C00;
        var rows = new List<string>();
        var entries = new List<(string Combo, string Action, bool Ok)>();
        foreach (var (action, mods, vk) in table)
        {
            ++id;
            bool ok = RegisterHotKey(_src.Handle, id, mods | ModNoRepeat, vk);
            if (ok) { _ids[id] = action; ++_ok; }
            rows.Add($"{ComboText(mods, vk)} {Label(action)}{(ok ? "" : "（被占用）")}");
            entries.Add((ComboText(mods, vk), Label(action), ok));
        }
        // 逐条列出全部组合键：只说"5/5"但只写三项说明，用户会以为是漏了（实测报障）
        Entries = entries;
        Status = $"（{_ok}/{_total}）：" + string.Join(" · ", rows);
    }

    static string ComboText(uint mods, uint vk)
    {
        var parts = new List<string>();
        if ((mods & ModControl) != 0) parts.Add("Ctrl");
        if ((mods & ModAlt) != 0) parts.Add("Alt");
        if ((mods & ModShift) != 0) parts.Add("Shift");
        parts.Add(vk switch
        {
            0x42 => "B",
            0x26 => "↑",
            0x28 => "↓",
            _ => $"0x{vk:X2}",
        });
        return string.Join("+", parts);
    }

    static string Label(HotkeyAction a) => a switch
    {
        HotkeyAction.Bypass => "旁路 开/关",
        HotkeyAction.CeilingUp => "峰值上限 +0.5 dB（枪声压回这条线）",
        HotkeyAction.CeilingDown => "峰值上限 −0.5 dB（枪声压回这条线）",
        HotkeyAction.FloorUp => "噪声下限 +0.5 dB（低于它的才抬）",
        HotkeyAction.FloorDown => "噪声下限 −0.5 dB（低于它的才抬）",
        _ => a.ToString(),
    };

    IntPtr WndProc(IntPtr hwnd, int msg, IntPtr wParam, IntPtr lParam, ref bool handled)
    {
        if (msg != WmHotkey) return IntPtr.Zero;
        if (_ids.TryGetValue(wParam.ToInt32(), out var action))
        {
            Triggered?.Invoke(action);
            handled = true;
        }
        return IntPtr.Zero;
    }

    [DllImport("user32.dll", SetLastError = true)]
    static extern bool RegisterHotKey(IntPtr hWnd, int id, uint fsModifiers, uint vk);

    [DllImport("user32.dll", SetLastError = true)]
    static extern bool UnregisterHotKey(IntPtr hWnd, int id);

    public void Dispose()
    {
        if (_src != null)
        {
            foreach (int id in _ids.Keys) UnregisterHotKey(_src.Handle, id);
            _src.RemoveHook(WndProc);
        }
        _ids.Clear();
        _src = null;
    }
}
